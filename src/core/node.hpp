// loti-core — the Node: the environment-agnostic protocol engine.
//
// The protocol engine, with every effectful call (time, scheduling, networking,
// randomness, signing, telemetry) routed through a port. One Node instance per
// participant; no globals, no blocking. The OMNeT++ modules and the production
// daemon are thin adapters that host a Node and wire it to a runtime's ports
// (doc/architecture.md).
#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include "domain/types.hpp"
#include "ports/clock.hpp"
#include "ports/rng.hpp"
#include "ports/scheduler.hpp"
#include "ports/signer.hpp"
#include "ports/store.hpp"
#include "ports/telemetry.hpp"
#include "ports/transport.hpp"
#include "routing/discovery_router.hpp"
#include "wire/packets.hpp"

namespace loti {

// Discovery result callbacks (ported from Daemon.h's IEvent*DiscoveryCallback).
class ChainCallback {
 public:
  virtual ~ChainCallback() = default;
  virtual void on_chain_completed(const domain::Event&, const domain::EventChain&) = 0;
  virtual void on_chain_aborted(const domain::Event&) = 0;
};
class BoundsCallback {
 public:
  virtual ~BoundsCallback() = default;
  virtual void on_bounds_completed(const domain::Event&, domain::Timestamp lower,
                                   domain::Timestamp upper) = 0;
  virtual void on_bounds_aborted(const domain::Event&) = 0;
};
class OrderCallback {
 public:
  virtual ~OrderCallback() = default;
  virtual void on_order_completed(const domain::Event&, const domain::Event&, domain::Order) = 0;
  virtual void on_order_aborted(const domain::Event&, const domain::Event&) = 0;
};

// One resolution level of the multi-resolution clock schedule: how often this chain ticks
// and how many of its clock events to retain (the ring capacity; 0 = unbounded / no prune).
struct ChainConfig {
  domain::Duration interval = 0;  // > 0 → auto-create this chain's clock events on a timer
  std::size_t keep = 0;           // retained clock events for this chain (0 = unbounded)
};

struct NodeConfig {
  // The chain schedule, index = chain/level (0 = fastest). Empty → a single implicit chain
  // (chain 0) with no timer — the caller drives create_clock_event() by hand (sim / tests).
  std::vector<ChainConfig> chains;
  domain::Duration discovery_expiry = 0;    // > 0 → auto-purge stale discoveries
  std::uint32_t discovery_hop_limit = 0;    // max discovery forward hops (0 = unlimited) — flood cap
};

// Non-owning references to the ports a Node runs on. The Store holds the whole DAG — the
// Node keeps no copy of the event/clock-event log in RAM, reading and writing it through
// this port (LMDB in production, an in-memory map in the simulation).
struct NodePorts {
  ports::Clock& clock;
  ports::Scheduler& scheduler;
  ports::Transport& transport;
  ports::Rng& rng;
  ports::Signer& signer;
  ports::Telemetry& telemetry;
  ports::Store& store;
};

class Node final : private ChainCallback {
 public:
  Node(domain::NodeId id, NodePorts ports, NodeConfig config);
  ~Node() override;

  Node(const Node&) = delete;
  Node& operator=(const Node&) = delete;

  // Schedule the periodic clock-event and purge timers (if enabled by config).
  void start();

  // Overlay, filled in by the configurator (sim) or peering subsystem (prod).
  void add_neighbor(domain::NodeId neighbor);
  void learn_route(domain::NodeId destination, domain::NodeId next_hop);

  // Application API. `range` is the querying party's estimated time window for the target
  // event — a required input the network cannot infer; forwarding routes over the overlay
  // as it was within it (doc/dynamic-discovery.md). Pass domain::TimeRange::all() when the
  // whole history is acceptable.
  domain::Event publish_event(domain::Bytes data);
  void discover_event_chain(const domain::Event& event, domain::TimeRange range,
                            ChainCallback& callback);
  void discover_event_bounds(const domain::Event& event, domain::TimeRange range,
                             BoundsCallback& callback);
  void discover_event_order(const domain::Event& event1, const domain::Event& event2,
                            domain::TimeRange range, OrderCallback& callback);

  // Inbound datagram from the transport adapter.
  void on_packet_received(const domain::Bytes& datagram);

  // Create a clock event now on the given chain/level (also the body of that chain's
  // periodic timer). Defaults to the fastest chain (0), which is what single-chain callers
  // and the test harness use.
  void create_clock_event(std::uint32_t chain = 0);

  // Number of clock chains this node runs (at least 1).
  [[nodiscard]] std::size_t chain_count() const noexcept { return num_chains_; }

  // Prune every chain to its configured ring capacity — the manual form of the per-tick
  // prune (backs `db gc`). A no-op for chains with keep == 0 (unbounded).
  void gc();

  // Read access — the DAG lives in the store; these are thin reads through it.
  [[nodiscard]] domain::NodeId id() const noexcept { return id_; }
  [[nodiscard]] std::size_t event_count() const { return store_.event_count(); }
  [[nodiscard]] domain::Event event_at(std::size_t i) const { return store_.event_by_seq(i); }
  [[nodiscard]] std::size_t clock_event_count() const { return store_.clock_event_count(); }

  // Rebuild the small in-RAM working set (neighbors, routes, unreferenced-event tail) from
  // whatever the store already holds. Called from the constructor, so a store-backed Node
  // is immediately consistent with its store (a fresh store yields empty working state; a
  // production restart recovers the persisted overlay + the tail published since the last
  // clock event). The daemon may call it again after wiring peers if it prefers.
  void hydrate_from_store() { hydrate_working_state(); }

  // Persistence. snapshot() serializes the full DAG (events, clock events with their
  // learned back-references, the unreferenced tail, neighbors, routes) to an opaque blob
  // by reading it back through the store — the portable db-backup format. restore() clears
  // the store and imports one. In-flight discoveries are transient and excluded.
  [[nodiscard]] domain::Bytes snapshot() const;
  void restore(const domain::Bytes& blob);

  // Import DAG state into the store (replacing whatever is there) and re-hydrate the
  // working set. The unreferenced tail is rederived from the events/clock events (an event
  // is unreferenced iff no clock event references it). restore() is implemented on top of it.
  void load(std::vector<domain::Event> events,
            std::vector<domain::LocalClockEvent> clock_events,
            std::map<domain::NodeId, domain::Neighbor> neighbors,
            std::map<domain::NodeId, domain::NodeId> routes);

 private:
  void hydrate_working_state();

  using Hash = domain::EventHash;

  // Node itself is the ChainCallback that drives bounds/order off chain completion.
  void on_chain_completed(const domain::Event&, const domain::EventChain&) override;
  void on_chain_aborted(const domain::Event&) override;

  // timers
  void schedule_clock_timer(std::uint32_t chain);
  void schedule_purge_timer();
  void purge_discoveries();

  // networking
  void send_clock_event_notification(const domain::Neighbor& neighbor,
                                     const domain::ClockEvent& clock_event);
  void process_clock_event_notification(domain::Neighbor& neighbor, std::uint32_t chain,
                                        const Hash& last_clock_event_hash,
                                        const Hash& neighbor_last_clock_event_hash);
  void send_chain_request(const domain::Neighbor& to, const wire::ChainRequest& m);
  void process_chain_request(const domain::Neighbor& sender, const wire::ChainRequest& m);
  void send_chain_response(const domain::Neighbor& to, const wire::ChainResponse& m);
  void process_chain_response(const domain::Neighbor& sender, const wire::ChainResponse& m);
  // Send a response one hop along the reverse-path breadcrumb: the next hop is path.back();
  // the response carries the rest (path minus that hop). Used by the creator and every
  // intermediate on the return leg (Decision 6 — the response never routes, it retraces).
  void send_chain_response_retrace(domain::NodeId originator, const domain::EventChain& chain,
                                   const std::vector<domain::NodeId>& path);

  // discovery bookkeeping
  domain::EventChainDiscovery& insert_chain_discovery(const domain::Event&, domain::NodeId originator,
                                                      ChainCallback*);
  void abort_chain_discovery(const Hash&);
  void complete_chain_discovery(const Hash&);
  domain::EventBoundsDiscovery& insert_bounds_discovery(const domain::Event&, BoundsCallback&);
  void abort_bounds_discovery(const Hash&);
  void complete_bounds_discovery(const Hash&);
  domain::EventOrderDiscovery& insert_order_discovery(const domain::Event&, const domain::Event&,
                                                      OrderCallback&);
  void abort_order_discovery(const Hash&, const Hash&);
  void complete_order_discovery(const Hash&, const Hash&);

  // chain building (the four primitives) + validation + comparison
  bool add_local_lower_bound(domain::EventChain&) const;
  bool add_local_upper_bound(domain::EventChain&) const;
  bool extend_lower_bound_for_neighbor(const domain::Neighbor&, domain::EventChain&) const;
  bool extend_upper_bound_for_neighbor(const domain::Neighbor&, domain::EventChain&) const;
  domain::Order compare_event_chains(const domain::EventChain&, const domain::EventChain&) const;
  void validate_event_chain(const domain::EventChain&) const;
  void validate_chain_discovery_result(const domain::EventChainDiscovery&) const;

  // DAG mutation / lookup — writes go to the store; the small overlay is read from RAM.
  domain::Event insert_event(domain::Bytes data);
  domain::LocalClockEvent insert_clock_event(std::uint32_t chain);
  [[nodiscard]] const domain::Neighbor* find_next_hop_neighbor(
      domain::NodeId, const routing::RouteContext&) const;
  [[nodiscard]] const domain::Neighbor* neighbor_by_id(domain::NodeId) const;

  // ports & config
  domain::NodeId id_;
  ports::Clock& clock_;
  ports::Scheduler& scheduler_;
  ports::Transport& transport_;
  ports::Rng& rng_;
  ports::Signer& signer_;
  ports::Telemetry& telemetry_;
  ports::Store& store_;  // the whole DAG lives here — the Node keeps no log copy in RAM
  NodeConfig config_;

  // Number of clock chains (levels) this node runs; at least 1 (max(1, config.chains.size())).
  std::size_t num_chains_ = 1;

  // Small in-RAM working set (the log itself is in the store). Neighbors and routes are
  // the overlay; unreferenced_per_chain_[ℓ] is the tail of published events not yet
  // referenced by a chain-ℓ clock event (each chain's tick references and clears its own
  // tail, so an event is pinned by every chain). All are rehydrated from the store at startup.
  std::map<domain::NodeId, domain::Neighbor> neighbors_;
  std::vector<std::vector<domain::Event>> unreferenced_per_chain_;
  std::map<domain::NodeId, domain::NodeId> destination_to_next_hop_;

  // Forwarding policy — the "where do we forward a discovery" seam. Holds const
  // references to the two overlay tables above, so it is declared after them (and thus
  // destroyed before them). Default = the width-1 StaticShortestPathRouter; later parts
  // swap in the time-dependent / probabilistic routers.
  std::unique_ptr<routing::DiscoveryRouter> router_;

  // in-flight discoveries (transient; not persisted)
  template <class Discovery, class Cb>
  struct Entry {
    Discovery discovery;
    std::vector<Cb*> callbacks;
  };
  std::map<Hash, Entry<domain::EventChainDiscovery, ChainCallback>> chain_discoveries_;
  std::map<Hash, Entry<domain::EventBoundsDiscovery, BoundsCallback>> bounds_discoveries_;
  std::map<std::pair<Hash, Hash>, Entry<domain::EventOrderDiscovery, OrderCallback>> order_discoveries_;

  std::vector<ports::TimerId> clock_timers_;  // one per chain (those with interval > 0)
  ports::TimerId purge_timer_ = 0;
};

}  // namespace loti
