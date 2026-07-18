// Adversarial-input robustness of the protocol engine's inbound entry point.
//
// The daemon calls Node::on_packet_received on raw bytes straight off a UDP socket,
// from any source (wire::decode runs BEFORE the sender is checked against the
// neighbor set). A permissionless, always-on node must therefore treat a malformed
// or dishonest datagram as a dropped packet, never as a fatal error — otherwise a
// single stray packet terminates the process (hardening plan, Phase 1).
//
// T1: a corpus of malformed datagrams must not throw out of on_packet_received.
// T2: a well-formed but *invalid* chain response (as a Byzantine peer would send)
//     must abort the discovery, not crash the node.
#include "doctest.h"

#include "harness/world.hpp"
#include "wire/codec.hpp"
#include "wire/packets.hpp"

using namespace loti;

namespace {

domain::EventHash bytes32(std::uint8_t fill) { return domain::EventHash(32, fill); }

}  // namespace

// --- T1 -------------------------------------------------------------------
TEST_CASE("T1: malformed datagrams are dropped, never fatal") {
  harness::World w;
  Node& n = w.add_node(domain::NodeId(1), NodeConfig{});
  n.add_neighbor(domain::NodeId(2));  // a known sender, so decode is reached

  std::vector<domain::Bytes> corpus = {
      {},                                            // empty
      {0x00},                                        // type only, sender truncated
      {0x2A},                                        // unknown-ish type, truncated
      {0x09, 1, 2, 3, 4, 5, 6, 7, 8},                // valid header, unknown datagram type
      {0x00, 1, 2, 3, 4, 5, 6, 7, 8},                // clock_notification header, payload truncated
      {0x01, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10},         // chain_request, fields truncated mid-way
      {0x02, 1, 2, 3, 4, 5, 6, 7, 8},                // chain_response, chain truncated
  };

  for (const auto& datagram : corpus) {
    CAPTURE(datagram.size());
    CHECK_NOTHROW(n.on_packet_received(datagram));
  }
}

// --- T2 -------------------------------------------------------------------
// A Byzantine neighbor returns a chain response whose interior clock events do not
// hash to their stored hashes. The originator splices its own real clock events onto
// both ends (so the endpoint checks pass) and then validates the whole chain — which
// must fail. The node must abort the discovery, not throw.
TEST_CASE("T2: an invalid chain response aborts the discovery, it does not crash") {
  harness::World w;
  Node& n1 = w.add_node(domain::NodeId(1), NodeConfig{});
  n1.add_neighbor(domain::NodeId(2));  // the (byzantine) sender

  // Give n1 two of its own linked clock events: C2 references C1 (previous tip).
  w.set_now(10);
  n1.create_clock_event();
  w.set_now(20);
  n1.create_clock_event();
  const domain::LocalClockEvent c1 = w.store_of(1).clock_event_by_seq(0);
  const domain::LocalClockEvent c2 = w.store_of(1).clock_event_by_seq(1);
  REQUIRE(c1.creator == n1.id());
  REQUIRE(c2.creator == n1.id());

  // A foreign event n1 is asked to bound (creator != n1, so a discovery goes
  // in-flight rather than completing locally).
  domain::Event evt;
  evt.creator = domain::NodeId(2);
  evt.hash = bytes32(0xAB);

  harness::RecordingChain cb;
  n1.discover_event_chain(evt, domain::TimeRange::all(), cb);  // now in_progress at n1

  // Craft the malicious response. Its lower bound references C1 (so add_local_lower
  // splices in n1's real C1), and its upper bound's hash == C1's hash (so add_local_upper
  // finds C2 via the reverse index). Both crafted clock events carry bogus hashes, so
  // validation of the assembled chain must fail.
  domain::ClockEvent lo;  // referenced by nothing yet; add_local_lower reads its refs
  lo.creator = domain::NodeId(9);
  lo.hash = bytes32(0x11);
  lo.referenced_events.push_back(domain::EventReference{n1.id(), c1.hash});

  domain::ClockEvent hi;
  hi.creator = domain::NodeId(9);
  hi.hash = c1.hash;  // makes clock_events_referencing(hi.hash) return C2

  wire::ChainResponse resp;
  resp.originator = n1.id();
  resp.chain.event = evt;
  resp.chain.lower_bound.push_back(lo);
  resp.chain.upper_bound.push_back(hi);
  // resp.path empty: the originator completes locally.

  const domain::Bytes datagram = wire::encode(domain::NodeId(2), resp);
  CHECK_NOTHROW(n1.on_packet_received(datagram));  // must not throw
  CHECK(cb.aborted);                               // and the discovery is aborted
  CHECK_FALSE(cb.completed);
}
