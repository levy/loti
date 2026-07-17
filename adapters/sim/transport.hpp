// Transport port, simulation adapter — carries the core's canonical datagram
// bytes over an INET UdpSocket.
//
// The core owns the wire encoding (core/wire) and hands this adapter an opaque,
// already-serialized datagram whose header carries the sender id; the adapter
// wraps it in an INET BytesChunk and sends it to the next hop's address. The
// NodeId -> address table is filled by the NetworkConfigurator, mirroring how the
// original Daemon read `Neighbor::address`. Inbound bytes are lifted back out in
// the host's socketDataArrived and pushed to Node::on_packet_received.
#pragma once

#include <map>

#include <omnetpp.h>

#include "inet/common/packet/Packet.h"
#include "inet/common/packet/chunk/BytesChunk.h"
#include "inet/networklayer/common/L3Address.h"
#include "inet/transportlayer/contract/udp/UdpSocket.h"

#include "ports/transport.hpp"

namespace loti::sim {

class SimTransport final : public ports::Transport {
 public:
  SimTransport(inet::UdpSocket& socket, int port) : socket_(socket), port_(port) {}

  // Learn (or update) the address a next-hop NodeId is reachable at.
  void set_address(domain::NodeId id, const inet::L3Address& address) { addresses_[id] = address; }

  void send(domain::NodeId to, const domain::Bytes& datagram) override {
    auto it = addresses_.find(to);
    if (it == addresses_.end()) return;  // unknown next hop — drop (as the original guarded)
    auto* packet = new inet::Packet("LotiDatagram");
    packet->insertAtBack(inet::makeShared<inet::BytesChunk>(datagram));
    socket_.sendTo(packet, it->second, port_);
  }

 private:
  inet::UdpSocket& socket_;
  int port_;
  std::map<domain::NodeId, inet::L3Address> addresses_;
};

}  // namespace loti::sim
