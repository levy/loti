// The wire codec must round-trip every packet type — including a full EventChain —
// so the bytes the transports carry decode back to the same objects.
#include "doctest.h"

#include "domain/types.hpp"
#include "wire/packets.hpp"

using namespace loti;

TEST_CASE("clock notification round-trips") {
  wire::ClockNotification m{/*chain=*/2, domain::EventHash(32, 0x11), domain::EventHash(32, 0x22)};
  const auto dg = wire::decode(wire::encode(/*sender=*/7, m));
  CHECK(dg.sender == 7);
  const auto* got = std::get_if<wire::ClockNotification>(&dg.payload);
  REQUIRE(got != nullptr);
  CHECK(got->chain == m.chain);
  CHECK(got->last_clock_event_hash == m.last_clock_event_hash);
  CHECK(got->neighbor_last_clock_event_hash == m.neighbor_last_clock_event_hash);
}

TEST_CASE("chain request round-trips") {
  wire::ChainRequest m;
  m.originator = 3;
  m.event = domain::EventReference{5, domain::EventHash(32, 0xAB)};
  m.range = domain::TimeRange{100, 200};
  m.hop_limit = 7;
  m.path = {3, 4, 5};
  const auto dg = wire::decode(wire::encode(9, m));
  CHECK(dg.sender == 9);
  const auto* got = std::get_if<wire::ChainRequest>(&dg.payload);
  REQUIRE(got != nullptr);
  CHECK(got->originator == 3);
  CHECK(got->event == m.event);
  CHECK(got->range == m.range);
  CHECK(got->hop_limit == m.hop_limit);
  CHECK(got->path == m.path);
}

TEST_CASE("chain response with a full EventChain round-trips") {
  domain::EventChain chain;
  chain.event.creator = 1;
  chain.event.hash = domain::EventHash(32, 0xE0);
  chain.event.data = {0xDA, 0x7A};
  chain.event.salt = 0x1234;
  chain.event.referenced_events.push_back({2, domain::EventHash(32, 0xC0)});

  domain::ClockEvent lo;
  lo.creator = 2;
  lo.hash = domain::EventHash(32, 0x0A);
  lo.timestamp = 111;
  lo.salt = 0xAA;
  lo.referenced_events.push_back({3, domain::EventHash(32, 0x0B)});
  chain.lower_bound.push_back(lo);

  domain::ClockEvent hi = lo;
  hi.creator = 4;
  hi.timestamp = 222;
  chain.upper_bound.push_back(hi);

  wire::ChainResponse m;
  m.originator = 42;
  m.chain = chain;
  m.path = {42, 7, 9};
  const auto dg = wire::decode(wire::encode(1, m));
  CHECK(dg.sender == 1);
  const auto* got = std::get_if<wire::ChainResponse>(&dg.payload);
  REQUIRE(got != nullptr);
  CHECK(got->originator == 42);
  CHECK(got->chain == chain);
  CHECK(got->path == m.path);
}
