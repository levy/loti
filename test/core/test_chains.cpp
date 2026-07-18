// Multi-resolution clock chains: a node runs several independent clock chains at
// different resolutions. These tests drive more than one chain and check that events
// pin into every chain, that per-chain tips are tracked, and that discovery still
// builds and validates an enclosing chain across nodes (the extend_* walks now follow
// same-chain hash-refs rather than the global sequence).
#include "doctest.h"

#include "harness/world.hpp"

using namespace loti;

namespace {

// A two-node path whose nodes each run `chains` independent chains (timers off — the
// test drives ticks by hand via create_clock_event(chain)).
std::vector<Node*> multi_chain_pair(harness::World& w, std::size_t chains) {
  NodeConfig cfg;
  for (std::size_t i = 0; i < chains; ++i)
    cfg.chains.push_back(ChainConfig{/*interval=*/domain::Duration(1), /*keep=*/0});
  Node& a = w.add_node(1, cfg);
  Node& b = w.add_node(2, cfg);
  a.add_neighbor(2);
  b.add_neighbor(1);
  a.learn_route(2, 2);
  b.learn_route(1, 1);
  return {&a, &b};
}

// One gossip round on a specific chain: every node ticks that chain and its
// notifications are delivered before the next node ticks (so cross-links form).
void gossip_chain(harness::World& w, const std::vector<Node*>& nodes, std::uint32_t chain,
                  int rounds, domain::Duration dt = 10) {
  for (int r = 0; r < rounds; ++r)
    for (Node* n : nodes) {
      w.set_now(w.now() + dt);
      n->create_clock_event(chain);
      w.pump();
    }
}

}  // namespace

TEST_CASE("a node reports the configured number of chains") {
  harness::World w;
  auto nodes = multi_chain_pair(w, 3);
  CHECK(nodes[0]->chain_count() == 3);
  // A default (empty-schedule) node is single-chain.
  Node& single = w.add_node(9, NodeConfig{});
  CHECK(single.chain_count() == 1);
}

TEST_CASE("an event pins a lower anchor into every chain that has ticked") {
  harness::World w;
  auto nodes = multi_chain_pair(w, 2);

  // Only chain 0 has ticked so far → one anchor.
  gossip_chain(w, nodes, 0, 2);
  const auto e1 = nodes[0]->publish_event({0x01});
  CHECK(e1.referenced_events.size() == 1);

  // Now chain 1 has a tip too → the next event anchors into both chains.
  gossip_chain(w, nodes, 1, 1);
  const auto e2 = nodes[0]->publish_event({0x02});
  CHECK(e2.referenced_events.size() == 2);
}

TEST_CASE("per-chain tips are tracked independently and carry their chain id") {
  harness::World w;
  auto nodes = multi_chain_pair(w, 2);
  gossip_chain(w, nodes, 0, 3);
  gossip_chain(w, nodes, 1, 2);
  auto& store = w.store_of(1);

  REQUIRE(store.latest_clock_event(0).has_value());
  REQUIRE(store.latest_clock_event(1).has_value());
  CHECK(store.latest_clock_event(0)->chain == 0);
  CHECK(store.latest_clock_event(1)->chain == 1);
  // The two chains' tips are distinct clock events.
  CHECK(store.latest_clock_event(0)->hash != store.latest_clock_event(1)->hash);
  // A chain that never ticked has no tip.
  CHECK_FALSE(store.latest_clock_event(2).has_value());
}

TEST_CASE("cross-node bounds discovery completes and validates with multiple chains") {
  harness::World w;
  auto nodes = multi_chain_pair(w, 2);
  Node& n1 = *nodes[0];
  Node& n2 = *nodes[1];

  // Interleave both chains so cross-links form at each resolution.
  gossip_chain(w, nodes, 0, 4);
  gossip_chain(w, nodes, 1, 2);
  gossip_chain(w, nodes, 0, 2);

  const auto event = n1.publish_event({0xBE, 0xEF});

  gossip_chain(w, nodes, 0, 4);
  gossip_chain(w, nodes, 1, 2);
  gossip_chain(w, nodes, 0, 2);

  harness::RecordingChain chain_cb;
  n2.discover_event_chain(event, chain_cb);  // reaching completion means validation passed
  w.pump();
  REQUIRE(chain_cb.completed);
  CHECK_FALSE(chain_cb.aborted);
  CHECK(chain_cb.chain.event.hash == event.hash);
  CHECK(chain_cb.chain.lower_bound.front().creator == n2.id());
  CHECK(chain_cb.chain.upper_bound.back().creator == n2.id());

  // The enclosing bound is the finest available resolution (chain 0), since it is never
  // pruned here — every clock event in the built chain is on chain 0.
  for (const auto& ce : chain_cb.chain.lower_bound) CHECK(ce.chain == 0);
  for (const auto& ce : chain_cb.chain.upper_bound) CHECK(ce.chain == 0);

  harness::RecordingBounds bounds_cb;
  n2.discover_event_bounds(event, bounds_cb);
  w.pump();
  REQUIRE(bounds_cb.completed);
  CHECK(bounds_cb.lower <= bounds_cb.upper);
}
