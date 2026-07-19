// Tests for the LMDB-backed store (src/adapters/os/lmdb_store).
//
// Step 1 covers the lifecycle only: an env opens, stamps the format version, and
// reopens cleanly. The write/replay round-trips arrive with later steps.
#include "adapters/os/lmdb_store.hpp"

#include <array>
#include <filesystem>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

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

TEST_CASE("LmdbStore default and max mapsize are word-size-consistent") {
  // On 32-bit, `size_t{16} << 30` (2^34) would wrap to 0; the word-size-aware default
  // must stay a real, mmappable size that fits under the address-space ceiling.
  CHECK(LmdbStore::kDefaultMapSize > 0);
  if constexpr (sizeof(std::size_t) >= 8) {
    CHECK(LmdbStore::kMaxMapSize == 0);  // 64-bit: address space is effectively unbounded
    CHECK(LmdbStore::kDefaultMapSize == (std::size_t{16} << 30));
  } else {
    CHECK(LmdbStore::kMaxMapSize > 0);                            // 32-bit: a real ceiling
    CHECK(LmdbStore::kDefaultMapSize <= LmdbStore::kMaxMapSize);  // the default fits under it
    CHECK(LmdbStore::kMaxMapSize <= (std::size_t{2} << 30));      // and under the ~2 GiB space
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
    b.put_neighbor(domain::Neighbor{7, {hash_of(70)}});
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
  CHECK(neighbors.at(7).last_clock_event_hashes ==
        std::vector<domain::EventHash>{hash_of(70)});

  CHECK(store.load_routes() == std::map<domain::NodeId, domain::NodeId>{{9, 7}});

  // Sequence numbers continue across reopen.
  auto b = store.begin();
  CHECK(b.append_event(make_event(1, 12, "again")) == 2);
  b.commit();
}

TEST_CASE("LmdbStore persists and reloads the time-dependent routing table") {
  TempEnv env("timed_routes");
  domain::TimedRouteTable table;
  table[42].push_back(domain::TimedRoute{domain::TimeRange{100, 200}, {7, 9}});
  table[42].push_back(domain::TimedRoute{domain::TimeRange::all(), {3}});
  table[99].push_back(domain::TimedRoute{domain::TimeRange{-5, 5}, {1}});
  {
    LmdbStore store(env.str());
    store.put_timed_routes(table);
  }
  LmdbStore reopened(env.str());
  CHECK(reopened.load_timed_routes() == table);  // survives a reopen, byte-for-byte

  SUBCASE("put_timed_routes replaces the whole table") {
    domain::TimedRouteTable smaller;
    smaller[1].push_back(domain::TimedRoute{domain::TimeRange::all(), {2}});
    reopened.put_timed_routes(smaller);
    CHECK(reopened.load_timed_routes() == smaller);
  }
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
    REQUIRE(r.u64() == 2);  // snapshot format version
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
      for (auto m = r.u64(); m > 0; --m) nb.last_clock_event_hashes.push_back(r.blob());
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

TEST_CASE("Node writes each durable state change through its Store") {
  harness::World w;
  Node& n = w.add_node(1, NodeConfig{});
  auto& store = w.store_of(1);  // the DAG store backing this node

  n.add_neighbor(2);
  n.add_neighbor(2);  // already present → no second write
  n.learn_route(3, 2);
  const auto e = n.publish_event({0x01});
  n.create_clock_event();

  CHECK(store.load_neighbors().count(2) == 1);
  CHECK(store.load_routes() == std::map<domain::NodeId, domain::NodeId>{{3, 2}});
  REQUIRE(store.event_count() == 1);
  CHECK(store.event_by_seq(0).hash == e.hash);
  REQUIRE(store.clock_event_count() == 1);
  CHECK(store.clock_event_by_seq(0).referencing_events.empty());  // no notifications processed yet
}

TEST_CASE("Node persists neighbor and clock-event updates learned from gossip") {
  harness::World w;
  auto nodes = harness::build_path(w, 2);
  harness::gossip(w, nodes, 8);
  auto& store = w.store_of(nodes[1]->id());

  CHECK(store.clock_event_count() > 0);         // n2 created its own clock events
  CHECK_FALSE(store.load_neighbors().empty());  // n2 learned n1's latest clock-event hash
  // n1's notifications back-linked at least one of n2's clock events (referencing_events).
  bool any_referencing = false;
  for (std::size_t i = 0; i < store.clock_event_count(); ++i)
    if (!store.clock_event_by_seq(i).referencing_events.empty()) {
      any_referencing = true;
      break;
    }
  CHECK(any_referencing);
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

TEST_CASE("LmdbStore lazy (MDB_NOSYNC) mode round-trips after an explicit sync") {
  TempEnv env("lazysync");
  const auto e0 = make_event(1, 50, "lazy0");
  const auto e1 = make_event(1, 51, "lazy1");
  {
    // Lazy: commits skip the fsync; sync() forces them out before close — modeling the
    // daemon's clean-shutdown flush (run() calls store_->sync() on exit).
    LmdbStore store(env.str(), LmdbStore::kDefaultMapSize, LmdbStore::SyncPolicy::lazy);
    CHECK(store.sync_policy() == LmdbStore::SyncPolicy::lazy);
    auto b = store.begin();
    b.append_event(e0);
    b.append_event(e1);
    b.commit();
    store.sync();  // clean-shutdown flush
  }
  // Reopen (default safe policy) and confirm the synced data survived the restart.
  LmdbStore reopened(env.str());
  CHECK(reopened.event_count() == 2);
  CHECK(reopened.load_events() == std::vector<domain::Event>{e0, e1});
}

TEST_CASE("LmdbStore keeps the committed prefix intact when a later write is abandoned") {
  TempEnv env("crash");
  const auto e0 = make_event(1, 40, "kept");
  const auto e1 = make_event(1, 41, "kept-too");
  {
    LmdbStore store(env.str());
    auto b = store.begin();
    b.append_event(e0);
    b.append_event(e1);
    b.commit();  // durable

    // A second batch starts but the "process crashes" before commit: ~Batch aborts it,
    // and the store is destroyed without committing.
    auto crashed = store.begin();
    crashed.append_event(make_event(1, 42, "lost"));
  }
  // Reopen: exactly the committed prefix, fully readable, uncorrupted.
  LmdbStore store(env.str());
  CHECK(store.event_count() == 2);
  CHECK(store.load_events() == std::vector<domain::Event>{e0, e1});
}

// ---- ports::Store read API (by hash, by seq, reverse index) -----------------

TEST_CASE("LmdbStore serves the Store read API by hash and by seq") {
  TempEnv env("readapi");
  const auto e0 = make_event(1, 10, "e-zero");
  const auto e1 = make_event(1, 11, "e-one");
  const auto c0 = make_clock(1, 20, 1000);
  const auto c1 = make_clock(1, 21, 2000);
  {
    LmdbStore store(env.str());
    auto b = store.begin();
    b.append_event(e0);
    b.append_event(e1);
    b.append_clock_event(c0);
    b.append_clock_event(c1);
    b.commit();
  }
  LmdbStore store(env.str());
  // by dense seq
  CHECK(store.event_by_seq(0) == e0);
  CHECK(store.event_by_seq(1) == e1);
  CHECK(store.clock_event_by_seq(0) == c0);
  CHECK(store.clock_event_by_seq(1) == c1);
  // by hash (nullopt for an unknown hash)
  REQUIRE(store.event_by_hash(e1.hash).has_value());
  CHECK(*store.event_by_hash(e1.hash) == e1);
  CHECK_FALSE(store.event_by_hash(hash_of(200)).has_value());
  REQUIRE(store.clock_event_by_hash(c0.hash).has_value());
  CHECK(*store.clock_event_by_hash(c0.hash) == c0);
  // seq lookup + latest tip
  CHECK(store.clock_event_seq(c1.hash) == std::optional<std::uint64_t>{1});
  CHECK_FALSE(store.clock_event_seq(hash_of(200)).has_value());
  REQUIRE(store.latest_clock_event().has_value());
  CHECK(*store.latest_clock_event() == c1);
}

TEST_CASE("LmdbStore's referencing index maps a referenced hash to its clock events") {
  TempEnv env("revidx");
  // make_clock(tag) already references hash_of(tag+100); add a hash both clock events share.
  domain::LocalClockEvent a = make_clock(1, 30, 100);
  domain::LocalClockEvent b = make_clock(1, 31, 200);
  const domain::EventHash shared = hash_of(250);
  a.referenced_events.push_back({1, shared});
  b.referenced_events.push_back({1, shared});
  {
    LmdbStore store(env.str());
    auto batch = store.begin();
    CHECK(batch.append_clock_event(a) == 0);
    CHECK(batch.append_clock_event(b) == 1);
    batch.commit();
  }
  LmdbStore store(env.str());
  // Both reference `shared`, returned in ascending seq order.
  CHECK(store.clock_events_referencing(shared) == std::vector<std::uint64_t>{0, 1});
  // Each also references its own make_clock target, only from its own seq.
  CHECK(store.clock_events_referencing(hash_of(130)) == std::vector<std::uint64_t>{0});
  CHECK(store.clock_events_referencing(hash_of(131)) == std::vector<std::uint64_t>{1});
  // An unreferenced hash yields nothing.
  CHECK(store.clock_events_referencing(hash_of(199)).empty());
}

TEST_CASE("LmdbStore migrates a v1 store by rebuilding the referencing index") {
  TempEnv env("migrate");
  // A clock event (seq 0) referencing two event hashes — as a pre-v2 store would hold it,
  // with no `referencing` sub-DB. Opening it with the current LmdbStore must add the index.
  domain::LocalClockEvent c = make_clock(5, 60, 123);
  c.referenced_events.clear();
  c.referenced_events.push_back({5, hash_of(61)});
  c.referenced_events.push_back({5, hash_of(62)});

  auto be8 = [](std::uint64_t v) {
    std::array<unsigned char, 8> out{};
    for (int i = 7; i >= 0; --i) {
      out[i] = static_cast<unsigned char>(v & 0xFF);
      v >>= 8;
    }
    return out;
  };

  {  // hand-write a v1-format env: v1's 7 sub-DBs (no `referencing`), version = 1, one clock event
    MDB_env* e = nullptr;
    REQUIRE(mdb_env_create(&e) == MDB_SUCCESS);
    REQUIRE(mdb_env_set_maxdbs(e, 7) == MDB_SUCCESS);
    REQUIRE(mdb_env_set_mapsize(e, std::size_t{1} << 20) == MDB_SUCCESS);
    REQUIRE(mdb_env_open(e, env.str().c_str(), MDB_NOSUBDIR, 0644) == MDB_SUCCESS);
    MDB_txn* t = nullptr;
    REQUIRE(mdb_txn_begin(e, nullptr, 0, &t) == MDB_SUCCESS);
    MDB_dbi meta = 0, events = 0, event_index = 0, clock_events = 0, clock_index = 0, neighbors = 0,
            routes = 0;
    struct {
      const char* name;
      MDB_dbi* dbi;
    } subs[] = {{"meta", &meta},         {"events", &events},         {"event_index", &event_index},
                {"clock_events", &clock_events}, {"clock_index", &clock_index},
                {"neighbors", &neighbors},  {"routes", &routes}};
    for (auto& s : subs) REQUIRE(mdb_dbi_open(t, s.name, MDB_CREATE, s.dbi) == MDB_SUCCESS);
    const char* vk = "version";
    auto ver = be8(1);
    MDB_val vkey{7, const_cast<char*>(vk)}, vval{ver.size(), ver.data()};
    REQUIRE(mdb_put(t, meta, &vkey, &vval, 0) == MDB_SUCCESS);

    wire::Writer w;
    w.clock_event(c);
    w.refs(c.referencing_events);
    auto seq0 = be8(0);
    MDB_val ck{seq0.size(), seq0.data()};
    MDB_val cv{w.bytes().size(), const_cast<std::uint8_t*>(w.bytes().data())};
    REQUIRE(mdb_put(t, clock_events, &ck, &cv, 0) == MDB_SUCCESS);
    MDB_val ik{c.hash.size(), c.hash.data()}, iv{seq0.size(), seq0.data()};
    REQUIRE(mdb_put(t, clock_index, &ik, &iv, 0) == MDB_SUCCESS);
    REQUIRE(mdb_txn_commit(t) == MDB_SUCCESS);
    mdb_env_close(e);
  }

  LmdbStore store(env.str());
  CHECK(store.format_version() == LmdbStore::kFormatVersion);  // bumped to v2
  CHECK(store.clock_event_count() == 1);
  // The reverse index, absent on disk, was rebuilt from the clock event on open.
  CHECK(store.clock_events_referencing(hash_of(61)) == std::vector<std::uint64_t>{0});
  CHECK(store.clock_events_referencing(hash_of(62)) == std::vector<std::uint64_t>{0});
}

// 5.2 — a single-record write (the ports::Store path, via durable()) must grow the map as
// many times as the record needs, not give up after ONE doubling. Before the fix, durable()
// caught MDB_MAP_FULL once, grew once, and retried once; a record larger than a single
// doubling threw uncaught on the second attempt and crashed the daemon.
TEST_CASE("LmdbStore single-record write grows the map as many times as needed") {
  TempEnv env("bigrec");
  constexpr std::size_t kStart = std::size_t{1} << 20;  // 1 MiB start
  LmdbStore store(env.str(), kStart);

  domain::Event e;
  e.creator = 1;
  e.hash = domain::EventHash(32, 0xAB);
  e.data = domain::Bytes(std::size_t{5} << 20, 'x');  // ~5 MiB, far past a single 1→2 MiB doubling

  CHECK_NOTHROW(store.append_event(e));               // must grow 1→2→4→8 MiB, not throw after one
  CHECK(store.event_count() == 1);
  CHECK(store.map_size() >= (std::size_t{5} << 20));
}
