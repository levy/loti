// Unit tests for the portable proof (core/proof): binary round-trip, offline
// verification of a real discovered chain, rejection of every tampering, order
// proofs, and the signature-checking wiring (both a stub signer and real Ed25519).
#include "doctest.h"

#include "adapters/os/keystore.hpp"
#include "harness/world.hpp"
#include "proof/proof.hpp"

using namespace loti;

namespace {

// Run a discovery and return the reference node's enclosing chain.
domain::EventChain discover_chain(harness::World& w, const domain::Event& event, Node& reference) {
  harness::RecordingChain cb;
  reference.discover_event_chain(event, cb);
  w.pump();
  REQUIRE(cb.completed);
  return cb.chain;
}

// A signer that rejects everything — used to prove verify() honors signature checks.
struct RejectingSigner final : ports::Signer {
  domain::Signature sign(const domain::Bytes&) override { return {}; }
  bool verify(const domain::Bytes&, const domain::Signature&, domain::NodeId) const override {
    return false;
  }
};

// Build a minimal self-chain (C1 · event · C2) on one node, signed by its signer.
domain::EventChain self_chain(Node& node, domain::Timestamp& now) {
  now = 10;
  node.create_clock_event();                 // C1
  const auto event = node.publish_event({0xAB, 0xCD});  // references C1
  now = 20;
  node.create_clock_event();                 // C2 references the event
  harness::RecordingChain cb;
  node.discover_event_chain(event, cb);      // event.creator == node.id() → no network
  REQUIRE(cb.completed);
  return cb.chain;
}

}  // namespace

TEST_CASE("proof serialize/deserialize round-trips a bounds proof") {
  harness::World w;
  auto nodes = harness::build_path(w, 2);
  harness::gossip(w, nodes, 5);
  const auto event = nodes[0]->publish_event({0xBE, 0xEF});
  harness::gossip(w, nodes, 6);

  proof::Proof p;
  p.kind = proof::Kind::bounds;
  p.reference.node = nodes[1]->id();
  p.chain = discover_chain(w, event, *nodes[1]);

  const auto bytes = proof::serialize(p);
  const auto back = proof::deserialize(bytes);
  CHECK(back.kind == proof::Kind::bounds);
  CHECK(back.reference.node == p.reference.node);
  CHECK(back.chain == p.chain);
  CHECK(proof::serialize(back) == bytes);  // stable
}

TEST_CASE("deserialize rejects a foreign / corrupt buffer") {
  CHECK_THROWS_AS((void)proof::deserialize(domain::Bytes{0, 1, 2, 3}), std::runtime_error);
}

TEST_CASE("a valid bounds proof verifies offline with the correct interval") {
  harness::World w;
  auto nodes = harness::build_path(w, 3);
  harness::gossip(w, nodes, 8);
  const auto event = nodes[0]->publish_event({0x01});
  harness::gossip(w, nodes, 8);

  proof::Proof p;
  p.kind = proof::Kind::bounds;
  p.reference.node = nodes[2]->id();
  p.chain = discover_chain(w, event, *nodes[2]);

  harness::NullSigner signer;
  const auto res = proof::verify(p, signer);
  CHECK(res.valid);
  CHECK(res.reason.empty());
  CHECK(res.lower == p.chain.lower_bound.front().timestamp);
  CHECK(res.upper == p.chain.upper_bound.back().timestamp);
  CHECK(res.lower <= res.upper);
}

TEST_CASE("tampering a bounds proof makes verification fail") {
  harness::World w;
  auto nodes = harness::build_path(w, 2);
  harness::gossip(w, nodes, 5);
  const auto event = nodes[0]->publish_event({0x2C, 0x7F});
  harness::gossip(w, nodes, 6);

  proof::Proof base;
  base.kind = proof::Kind::bounds;
  base.reference.node = nodes[1]->id();
  base.chain = discover_chain(w, event, *nodes[1]);
  harness::NullSigner signer;
  REQUIRE(proof::verify(base, signer).valid);

  SUBCASE("mutated clock-event hash") {
    auto p = base;
    p.chain.lower_bound.front().hash[0] ^= 0xFF;
    CHECK_FALSE(proof::verify(p, signer).valid);
  }
  SUBCASE("mutated timestamp (breaks the recomputed hash)") {
    auto p = base;
    p.chain.upper_bound.back().timestamp += 1;
    CHECK_FALSE(proof::verify(p, signer).valid);
  }
  SUBCASE("broken linkage") {
    auto p = base;
    if (!p.chain.event.referenced_events.empty())
      p.chain.event.referenced_events.front().hash[0] ^= 0xFF;
    CHECK_FALSE(proof::verify(p, signer).valid);
  }
  SUBCASE("endpoint not the reference node's") {
    auto p = base;
    p.reference.node ^= 0x1;  // now the endpoints' creator != reference.node
    CHECK_FALSE(proof::verify(p, signer).valid);
  }
  SUBCASE("mutated event content (breaks the event hash)") {
    auto p = base;
    p.chain.event.salt ^= 0x1;
    CHECK_FALSE(proof::verify(p, signer).valid);
  }
}

TEST_CASE("verify honors signature failures") {
  harness::World w;
  auto nodes = harness::build_path(w, 2);
  harness::gossip(w, nodes, 5);
  const auto event = nodes[0]->publish_event({0x99});
  harness::gossip(w, nodes, 6);

  proof::Proof p;
  p.kind = proof::Kind::bounds;
  p.reference.node = nodes[1]->id();
  p.chain = discover_chain(w, event, *nodes[1]);

  RejectingSigner rejecting;
  CHECK_FALSE(proof::verify(p, rejecting).valid);  // structurally sound, signatures rejected
}

TEST_CASE("an order proof verifies and detects a forged order") {
  harness::World w;
  auto nodes = harness::build_path(w, 3);
  harness::gossip(w, nodes, 8);
  const auto early = nodes[0]->publish_event({0xEE});
  harness::gossip(w, nodes, 14);  // wide separation → disjoint intervals
  const auto late = nodes[0]->publish_event({0x1A, 0x7E});
  harness::gossip(w, nodes, 8);

  proof::Proof p;
  p.kind = proof::Kind::order;
  p.reference.node = nodes[2]->id();
  p.chain = discover_chain(w, early, *nodes[2]);
  p.chain2 = discover_chain(w, late, *nodes[2]);
  p.order = domain::Order::before;

  harness::NullSigner signer;
  const auto res = proof::verify(p, signer);
  CHECK(res.valid);
  CHECK(res.order == domain::Order::before);

  auto forged = p;
  forged.order = domain::Order::after;  // does not match the intervals
  CHECK_FALSE(proof::verify(forged, signer).valid);
}

TEST_CASE("a real Ed25519-signed proof verifies, and a signature tamper is caught") {
  domain::Timestamp now = 0;
  harness::FakeClock clock{now};
  harness::FakeScheduler sched{now};
  harness::FakeTransport transport;
  harness::SeededRng rng{1};
  os::Ed25519KeyStore signer;  // ephemeral key; id == fingerprint(pubkey)
  ports::NoopTelemetry telemetry;
  sim::InMemoryStore store;

  const domain::NodeId id = signer.node_id();
  Node node(id, NodePorts{clock, sched, transport, rng, signer, telemetry, store}, NodeConfig{});

  proof::Proof p;
  p.kind = proof::Kind::bounds;
  p.reference.node = id;
  p.reference.pubkey = signer.public_key();
  p.chain = self_chain(node, now);

  os::Ed25519KeyStore verifier;  // a DIFFERENT key; verify uses each signature's embedded pubkey
  CHECK(proof::verify(p, verifier).valid);

  SUBCASE("flipped signature byte") {
    auto bad = p;
    REQUIRE_FALSE(bad.chain.lower_bound.front().signature.empty());
    bad.chain.lower_bound.front().signature[0] ^= 0xFF;
    CHECK_FALSE(proof::verify(bad, verifier).valid);
  }
  SUBCASE("reference id not matching the embedded pubkey") {
    auto bad = p;
    bad.reference.node ^= 0x1;
    CHECK_FALSE(proof::verify(bad, verifier).valid);
  }
}
