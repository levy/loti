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

namespace loti::os {

class UdpTransport final : public ports::Transport {
 public:
  explicit UdpTransport(std::uint16_t port) {
    fd_ = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd_ < 0) throw std::runtime_error("socket() failed");
    int flags = ::fcntl(fd_, F_GETFL, 0);
    ::fcntl(fd_, F_SETFL, flags | O_NONBLOCK);
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

  // Read one pending datagram; returns empty when the socket would block.
  domain::Bytes receive() {
    domain::Bytes buf(kMaxDatagram);
    const ssize_t n = ::recvfrom(fd_, buf.data(), buf.size(), 0, nullptr, nullptr);
    if (n <= 0) return {};  // EWOULDBLOCK / EAGAIN / empty
    buf.resize(static_cast<std::size_t>(n));
    return buf;
  }

 private:
  static constexpr std::size_t kMaxDatagram = 65535;
  int fd_ = -1;
  std::map<domain::NodeId, sockaddr_in> peers_;
};

}  // namespace loti::os
