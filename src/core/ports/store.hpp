// The DAG's backing store, abstracted as a port so loti_core stays environment-agnostic.
//
// The Node reads AND writes its whole DAG — events, local clock events, the reverse
// cross-reference index, neighbors, and routes — exclusively through this port; it keeps
// no copy of the log in RAM. Production is LMDB (adapters/os): its mmap is backed by the
// OS page cache, so a node's resident memory stays bounded as the DAG grows on disk (cold
// pages evict under pressure, hot pages stay, reads are zero-copy). The simulation is an
// in-memory map (adapters/sim) whose state dies with the run. This port subsumes the old
// PersistenceListener seam — there is one path for durable state, not two.
//
// Sequence numbers are dense: 0-based and gap-free (an aborted append consumes none), so
// they double as array-like indices into the event / clock-event logs.
#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <vector>

#include "domain/types.hpp"

namespace loti::ports {

class Store {
 public:
  virtual ~Store() = default;

  // ---- writes (durable on return for a persistent implementation) --------------------
  // Append an event / local clock event to its log; returns the assigned sequence.
  // append_clock_event also records the reverse index — for each hash the clock event
  // references, that this seq references it — which backs clock_events_referencing().
  virtual std::uint64_t append_event(const domain::Event&) = 0;
  virtual std::uint64_t append_clock_event(const domain::LocalClockEvent&) = 0;
  // Re-persist an already-appended local clock event whose referencing_events grew (a
  // learned reverse cross-link from a neighbor notification).
  virtual void update_clock_event(const domain::LocalClockEvent&) = 0;
  virtual void put_neighbor(const domain::Neighbor&) = 0;
  virtual void put_route(domain::NodeId destination, domain::NodeId next_hop) = 0;

  // ---- reads -------------------------------------------------------------------------
  [[nodiscard]] virtual std::optional<domain::Event> event_by_hash(
      const domain::EventHash&) const = 0;
  [[nodiscard]] virtual domain::Event event_by_seq(std::uint64_t seq) const = 0;
  [[nodiscard]] virtual std::optional<domain::LocalClockEvent> clock_event_by_hash(
      const domain::EventHash&) const = 0;
  [[nodiscard]] virtual domain::LocalClockEvent clock_event_by_seq(std::uint64_t seq) const = 0;
  // The dense seq of the clock event with this hash (nullopt if unknown).
  [[nodiscard]] virtual std::optional<std::uint64_t> clock_event_seq(
      const domain::EventHash&) const = 0;
  // Ascending seqs of the local clock events that reference `hash` in referenced_events.
  [[nodiscard]] virtual std::vector<std::uint64_t> clock_events_referencing(
      const domain::EventHash&) const = 0;
  // The newest local clock event (nullopt if none) — the local clock chain's tip.
  [[nodiscard]] virtual std::optional<domain::LocalClockEvent> latest_clock_event() const = 0;

  [[nodiscard]] virtual std::size_t event_count() const = 0;
  [[nodiscard]] virtual std::size_t clock_event_count() const = 0;

  // Bulk read-back of the small overlay state, loaded into the Node's working set at
  // startup (neighbors carry their learned last-clock-event hash across a restart).
  [[nodiscard]] virtual std::map<domain::NodeId, domain::Neighbor> load_neighbors() const = 0;
  [[nodiscard]] virtual std::map<domain::NodeId, domain::NodeId> load_routes() const = 0;

  // Drop every DAG and overlay record (before a db-restore rewrites the store).
  virtual void clear() = 0;
};

}  // namespace loti::ports
