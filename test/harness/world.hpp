// In-process multi-node test harness (plan Tier 2): a deterministic virtual-time
// world that hosts real Node instances behind fake ports and routes datagrams
// between them — no OMNeT++, no sockets. This is what verifies the protocol runs
// on loti-core end to end.
#pragma once

#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <utility>
#include <vector>

#include "adapters/sim/in_memory_store.hpp"
#include "node.hpp"
#include "ports/clock.hpp"
#include "ports/rng.hpp"
#include "ports/scheduler.hpp"
#include "ports/signer.hpp"
#include "ports/store.hpp"
#include "ports/telemetry.hpp"
#include "ports/transport.hpp"

namespace loti::harness {

class FakeClock final : public ports::Clock {
 public:
  explicit FakeClock(const domain::Timestamp& now) : now_(now) {}
  domain::Timestamp now() const override { return now_; }

 private:
  const domain::Timestamp& now_;
};

// Fires timers whose due time has passed when fire_due() is called.
class FakeScheduler final : public ports::Scheduler {
 public:
  explicit FakeScheduler(const domain::Timestamp& now) : now_(now) {}

  ports::TimerId after(domain::Duration delay, std::function<void()> cb) override {
    const auto id = ++last_id_;
    timers_[id] = Timer{now_ + delay, std::move(cb)};
    return id;
  }
  void cancel(ports::TimerId id) override { timers_.erase(id); }

  void fire_due() {
    for (bool again = true; again;) {
      again = false;
      auto best = timers_.end();
      for (auto it = timers_.begin(); it != timers_.end(); ++it)
        if (it->second.due <= now_ && (best == timers_.end() || it->second.due < best->second.due))
          best = it;
      if (best != timers_.end()) {
        auto cb = best->second.cb;
        timers_.erase(best);
        cb();
        again = true;
      }
    }
  }

 private:
  struct Timer {
    domain::Timestamp due = 0;
    std::function<void()> cb;
  };
  const domain::Timestamp& now_;
  std::map<ports::TimerId, Timer> timers_;
  ports::TimerId last_id_ = 0;
};

class FakeTransport final : public ports::Transport {
 public:
  struct Msg {
    domain::NodeId to = 0;
    domain::Bytes bytes;
  };
  void send(domain::NodeId to, const domain::Bytes& bytes) override { queue.push_back({to, bytes}); }
  std::deque<Msg> queue;
};

// Deterministic xorshift64* — stands in for the seeded sim RNG.
class SeededRng final : public ports::Rng {
 public:
  explicit SeededRng(std::uint64_t seed) : state_(seed ? seed : 0x9E3779B97F4A7C15ull) {}
  domain::Salt next_salt() override {
    state_ ^= state_ >> 12;
    state_ ^= state_ << 25;
    state_ ^= state_ >> 27;
    return state_ * 0x2545F4914F6CDD1Dull;
  }

 private:
  std::uint64_t state_;
};

struct NullSigner final : ports::Signer {
  domain::Signature sign(const domain::Bytes&) override { return {}; }
  bool verify(const domain::Bytes&, const domain::Signature&, domain::NodeId) const override {
    return true;
  }
};

class World {
 public:
  World() = default;
  World(const World&) = delete;
  World& operator=(const World&) = delete;

  Node& add_node(domain::NodeId id, NodeConfig cfg) {
    stores_.push_back(std::make_unique<sim::InMemoryStore>());  // one DAG store per node
    node_stores_[id] = stores_.back().get();
    owned_.push_back(std::make_unique<Node>(id, ports(*stores_.back()), cfg));
    Node* p = owned_.back().get();
    nodes_[id] = p;
    return *p;
  }

  // The DAG store backing a node (for tests that assert on persisted state directly).
  [[nodiscard]] sim::InMemoryStore& store_of(domain::NodeId id) { return *node_stores_.at(id); }

  // Deliver queued datagrams until the network is quiet (each delivery may enqueue more).
  void pump() {
    while (!transport_.queue.empty()) {
      auto m = transport_.queue.front();
      transport_.queue.pop_front();
      if (auto it = nodes_.find(m.to); it != nodes_.end()) it->second->on_packet_received(m.bytes);
    }
  }

  [[nodiscard]] domain::Timestamp now() const noexcept { return now_; }
  void set_now(domain::Timestamp t) noexcept { now_ = t; }
  void advance(domain::Duration d) {
    now_ += d;
    scheduler_.fire_due();
    pump();
  }

 private:
  NodePorts ports(ports::Store& store) {
    return NodePorts{clock_, scheduler_, transport_, rng_, signer_, telemetry_, store};
  }

  domain::Timestamp now_ = 0;
  FakeClock clock_{now_};
  FakeScheduler scheduler_{now_};
  FakeTransport transport_;
  SeededRng rng_{0xC0FFEEULL};
  NullSigner signer_;
  ports::NoopTelemetry telemetry_;
  std::map<domain::NodeId, Node*> nodes_;
  std::map<domain::NodeId, sim::InMemoryStore*> node_stores_;
  std::vector<std::unique_ptr<sim::InMemoryStore>> stores_;
  std::vector<std::unique_ptr<Node>> owned_;
};

// ---- convenience builders -------------------------------------------------

// A path N1 — N2 — … — Nn with shortest-path overlay routes along the line.
inline std::vector<Node*> build_path(World& w, int n) {
  std::vector<Node*> nodes;
  for (int i = 0; i < n; ++i) nodes.push_back(&w.add_node(domain::NodeId(i + 1), NodeConfig{}));
  for (int i = 0; i + 1 < n; ++i) {
    nodes[i]->add_neighbor(nodes[i + 1]->id());
    nodes[i + 1]->add_neighbor(nodes[i]->id());
  }
  for (int src = 0; src < n; ++src)
    for (int dst = 0; dst < n; ++dst)
      if (src != dst) {
        const int step = dst > src ? src + 1 : src - 1;
        nodes[src]->learn_route(nodes[dst]->id(), nodes[step]->id());
      }
  return nodes;
}

// One gossip round per iteration: every node creates a clock event (with a fresh,
// increasing timestamp) and its notifications are delivered.
inline void gossip(World& w, const std::vector<Node*>& nodes, int rounds,
                   domain::Duration dt = 10) {
  for (int r = 0; r < rounds; ++r)
    for (Node* n : nodes) {
      w.set_now(w.now() + dt);
      n->create_clock_event();
      w.pump();
    }
}

// ---- recording callbacks --------------------------------------------------

struct RecordingChain final : ChainCallback {
  bool completed = false;
  bool aborted = false;
  domain::EventChain chain;
  void on_chain_completed(const domain::Event&, const domain::EventChain& c) override {
    completed = true;
    chain = c;
  }
  void on_chain_aborted(const domain::Event&) override { aborted = true; }
};

struct RecordingBounds final : BoundsCallback {
  bool completed = false;
  bool aborted = false;
  domain::Timestamp lower = 0;
  domain::Timestamp upper = 0;
  void on_bounds_completed(const domain::Event&, domain::Timestamp l, domain::Timestamp u) override {
    completed = true;
    lower = l;
    upper = u;
  }
  void on_bounds_aborted(const domain::Event&) override { aborted = true; }
};

struct RecordingOrder final : OrderCallback {
  bool completed = false;
  bool aborted = false;
  domain::Order order = domain::Order::undetermined;
  void on_order_completed(const domain::Event&, const domain::Event&, domain::Order o) override {
    completed = true;
    order = o;
  }
  void on_order_aborted(const domain::Event&, const domain::Event&) override { aborted = true; }
};

}  // namespace loti::harness
