// loti-core — the discovery forwarding policy (the "where do we forward" seam).
//
// A discovery reconstructs an event chain over the overlay as it was during the
// target event's time range, so forwarding is a pluggable, time-dependent decision:
// given a destination (multi-hop away) and a time range, produce the ordered
// candidate next-hop neighbors to forward to. The Node calls this instead of hard-
// wiring a single static next hop, and validation keeps every returned chain sound,
// so a router may be as heuristic/lossy as it likes (doc/dynamic-discovery.md,
// plan/pending/scalable-pluggable-discovery.md).
//
// This file introduces the seam (Part 1). StaticShortestPathRouter reproduces the
// historical single-shortest-path behavior exactly and is the width-1 anchor; the
// time-dependent and probabilistic routers are added in later parts.
#pragma once

#include <algorithm>
#include <cstddef>
#include <map>
#include <set>
#include <utility>
#include <vector>

#include "domain/types.hpp"
#include "ports/rng.hpp"
#include "ports/store.hpp"

namespace loti::routing {

// Per-hop context a router may consult. `range` is the discovery's estimated time window,
// which the time-dependent routers use to select the overlay as it was then. (It grows as
// later parts land: request/response leg, accumulated looseness `g`, etc.)
struct RouteContext {
  domain::TimeRange range;
};

// A pluggable forwarding policy. Returns the ordered/weighted candidate next-hop
// neighbor ids to forward a discovery toward `destination` (best first). An empty
// result means "no route from here" — the discovery makes no further progress on
// this branch (and later expires at the originator).
class DiscoveryRouter {
 public:
  virtual ~DiscoveryRouter() = default;
  [[nodiscard]] virtual std::vector<domain::NodeId> next_hops(
      domain::NodeId destination, const RouteContext& ctx) const = 0;
};

// Today's behavior, wrapped: the single next hop from the static destination→next-hop
// overlay table, if that next hop is a known neighbor. Time-agnostic; k = 1. This is
// the seam's width-1 anchor and the degenerate case the later routers generalize.
class StaticShortestPathRouter final : public DiscoveryRouter {
 public:
  StaticShortestPathRouter(const std::map<domain::NodeId, domain::Neighbor>& neighbors,
                           const std::map<domain::NodeId, domain::NodeId>& routes)
      : neighbors_(neighbors), routes_(routes) {}

  [[nodiscard]] std::vector<domain::NodeId> next_hops(
      domain::NodeId destination, const RouteContext&) const override {
    auto it = routes_.find(destination);
    if (it == routes_.end()) return {};
    if (neighbors_.find(it->second) == neighbors_.end()) return {};
    return {it->second};
  }

 private:
  const std::map<domain::NodeId, domain::Neighbor>& neighbors_;
  const std::map<domain::NodeId, domain::NodeId>& routes_;
};

// The time-dependent neighbor history, read straight from the DAG. The candidate next hops
// are exactly the neighbors this node cross-linked with during `ctx.range` — the set the
// chain-building `extend_*_for_neighbor` primitives can actually close through then. This is
// **undirected**: local 1-hop cross-links cannot say which neighbor lies toward a far
// destination (that is the routing table's job), so it returns *all* such neighbors, which the
// flood (Part 5) explores. It is the always-available, always-time-correct fallback when no
// routing table is filled.
//
// Source: scan this node's own live clock events and collect the non-self creators referenced
// (forward cross-links to neighbor tips) or referencing (reverse cross-links learned from
// neighbor notifications) them within the window. Only ids still in `neighbors_` are returned,
// so we can actually send to them — old neighbors are retained there (Decision 1), so a past
// neighbor that has since gone stays a candidate. O(retained clock events) per call; a compact
// time-indexed adjacency is a later optimization.
class NeighborHistoryRouter final : public DiscoveryRouter {
 public:
  NeighborHistoryRouter(domain::NodeId self, const ports::Store& store,
                        const std::map<domain::NodeId, domain::Neighbor>& neighbors)
      : self_(self), store_(store), neighbors_(neighbors) {}

  [[nodiscard]] std::vector<domain::NodeId> next_hops(
      domain::NodeId /*destination*/, const RouteContext& ctx) const override {
    std::vector<domain::NodeId> out;
    std::set<domain::NodeId> seen;
    const auto collect = [&](const std::vector<domain::EventReference>& refs) {
      for (const auto& r : refs)
        if (r.creator != self_ && neighbors_.find(r.creator) != neighbors_.end() &&
            seen.insert(r.creator).second)
          out.push_back(r.creator);
    };
    for (const auto& ce : store_.load_clock_events()) {
      if (!ctx.range.contains(ce.timestamp)) continue;
      collect(ce.referenced_events);
      collect(ce.referencing_events);
    }
    return out;
  }

 private:
  domain::NodeId self_;
  const ports::Store& store_;
  const std::map<domain::NodeId, domain::Neighbor>& neighbors_;
};

// A single time-scoped routing hint: during `validity`, these are the ordered next hops
// (shortest first) toward some far destination. Not derivable from the DAG (multi-hop direction
// is a global property); filled by a routing protocol or static config — out of scope here. The
// router only consumes it.
struct TimedRoute {
  domain::TimeRange validity;
  std::vector<domain::NodeId> next_hops;
};

// destination → the time-scoped routes learned toward it.
using TimedRouteTable = std::map<domain::NodeId, std::vector<TimedRoute>>;

// The directional, time-dependent router. On a **hit** — a route toward `destination` whose
// validity overlaps `ctx.range` — it returns those next hops (shortest first) **intersected with
// the neighbor history**, so every hop is one this node actually cross-linked with in the window
// (otherwise the chain could not close through it). On a **miss**, or when no directed hop is
// cross-linked, it degrades to the fallback — the undirected NeighborHistory flood. The table is
// "either filled or not" (routing protocols come later); the fallback covers every gap.
class RoutingTableRouter final : public DiscoveryRouter {
 public:
  RoutingTableRouter(const TimedRouteTable& table, const DiscoveryRouter& fallback)
      : table_(table), fallback_(fallback) {}

  [[nodiscard]] std::vector<domain::NodeId> next_hops(
      domain::NodeId destination, const RouteContext& ctx) const override {
    const std::vector<domain::NodeId> crosslinked = fallback_.next_hops(destination, ctx);
    auto it = table_.find(destination);
    if (it == table_.end()) return crosslinked;  // no route learned → undirected fallback
    std::vector<domain::NodeId> directed;
    std::set<domain::NodeId> added;
    for (const auto& route : it->second) {
      if (!overlaps(route.validity, ctx.range)) continue;
      for (domain::NodeId hop : route.next_hops)
        if (std::find(crosslinked.begin(), crosslinked.end(), hop) != crosslinked.end() &&
            added.insert(hop).second)
          directed.push_back(hop);
    }
    return directed.empty() ? crosslinked : directed;  // no usable directed hop → fallback
  }

 private:
  [[nodiscard]] static bool overlaps(const domain::TimeRange& a, const domain::TimeRange& b) {
    return a.lo <= b.hi && b.lo <= a.hi;
  }
  const TimedRouteTable& table_;
  const DiscoveryRouter& fallback_;
};

// The scalability lid on the flood: caps the fan-out to width `k`. With `k == 0` (or `k >=` the
// candidate count) it is a pass-through — the full flood, or the width-1 anchor when the inner
// router already returns a single directed hop. Otherwise it samples `k` candidates uniformly at
// random via the Rng port (seeded ⇒ deterministic in tests), which spreads load and diversifies
// paths so one node's churn does not kill every branch. (Score-weighted sampling ∝ exp(−f/τ) is
// the beam layer's job — it needs a per-candidate `g`/`h`, deferred beyond this plan.)
class ProbabilisticRouter final : public DiscoveryRouter {
 public:
  ProbabilisticRouter(const DiscoveryRouter& inner, ports::Rng& rng, std::size_t k)
      : inner_(inner), rng_(rng), k_(k) {}

  [[nodiscard]] std::vector<domain::NodeId> next_hops(
      domain::NodeId destination, const RouteContext& ctx) const override {
    std::vector<domain::NodeId> cand = inner_.next_hops(destination, ctx);
    if (k_ == 0 || cand.size() <= k_) return cand;
    for (std::size_t i = 0; i < k_; ++i) {  // partial Fisher–Yates: k without replacement
      const std::size_t j = i + static_cast<std::size_t>(rng_.next_salt() % (cand.size() - i));
      std::swap(cand[i], cand[j]);
    }
    cand.resize(k_);
    return cand;
  }

 private:
  const DiscoveryRouter& inner_;
  ports::Rng& rng_;
  std::size_t k_;
};

}  // namespace loti::routing
