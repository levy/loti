// loti-core — the Node: the environment-agnostic protocol engine.
//
// The protocol engine, with every effectful call (time, scheduling, networking,
// randomness, signing, telemetry) routed through a port. One Node instance per
// participant; no globals, no blocking. The OMNeT++ modules and the production
// daemon are thin adapters that host a Node and wire it to a runtime's ports
// (doc/architecture.md).
#pragma once

#include <cstddef>
#include <map>
#include <optional>
#include <utility>
#include <vector>

#include "domain/types.hpp"
#include "ports/clock.hpp"
#include "ports/rng.hpp"
#include "ports/scheduler.hpp"
#include "ports/signer.hpp"
#include "ports/telemetry.hpp"
#include "ports/transport.hpp"

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

// Persistence hook: the Node reports each durable state change so a host can mirror it
// into a store incrementally. The simulation leaves this unset; only the production
// daemon installs one. Deliberately NOT fired during load()/restore(), which set state
// in bulk (the store is already the source of that state).
class PersistenceListener {
 public:
  virtual ~PersistenceListener() = default;
  virtual void on_event_appended(const domain::Event&) = 0;
  virtual void on_clock_event_appended(const domain::LocalClockEvent&) = 0;
  // An already-appended local clock event gained a learned reverse cross-link
  // (its referencing_events changed) — the host should re-persist it.
  virtual void on_clock_event_updated(const domain::LocalClockEvent&) = 0;
  virtual void on_neighbor_changed(const domain::Neighbor&) = 0;
  virtual void on_route_changed(domain::NodeId destination, domain::NodeId next_hop) = 0;
};

struct NodeConfig {
  domain::Duration clock_event_interval = 0;  // > 0 → auto-create clock events on a timer
  domain::Duration discovery_expiry = 0;      // > 0 → auto-purge stale discoveries
};

// Non-owning references to the ports a Node runs on.
struct NodePorts {
  ports::Clock& clock;
  ports::Scheduler& scheduler;
  ports::Transport& transport;
  ports::Rng& rng;
  ports::Signer& signer;
  ports::Telemetry& telemetry;
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

  // Application API.
  const domain::Event& publish_event(domain::Bytes data);
  void discover_event_chain(const domain::Event& event, ChainCallback& callback);
  void discover_event_bounds(const domain::Event& event, BoundsCallback& callback);
  void discover_event_order(const domain::Event& event1, const domain::Event& event2,
                            OrderCallback& callback);

  // Inbound datagram from the transport adapter.
  void on_packet_received(const domain::Bytes& datagram);

  // Create a clock event now (also the body of the periodic timer).
  void create_clock_event();

  // Read access.
  [[nodiscard]] domain::NodeId id() const noexcept { return id_; }
  [[nodiscard]] std::size_t event_count() const noexcept { return all_events_.size(); }
  [[nodiscard]] const domain::Event& event_at(std::size_t i) const { return all_events_[i]; }
  [[nodiscard]] std::size_t clock_event_count() const noexcept { return all_clock_events_.size(); }

  // Persistence. snapshot() serializes the full DAG state (events, clock events with
  // their learned back-references, unreferenced events, neighbors, routes) to an
  // opaque blob; restore() loads one and rebuilds the derived indices. The
  // production daemon uses these to survive restart; the simulation never calls
  // them. In-flight discoveries are transient and deliberately excluded.
  [[nodiscard]] domain::Bytes snapshot() const;
  void restore(const domain::Bytes& blob);

  // Bulk-load persisted DAG state directly (the daemon's LMDB store feeds this at
  // startup). Populates the DAG and rebuilds the derived indices exactly as restore()
  // does; the unreferenced-event set is rederived from the clock events (an event is
  // unreferenced iff no clock event references it). restore() is implemented on top of this.
  void load(std::vector<domain::Event> events,
            std::vector<domain::LocalClockEvent> clock_events,
            std::map<domain::NodeId, domain::Neighbor> neighbors,
            std::map<domain::NodeId, domain::NodeId> routes);

  // Install (or clear, with nullptr) the persistence hook. The daemon sets this at
  // startup so every later state change is mirrored into its store; the sim leaves it
  // unset. Not retroactive — call load()/restore() first, then install, then run.
  void set_persistence_listener(PersistenceListener* listener) noexcept { persistence_ = listener; }

 private:
  using Hash = domain::EventHash;

  // Node itself is the ChainCallback that drives bounds/order off chain completion.
  void on_chain_completed(const domain::Event&, const domain::EventChain&) override;
  void on_chain_aborted(const domain::Event&) override;

  // timers
  void schedule_clock_timer();
  void schedule_purge_timer();
  void purge_discoveries();

  // networking
  void send_clock_event_notification(const domain::Neighbor& neighbor,
                                     const domain::ClockEvent& clock_event);
  void process_clock_event_notification(domain::Neighbor& neighbor, const Hash& last_clock_event_hash,
                                        const Hash& neighbor_last_clock_event_hash);
  void send_chain_request(domain::NodeId originator, const domain::Neighbor& neighbor,
                          const domain::EventReference& event);
  void process_chain_request(domain::NodeId originator, const domain::Neighbor& neighbor,
                             const domain::EventReference& event);
  void send_chain_response(domain::NodeId originator, const domain::Neighbor& neighbor,
                           const domain::EventChain& chain);
  void process_chain_response(domain::NodeId originator, const domain::Neighbor& neighbor,
                              const domain::EventChain& chain);

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

  // DAG mutation / lookup
  const domain::Event& insert_event(domain::Bytes data);
  const domain::LocalClockEvent& insert_clock_event();
  [[nodiscard]] std::optional<std::size_t> find_clock_event_index(const Hash&) const;
  [[nodiscard]] std::size_t get_clock_event_index(const Hash&) const;
  [[nodiscard]] std::optional<std::size_t> find_event_index(const Hash&) const;
  [[nodiscard]] const domain::Neighbor* find_next_hop_neighbor(domain::NodeId) const;

  // ports & config
  domain::NodeId id_;
  ports::Clock& clock_;
  ports::Scheduler& scheduler_;
  ports::Transport& transport_;
  ports::Rng& rng_;
  ports::Signer& signer_;
  ports::Telemetry& telemetry_;
  NodeConfig config_;
  PersistenceListener* persistence_ = nullptr;  // set by the daemon; null in the sim

  // DAG state (kept in the Node in Stage 2; extracted behind a Store port in Stage 3).
  std::map<domain::NodeId, domain::Neighbor> neighbors_;
  std::vector<domain::Event> all_events_;
  std::vector<domain::Event> unreferenced_events_;
  std::vector<domain::LocalClockEvent> all_clock_events_;
  std::map<domain::NodeId, domain::NodeId> destination_to_next_hop_;
  std::map<Hash, std::size_t> event_hash_to_event_index_;
  std::map<Hash, std::size_t> event_hash_to_clock_event_index_;
  std::multimap<Hash, std::size_t> event_hash_to_referencing_event_index_;

  // in-flight discoveries (transient; not persisted)
  template <class Discovery, class Cb>
  struct Entry {
    Discovery discovery;
    std::vector<Cb*> callbacks;
  };
  std::map<Hash, Entry<domain::EventChainDiscovery, ChainCallback>> chain_discoveries_;
  std::map<Hash, Entry<domain::EventBoundsDiscovery, BoundsCallback>> bounds_discoveries_;
  std::map<std::pair<Hash, Hash>, Entry<domain::EventOrderDiscovery, OrderCallback>> order_discoveries_;

  ports::TimerId clock_timer_ = 0;
  ports::TimerId purge_timer_ = 0;
};

}  // namespace loti
