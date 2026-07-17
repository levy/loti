// The local control channel shared by lotid (server) and loti (client).
//
// A Unix-domain stream socket carries a tiny versioned line protocol:
//
//   request   (client -> server):  one line   "<verb> <arg> <arg> ...\n"
//   response  (server -> client):  "LOTI/1 OK <code>\n"  or
//                                  "LOTI/1 ERR <code> <message>\n"
//                                  then zero or more  "<key>\t<value>\n"  field
//                                  lines, terminated by a blank line.
//
// <code> is the process exit code the CLI returns. Fields are the result payload;
// the CLI renders them as human text or, with --json, as an object (repeated keys
// become an array). The daemon runs the server inside its reactor (non-blocking,
// per-connection buffering); the CLI uses the blocking helper below.
#pragma once

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cstring>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace loti::os {

constexpr char kControlVersion[] = "LOTI/1";

using ControlFields = std::vector<std::pair<std::string, std::string>>;

// A parsed server response.
struct ControlReply {
  bool ok = false;
  int code = 0;
  std::string message;   // set when !ok
  ControlFields fields;  // result payload
};

// Serialize a reply to the wire form (used by the server).
inline std::string format_reply(int code, const std::string& error, const ControlFields& fields) {
  std::string out = kControlVersion;
  if (error.empty()) {
    out += " OK " + std::to_string(code) + "\n";
  } else {
    out += " ERR " + std::to_string(code) + " " + error + "\n";
  }
  for (const auto& [k, v] : fields) out += k + "\t" + v + "\n";
  out += "\n";  // blank line terminates
  return out;
}

inline bool write_all(int fd, const std::string& s) {
  std::size_t off = 0;
  while (off < s.size()) {
    const ssize_t n = ::write(fd, s.data() + off, s.size() - off);
    if (n <= 0) return false;
    off += static_cast<std::size_t>(n);
  }
  return true;
}

// --- server side ---------------------------------------------------------

// Create + bind + listen a Unix stream socket at `path` (removing any stale one).
// Returns the listening fd, or -1.
inline int control_listen(const std::string& path) {
  ::unlink(path.c_str());
  const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) return -1;
  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);
  if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0 || ::listen(fd, 8) < 0) {
    ::close(fd);
    return -1;
  }
  return fd;
}

// --- client side ---------------------------------------------------------

// Connect to a Unix control socket. Returns a connected fd, or -1.
inline int control_connect(const std::string& path) {
  const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) return -1;
  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);
  if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
    ::close(fd);
    return -1;
  }
  return fd;
}

// Send `request` (a single command line, no trailing newline needed) and read the
// full response. Blocking; intended for the CLI.
inline std::optional<ControlReply> control_request(const std::string& path,
                                                   const std::string& request) {
  const int fd = control_connect(path);
  if (fd < 0) return std::nullopt;
  if (!write_all(fd, request + "\n")) {
    ::close(fd);
    return std::nullopt;
  }
  std::string buf;
  char chunk[4096];
  for (;;) {
    const ssize_t n = ::read(fd, chunk, sizeof(chunk));
    if (n < 0) {
      ::close(fd);
      return std::nullopt;
    }
    if (n == 0) break;  // server closed
    buf.append(chunk, static_cast<std::size_t>(n));
    if (buf.find("\n\n") != std::string::npos) break;  // blank-line terminator seen
  }
  ::close(fd);

  ControlReply reply;
  std::size_t pos = 0;
  auto next_line = [&](std::string& line) -> bool {
    if (pos >= buf.size()) return false;
    const std::size_t nl = buf.find('\n', pos);
    line = buf.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
    pos = (nl == std::string::npos) ? buf.size() : nl + 1;
    return true;
  };
  std::string status;
  if (!next_line(status)) return std::nullopt;
  // "LOTI/1 OK <code>" or "LOTI/1 ERR <code> <message>"
  {
    const std::size_t s1 = status.find(' ');
    const std::size_t s2 = status.find(' ', s1 + 1);
    const std::size_t s3 = status.find(' ', s2 + 1);
    if (s1 == std::string::npos || s2 == std::string::npos) return std::nullopt;
    const std::string kind = status.substr(s1 + 1, s2 - s1 - 1);
    reply.ok = (kind == "OK");
    const std::string code = status.substr(s2 + 1, s3 == std::string::npos ? std::string::npos : s3 - s2 - 1);
    reply.code = std::atoi(code.c_str());
    if (!reply.ok && s3 != std::string::npos) reply.message = status.substr(s3 + 1);
  }
  std::string line;
  while (next_line(line)) {
    if (line.empty()) break;  // terminator
    const std::size_t tab = line.find('\t');
    if (tab == std::string::npos)
      reply.fields.emplace_back(line, "");
    else
      reply.fields.emplace_back(line.substr(0, tab), line.substr(tab + 1));
  }
  return reply;
}

}  // namespace loti::os
