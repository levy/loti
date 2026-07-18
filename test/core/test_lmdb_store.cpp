// Tests for the LMDB-backed store (src/adapters/os/lmdb_store).
//
// Step 1 covers the lifecycle only: an env opens, stamps the format version, and
// reopens cleanly. The write/replay round-trips arrive with later steps.
#include "adapters/os/lmdb_store.hpp"

#include <filesystem>
#include <string>
#include <system_error>
#include <utility>

#include <unistd.h>

#include "doctest.h"
#include "harness/world.hpp"
#include "wire/codec.hpp"

namespace fs = std::filesystem;
using loti::os::LmdbStore;
using loti::Node;
using loti::NodeConfig;
namespace harness = loti::harness;
namespace wire = loti::wire;

namespace {

// A unique temp path for an LMDB env, removed (with its `-lock` sibling) on scope exit.
struct TempEnv {
  fs::path path;
  explicit TempEnv(const std::string& tag)
      : path(fs::temp_directory_path() /
             ("loti_lmdb_test_" + tag + "_" + std::to_string(::getpid()))) {
    remove();
  }
  ~TempEnv() { remove(); }
  void remove() const {
    std::error_code ec;
    fs::remove(path, ec);
    fs::remove(fs::path(path.string() + "-lock"), ec);
  }
  std::string str() const { return path.string(); }
};

namespace domain = loti::domain;

domain::EventHash hash_of(std::uint8_t tag) { return domain::EventHash(32, tag); }

domain::Event make_event(domain::NodeId creator, std::uint8_t tag, const std::string& data) {
  domain::Event e;
  e.creator = creator;
  e.hash = hash_of(tag);
  e.data = domain::Bytes(data.begin(), data.end());
  e.salt = 0x0102030405060708ull;
  e.referenced_events.push_back({creator, hash_of(static_cast<std::uint8_t>(tag - 1))});
  return e;
}

domain::LocalClockEvent make_clock(domain::NodeId creator, std::uint8_t tag, domain::Timestamp ts) {
  domain::LocalClockEvent c;
  c.creator = creator;
  c.hash = hash_of(tag);
  c.timestamp = ts;
  c.salt = 42;
  c.referenced_events.push_back({creator, hash_of(static_cast<std::uint8_t>(tag + 100))});
  c.referencing_events.push_back({creator, hash_of(static_cast<std::uint8_t>(tag + 200))});
  return c;
}

}  // namespace

TEST_CASE("LmdbStore opens, stamps a version, and reopens") {
  TempEnv env("open");

  SUBCASE("a fresh env is created and stamped with the current format version") {
    LmdbStore store(env.str());
    CHECK(store.path() == env.str());
    CHECK(store.format_version() == LmdbStore::kFormatVersion);
    CHECK(fs::exists(env.path));
  }

  SUBCASE("reopening an existing env reads back the same version") {
    { LmdbStore first(env.str()); }
    LmdbStore second(env.str());
    CHECK(second.format_version() == LmdbStore::kFormatVersion);
  }
}

TEST_CASE("LmdbStore round-trips a committed batch") {
  TempEnv env("roundtrip");

  const auto e0 = make_event(1, 10, "hello");
  const auto e1 = make_event(1, 11, "world");
  const auto c0 = make_clock(1, 20, 1000);
  const auto c1 = make_clock(1, 21, 2000);

  // Write a mix of records in one batch.
  {
    LmdbStore store(env.str());
    auto b = store.begin();
    CHECK(b.append_event(e0) == 0);
    CHECK(b.append_event(e1) == 1);
    CHECK(b.append_clock_event(c0) == 0);
    CHECK(b.append_clock_event(c1) == 1);
    b.put_neighbor(domain::Neighbor{7, hash_of(70)});
    b.put_route(9, 7);
    b.commit();
  }

  // Reopen and read it all back.
  LmdbStore store(env.str());
  CHECK(store.event_count() == 2);
  CHECK(store.clock_event_count() == 2);

  CHECK(store.load_events() == std::vector<domain::Event>{e0, e1});
  CHECK(store.load_clock_events() == std::vector<domain::LocalClockEvent>{c0, c1});

  const auto neighbors = store.load_neighbors();
  REQUIRE(neighbors.count(7) == 1);
  CHECK(neighbors.at(7).last_clock_event_hash == hash_of(70));

  CHECK(store.load_routes() == std::map<domain::NodeId, domain::NodeId>{{9, 7}});

  // Sequence numbers continue across reopen.
  auto b = store.begin();
  CHECK(b.append_event(make_event(1, 12, "again")) == 2);
  b.commit();
}

TEST_CASE("LmdbStore aborts an uncommitted batch without leaving a seq gap") {
  TempEnv env("abort");
  LmdbStore store(env.str());

  {
    auto b = store.begin();
    b.append_event(make_event(1, 30, "x"));  // seq 0, never committed → aborted on scope exit
  }
  CHECK(store.event_count() == 0);

  auto b = store.begin();
  CHECK(b.append_event(make_event(1, 31, "y")) == 0);  // reuses seq 0, no gap
  b.commit();
  CHECK(store.event_count() == 1);
}

TEST_CASE("Node::restore round-trips a snapshot (now routed through load)") {
  harness::World w;
  auto nodes = harness::build_path(w, 2);
  harness::gossip(w, nodes, 3);
  nodes[0]->publish_event({0x11});
  const auto snap1 = nodes[0]->snapshot();

  harness::World w2;
  Node& n2 = w2.add_node(1, NodeConfig{});
  n2.restore(snap1);
  CHECK(n2.snapshot() == snap1);
}

TEST_CASE("LmdbStore persists a Node's DAG and Node::load restores it byte-for-byte") {
  // 1. Build a real DAG on n1, ending with a trailing unreferenced-event tail.
  harness::World w;
  auto nodes = harness::build_path(w, 3);
  Node& n1 = *nodes[0];
  harness::gossip(w, nodes, 4);
  n1.publish_event({0xAA});
  harness::gossip(w, nodes, 2);     // references 0xAA and clears the unreferenced set
  n1.publish_event({0xBB, 0xCC});   // these two stay unreferenced (no gossip after)
  n1.publish_event({0xDD});
  const domain::Bytes snap1 = n1.snapshot();

  // 2. Parse the snapshot and persist every record into an LMDB store.
  TempEnv env("nodeload");
  {
    LmdbStore store(env.str());
    auto b = store.begin();
    wire::Reader r(snap1);
    REQUIRE(r.u64() == 1);  // format version
    for (auto n = r.u64(); n > 0; --n) b.append_event(r.event());
    for (auto n = r.u64(); n > 0; --n) {
      domain::LocalClockEvent c;
      static_cast<domain::ClockEvent&>(c) = r.clock_event();
      c.referencing_events = r.refs();
      b.append_clock_event(c);
    }
    for (auto n = r.u64(); n > 0; --n) r.event();  // unreferenced section — derived on load
    for (auto n = r.u64(); n > 0; --n) {
      domain::Neighbor nb;
      nb.node_id = r.u64();
      nb.last_clock_event_hash = r.blob();
      b.put_neighbor(nb);
    }
    for (auto n = r.u64(); n > 0; --n) {
      const auto dst = r.u64();
      const auto next_hop = r.u64();
      b.put_route(dst, next_hop);
    }
    CHECK(r.at_end());
    b.commit();
  }

  // 3. Reopen, load into a fresh Node, and compare snapshots byte-for-byte.
  LmdbStore store(env.str());
  harness::World w2;
  Node& n2 = w2.add_node(99, NodeConfig{});
  n2.load(store.load_events(), store.load_clock_events(), store.load_neighbors(),
          store.load_routes());
  CHECK(n2.snapshot() == snap1);
}

namespace {
struct RecordingPersistence final : loti::PersistenceListener {
  std::vector<domain::EventHash> events;
  std::vector<domain::EventHash> clock_appended;
  std::vector<domain::EventHash> clock_updated;
  std::vector<domain::NodeId> neighbors;
  std::vector<std::pair<domain::NodeId, domain::NodeId>> routes;
  void on_event_appended(const domain::Event& e) override { events.push_back(e.hash); }
  void on_clock_event_appended(const domain::LocalClockEvent& c) override {
    clock_appended.push_back(c.hash);
  }
  void on_clock_event_updated(const domain::LocalClockEvent& c) override {
    clock_updated.push_back(c.hash);
  }
  void on_neighbor_changed(const domain::Neighbor& n) override { neighbors.push_back(n.node_id); }
  void on_route_changed(domain::NodeId d, domain::NodeId nh) override { routes.emplace_back(d, nh); }
};
}  // namespace

TEST_CASE("Node reports each durable state change to a PersistenceListener") {
  harness::World w;
  Node& n = w.add_node(1, NodeConfig{});
  RecordingPersistence rec;
  n.set_persistence_listener(&rec);

  n.add_neighbor(2);
  n.add_neighbor(2);  // already present → no second neighbor_changed
  n.learn_route(3, 2);
  const auto e = n.publish_event({0x01});
  n.create_clock_event();

  CHECK(rec.neighbors == std::vector<domain::NodeId>{2});
  CHECK(rec.routes == std::vector<std::pair<domain::NodeId, domain::NodeId>>{{3, 2}});
  REQUIRE(rec.events.size() == 1);
  CHECK(rec.events[0] == e.hash);
  REQUIRE(rec.clock_appended.size() == 1);
  CHECK(rec.clock_updated.empty());  // no neighbor notifications processed yet
}

TEST_CASE("PersistenceListener sees neighbor and clock-event updates learned from gossip") {
  harness::World w;
  auto nodes = harness::build_path(w, 2);
  RecordingPersistence rec;
  nodes[1]->set_persistence_listener(&rec);  // attach AFTER build_path's add_neighbor/learn_route
  harness::gossip(w, nodes, 8);

  CHECK_FALSE(rec.clock_appended.empty());  // n2 created its own clock events
  CHECK_FALSE(rec.neighbors.empty());       // n2 learned n1's latest clock-event hash
  CHECK_FALSE(rec.clock_updated.empty());   // n1's notifications back-linked n2's clock events
}

TEST_CASE("LmdbStore grows its map on MDB_MAP_FULL and keeps the data") {
  TempEnv env("grow");
  constexpr std::size_t kStart = std::size_t{1} << 20;  // 1 MiB — small enough to fill

  auto big_event = [](int seq) {
    domain::Event e;
    e.creator = 1;
    e.hash.resize(32);  // unique hash from the sequence number
    for (int k = 0; k < 4; ++k) e.hash[k] = static_cast<std::uint8_t>(seq >> (8 * k));
    e.data = domain::Bytes(600, 'x');
    return e;
  };

  int written = 0;
  bool grew = false;
  {
    LmdbStore store(env.str(), kStart);
    for (int chunk = 0; chunk < 40; ++chunk) {   // 4000 * ~640 B ≈ 2.5 MiB > the 1 MiB map
      for (;;) {
        try {
          auto b = store.begin();
          for (int i = 0; i < 100; ++i) b.append_event(big_event(chunk * 100 + i));
          b.commit();
          written += 100;
          break;
        } catch (const loti::os::LmdbMapFull&) {
          store.grow_map();  // aborted txn consumed no seqs; retry the chunk
          grew = true;
        }
      }
    }
    CHECK(grew);                       // the small map really filled
    CHECK(store.map_size() > kStart);  // and was grown
    CHECK(store.event_count() == static_cast<std::size_t>(written));
  }

  // The grown data survives a reopen (the default mapsize covers it).
  LmdbStore reopened(env.str());
  CHECK(reopened.event_count() == static_cast<std::size_t>(written));
}
