// Transport port, production adapter — sends the core's canonical datagram bytes
// over a real non-blocking UDP socket.
//
// Symmetric with the simulation's SimTransport: the core hands over opaque,
// already-serialized bytes (whose header carries the sender id) and a destination
// NodeId; this adapter looks up the peer's address and sendto()s. Inbound
// datagrams are pulled by the reactor (which watches fd()) and handed to
// Node::on_packet_received. The NodeId -> address map is filled from config (static
// peering in Stage 3; dynamic `peer add` in Stage 5).
#pragma once

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <map>
#include <stdexcept>
#include <string>

#include "ports/transport.hpp"
#include "wire/packets.hpp"

namespace loti::os {

class UdpTransport final : public ports::Transport {
 public:
  explicit UdpTransport(std::uint16_t port) {
    fd_ = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd_ < 0) throw std::runtime_error("socket() failed");
    int flags = ::fcntl(fd_, F_GETFL, 0);
    ::fcntl(fd_, F_SETFL, flags | O_NONBLOCK);
    // Enlarge the kernel socket buffers (best-effort) so a burst of discovery traffic is less
    // likely to be dropped before the single reactor thread drains it. (Inbound-drop counting
    // via SO_RXQ_OVFL is a deferred telemetry follow-up — see plan 3.5.)
    const int bufsize = 1 << 20;  // 1 MiB
    ::setsockopt(fd_, SOL_SOCKET, SO_RCVBUF, &bufsize, sizeof(bufsize));
    ::setsockopt(fd_, SOL_SOCKET, SO_SNDBUF, &bufsize, sizeof(bufsize));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);
    if (::bind(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
      ::close(fd_);
      throw std::runtime_error("bind() failed on UDP port " + std::to_string(port));
    }
  }

  ~UdpTransport() override {
    if (fd_ >= 0) ::close(fd_);
  }

  UdpTransport(const UdpTransport&) = delete;
  UdpTransport& operator=(const UdpTransport&) = delete;

  [[nodiscard]] int fd() const { return fd_; }

  // The actual bound UDP port (resolves an ephemeral port when constructed with port 0).
  [[nodiscard]] std::uint16_t bound_port() const {
    sockaddr_in addr{};
    socklen_t len = sizeof(addr);
    if (::getsockname(fd_, reinterpret_cast<sockaddr*>(&addr), &len) != 0) return 0;
    return ntohs(addr.sin_port);
  }

  void set_peer(domain::NodeId id, const std::string& ip, std::uint16_t port) {
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (::inet_pton(AF_INET, ip.c_str(), &addr.sin_addr) != 1)
      throw std::runtime_error("bad peer address: " + ip);
    peers_[id] = addr;
  }

  void send(domain::NodeId to, const domain::Bytes& datagram) override {
    auto it = peers_.find(to);
    if (it == peers_.end()) return;  // unknown next hop — drop
    ::sendto(fd_, datagram.data(), datagram.size(), 0,
             reinterpret_cast<sockaddr*>(&it->second), sizeof(it->second));
  }

  // Read one deliverable datagram; returns empty when the socket would block. A datagram
  // whose claimed sender is a known peer but whose UDP source address does not match that
  // peer's registered address is a spoof — it is skipped and the next datagram is read, so
  // the caller's drain loop never mistakes a dropped spoof for an empty socket. (An unknown
  // sender is passed through; the core ignores packets from non-neighbors anyway.)
  domain::Bytes receive() {
    for (;;) {
      domain::Bytes buf(kMaxDatagram);
      sockaddr_in src{};
      socklen_t srclen = sizeof(src);
      const ssize_t n =
          ::recvfrom(fd_, buf.data(), buf.size(), 0, reinterpret_cast<sockaddr*>(&src), &srclen);
      if (n <= 0) return {};  // EWOULDBLOCK / EAGAIN / empty
      buf.resize(static_cast<std::size_t>(n));
      if (const auto sender = wire::sender_of(buf)) {
        const auto it = peers_.find(*sender);
        if (it != peers_.end() && !same_source(it->second, src)) continue;  // spoofed source
      }
      return buf;
    }
  }

 private:
  static constexpr std::size_t kMaxDatagram = 65535;

  // Same host? Compare the address and port a peer was registered with against the datagram's
  // UDP source. NB: assumes the peer sends from its registered (listening) port and is not
  // behind a port-rewriting NAT — see doc note; a per-datagram MAC is the stronger follow-up.
  static bool same_source(const sockaddr_in& a, const sockaddr_in& b) {
    return a.sin_addr.s_addr == b.sin_addr.s_addr && a.sin_port == b.sin_port;
  }
  int fd_ = -1;
  std::map<domain::NodeId, sockaddr_in> peers_;
};

}  // namespace loti::os
