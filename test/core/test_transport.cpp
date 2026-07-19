// Production UDP transport: source-address anti-spoofing (hardening plan, Phase 3.2).
//
// The gossip layer's sender is a self-declared NodeId in the datagram header, so anyone
// who knows a neighbor's (public) NodeId can spoof clock notifications / requests from it.
// The transport now drops a datagram whose claimed sender is a known peer but whose UDP
// source address does not match that peer's registered address.
#include "doctest.h"

#include "adapters/os/transport.hpp"
#include "wire/packets.hpp"

using namespace loti;

TEST_CASE("wire::sender_of reads the header sender id") {
  wire::ClockNotification note;
  note.chain = 0;
  const auto bytes = wire::encode(domain::NodeId(0xA1B2C3D4), note);
  const auto s = wire::sender_of(bytes);
  REQUIRE(s.has_value());
  CHECK(*s == domain::NodeId(0xA1B2C3D4));
  CHECK_FALSE(wire::sender_of(domain::Bytes{1, 2, 3}).has_value());  // shorter than a header
}

TEST_CASE("UdpTransport drops a datagram whose source != the claimed sender's address") {
  os::UdpTransport receiver(0);  // ephemeral port
  const std::uint16_t rport = receiver.bound_port();
  REQUIRE(rport != 0);
  const domain::NodeId receiver_id = 2;

  os::UdpTransport peerA(0);  // the legitimate peer A (id 1)
  const domain::NodeId A = 1;
  peerA.set_peer(receiver_id, "127.0.0.1", rport);
  receiver.set_peer(A, "127.0.0.1", peerA.bound_port());  // register A's REAL address

  os::UdpTransport attacker(0);  // a third host, spoofing sender == A
  attacker.set_peer(receiver_id, "127.0.0.1", rport);

  wire::ClockNotification note;
  note.chain = 0;
  const auto legit = wire::encode(A, note);  // from peerA, claims A
  const auto spoof = wire::encode(A, note);  // from attacker, also claims A

  attacker.send(receiver_id, spoof);
  peerA.send(receiver_id, legit);

  // Loopback delivery is synchronous, so both datagrams are already queued. Count how many
  // the receiver actually delivers: with the source check, only the legit one survives.
  int delivered = 0;
  for (int i = 0; i < 200; ++i)
    if (!receiver.receive().empty()) ++delivered;

  CHECK(delivered == 1);  // the spoofed datagram was dropped; the legit one delivered
}
