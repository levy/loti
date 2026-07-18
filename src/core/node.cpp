#include "node.hpp"

#include <algorithm>
#include <cassert>
#include <stdexcept>

#include "hash/hashing.hpp"
#include "validate/chain.hpp"
#include "wire/codec.hpp"
#include "wire/packets.hpp"

namespace loti {

using namespace domain;

namespace {
// Membership test for the reverse-path breadcrumb (which doubles as the per-copy visited set).
bool contains(const std::vector<NodeId>& path, NodeId id) {
  return std::find(path.begin(), path.end(), id) != path.end();
}
}  // namespace

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
  // Build the forwarding policy from config. Static (default) reproduces the historical single
  // next hop; flood builds the time-dependent stack (neighbor-history → routing table → width
  // cap). All routers hold references to members that already exist.
  if (config_.discovery_routing == DiscoveryRouting::flood) {
    history_router_ = std::make_unique<routing::NeighborHistoryRouter>(id_, store_, neighbors_);
    routing_table_router_ =
        std::make_unique<routing::RoutingTableRouter>(timed_routes_, *history_router_);
    router_ = std::make_unique<routing::ProbabilisticRouter>(*routing_table_router_, rng_,
                                                             config_.discovery_fanout);
  } else {
    router_ =
        std::make_unique<routing::StaticShortestPathRouter>(neighbors_, destination_to_next_hop_);
  }
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

void Node::learn_route_at(NodeId destination, NodeId next_hop, TimeRange validity) {
  timed_routes_[destination].push_back(routing::TimedRoute{validity, {next_hop}});
  store_.put_timed_routes(timed_routes_);  // persist the (small) table as one unit
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
  for (auto it = flood_seen_.begin(); it != flood_seen_.end();) {  // drop stale flood-dedup entries
    if (it->second.first_seen < limit) it = flood_seen_.erase(it);
    else ++it;
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
    process_chain_request(neighbor, *m);
  } else if (const auto* m = std::get_if<wire::ChainResponse>(&dg.payload)) {
    process_chain_response(neighbor, *m);
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

void Node::send_chain_request(const Neighbor& to, const wire::ChainRequest& m) {
  transport_.send(to.node_id, wire::encode(id_, m));
}

void Node::flood_chain_request(NodeId dest, const routing::RouteContext& ctx,
                               wire::ChainRequest base) {
  const std::vector<NodeId> incoming = base.path;  // the breadcrumb before we append ourselves
  base.path.push_back(id_);
  for (NodeId hop : router_->next_hops(dest, ctx)) {
    if (hop == id_ || contains(incoming, hop)) continue;  // never forward back onto the breadcrumb
    if (const auto* nb = neighbor_by_id(hop)) send_chain_request(*nb, base);
  }
}

void Node::process_chain_request(const Neighbor& sender, const wire::ChainRequest& m) {
  if (m.event.creator == id_) {
    // Creator: seed the enclosing chain and reflect it back along the reverse-path breadcrumb.
    auto local = store_.event_by_hash(m.event.hash);
    if (!local) return;
    EventChain chain;
    chain.event = *local;
    if (!add_local_lower_bound(chain)) return;
    if (!add_local_upper_bound(chain)) return;
    if (!extend_lower_bound_for_neighbor(sender, chain)) return;
    if (!extend_upper_bound_for_neighbor(sender, chain)) return;
    send_chain_response_retrace(m.originator, chain, m.path);
  } else {
    // Intermediate: fan out toward the creator, appending ourself to the breadcrumb (which is also
    // the per-copy visited set for loop avoidance and bounds the flood depth).
    if (contains(m.path, id_)) return;                            // already visited — loop, drop
    if (m.hop_limit > 0 && m.path.size() >= m.hop_limit) return;  // hop-limit cap
    if (config_.discovery_forward_cap > 0) {                      // cross-branch fan-in cap
      auto& seen = flood_seen_[{m.originator, m.event.hash}];
      if (seen.forwards == 0) seen.first_seen = clock_.now();
      if (seen.forwards >= config_.discovery_forward_cap) return;  // re-flooded enough copies already
      ++seen.forwards;
    }
    flood_chain_request(m.event.creator, routing::RouteContext{m.range}, m);
  }
}

void Node::send_chain_response(const Neighbor& to, const wire::ChainResponse& m) {
  transport_.send(to.node_id, wire::encode(id_, m));
}

void Node::send_chain_response_retrace(NodeId originator, const EventChain& chain,
                                       const std::vector<NodeId>& path) {
  if (path.empty()) return;  // nowhere left to send (the originator completes locally)
  wire::ChainResponse resp;
  resp.originator = originator;
  resp.chain = chain;
  resp.path = path;
  const NodeId next_id = resp.path.back();
  resp.path.pop_back();
  if (const auto* to = neighbor_by_id(next_id)) send_chain_response(*to, resp);
}

void Node::process_chain_response(const Neighbor& sender, const wire::ChainResponse& m) {
  const Hash& hash = m.chain.event.hash;
  if (m.originator == id_) {
    // Home: attach our own clock events at both ends and complete (or abort).
    auto it = chain_discoveries_.find(hash);
    if (it == chain_discoveries_.end()) return;
    auto& discovery = it->second.discovery;
    if (discovery.state != DiscoveryState::in_progress) return;
    discovery.chain = m.chain;
    auto& updated = discovery.chain;
    if (!add_local_lower_bound(updated)) {
      abort_chain_discovery(hash);
    } else if (!add_local_upper_bound(updated)) {
      abort_chain_discovery(hash);
    } else {
      complete_chain_discovery(hash);
    }
  } else {
    // Intermediate on the return leg: splice our clock events, extend for the node the
    // response came from, then retrace one hop further along the breadcrumb — never routing.
    if (m.path.empty()) return;  // malformed: not home yet, but no breadcrumb left
    EventChain updated = m.chain;
    if (!add_local_lower_bound(updated)) return;
    if (!add_local_upper_bound(updated)) return;
    if (!extend_lower_bound_for_neighbor(sender, updated)) return;
    if (!extend_upper_bound_for_neighbor(sender, updated)) return;
    send_chain_response_retrace(m.originator, updated, m.path);
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

void Node::discover_event_chain(const Event& event, TimeRange range, ChainCallback& callback) {
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
  discovery.range = range;
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
  } else {
    wire::ChainRequest req;
    req.originator = id_;
    req.event = EventReference{event.creator, event.hash};
    req.range = range;
    req.hop_limit = config_.discovery_hop_limit;
    // flood_chain_request seeds the breadcrumb with id_ and sends a copy to each candidate
    // toward the creator (Static → the one next hop; flood → all cross-linked neighbors).
    flood_chain_request(event.creator, routing::RouteContext{range}, req);
  }
}

void Node::discover_event_bounds(const Event& event, TimeRange range, BoundsCallback& callback) {
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
  discover_event_chain(event, range, *this);
}

void Node::discover_event_order(const Event& event1, const Event& event2, TimeRange range,
                                OrderCallback& callback) {
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
  discover_event_chain(event1, range, *this);
  discover_event_chain(event2, range, *this);
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

void Node::validate_chain_discovery_result(const EventChainDiscovery& discovery) const {
  // The discovering node is the reference: the chain must be enclosed by two of its own
  // clock events. The full soundness walk (hashes, linkage, endpoints, signatures) is the
  // canonical validator shared with offline proof verification (core/validate/chain.hpp).
  const validate::ChainResult result = validate::verify_chain(discovery.chain, id_, signer_);
  if (!result.ok) throw std::runtime_error(result.reason);
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
const Neighbor* Node::neighbor_by_id(NodeId id) const {
  auto it = neighbors_.find(id);
  return it == neighbors_.end() ? nullptr : &it->second;
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
  timed_routes_ = store_.load_timed_routes();

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
