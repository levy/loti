// loti-core — the domain model.
//
// Plain-C++ port of the OMNeT++ message classes in src/Data.msg / src/Type.msg,
// with no dependency on OMNeT++/INET (`cObject`, `simtime_t`, INET chunks). These
// structs are what the DAG, the discoveries, and validation operate on, and what
// the canonical serializer (core/hash) turns into bytes.
//
// Signatures: `Event` and `ClockEvent` carry an OPTIONAL `signature` field from
// the start (empty = unsigned). It is deliberately EXCLUDED from the hash — the
// signature signs the hash — so wiring real signing in a later stage does not
// change any hash. See documentation/architecture.md and plan/pending/mvp-dual-target.md.
#pragma once

#include <cstdint>
#include <deque>
#include <vector>

namespace loti::domain {

using NodeId    = std::uint64_t;
using Salt      = std::uint64_t;
using Bytes     = std::vector<std::uint8_t>;
using EventHash = std::vector<std::uint8_t>;  // 32-byte SHA-256 digest
using Signature = std::vector<std::uint8_t>;  // empty = unsigned; excluded from the hash

// A node's local clock reading, as a raw integer tick that is hashed as u64 big-endian.
// The simulation maps OMNeT++ `simtime_t::raw()` onto this; production maps a wall-clock
// tick (e.g. nanoseconds). The core neither knows nor cares which.
using Timestamp = std::int64_t;

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
struct ClockEvent {
  NodeId creator = 0;
  EventHash hash;
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

// An overlay neighbor and the newest clock event of theirs we currently know.
struct Neighbor {
  NodeId node_id = 0;
  EventHash last_clock_event_hash;
};

enum class DiscoveryState { in_progress, completed, aborted };

// Order of two events: -1 (event1 before event2), +1 (after), 0 (undetermined).
enum class Order : int { before = -1, undetermined = 0, after = 1 };

struct EventChainDiscovery {
  Timestamp start_time = 0;
  Timestamp end_time = 0;
  NodeId originator = 0;
  Event event;
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
