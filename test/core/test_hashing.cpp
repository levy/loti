// Golden tests that lock the canonical hash/serialization format. The expected
// digests were computed independently (Python hashlib over the byte layout), so
// these are a real cross-check, not a self-fulfilling snapshot. If the layout
// ever drifts, these break — which is the point: a drift would silently break
// every existing proof and the simulation's byte accounting.
#include "doctest.h"

#include <string>

#include "domain/types.hpp"
#include "hash/hashing.hpp"
#include "hash/picosha2.hpp"

using namespace loti;

namespace {
std::string hex(const domain::EventHash& h) { return picosha2::bytes_to_hex_string(h); }
}  // namespace

TEST_CASE("event hash matches golden (no references)") {
  domain::Event e;
  e.data = {0x68, 0x69};  // "hi"
  e.salt = 0x0123456789ABCDEFull;
  CHECK(hex(hash::calculate_event_hash(e)) ==
        "860472300bad041f13d38e42cbb9599653a1e85f1c3bd57fdfa9f8af760931c0");
}

TEST_CASE("event hash matches golden (one reference)") {
  domain::Event e;
  e.data = {0xDE, 0xAD};
  e.salt = 0x1122334455667788ull;
  e.referenced_events.push_back({/*creator=*/7, domain::EventHash(32, 0xAB)});
  CHECK(hex(hash::calculate_event_hash(e)) ==
        "db29865a4df0463f877798dd16a9520d886a937546a4baec534a3b738d396a52");
}

TEST_CASE("clock event hash matches golden (one reference)") {
  domain::ClockEvent c;
  c.chain = 0;  // the fastest chain; chain is now part of the hashed content
  c.timestamp = 1000000000;
  c.salt = 0xDEADBEEFCAFEBABEull;
  c.referenced_events.push_back({/*creator=*/42, domain::EventHash(32, 0xCD)});
  CHECK(hex(hash::calculate_clock_event_hash(c)) ==
        "1668765f3097a7e83514c47fda0e07b59921626c8ed1374f899f94171ad5604c");
}

TEST_CASE("the event's own creator/hash and signature are excluded from the hash") {
  domain::Event base;
  base.data = {0x01, 0x02, 0x03};
  base.salt = 0xABCDEF0123456789ull;
  const auto h0 = hash::calculate_event_hash(base);

  domain::Event decorated = base;
  decorated.creator = 999;                        // not serialized
  decorated.hash = domain::EventHash(32, 0xFF);   // not serialized
  decorated.signature = domain::Signature(64, 0x5A);  // reserved, never hashed
  CHECK(hash::calculate_event_hash(decorated) == h0);
}

TEST_CASE("accounted sizes match Data.cc (content excluded)") {
  domain::Event e;
  e.data = {1, 2, 3, 4, 5};  // content is NOT counted
  CHECK(hash::event_size_bytes(e) == 48u);  // 8 creator + 32 hash + 8 salt
  e.referenced_events.push_back({1, domain::EventHash(32, 0)});
  CHECK(hash::event_size_bytes(e) == 88u);  // + (8 + 32)

  domain::ClockEvent c;
  c.referenced_events.push_back({1, domain::EventHash(32, 0)});
  CHECK(hash::clock_event_size_bytes(c) == 104u);  // 8 + 32 + 8 chain + 8 ts + 8 salt + (8 + 32)
}
