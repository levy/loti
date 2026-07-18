#include "node.hpp"

#include <algorithm>
#include <cassert>
#include <stdexcept>

#include "hash/hashing.hpp"
#include "wire/codec.hpp"
#include "wire/packets.hpp"

namespace loti {

using namespace domain;

Node::Node(NodeId id, NodePorts ports, NodeConfig config)
    : id_(id),
      clock_(ports.clock),
      scheduler_(ports.scheduler),
      transport_(ports.transport),
      rng_(ports.rng),
      signer_(ports.signer),
      telemetry_(ports.telemetry),
      store_(ports.store),
      config_(config),
      num_chains_(std::max<std::size_t>(1, config.chains.size())) {
  clock_timers_.assign(num_chains_, 0);
  // Default forwarding policy: reproduce the historical single-shortest-path behavior.
  // Holds references to neighbors_ / destination_to_next_hop_, which already exist.
  router_ = std::make_unique<routing::StaticShortestPathRouter>(neighbors_, destination_to_next_hop_);
  hydrate_working_state();  // recover overlay + unreferenced tails from the store (empty if fresh)
}

Node::~Node() {
  for (auto id : clock_timers_) scheduler_.cancel(id);
  scheduler_.cancel(purge_timer_);
}

void Node::start() {
  for (std::uint32_t chain = 0; chain < config_.chains.size(); ++chain)
    if (config_.chains[chain].interval > 0) schedule_clock_timer(chain);
  if (config_.discovery_expiry > 0) schedule_purge_timer();
}

void Node::add_neighbor(NodeId neighbor) {
  auto [it, inserted] = neighbors_.try_emplace(neighbor, Neighbor{neighbor, {}});
  if (inserted) store_.put_neighbor(it->second);
}

void Node::learn_route(NodeId destination, NodeId next_hop) {
  destination_to_next_hop_[destination] = next_hop;
  store_.put_route(destination, next_hop);
}

// ---------------------------------------------------------------------------
// timers
// ---------------------------------------------------------------------------
void Node::schedule_clock_timer(std::uint32_t chain) {
  clock_timers_[chain] = scheduler_.after(config_.chains[chain].interval, [this, chain] {
    create_clock_event(chain);
    schedule_clock_timer(chain);
  });
}

void Node::schedule_purge_timer() {
  purge_timer_ = scheduler_.after(config_.discovery_expiry / 100, [this] {
    purge_discoveries();
    schedule_purge_timer();
  });
}

void Node::create_clock_event(std::uint32_t chain) {
  // insert_clock_event() appends to the store, which also records the reverse index
  // (each referenced hash → this clock event's seq) that used to be an in-RAM multimap.
  const LocalClockEvent clock_event = insert_clock_event(chain);
  unreferenced_per_chain_[chain].clear();  // this chain's tick referenced them all
  telemetry_.on_clock_event_created(clock_event);
  // Advertise this chain's new tip to neighbors. A chain only ticks when its tip changes,
  // so this is inherently change-only per chain.
  for (const auto& [nid, neighbor] : neighbors_) send_clock_event_notification(neighbor, clock_event);
  // Ring-prune this chain to its retention. Conservative by construction: any event a dropped
  // clock event referenced is still referenced by a slower chain (events pin into every chain),
  // so pruning widens an event's bounds to a coarser resolution — it never loses them.
  if (chain < config_.chains.size() && config_.chains[chain].keep > 0)
    store_.prune_chain(chain, config_.chains[chain].keep);
}

void Node::gc() {
  for (std::uint32_t chain = 0; chain < config_.chains.size(); ++chain)
    if (config_.chains[chain].keep > 0) store_.prune_chain(chain, config_.chains[chain].keep);
}

void Node::purge_discoveries() {
  const Timestamp limit = clock_.now() - config_.discovery_expiry;
  for (auto it = chain_discoveries_.begin(); it != chain_discoveries_.end();) {
    const auto& d = it->second.discovery;
    if (d.start_time < limit) {
      if (d.originator == id_ && d.state == DiscoveryState::in_progress)
        abort_chain_discovery(d.event.hash);
      it = chain_discoveries_.erase(it);
    } else {
      ++it;
    }
  }
  for (auto it = bounds_discoveries_.begin(); it != bounds_discoveries_.end();) {
    const auto& d = it->second.discovery;
    if (d.start_time < limit) {
      if (d.state == DiscoveryState::in_progress) abort_bounds_discovery(d.event.hash);
      it = bounds_discoveries_.erase(it);
    } else {
      ++it;
    }
  }
  for (auto it = order_discoveries_.begin(); it != order_discoveries_.end();) {
    const auto& d = it->second.discovery;
    if (d.start_time < limit) {
      if (d.state == DiscoveryState::in_progress)
        abort_order_discovery(d.event1.hash, d.event2.hash);
      it = order_discoveries_.erase(it);
    } else {
      ++it;
    }
  }
}

// ---------------------------------------------------------------------------
// networking
// ---------------------------------------------------------------------------
void Node::send_clock_event_notification(const Neighbor& neighbor, const ClockEvent& clock_event) {
  const std::uint32_t chain = clock_event.chain;
  const Hash their = chain < neighbor.last_clock_event_hashes.size()
                         ? neighbor.last_clock_event_hashes[chain]
                         : Hash{};
  wire::ClockNotification m{chain, clock_event.hash, their};
  transport_.send(neighbor.node_id, wire::encode(id_, m));
}

void Node::on_packet_received(const Bytes& datagram) {
  const wire::Datagram dg = wire::decode(datagram);
  auto it = neighbors_.find(dg.sender);
  if (it == neighbors_.end()) return;  // packet from unknown neighbor — ignore
  Neighbor& neighbor = it->second;
  if (const auto* m = std::get_if<wire::ClockNotification>(&dg.payload)) {
    process_clock_event_notification(neighbor, m->chain, m->last_clock_event_hash,
                                     m->neighbor_last_clock_event_hash);
  } else if (const auto* m = std::get_if<wire::ChainRequest>(&dg.payload)) {
    process_chain_request(m->originator, neighbor, m->event);
  } else if (const auto* m = std::get_if<wire::ChainResponse>(&dg.payload)) {
    process_chain_response(m->originator, neighbor, m->chain);
  }
}

void Node::process_clock_event_notification(Neighbor& neighbor, std::uint32_t chain,
                                            const Hash& last_clock_event_hash,
                                            const Hash& neighbor_last_clock_event_hash) {
  // Bucket the advertised tip into this neighbor's chain-`chain` slot.
  if (neighbor.last_clock_event_hashes.size() <= chain)
    neighbor.last_clock_event_hashes.resize(chain + 1);
  const bool neighbor_changed = neighbor.last_clock_event_hashes[chain] != last_clock_event_hash;
  neighbor.last_clock_event_hashes[chain] = last_clock_event_hash;
  if (neighbor_changed) store_.put_neighbor(neighbor);
  // Record the learned reverse cross-link on our clock event the neighbor referenced.
  if (auto local = store_.clock_event_by_hash(neighbor_last_clock_event_hash)) {
    local->referencing_events.push_back(EventReference{neighbor.node_id, last_clock_event_hash});
    store_.update_clock_event(*local);
  }
}

void Node::send_chain_request(NodeId originator, const Neighbor& neighbor,
                              const EventReference& event) {
  wire::ChainRequest m{originator, event};
  transport_.send(neighbor.node_id, wire::encode(id_, m));
}

void Node::process_chain_request(NodeId originator, const Neighbor& neighbor,
                                 const EventReference& event) {
  if (event.creator == id_) {
    auto local = store_.event_by_hash(event.hash);
    if (!local) return;
    EventChain chain;
    chain.event = *local;
    if (!add_local_lower_bound(chain)) return;
    if (!add_local_upper_bound(chain)) return;
    if (!extend_lower_bound_for_neighbor(neighbor, chain)) return;
    if (!extend_upper_bound_for_neighbor(neighbor, chain)) return;
    send_chain_response(originator, neighbor, chain);
  } else {
    if (const auto* next = find_next_hop_neighbor(event.creator))
      send_chain_request(originator, *next, event);
  }
}

void Node::send_chain_response(NodeId originator, const Neighbor& neighbor, const EventChain& chain) {
  wire::ChainResponse m{originator, chain};
  transport_.send(neighbor.node_id, wire::encode(id_, m));
}

void Node::process_chain_response(NodeId originator, const Neighbor& neighbor,
                                  const EventChain& chain) {
  const Hash& hash = chain.event.hash;
  if (originator == id_) {
    auto it = chain_discoveries_.find(hash);
    if (it == chain_discoveries_.end()) return;
    auto& discovery = it->second.discovery;
    if (discovery.state != DiscoveryState::in_progress) return;
    discovery.chain = chain;
    auto& updated = discovery.chain;
    if (!add_local_lower_bound(updated)) {
      abort_chain_discovery(hash);
    } else if (!add_local_upper_bound(updated)) {
      abort_chain_discovery(hash);
    } else {
      complete_chain_discovery(hash);
    }
  } else {
    const auto* next = find_next_hop_neighbor(originator);
    if (!next) return;
    EventChain updated = chain;
    if (!add_local_lower_bound(updated)) return;
    if (!add_local_upper_bound(updated)) return;
    if (!extend_lower_bound_for_neighbor(neighbor, updated)) return;
    if (!extend_upper_bound_for_neighbor(neighbor, updated)) return;
    send_chain_response(originator, *next, updated);
  }
}

// ---------------------------------------------------------------------------
// application API
// ---------------------------------------------------------------------------
Event Node::publish_event(Bytes data) {
  const Event event = insert_event(std::move(data));  // appended to the store (indexed by hash)
  for (auto& tail : unreferenced_per_chain_) tail.push_back(event);  // pinned by every chain's next tick
  telemetry_.on_event_created(event);
  return event;
}

void Node::discover_event_chain(const Event& event, ChainCallback& callback) {
  auto it = chain_discoveries_.find(event.hash);
  if (it != chain_discoveries_.end()) {
    const auto& d = it->second.discovery;
    switch (d.state) {
      case DiscoveryState::in_progress: it->second.callbacks.push_back(&callback); break;
      case DiscoveryState::aborted: callback.on_chain_aborted(event); break;
      case DiscoveryState::completed: callback.on_chain_completed(event, d.chain); break;
    }
    return;
  }
  auto& discovery = insert_chain_discovery(event, id_, &callback);
  telemetry_.on_chain_discovery_started(discovery);
  if (event.creator == id_) {
    auto& chain = discovery.chain;
    if (!add_local_lower_bound(chain)) {
      abort_chain_discovery(event.hash);
    } else if (!add_local_upper_bound(chain)) {
      abort_chain_discovery(event.hash);
    } else {
      complete_chain_discovery(event.hash);
    }
  } else if (const auto* next = find_next_hop_neighbor(event.creator)) {
    send_chain_request(id_, *next, EventReference{event.creator, event.hash});
  }
}

void Node::discover_event_bounds(const Event& event, BoundsCallback& callback) {
  auto it = bounds_discoveries_.find(event.hash);
  if (it != bounds_discoveries_.end()) {
    const auto& d = it->second.discovery;
    switch (d.state) {
      case DiscoveryState::in_progress: it->second.callbacks.push_back(&callback); break;
      case DiscoveryState::aborted: callback.on_bounds_aborted(event); break;
      case DiscoveryState::completed:
        callback.on_bounds_completed(event, d.lower_bound, d.upper_bound);
        break;
    }
    return;
  }
  auto& discovery = insert_bounds_discovery(event, callback);
  telemetry_.on_bounds_discovery_started(discovery);
  discover_event_chain(event, *this);
}

void Node::discover_event_order(const Event& event1, const Event& event2, OrderCallback& callback) {
  auto key = std::make_pair(event1.hash, event2.hash);
  auto it = order_discoveries_.find(key);
  if (it != order_discoveries_.end()) {
    const auto& d = it->second.discovery;
    switch (d.state) {
      case DiscoveryState::in_progress: it->second.callbacks.push_back(&callback); break;
      case DiscoveryState::aborted: callback.on_order_aborted(event1, event2); break;
      case DiscoveryState::completed: callback.on_order_completed(event1, event2, d.order); break;
    }
    return;
  }
  auto& discovery = insert_order_discovery(event1, event2, callback);
  telemetry_.on_order_discovery_started(discovery);
  discover_event_chain(event1, *this);
  discover_event_chain(event2, *this);
}

// ---------------------------------------------------------------------------
// Node as ChainCallback — drive bounds/order off chain completion/abort.
// ---------------------------------------------------------------------------
void Node::on_chain_completed(const Event&, const EventChain& chain) {
  const Hash& hash = chain.event.hash;
  if (auto it = bounds_discoveries_.find(hash);
      it != bounds_discoveries_.end() && it->second.discovery.state == DiscoveryState::in_progress) {
    it->second.discovery.lower_bound = chain.lower_bound.front().timestamp;
    it->second.discovery.upper_bound = chain.upper_bound.back().timestamp;
    complete_bounds_discovery(hash);
  }
  for (auto& [key, entry] : order_discoveries_) {
    auto& d = entry.discovery;
    if (d.state != DiscoveryState::in_progress) continue;
    if (key.first == hash) {
      auto jt = chain_discoveries_.find(key.second);
      if (jt != chain_discoveries_.end() &&
          jt->second.discovery.state == DiscoveryState::completed) {
        d.order = compare_event_chains(chain, jt->second.discovery.chain);
        complete_order_discovery(key.first, key.second);
      }
    } else if (key.second == hash) {
      auto jt = chain_discoveries_.find(key.first);
      if (jt != chain_discoveries_.end() &&
          jt->second.discovery.state == DiscoveryState::completed) {
        d.order = compare_event_chains(jt->second.discovery.chain, chain);
        complete_order_discovery(key.first, key.second);
      }
    }
  }
}

void Node::on_chain_aborted(const Event& event) {
  if (bounds_discoveries_.count(event.hash)) abort_bounds_discovery(event.hash);
  for (const auto& [key, entry] : order_discoveries_) {
    if (entry.discovery.state == DiscoveryState::in_progress &&
        (key.first == event.hash || key.second == event.hash)) {
      abort_order_discovery(key.first, key.second);
    }
  }
}

// ---------------------------------------------------------------------------
// discovery bookkeeping
// ---------------------------------------------------------------------------
EventChainDiscovery& Node::insert_chain_discovery(const Event& event, NodeId originator,
                                                  ChainCallback* callback) {
  EventChainDiscovery d;
  d.start_time = clock_.now();
  d.event = event;
  d.originator = originator;
  d.chain.event = event;
  d.state = DiscoveryState::in_progress;
  auto it = chain_discoveries_.insert({event.hash, {std::move(d), {callback}}}).first;
  return it->second.discovery;
}

void Node::abort_chain_discovery(const Hash& hash) {
  auto it = chain_discoveries_.find(hash);
  assert(it != chain_discoveries_.end());
  auto& d = it->second.discovery;
  assert(d.state == DiscoveryState::in_progress);
  d.end_time = clock_.now();
  d.state = DiscoveryState::aborted;
  for (auto* cb : it->second.callbacks) cb->on_chain_aborted(d.event);
  telemetry_.on_chain_discovery_aborted(d);
}

void Node::complete_chain_discovery(const Hash& hash) {
  auto it = chain_discoveries_.find(hash);
  assert(it != chain_discoveries_.end());
  auto& d = it->second.discovery;
  assert(d.state == DiscoveryState::in_progress);
  d.end_time = clock_.now();
  d.state = DiscoveryState::completed;
  validate_chain_discovery_result(d);
  for (auto* cb : it->second.callbacks) cb->on_chain_completed(d.event, d.chain);
  telemetry_.on_chain_discovery_completed(d);
}

EventBoundsDiscovery& Node::insert_bounds_discovery(const Event& event, BoundsCallback& callback) {
  EventBoundsDiscovery d;
  d.start_time = clock_.now();
  d.event = event;
  d.state = DiscoveryState::in_progress;
  auto it = bounds_discoveries_.insert({event.hash, {std::move(d), {&callback}}}).first;
  return it->second.discovery;
}

void Node::abort_bounds_discovery(const Hash& hash) {
  auto it = bounds_discoveries_.find(hash);
  assert(it != bounds_discoveries_.end());
  auto& d = it->second.discovery;
  assert(d.state == DiscoveryState::in_progress);
  d.end_time = clock_.now();
  d.state = DiscoveryState::aborted;
  for (auto* cb : it->second.callbacks) cb->on_bounds_aborted(d.event);
  telemetry_.on_bounds_discovery_aborted(d);
}

void Node::complete_bounds_discovery(const Hash& hash) {
  auto it = bounds_discoveries_.find(hash);
  assert(it != bounds_discoveries_.end());
  auto& d = it->second.discovery;
  assert(d.state == DiscoveryState::in_progress);
  d.end_time = clock_.now();
  d.state = DiscoveryState::completed;
  for (auto* cb : it->second.callbacks) cb->on_bounds_completed(d.event, d.lower_bound, d.upper_bound);
  telemetry_.on_bounds_discovery_completed(d);
}

EventOrderDiscovery& Node::insert_order_discovery(const Event& event1, const Event& event2,
                                                  OrderCallback& callback) {
  EventOrderDiscovery d;
  d.start_time = clock_.now();
  d.event1 = event1;
  d.event2 = event2;
  d.state = DiscoveryState::in_progress;
  auto it =
      order_discoveries_.insert({{event1.hash, event2.hash}, {std::move(d), {&callback}}}).first;
  return it->second.discovery;
}

void Node::abort_order_discovery(const Hash& hash1, const Hash& hash2) {
  auto it = order_discoveries_.find({hash1, hash2});
  assert(it != order_discoveries_.end());
  auto& d = it->second.discovery;
  assert(d.state == DiscoveryState::in_progress);
  d.end_time = clock_.now();
  d.state = DiscoveryState::aborted;
  for (auto* cb : it->second.callbacks) cb->on_order_aborted(d.event1, d.event2);
  telemetry_.on_order_discovery_aborted(d);
}

void Node::complete_order_discovery(const Hash& hash1, const Hash& hash2) {
  auto it = order_discoveries_.find({hash1, hash2});
  assert(it != order_discoveries_.end());
  auto& d = it->second.discovery;
  assert(d.state == DiscoveryState::in_progress);
  d.end_time = clock_.now();
  d.state = DiscoveryState::completed;
  for (auto* cb : it->second.callbacks) cb->on_order_completed(d.event1, d.event2, d.order);
  telemetry_.on_order_discovery_completed(d);
}

// ---------------------------------------------------------------------------
// chain building primitives
// ---------------------------------------------------------------------------
bool Node::add_local_lower_bound(EventChain& chain) const {
  const auto& refs = chain.lower_bound.empty() ? chain.event.referenced_events
                                               : chain.lower_bound.front().referenced_events;
  for (const auto& ref : refs) {
    if (ref.creator == id_) {
      if (auto ce = store_.clock_event_by_hash(ref.hash)) {
        chain.lower_bound.push_front(*ce);  // slice LocalClockEvent -> ClockEvent
        return true;
      }
    }
  }
  return false;
}

bool Node::add_local_upper_bound(EventChain& chain) const {
  const Hash& referenced =
      chain.upper_bound.empty() ? chain.event.hash : chain.upper_bound.back().hash;
  const auto seqs = store_.clock_events_referencing(referenced);
  if (seqs.empty()) return false;
  chain.upper_bound.push_back(store_.clock_event_by_seq(seqs.front()));  // lowest seq (mirrors Daemon)
  return true;
}

// Walk this node's OWN chain backward — following the same-chain predecessor, not the
// global sequence — until a clock event references the neighbor (the cross-link into the
// neighbor's chain). Staying on one chain keeps the spliced sub-chain hash-linked: a
// chain-ℓ clock event's only same-node clock-event reference is its chain-ℓ predecessor.
bool Node::extend_lower_bound_for_neighbor(const Neighbor& neighbor, EventChain& chain) const {
  assert(chain.lower_bound.front().creator == id_);
  LocalClockEvent current = *store_.clock_event_by_hash(chain.lower_bound.front().hash);
  while (true) {
    for (const auto& ref : current.referenced_events)
      if (ref.creator == neighbor.node_id) return true;
    // Step to the same-chain predecessor (the one same-node ref that is a clock event).
    std::optional<LocalClockEvent> prev;
    for (const auto& ref : current.referenced_events)
      if (ref.creator == id_)
        if (auto ce = store_.clock_event_by_hash(ref.hash)) { prev = std::move(ce); break; }
    if (!prev) return false;
    current = *prev;
    chain.lower_bound.push_front(current);
  }
}

bool Node::extend_upper_bound_for_neighbor(const Neighbor& neighbor, EventChain& chain) const {
  assert(chain.upper_bound.back().creator == id_);
  LocalClockEvent current = *store_.clock_event_by_hash(chain.upper_bound.back().hash);
  while (true) {
    for (const auto& ref : current.referencing_events)
      if (ref.creator == neighbor.node_id) return true;
    // Step to the same-chain successor: our own clock event that references `current`.
    std::optional<LocalClockEvent> next;
    for (std::uint64_t seq : store_.clock_events_referencing(current.hash)) {
      LocalClockEvent ce = store_.clock_event_by_seq(seq);
      if (ce.creator == id_ && ce.chain == current.chain) { next = std::move(ce); break; }
    }
    if (!next) return false;
    current = *next;
    chain.upper_bound.push_back(current);
  }
}

Order Node::compare_event_chains(const EventChain& a, const EventChain& b) const {
  if (a.upper_bound.back().timestamp < b.lower_bound.front().timestamp) return Order::before;
  if (b.upper_bound.back().timestamp < a.lower_bound.front().timestamp) return Order::after;
  return Order::undetermined;
}

void Node::validate_event_chain(const EventChain& chain) const {
  EventReference prev;  // empty
  bool have_prev = false;
  for (auto it = chain.lower_bound.begin(); it != chain.lower_bound.end(); ++it) {
    const auto& ce = *it;
    if (hash::calculate_clock_event_hash(ce) != ce.hash)
      throw std::runtime_error("invalid event hash");
    if (!signer_.verify(ce.hash, ce.signature, ce.creator))
      throw std::runtime_error("invalid clock event signature");
    if (have_prev) {
      bool found = false;
      for (const auto& ref : ce.referenced_events)
        if (ref == prev) { found = true; break; }
      if (!found) throw std::runtime_error("invalid lower bound");
    }
    prev = EventReference{ce.creator, ce.hash};
    have_prev = true;
  }
  {
    bool found = false;
    for (const auto& ref : chain.event.referenced_events)
      if (ref == prev) { found = true; break; }
    if (!found && !chain.lower_bound.empty()) throw std::runtime_error("invalid event");
  }
  if (!signer_.verify(chain.event.hash, chain.event.signature, chain.event.creator))
    throw std::runtime_error("invalid event signature");
  prev = EventReference{chain.event.creator, chain.event.hash};
  for (auto it = chain.upper_bound.begin(); it != chain.upper_bound.end(); ++it) {
    const auto& ce = *it;
    if (hash::calculate_clock_event_hash(ce) != ce.hash)
      throw std::runtime_error("invalid event hash");
    if (!signer_.verify(ce.hash, ce.signature, ce.creator))
      throw std::runtime_error("invalid clock event signature");
    if (it != chain.upper_bound.begin()) {
      bool found = false;
      for (const auto& ref : ce.referenced_events)
        if (ref == prev) { found = true; break; }
      if (!found) throw std::runtime_error("invalid upper bound");
    }
    prev = EventReference{ce.creator, ce.hash};
  }
}

void Node::validate_chain_discovery_result(const EventChainDiscovery& discovery) const {
  const auto& chain = discovery.chain;
  if (chain.lower_bound.front().creator != id_) throw std::runtime_error("invalid first clock event");
  if (chain.upper_bound.back().creator != id_) throw std::runtime_error("invalid last clock event");
  validate_event_chain(chain);
}

// ---------------------------------------------------------------------------
// DAG mutation / lookup
// ---------------------------------------------------------------------------
Event Node::insert_event(Bytes data) {
  Event event;
  event.data = std::move(data);
  event.creator = id_;
  event.salt = rng_.next_salt();
  // Lower anchor: pin into every chain by referencing each chain's current tip. A coarse
  // anchor survives after the finer chains are pruned, so the event stays lower-boundable.
  for (std::uint32_t chain = 0; chain < num_chains_; ++chain)
    if (auto tip = store_.latest_clock_event(chain))
      event.referenced_events.push_back(EventReference{tip->creator, tip->hash});
  event.hash = hash::calculate_event_hash(event);
  event.signature = signer_.sign(event.hash);
  store_.append_event(event);
  return event;
}

LocalClockEvent Node::insert_clock_event(std::uint32_t chain) {
  LocalClockEvent clock_event;
  clock_event.chain = chain;
  clock_event.timestamp = clock_.now();
  clock_event.creator = id_;
  clock_event.salt = rng_.next_salt();
  // (a) the previous same-chain clock event — this chain's hash-linked predecessor.
  if (auto last = store_.latest_clock_event(chain))
    clock_event.referenced_events.push_back(EventReference{last->creator, last->hash});
  // (b) each neighbor's matched-resolution tip (their chain-`chain` clock event).
  for (const auto& [nid, neighbor] : neighbors_) {
    if (chain < neighbor.last_clock_event_hashes.size() &&
        !neighbor.last_clock_event_hashes[chain].empty())
      clock_event.referenced_events.push_back(
          EventReference{neighbor.node_id, neighbor.last_clock_event_hashes[chain]});
  }
  // (c) the local events published since this chain's last tick (their upper pin).
  for (const auto& event : unreferenced_per_chain_[chain])
    clock_event.referenced_events.push_back(EventReference{event.creator, event.hash});
  clock_event.hash = hash::calculate_clock_event_hash(clock_event);
  clock_event.signature = signer_.sign(clock_event.hash);
  store_.append_clock_event(clock_event);  // also records the reverse index
  return clock_event;
}

// Ask the forwarding policy for the candidate next hops toward `node` and take the first
// that is a known neighbor. The default StaticShortestPathRouter returns exactly the one
// static next hop (already filtered to a known neighbor), so this preserves the historical
// single-path behavior; later parts return more candidates, which fan-out (Part 5) consumes.
const Neighbor* Node::find_next_hop_neighbor(NodeId node) const {
  for (NodeId hop : router_->next_hops(node, routing::RouteContext{})) {
    auto jt = neighbors_.find(hop);
    if (jt != neighbors_.end()) return &jt->second;
  }
  return nullptr;
}

// ---------------------------------------------------------------------------
// persistence (snapshot / restore) — see node.hpp
// ---------------------------------------------------------------------------
namespace {
constexpr std::uint64_t kSnapshotVersion = 2;  // v2: clock-event chain field + per-chain neighbor tips
}  // namespace

Bytes Node::snapshot() const {
  wire::Writer w;
  w.u64(kSnapshotVersion);
  const std::size_t event_count = store_.event_count();
  w.u64(event_count);
  for (std::size_t i = 0; i < event_count; ++i) w.event(store_.event_by_seq(i));
  const auto clock_events = store_.load_clock_events();  // live, seq order (gap-tolerant after prune)
  w.u64(clock_events.size());
  for (const auto& c : clock_events) {
    w.clock_event(c);              // the ClockEvent part
    w.refs(c.referencing_events);  // the learned back-references (LocalClockEvent extra)
  }
  // The unreferenced tail is kept only for on-disk format shape — load() rederives every
  // chain's tail from the clock events. Write the fastest chain's tail (chain 0), which for a
  // single-chain node is the whole unreferenced set. (num_chains_ ≥ 1, so index 0 exists.)
  const auto& tail0 = unreferenced_per_chain_[0];
  w.u64(tail0.size());
  for (const auto& e : tail0) w.event(e);
  w.u64(neighbors_.size());
  for (const auto& [id, n] : neighbors_) {
    w.u64(n.node_id);
    w.u64(n.last_clock_event_hashes.size());
    for (const auto& h : n.last_clock_event_hashes) w.blob(h);
  }
  w.u64(destination_to_next_hop_.size());
  for (const auto& [dst, next_hop] : destination_to_next_hop_) {
    w.u64(dst);
    w.u64(next_hop);
  }
  return w.bytes();
}

void Node::restore(const Bytes& blob) {
  wire::Reader r(blob);
  if (r.u64() != kSnapshotVersion) throw std::runtime_error("snapshot: unsupported version");

  std::vector<Event> events;
  for (auto n = r.u64(); n > 0; --n) events.push_back(r.event());

  std::vector<LocalClockEvent> clock_events;
  for (auto n = r.u64(); n > 0; --n) {
    LocalClockEvent clock_event;
    static_cast<ClockEvent&>(clock_event) = r.clock_event();
    clock_event.referencing_events = r.refs();
    clock_events.push_back(std::move(clock_event));
  }

  // The snapshot still stores the unreferenced events in full; skip past them —
  // load() rederives the set from the clock events (kept for on-disk format compat).
  for (auto n = r.u64(); n > 0; --n) r.event();

  std::map<NodeId, Neighbor> neighbors;
  for (auto n = r.u64(); n > 0; --n) {
    Neighbor neighbor;
    neighbor.node_id = r.u64();
    for (auto m = r.u64(); m > 0; --m) neighbor.last_clock_event_hashes.push_back(r.blob());
    neighbors[neighbor.node_id] = std::move(neighbor);
  }

  std::map<NodeId, NodeId> routes;
  for (auto n = r.u64(); n > 0; --n) {
    const auto dst = r.u64();
    const auto next_hop = r.u64();
    routes[dst] = next_hop;
  }

  load(std::move(events), std::move(clock_events), std::move(neighbors), std::move(routes));
}

void Node::load(std::vector<Event> events, std::vector<LocalClockEvent> clock_events,
                std::map<NodeId, Neighbor> neighbors, std::map<NodeId, NodeId> routes) {
  // Import the DAG into the store (replacing whatever it held) — append_clock_event also
  // rebuilds the reverse index — then rehydrate the working set from it.
  store_.clear();
  for (const auto& e : events) store_.append_event(e);
  for (const auto& c : clock_events) store_.append_clock_event(c);
  for (const auto& [id, n] : neighbors) store_.put_neighbor(n);
  for (const auto& [dst, next_hop] : routes) store_.put_route(dst, next_hop);
  hydrate_working_state();
}

void Node::hydrate_working_state() {
  neighbors_ = store_.load_neighbors();
  destination_to_next_hop_ = store_.load_routes();

  // Rederive each chain's unreferenced-event tail: for chain ℓ, the events published since
  // its last tick — i.e. the suffix of events not yet referenced by any chain-ℓ clock event.
  // Walk events newest-first per chain until one is already referenced by that chain; a
  // bounded scan (each chain references and clears its whole then-current tail on every tick).
  unreferenced_per_chain_.assign(num_chains_, {});
  for (std::uint32_t chain = 0; chain < num_chains_; ++chain) {
    std::vector<Event> tail;
    for (std::size_t k = store_.event_count(); k-- > 0;) {
      Event e = store_.event_by_seq(k);
      bool referenced_by_chain = false;
      for (std::uint64_t seq : store_.clock_events_referencing(e.hash))
        if (store_.clock_event_by_seq(seq).chain == chain) { referenced_by_chain = true; break; }
      if (referenced_by_chain) break;  // reached this chain's referenced prefix
      tail.push_back(std::move(e));
    }
    for (auto it = tail.rbegin(); it != tail.rend(); ++it)  // restore publish order
      unreferenced_per_chain_[chain].push_back(std::move(*it));
  }
}

}  // namespace loti
