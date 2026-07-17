#include "node.hpp"

#include <cassert>
#include <stdexcept>

#include "hash/hashing.hpp"
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
      config_(config) {}

Node::~Node() {
  scheduler_.cancel(clock_timer_);
  scheduler_.cancel(purge_timer_);
}

void Node::start() {
  if (config_.clock_event_interval > 0) schedule_clock_timer();
  if (config_.discovery_expiry > 0) schedule_purge_timer();
}

void Node::add_neighbor(NodeId neighbor) {
  neighbors_.try_emplace(neighbor, Neighbor{neighbor, {}});
}

void Node::learn_route(NodeId destination, NodeId next_hop) {
  destination_to_next_hop_[destination] = next_hop;
}

// ---------------------------------------------------------------------------
// timers
// ---------------------------------------------------------------------------
void Node::schedule_clock_timer() {
  clock_timer_ = scheduler_.after(config_.clock_event_interval, [this] {
    create_clock_event();
    schedule_clock_timer();
  });
}

void Node::schedule_purge_timer() {
  purge_timer_ = scheduler_.after(config_.discovery_expiry / 100, [this] {
    purge_discoveries();
    schedule_purge_timer();
  });
}

void Node::create_clock_event() {
  const auto index = all_clock_events_.size();
  const auto& clock_event = insert_clock_event();
  event_hash_to_clock_event_index_[clock_event.hash] = index;
  for (const auto& referenced : clock_event.referenced_events)
    event_hash_to_referencing_event_index_.insert({referenced.hash, index});
  unreferenced_events_.clear();
  telemetry_.on_clock_event_created(clock_event);
  for (const auto& [nid, neighbor] : neighbors_) send_clock_event_notification(neighbor, clock_event);
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
  wire::ClockNotification m{clock_event.hash, neighbor.last_clock_event_hash};
  transport_.send(neighbor.node_id, wire::encode(id_, m));
}

void Node::on_packet_received(const Bytes& datagram) {
  const wire::Datagram dg = wire::decode(datagram);
  auto it = neighbors_.find(dg.sender);
  if (it == neighbors_.end()) return;  // packet from unknown neighbor — ignore
  Neighbor& neighbor = it->second;
  if (const auto* m = std::get_if<wire::ClockNotification>(&dg.payload)) {
    process_clock_event_notification(neighbor, m->last_clock_event_hash,
                                     m->neighbor_last_clock_event_hash);
  } else if (const auto* m = std::get_if<wire::ChainRequest>(&dg.payload)) {
    process_chain_request(m->originator, neighbor, m->event);
  } else if (const auto* m = std::get_if<wire::ChainResponse>(&dg.payload)) {
    process_chain_response(m->originator, neighbor, m->chain);
  }
}

void Node::process_clock_event_notification(Neighbor& neighbor, const Hash& last_clock_event_hash,
                                            const Hash& neighbor_last_clock_event_hash) {
  neighbor.last_clock_event_hash = last_clock_event_hash;
  if (auto idx = find_clock_event_index(neighbor_last_clock_event_hash)) {
    all_clock_events_[*idx].referencing_events.push_back(
        EventReference{neighbor.node_id, last_clock_event_hash});
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
    auto idx = find_event_index(event.hash);
    if (!idx) return;
    EventChain chain;
    chain.event = all_events_[*idx];
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
const Event& Node::publish_event(Bytes data) {
  const auto index = all_events_.size();
  const auto& event = insert_event(std::move(data));
  event_hash_to_event_index_[event.hash] = index;
  unreferenced_events_.push_back(event);
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
      if (auto idx = find_clock_event_index(ref.hash)) {
        chain.lower_bound.push_front(all_clock_events_[*idx]);  // slice LocalClockEvent -> ClockEvent
        return true;
      }
    }
  }
  return false;
}

bool Node::add_local_upper_bound(EventChain& chain) const {
  const Hash& referenced =
      chain.upper_bound.empty() ? chain.event.hash : chain.upper_bound.back().hash;
  auto range = event_hash_to_referencing_event_index_.equal_range(referenced);
  for (auto it = range.first; it != range.second; ++it) {
    chain.upper_bound.push_back(all_clock_events_[it->second]);
    return true;  // first local clock event that references it (mirrors Daemon)
  }
  return false;
}

bool Node::extend_lower_bound_for_neighbor(const Neighbor& neighbor, EventChain& chain) const {
  assert(chain.lower_bound.front().creator == id_);
  std::size_t index = get_clock_event_index(chain.lower_bound.front().hash);
  while (true) {
    const auto& current = all_clock_events_[index];
    for (const auto& ref : current.referenced_events)
      if (ref.creator == neighbor.node_id) return true;
    if (index == 0) return false;
    --index;
    chain.lower_bound.push_front(all_clock_events_[index]);
  }
}

bool Node::extend_upper_bound_for_neighbor(const Neighbor& neighbor, EventChain& chain) const {
  assert(chain.upper_bound.back().creator == id_);
  std::size_t index = get_clock_event_index(chain.upper_bound.back().hash);
  while (true) {
    const auto& current = all_clock_events_[index];
    for (const auto& ref : current.referencing_events)
      if (ref.creator == neighbor.node_id) return true;
    if (index == all_clock_events_.size() - 1) return false;
    ++index;
    chain.upper_bound.push_back(all_clock_events_[index]);
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
  prev = EventReference{chain.event.creator, chain.event.hash};
  for (auto it = chain.upper_bound.begin(); it != chain.upper_bound.end(); ++it) {
    const auto& ce = *it;
    if (hash::calculate_clock_event_hash(ce) != ce.hash)
      throw std::runtime_error("invalid event hash");
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
const Event& Node::insert_event(Bytes data) {
  Event event;
  event.data = std::move(data);
  event.creator = id_;
  event.salt = rng_.next_salt();
  if (!all_clock_events_.empty()) {
    const auto& last = all_clock_events_.back();
    event.referenced_events.push_back(EventReference{last.creator, last.hash});
  }
  event.hash = hash::calculate_event_hash(event);
  event.signature = signer_.sign(event.hash);
  all_events_.push_back(std::move(event));
  return all_events_.back();
}

const LocalClockEvent& Node::insert_clock_event() {
  LocalClockEvent clock_event;
  clock_event.timestamp = clock_.now();
  clock_event.creator = id_;
  clock_event.salt = rng_.next_salt();
  if (!all_clock_events_.empty()) {
    const auto& last = all_clock_events_.back();
    clock_event.referenced_events.push_back(EventReference{last.creator, last.hash});
  }
  for (const auto& [nid, neighbor] : neighbors_) {
    if (!neighbor.last_clock_event_hash.empty())
      clock_event.referenced_events.push_back(
          EventReference{neighbor.node_id, neighbor.last_clock_event_hash});
  }
  for (const auto& event : unreferenced_events_)
    clock_event.referenced_events.push_back(EventReference{event.creator, event.hash});
  clock_event.hash = hash::calculate_clock_event_hash(clock_event);
  clock_event.signature = signer_.sign(clock_event.hash);
  all_clock_events_.push_back(std::move(clock_event));
  return all_clock_events_.back();
}

std::optional<std::size_t> Node::find_clock_event_index(const Hash& hash) const {
  auto it = event_hash_to_clock_event_index_.find(hash);
  if (it == event_hash_to_clock_event_index_.end()) return std::nullopt;
  return it->second;
}

std::size_t Node::get_clock_event_index(const Hash& hash) const {
  if (auto idx = find_clock_event_index(hash)) return *idx;
  throw std::runtime_error("cannot find clock event");
}

std::optional<std::size_t> Node::find_event_index(const Hash& hash) const {
  auto it = event_hash_to_event_index_.find(hash);
  if (it == event_hash_to_event_index_.end()) return std::nullopt;
  return it->second;
}

const Neighbor* Node::find_next_hop_neighbor(NodeId node) const {
  auto it = destination_to_next_hop_.find(node);
  if (it == destination_to_next_hop_.end()) return nullptr;
  auto jt = neighbors_.find(it->second);
  return jt == neighbors_.end() ? nullptr : &jt->second;
}

}  // namespace loti
