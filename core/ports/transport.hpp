// Transport port — sends an already-serialized datagram to a neighbor by NodeId.
// The bytes carry their own sender id (in the wire header), matching the original
// protocol. The simulation maps this onto a UdpSocket; production onto a real
// UDP/QUIC socket. Inbound bytes are delivered by the adapter calling
// Node::on_packet_received().
#pragma once

#include "domain/types.hpp"

namespace loti::ports {

class Transport {
 public:
  virtual ~Transport() = default;
  virtual void send(domain::NodeId to, const domain::Bytes& datagram) = 0;
};

}  // namespace loti::ports
