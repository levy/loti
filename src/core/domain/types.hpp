// loti-core — the domain model.
//
// Plain-C++ structs with no dependency on OMNeT++/INET (`cObject`, `simtime_t`,
// INET chunks). These are what the DAG, the discoveries, and validation operate on,
// and what the canonical serializer (src/core/hash) turns into bytes.
//
// Signatures: `Event` and `ClockEvent` carry an OPTIONAL `signature` field from
// the start (empty = unsigned). It is deliberately EXCLUDED from the hash — the
// signature signs the hash — so wiring real signing in a later stage does not
// change any hash. See doc/architecture.md and plan/done/mvp-dual-target.md.
#pragma once

#include <array>
#include <cstdint>
#include <deque>
#include <limits>
#include <map>
#include <vector>

namespace loti::domain {

// A node's identity: a 128-bit fingerprint (the first 16 bytes of SHA-256(pubkey)). It is an
// OPAQUE 16-byte identity, not an integer — but it keeps an implicit constructor from uint64_t
// (big-endian in the low 8 bytes, high 8 zero) purely as a convenience for small simulation /
// test ids and for the `= 0` "none" sentinel. Real ids are the full 16 bytes and are never a
// zero-extended integer. 128 bits gives ~2^64 birthday safety (negligible collision probability
// even at billions of nodes) and 2^128 impersonation resistance — see plan/pending/wider-node-id.md.
struct NodeId {
  std::array<std::uint8_t, 16> bytes{};
  constexpr NodeId() = default;
  constexpr NodeId(std::uint64_t v) noexcept {  // implicit: small/sim ids and the `= 0` sentinel
    for (int i = 0; i < 8; ++i) bytes[15 - static_cast<std::size_t>(i)] =
        static_cast<std::uint8_t>(v >> (8 * i));
  }
  [[nodiscard]] bool is_zero() const noexcept { return bytes == std::array<std::uint8_t, 16>{}; }
  auto operator<=>(const NodeId&) const = default;
  bool operator==(const NodeId&) const = default;
};

using Salt      = std::uint64_t;
using Bytes     = std::vector<std::uint8_t>;
using EventHash = std::vector<std::uint8_t>;  // 32-byte SHA-256 digest
using Signature = std::vector<std::uint8_t>;  // empty = unsigned; excluded from the hash

// A node's local clock reading, as a raw integer tick that is hashed as u64 big-endian.
// The simulation maps OMNeT++ `simtime_t::raw()` onto this; production maps a wall-clock
// tick (e.g. nanoseconds). The core neither knows nor cares which.
using Timestamp = std::int64_t;

// A span of time in the same tick unit as Timestamp (a difference of Timestamps).
using Duration = std::int64_t;

// A closed time window [lo, hi] in Timestamp ticks: the estimated span during which the
// target event existed. It is supplied by the querying party — the network cannot estimate
// it — and it is what makes forwarding time-dependent: a discovery routes over the overlay
// as it was within this window. The default is the unconstrained full range
// (`TimeRange::all()`), which the time-dependent routers treat as "any time / all history".
struct TimeRange {
  Timestamp lo = std::numeric_limits<Timestamp>::min();
  Timestamp hi = std::numeric_limits<Timestamp>::max();
  [[nodiscard]] static constexpr TimeRange all() noexcept { return {}; }
  [[nodiscard]] constexpr bool contains(Timestamp t) const noexcept { return lo <= t && t <= hi; }
  bool operator==(const TimeRange&) const = default;
};

// A typed pointer to another event / clock event (its creator + hash).
struct EventReference {
  NodeId creator = 0;
  EventHash hash;
  bool operator==(const EventReference&) const = default;
};

// A published event: some content plus references to recent local clock events.
struct Event {
  NodeId creator = 0;
  EventHash hash;
  Bytes data;
  Salt salt = 0;
  std::vector<EventReference> referenced_events;
  Signature signature;  // optional; not hashed
  bool operator==(const Event&) const = default;
};

// A timestamped event. `referenced_events` embeds the previous local clock event,
// the latest known neighbor clock events, and recently created local events.
//
// A node runs several independent clock chains at geometrically spaced intervals
// (multi-resolution retention). `chain` is the resolution level this clock event
// belongs to — 0 is the fastest chain; each chain is an ordinary hash chain whose
// events only reference their own predecessor. It is part of the hashed content, so
// a clock event cannot lie about which chain it is on.
struct ClockEvent {
  NodeId creator = 0;
  EventHash hash;
  std::uint32_t chain = 0;
  Timestamp timestamp = 0;
  Salt salt = 0;
  std::vector<EventReference> referenced_events;
  Signature signature;  // optional; not hashed
  bool operator==(const ClockEvent&) const = default;
};

// A clock event created by THIS node. Also tracks the reverse cross-links —
// which neighbor clock events reference it — learned lazily from notifications.
struct LocalClockEvent : ClockEvent {
  std::vector<EventReference> referencing_events;
  bool operator==(const LocalClockEvent&) const = default;
};

// The proof object: a hash chain `lower_bound · event · upper_bound` that encloses
// `event` between two clock events of the reference node.
struct EventChain {
  Event event;
  std::deque<ClockEvent> lower_bound;
  std::deque<ClockEvent> upper_bound;
  bool operator==(const EventChain&) const = default;
};

// An overlay neighbor and the newest clock event of theirs we currently know, per
// chain: `last_clock_event_hashes[chain]` is that neighbor's latest chain-`chain` tip
// we hold (empty = not yet learned). A local clock event on chain ℓ cross-links to the
// neighbor's chain-ℓ tip (the matched-resolution neighbor policy).
struct Neighbor {
  NodeId node_id = 0;
  std::vector<EventHash> last_clock_event_hashes;
};

// A time-scoped overlay route toward some (multi-hop) destination: during `validity`, these are
// the ordered next hops (shortest first). Not derivable from the DAG — multi-hop direction is a
// global property — so it is filled by a routing protocol / static config and consumed by the
// RoutingTableRouter. It lives here (not in routing/) so the Store port can persist it.
struct TimedRoute {
  TimeRange validity;
  std::vector<NodeId> next_hops;
  bool operator==(const TimedRoute&) const = default;
};
// destination → the time-scoped routes learned toward it.
using TimedRouteTable = std::map<NodeId, std::vector<TimedRoute>>;

enum class DiscoveryState { in_progress, completed, aborted };

// Order of two events: -1 (event1 before event2), +1 (after), 0 (undetermined).
enum class Order : int { before = -1, undetermined = 0, after = 1 };

struct EventChainDiscovery {
  Timestamp start_time = 0;
  Timestamp end_time = 0;
  NodeId originator = 0;
  Event event;
  TimeRange range;  // the querying party's estimated window for `event` (routes by it)
  EventChain chain;
  DiscoveryState state = DiscoveryState::in_progress;
};

struct EventBoundsDiscovery {
  Timestamp start_time = 0;
  Timestamp end_time = 0;
  Event event;
  Timestamp lower_bound = 0;
  Timestamp upper_bound = 0;
  DiscoveryState state = DiscoveryState::in_progress;
};

struct EventOrderDiscovery {
  Timestamp start_time = 0;
  Timestamp end_time = 0;
  Event event1;
  Event event2;
  Order order = Order::undetermined;
  DiscoveryState state = DiscoveryState::in_progress;
};

}  // namespace loti::domain
