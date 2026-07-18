// The simulation's Store port: the whole DAG lives in RAM and dies with the run. It
// mirrors the LMDB store's dense-sequence semantics exactly (0-based, gap-free) so the
// Node behaves identically whether it runs on this or on the production store. Used by
// the OMNeT++ simulation and the in-process test harness (test/harness/world.hpp).
#pragma once

#include <algorithm>
#include <cstdint>
#include <deque>
#include <map>
#include <optional>
#include <vector>

#include "domain/types.hpp"
#include "ports/store.hpp"

namespace loti::sim {

class InMemoryStore final : public ports::Store {
 public:
  std::uint64_t append_event(const domain::Event& e) override {
    const std::uint64_t seq = events_.size();
    event_seq_.emplace(e.hash, seq);
    events_.push_back(e);
    return seq;
  }
  std::uint64_t append_clock_event(const domain::LocalClockEvent& c) override {
    const std::uint64_t seq = next_clock_seq_++;  // monotonic; never reused after a prune
    clock_seq_.emplace(c.hash, seq);
    for (const auto& ref : c.referenced_events) referencing_.emplace(ref.hash, seq);
    chain_tip_seq_[c.chain] = seq;  // this chain's newest (seqs are ascending)
    chain_seqs_[c.chain].push_back(seq);
    clock_events_.emplace(seq, c);
    return seq;
  }
  void update_clock_event(const domain::LocalClockEvent& c) override {
    auto it = clock_seq_.find(c.hash);
    if (it != clock_seq_.end()) clock_events_[it->second] = c;
  }
  void put_neighbor(const domain::Neighbor& n) override { neighbors_[n.node_id] = n; }
  void put_route(domain::NodeId dst, domain::NodeId next_hop) override { routes_[dst] = next_hop; }
  void put_timed_routes(const domain::TimedRouteTable& t) override { timed_routes_ = t; }
  void prune_chain(std::uint32_t chain, std::size_t keep) override {
    auto it = chain_seqs_.find(chain);
    if (it == chain_seqs_.end() || keep == 0) return;
    while (it->second.size() > keep) {
      const std::uint64_t seq = it->second.front();
      it->second.pop_front();
      auto ce = clock_events_.find(seq);
      if (ce == clock_events_.end()) continue;
      clock_seq_.erase(ce->second.hash);
      for (const auto& ref : ce->second.referenced_events) {  // drop this seq from the reverse index
        auto range = referencing_.equal_range(ref.hash);
        for (auto r = range.first; r != range.second;)
          r = (r->second == seq) ? referencing_.erase(r) : std::next(r);
      }
      clock_events_.erase(ce);
    }
  }

  std::optional<domain::Event> event_by_hash(const domain::EventHash& h) const override {
    auto it = event_seq_.find(h);
    if (it == event_seq_.end()) return std::nullopt;
    return events_[it->second];
  }
  domain::Event event_by_seq(std::uint64_t seq) const override { return events_.at(seq); }
  std::optional<domain::LocalClockEvent> clock_event_by_hash(
      const domain::EventHash& h) const override {
    auto it = clock_seq_.find(h);
    if (it == clock_seq_.end()) return std::nullopt;
    return clock_events_.at(it->second);
  }
  domain::LocalClockEvent clock_event_by_seq(std::uint64_t seq) const override {
    return clock_events_.at(seq);
  }
  std::optional<std::uint64_t> clock_event_seq(const domain::EventHash& h) const override {
    auto it = clock_seq_.find(h);
    if (it == clock_seq_.end()) return std::nullopt;
    return it->second;
  }
  std::vector<std::uint64_t> clock_events_referencing(const domain::EventHash& h) const override {
    std::vector<std::uint64_t> out;
    auto range = referencing_.equal_range(h);
    for (auto it = range.first; it != range.second; ++it) out.push_back(it->second);
    std::sort(out.begin(), out.end());  // ascending seq, matching the LMDB DUPSORT order
    return out;
  }
  std::optional<domain::LocalClockEvent> latest_clock_event() const override {
    if (clock_events_.empty()) return std::nullopt;
    return clock_events_.rbegin()->second;  // highest live seq
  }
  std::optional<domain::LocalClockEvent> latest_clock_event(std::uint32_t chain) const override {
    auto it = chain_tip_seq_.find(chain);
    if (it == chain_tip_seq_.end()) return std::nullopt;
    return clock_events_.at(it->second);
  }
  std::size_t event_count() const override { return events_.size(); }
  std::size_t clock_event_count() const override { return clock_events_.size(); }
  std::vector<domain::LocalClockEvent> load_clock_events() const override {
    std::vector<domain::LocalClockEvent> out;
    out.reserve(clock_events_.size());
    for (const auto& [seq, c] : clock_events_) out.push_back(c);  // ascending seq order
    return out;
  }

  std::map<domain::NodeId, domain::Neighbor> load_neighbors() const override { return neighbors_; }
  std::map<domain::NodeId, domain::NodeId> load_routes() const override { return routes_; }
  domain::TimedRouteTable load_timed_routes() const override { return timed_routes_; }

  void clear() override {
    events_.clear();
    clock_events_.clear();
    next_clock_seq_ = 0;
    event_seq_.clear();
    clock_seq_.clear();
    referencing_.clear();
    chain_tip_seq_.clear();
    chain_seqs_.clear();
    neighbors_.clear();
    routes_.clear();
    timed_routes_.clear();
  }

 private:
  std::vector<domain::Event> events_;
  std::map<std::uint64_t, domain::LocalClockEvent> clock_events_;  // seq -> event (sparse after prune)
  std::uint64_t next_clock_seq_ = 0;
  std::map<domain::EventHash, std::uint64_t> event_seq_;
  std::map<domain::EventHash, std::uint64_t> clock_seq_;
  std::multimap<domain::EventHash, std::uint64_t> referencing_;
  std::map<std::uint32_t, std::uint64_t> chain_tip_seq_;               // chain/level -> newest seq
  std::map<std::uint32_t, std::deque<std::uint64_t>> chain_seqs_;      // chain -> live seqs, oldest first
  std::map<domain::NodeId, domain::Neighbor> neighbors_;
  std::map<domain::NodeId, domain::NodeId> routes_;
  domain::TimedRouteTable timed_routes_;
};

}  // namespace loti::sim
