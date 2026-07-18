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

TEST_CASE("flood discovery completes with no routing table (ring, breadcrumb retrace)") {
  harness::World w;
  NodeConfig cfg;
  cfg.discovery_routing = DiscoveryRouting::flood;  // no learn_route → the routing table is empty
  cfg.discovery_hop_limit = 16;

  // A 4-node ring n1—n2—n3—n4—n1. n3 is two hops from n1 either way; with an empty routing
  // table the discovery can only succeed by flooding the neighbor history and retracing home.
  std::vector<Node*> ring;
  for (int i = 0; i < 4; ++i) ring.push_back(&w.add_node(domain::NodeId(i + 1), cfg));
  for (int i = 0; i < 4; ++i) {
    Node& a = *ring[i];
    Node& b = *ring[(i + 1) % 4];
    a.add_neighbor(b.id());
    b.add_neighbor(a.id());
  }

  harness::gossip(w, ring, 10);
  const auto event = ring[0]->publish_event({0xAB, 0xCD});  // event lives on n1
  harness::gossip(w, ring, 10);

  harness::RecordingChain cb;
  ring[2]->discover_event_chain(event, domain::TimeRange::all(), cb);  // n3 is the reference node
  w.pump();

  REQUIRE(cb.completed);
  CHECK_FALSE(cb.aborted);
  CHECK(cb.chain.event.hash == event.hash);
  CHECK(cb.chain.lower_bound.front().creator == ring[2]->id());
  CHECK(cb.chain.upper_bound.back().creator == ring[2]->id());
}

TEST_CASE("flood with a forward cap still completes (cross-branch dedup)") {
  harness::World w;
  NodeConfig cfg;
  cfg.discovery_routing = DiscoveryRouting::flood;
  cfg.discovery_hop_limit = 16;
  cfg.discovery_forward_cap = 1;  // each node re-floods a given discovery at most once

  // Diamond + tail: 1—2—4, 1—3—4, 4—5. n4 receives the request from BOTH n2 and n3, so the
  // fan-in cap actually triggers there; the discovery must still complete via one of the paths.
  std::vector<Node*> nodes;
  for (int i = 0; i < 5; ++i) nodes.push_back(&w.add_node(domain::NodeId(i + 1), cfg));
  const auto link = [&](int a, int b) {
    nodes[a]->add_neighbor(nodes[b]->id());
    nodes[b]->add_neighbor(nodes[a]->id());
  };
  link(0, 1);
  link(0, 2);
  link(1, 3);
  link(2, 3);
  link(3, 4);

  harness::gossip(w, nodes, 12);
  const auto event = nodes[4]->publish_event({0x5E, 0xED});  // event on n5
  harness::gossip(w, nodes, 12);

  harness::RecordingChain cb;
  nodes[0]->discover_event_chain(event, domain::TimeRange::all(), cb);  // n1 finds n5's event
  w.pump();

  REQUIRE(cb.completed);
  CHECK_FALSE(cb.aborted);
  CHECK(cb.chain.event.hash == event.hash);
  CHECK(cb.chain.lower_bound.front().creator == nodes[0]->id());
}
