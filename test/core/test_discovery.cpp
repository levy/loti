// End-to-end protocol tests on the in-process harness: real Node instances gossip
// a clock-event DAG and answer chain / bounds / order discoveries across multiple
// hops — the Stage-2 verification that the protocol runs on loti-core, with no
// OMNeT++ and no sockets.
#include "doctest.h"

#include "harness/world.hpp"

using namespace loti;

namespace {
// Publish an event on `node` and return a copy (publish_event returns a reference
// into the node's store).
domain::Event publish(Node& node, domain::Bytes data) { return node.publish_event(std::move(data)); }
}  // namespace

TEST_CASE("two-node event chain discovery completes and validates") {
  harness::World w;
  auto nodes = harness::build_path(w, 2);
  Node& n1 = *nodes[0];
  Node& n2 = *nodes[1];

  harness::gossip(w, nodes, 5);                 // form cross-links
  const auto event = publish(n1, {0xBE, 0xEF});
  harness::gossip(w, nodes, 6);                 // pin the event and form the reverse links

  harness::RecordingChain cb;
  n2.discover_event_chain(event, domain::TimeRange::all(), cb);           // n2 is the reference node
  w.pump();

  REQUIRE(cb.completed);
  CHECK_FALSE(cb.aborted);
  // The chain encloses the event and begins/ends at the reference node's clock events.
  CHECK(cb.chain.event.hash == event.hash);
  CHECK(cb.chain.lower_bound.front().creator == n2.id());
  CHECK(cb.chain.upper_bound.back().creator == n2.id());
  CHECK(cb.chain.lower_bound.size() >= 1);
  CHECK(cb.chain.upper_bound.size() >= 1);
  // (reaching on_chain_completed means validate_chain_discovery_result did not throw)
}

TEST_CASE("three-node path bounds discovery yields a sane interval") {
  harness::World w;
  auto nodes = harness::build_path(w, 3);
  Node& n1 = *nodes[0];
  Node& n3 = *nodes[2];

  harness::gossip(w, nodes, 8);
  const auto event = publish(n1, {0x01, 0x02, 0x03});
  harness::gossip(w, nodes, 8);

  harness::RecordingBounds cb;
  n3.discover_event_bounds(event, domain::TimeRange::all(), cb);          // bounds according to n3's clock, 3 hops away
  w.pump();

  REQUIRE(cb.completed);
  CHECK_FALSE(cb.aborted);
  CHECK(cb.lower <= cb.upper);
}

TEST_CASE("event order discovery: an earlier event is ordered before a later one") {
  harness::World w;
  auto nodes = harness::build_path(w, 3);
  Node& n1 = *nodes[0];
  Node& n3 = *nodes[2];

  harness::gossip(w, nodes, 8);
  const auto early = publish(n1, {0xEE});
  harness::gossip(w, nodes, 14);                // wide separation so the intervals do not overlap
  const auto late = publish(n1, {0x1A, 0x7E});
  harness::gossip(w, nodes, 8);

  harness::RecordingOrder cb;
  n3.discover_event_order(early, late, domain::TimeRange::all(), cb);
  w.pump();

  REQUIRE(cb.completed);
  CHECK_FALSE(cb.aborted);
  CHECK(cb.order == domain::Order::before);
}
