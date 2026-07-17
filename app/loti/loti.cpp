// loti — the command-line client for a running lotid daemon.
//
// It speaks the control-socket line protocol in adapters/os/control.hpp: it maps a
// subcommand to a daemon verb, sends one request, and renders the reply as human
// text or, with --json, as an object (repeated field keys become an array). The
// process exit code is the daemon's reply code (see cli.md). `init` runs locally
// (no daemon): it creates a home directory, generates a key, and writes a config.
//
// Usage:
//   loti [--control <path>] [--json] [--quiet] <command> [args...]
//
//   status | node status              node id, counts, mode
//   publish <text...>                 publish an event
//   event show <hash|last>            show a known event
//   events                            list event hashes
//   bounds <event> | chain <event>    discover time bounds / an event chain
//   order <a> <b>                     discover the order of two events
//   peer add <id:ip:port> | peer ls   manage neighbors
//   key [show]                        node id + public key
//   node stop | stop                  stop the daemon
//   init [--home <dir>]               create home dir, key, and config

#include <sys/stat.h>

#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>

#include "adapters/os/control.hpp"
#include "adapters/os/keystore.hpp"

namespace {

using namespace loti;

std::string hex(const domain::EventHash& h) {
  static const char* d = "0123456789abcdef";
  std::string s;
  for (unsigned char b : h) {
    s.push_back(d[b >> 4]);
    s.push_back(d[b & 0xF]);
  }
  return s;
}
std::string hex_id(domain::NodeId id) {
  char buf[19];
  std::snprintf(buf, sizeof(buf), "0x%016llx", static_cast<unsigned long long>(id));
  return buf;
}

std::string json_escape(const std::string& s) {
  std::string o;
  for (char c : s) {
    switch (c) {
      case '"': o += "\\\""; break;
      case '\\': o += "\\\\"; break;
      case '\n': o += "\\n"; break;
      case '\t': o += "\\t"; break;
      default: o.push_back(c);
    }
  }
  return o;
}

void print_json(const os::ControlReply& r) {
  // group values by key (in first-seen order) so repeated keys form arrays
  std::vector<std::string> order;
  std::map<std::string, std::vector<std::string>> grouped;
  for (const auto& [k, v] : r.fields) {
    if (!grouped.count(k)) order.push_back(k);
    grouped[k].push_back(v);
  }
  std::string out = "{";
  out += "\"ok\":" + std::string(r.ok ? "true" : "false") + ",\"code\":" + std::to_string(r.code);
  if (!r.ok) out += ",\"error\":\"" + json_escape(r.message) + "\"";
  for (const auto& k : order) {
    out += ",\"" + json_escape(k) + "\":";
    const auto& vs = grouped[k];
    if (vs.size() == 1) {
      out += "\"" + json_escape(vs[0]) + "\"";
    } else {
      out += "[";
      for (std::size_t i = 0; i < vs.size(); ++i) out += (i ? "," : "") + std::string("\"") + json_escape(vs[i]) + "\"";
      out += "]";
    }
  }
  out += "}\n";
  std::fputs(out.c_str(), stdout);
}

// Map a CLI subcommand (argv after the globals) to a daemon request line.
// Returns "" if the command is unknown/malformed (usage printed by the caller).
std::string to_request(const std::vector<std::string>& a) {
  if (a.empty()) return "";
  auto join_from = [&](std::size_t i, const std::string& verb) {
    std::string r = verb;
    for (; i < a.size(); ++i) r += " " + a[i];
    return r;
  };
  const std::string& c = a[0];
  if (c == "status") return "status";
  if (c == "events") return "events";
  if (c == "publish" && a.size() >= 2) return join_from(1, "publish");
  if (c == "bounds" && a.size() >= 2) return "bounds " + a[1];
  if (c == "chain" && a.size() >= 2) return "chain " + a[1];
  if (c == "order" && a.size() >= 3) return "order " + a[1] + " " + a[2];
  if (c == "key") return "key";
  if (c == "stop") return "quit";
  if (c == "event" && a.size() >= 3 && a[1] == "show") return "event " + a[2];
  if (c == "peer" && a.size() >= 3 && a[1] == "add") return "peer-add " + a[2];
  if (c == "peer" && a.size() >= 2 && a[1] == "ls") return "peer-ls";
  if (c == "node" && a.size() >= 2 && a[1] == "status") return "status";
  if (c == "node" && a.size() >= 2 && a[1] == "stop") return "quit";
  return "";
}

int do_init(const std::string& home) {
  ::mkdir(home.c_str(), 0700);
  const std::string keypath = home + "/key";
  os::Ed25519KeyStore ks;
  ks.load_or_generate(keypath);
  const std::string store = home + "/state.snap";
  const std::string control = home + "/control.sock";
  if (FILE* f = std::fopen((home + "/config").c_str(), "w")) {
    std::fprintf(f, "# lotid config written by `loti init`\nport=7000\nkey=%s\nstore=%s\ncontrol=%s\n",
                 keypath.c_str(), store.c_str(), control.c_str());
    std::fclose(f);
  }
  std::printf("initialized %s\n", home.c_str());
  std::printf("node id: %s\n", hex_id(ks.node_id()).c_str());
  std::printf("public key: %s\n", hex(ks.public_key()).c_str());
  std::printf("start: lotid --key %s --port 7000 --store %s --control %s\n", keypath.c_str(),
              store.c_str(), control.c_str());
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  std::string control = std::getenv("LOTI_CONTROL") ? std::getenv("LOTI_CONTROL") : "loti.sock";
  std::string home = std::getenv("HOME") ? std::string(std::getenv("HOME")) + "/.loti" : ".loti";
  bool json = false, quiet = false;
  std::vector<std::string> args;

  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "--json") json = true;
    else if (a == "--quiet" || a == "-q") quiet = true;
    else if (a == "--control" && i + 1 < argc) control = argv[++i];
    else if (a == "--home" && i + 1 < argc) home = argv[++i];
    else args.push_back(a);
  }

  if (args.empty()) {
    std::fprintf(stderr, "usage: loti [--control <path>] [--json] [--quiet] <command> [args...]\n"
                         "commands: status | publish <text> | event show <h> | events | "
                         "bounds <e> | chain <e> | order <a> <b> | peer add|ls | key | stop | init\n");
    return 2;
  }
  if (args[0] == "init") return do_init(home);

  const std::string request = to_request(args);
  if (request.empty()) {
    std::fprintf(stderr, "loti: unknown or malformed command\n");
    return 2;
  }

  const auto reply = os::control_request(control, request);
  if (!reply) {
    std::fprintf(stderr, "loti: cannot reach daemon at %s\n", control.c_str());
    return 1;
  }
  if (json) {
    print_json(*reply);
  } else if (!quiet) {
    if (!reply->ok)
      std::fprintf(stderr, "error(%d): %s\n", reply->code, reply->message.c_str());
    for (const auto& [k, v] : reply->fields) std::printf("%s: %s\n", k.c_str(), v.c_str());
  }
  return reply->code;
}
