// The simulation's Store port: the whole DAG lives in RAM and dies with the run. It
// mirrors the LMDB store's dense-sequence semantics exactly (0-based, gap-free) so the
// Node behaves identically whether it runs on this or on the production store. Used by
// the OMNeT++ simulation and the in-process test harness (test/harness/world.hpp).
#pragma once

#include <algorithm>
#include <cstdint>
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
    const std::uint64_t seq = clock_events_.size();
    clock_seq_.emplace(c.hash, seq);
    for (const auto& ref : c.referenced_events) referencing_.emplace(ref.hash, seq);
    clock_events_.push_back(c);
    return seq;
  }
  void update_clock_event(const domain::LocalClockEvent& c) override {
    auto it = clock_seq_.find(c.hash);
    if (it != clock_seq_.end()) clock_events_[it->second] = c;
  }
  void put_neighbor(const domain::Neighbor& n) override { neighbors_[n.node_id] = n; }
  void put_route(domain::NodeId dst, domain::NodeId next_hop) override { routes_[dst] = next_hop; }

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
    return clock_events_[it->second];
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
    return clock_events_.back();
  }
  std::size_t event_count() const override { return events_.size(); }
  std::size_t clock_event_count() const override { return clock_events_.size(); }

  std::map<domain::NodeId, domain::Neighbor> load_neighbors() const override { return neighbors_; }
  std::map<domain::NodeId, domain::NodeId> load_routes() const override { return routes_; }

  void clear() override {
    events_.clear();
    clock_events_.clear();
    event_seq_.clear();
    clock_seq_.clear();
    referencing_.clear();
    neighbors_.clear();
    routes_.clear();
  }

 private:
  std::vector<domain::Event> events_;
  std::vector<domain::LocalClockEvent> clock_events_;
  std::map<domain::EventHash, std::uint64_t> event_seq_;
  std::map<domain::EventHash, std::uint64_t> clock_seq_;
  std::multimap<domain::EventHash, std::uint64_t> referencing_;
  std::map<domain::NodeId, domain::Neighbor> neighbors_;
  std::map<domain::NodeId, domain::NodeId> routes_;
};

}  // namespace loti::sim
