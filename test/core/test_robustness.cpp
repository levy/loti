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

#include <sys/stat.h>
#include <unistd.h>

#include <stdexcept>
#include <string>

#include "adapters/os/keystore.hpp"
#include "adapters/os/store.hpp"
#include "hash/hashing.hpp"
#include "harness/world.hpp"
#include "proof/proof.hpp"
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

// A length prefix must be sanity-checked against the datagram before any allocation:
// otherwise a ~20-byte packet claiming billions of elements triggers a multi-GB reserve
// (a down payment on Phase 3.1).
TEST_CASE("T1b: an implausible length prefix is rejected without a huge allocation") {
  domain::Bytes d(53, 0);                          // a chain_request header up to the path count
  d[0] = 0x01;                                     // wire::Type::chain_request
  for (int i = 0; i < 4; ++i) d.push_back(0xFF);   // path node-id count = 0xFFFFFFFF
  CHECK_THROWS_AS(wire::decode(d), std::runtime_error);

  harness::World w;
  Node& n = w.add_node(domain::NodeId(1), NodeConfig{});
  n.add_neighbor(domain::NodeId(2));
  CHECK_NOTHROW(n.on_packet_received(d));          // and it is simply dropped at the entry point
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

// --- Phase 2: proof soundness & identity ----------------------------------

// 2.1 — the flagship offline verifier (Ed25519KeyStore, as used by `loti verify`)
// currently accepts a chain whose clock events carry NO signature, because
// Ed25519KeyStore::verify returns true for an empty signature. An attacker can
// fabricate such an all-unsigned chain (hashes are over public content), attribute
// it to any reference node, and have `loti verify` call it valid. A real verifier
// must REQUIRE signatures.
TEST_CASE("2.1: the offline verifier rejects an unsigned (forged) proof") {
  harness::World w;                                   // World signs with NullSigner → unsigned
  auto nodes = harness::build_path(w, 3);
  harness::gossip(w, nodes, 8);
  const auto event = nodes[0]->publish_event({0x01});
  harness::gossip(w, nodes, 8);
  harness::RecordingChain cb;
  nodes[2]->discover_event_chain(event, domain::TimeRange::all(), cb);
  w.pump();
  REQUIRE(cb.completed);

  proof::Proof p;
  p.kind = proof::Kind::bounds;
  p.reference.node = nodes[2]->id();
  // p.reference.pubkey deliberately empty — as a forger would leave it.
  p.chain = cb.chain;                                 // every clock event is unsigned

  os::Ed25519KeyStore verifier;                       // the real offline verifier the CLI uses
  CHECK_FALSE(proof::verify(p, verifier).valid);      // an unsigned chain must NOT verify
}

// A soundness bug found in verify_chain: the FIRST upper-bound clock event's linkage
// to the event is skipped, so an upper bound disconnected from the event still
// validates. Dropping the one upper clock event that references the event leaves a
// chain whose upper bound no longer encloses it — which must be rejected.
TEST_CASE("2.x: a chain whose upper bound is not linked to the event is rejected") {
  harness::World w;
  auto nodes = harness::build_path(w, 3);
  harness::gossip(w, nodes, 8);
  const auto event = nodes[0]->publish_event({0x02});
  harness::gossip(w, nodes, 8);
  harness::RecordingChain cb;
  nodes[2]->discover_event_chain(event, domain::TimeRange::all(), cb);
  w.pump();
  REQUIRE(cb.completed);
  REQUIRE(cb.chain.upper_bound.size() >= 2);          // need a front element to drop

  proof::Proof p;
  p.kind = proof::Kind::bounds;
  p.reference.node = nodes[2]->id();
  p.chain = cb.chain;
  p.chain.upper_bound.pop_front();                    // drop the event's only upper reference

  harness::NullSigner signer;                         // isolate the linkage check from signatures
  CHECK_FALSE(proof::verify(p, signer).valid);
}

// 2.2 — a generated private-key file must not be world/group readable.
TEST_CASE("2.2: a generated key file is private (0600)") {
  const std::string path = "loti_test_key_0600.tmp";
  ::unlink(path.c_str());
  const mode_t old = ::umask(0022);                   // force a permissive umask for the test
  {
    os::Ed25519KeyStore ks;
    ks.load_or_generate(path);
  }
  ::umask(old);
  struct stat st{};
  REQUIRE(::stat(path.c_str(), &st) == 0);
  CHECK((st.st_mode & 0777) == 0600);
  ::unlink(path.c_str());
}

// --- Phase 3/4: anti-DoS + clock integrity --------------------------------

// 3.3 — a spoofed or merely re-sent clock-event notification must not grow a local
// clock event's reverse-edge list without bound. Reverse edges are learned from
// unauthenticated notifications; duplicates carry no new information and must be deduped.
TEST_CASE("3.3: a duplicate clock-event notification does not grow referencing_events") {
  harness::World w;
  Node& n1 = w.add_node(domain::NodeId(1), NodeConfig{});
  n1.add_neighbor(domain::NodeId(2));
  w.set_now(10);
  n1.create_clock_event();  // C1 of n1
  const domain::LocalClockEvent c1 = w.store_of(1).clock_event_by_seq(0);

  wire::ClockNotification note;
  note.chain = 0;
  note.last_clock_event_hash = bytes32(0x77);     // neighbor 2's clock event X
  note.neighbor_last_clock_event_hash = c1.hash;  // "X references your C1"
  const domain::Bytes datagram = wire::encode(domain::NodeId(2), note);

  n1.on_packet_received(datagram);
  const auto after1 = w.store_of(1).clock_event_by_hash(c1.hash)->referencing_events.size();
  n1.on_packet_received(datagram);  // exact duplicate
  const auto after2 = w.store_of(1).clock_event_by_hash(c1.hash)->referencing_events.size();

  CHECK(after1 == 1);
  CHECK(after2 == 1);  // the duplicate must not append a second identical reverse edge
}

// 4.2 — a backward wall-clock step (NTP correction, an RTC-less Pi booting near the
// epoch) must not produce a clock event older than its chain tip, or the proven
// interval would invert. The timestamp is clamped to a per-chain monotonic floor.
TEST_CASE("4.2: a backward wall clock does not produce a non-monotonic clock chain") {
  harness::World w;
  Node& n = w.add_node(domain::NodeId(1), NodeConfig{});
  w.set_now(1000);
  n.create_clock_event();   // C1 ts = 1000
  w.set_now(500);           // clock jumps backward
  n.create_clock_event();   // C2 must be clamped to >= C1's timestamp
  const auto c1 = w.store_of(1).clock_event_by_seq(0);
  const auto c2 = w.store_of(1).clock_event_by_seq(1);
  CHECK(c1.timestamp == 1000);
  CHECK(c2.timestamp >= c1.timestamp);
}

// 4.3 — validation must reject a chain whose interval is inverted (upper < lower),
// which a non-monotonic reference clock could otherwise present as a "valid" but
// meaningless bound. Built with correct hashes/linkage so only the interval is at fault.
TEST_CASE("4.3: a chain with an inverted interval (upper < lower) is rejected") {
  domain::ClockEvent lo;
  lo.creator = domain::NodeId(1);
  lo.chain = 0;
  lo.timestamp = 1000;
  lo.hash = hash::calculate_clock_event_hash(lo);

  domain::Event evt;
  evt.creator = domain::NodeId(1);
  evt.referenced_events.push_back(domain::EventReference{domain::NodeId(1), lo.hash});
  evt.hash = hash::calculate_event_hash(evt);

  domain::ClockEvent hi;
  hi.creator = domain::NodeId(1);
  hi.chain = 0;
  hi.timestamp = 10;  // earlier than lo -> inverted
  hi.referenced_events.push_back(domain::EventReference{domain::NodeId(1), evt.hash});
  hi.hash = hash::calculate_clock_event_hash(hi);

  proof::Proof p;
  p.kind = proof::Kind::bounds;
  p.reference.node = domain::NodeId(1);
  p.chain.event = evt;
  p.chain.lower_bound.push_back(lo);
  p.chain.upper_bound.push_back(hi);

  harness::NullSigner signer;  // isolate the interval check from signatures
  CHECK_FALSE(proof::verify(p, signer).valid);
}

// 2.3 — a backup that cannot be written must fail loudly, not silently "succeed".
// FileStore::save previously swallowed every I/O error and returned void, so db-backup
// reported success even when nothing was written. save() now throws on failure; combined
// with the daemon's dispatch_guarded (Phase 1.5) that turns db-backup into an ERR reply.
TEST_CASE("2.3: FileStore::save fails loudly on an unwritable path") {
  os::FileStore store("/nonexistent-loti-dir-xyztmp/backup.snap");  // parent dir does not exist
  CHECK_THROWS_AS(store.save(domain::Bytes{1, 2, 3}), std::runtime_error);
}

TEST_CASE("2.3: FileStore::save + load still round-trips to a writable path") {
  const std::string path = "loti_test_backup.tmp";
  ::unlink(path.c_str());
  os::FileStore store(path);
  const domain::Bytes blob{0xDE, 0xAD, 0xBE, 0xEF};
  CHECK_NOTHROW(store.save(blob));
  const auto back = store.load();
  REQUIRE(back.has_value());
  CHECK(*back == blob);
  ::unlink(path.c_str());
  ::unlink((path + ".tmp").c_str());
}

// 3.4 — enabling flood routing must not trigger an unbounded fan-out. With all caps left at
// their 0 (unset) defaults, the Node applies a safe default fan-out (8); a discovery from a
// node cross-linked with many neighbors floods to at most that many, not all of them.
TEST_CASE("3.4: flood routing with default caps bounds the discovery fan-out") {
  harness::World w;
  NodeConfig cfg;
  cfg.discovery_routing = DiscoveryRouting::flood;  // fanout/hop_limit/forward_cap all default (0)
  Node& center = w.add_node(domain::NodeId(1), cfg);

  std::vector<Node*> all{&center};
  for (int i = 2; i <= 13; ++i) {  // 12 leaf neighbors around the center
    Node& leaf = w.add_node(domain::NodeId(i), NodeConfig{});
    center.add_neighbor(leaf.id());
    leaf.add_neighbor(center.id());
    all.push_back(&leaf);
  }
  harness::gossip(w, all, 6);  // the center cross-links with every leaf (>8)
  REQUIRE(w.pending() == 0);   // gossip fully drained

  domain::Event evt;
  evt.creator = all[1]->id();  // a leaf — so the discovery floods rather than completing locally
  evt.hash = domain::EventHash(32, 0xAB);
  harness::RecordingChain cb;
  center.discover_event_chain(evt, domain::TimeRange::all(), cb);

  // Without the cap this would fan out to all 12 cross-linked neighbors; the default caps it to 8.
  CHECK(w.pending() == 8);
}
