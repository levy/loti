// Tests for the LMDB-backed store (src/adapters/os/lmdb_store).
//
// Step 1 covers the lifecycle only: an env opens, stamps the format version, and
// reopens cleanly. The write/replay round-trips arrive with later steps.
#include "adapters/os/lmdb_store.hpp"

#include <filesystem>
#include <string>
#include <system_error>

#include <unistd.h>

#include "doctest.h"

namespace fs = std::filesystem;
using loti::os::LmdbStore;

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
    b.mark_unreferenced(e1.hash);
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

  const auto unref = store.load_unreferenced();
  REQUIRE(unref.size() == 1);
  CHECK(unref[0] == e1.hash);

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
