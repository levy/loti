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

#include <map>
#include <vector>

#include "domain/types.hpp"

namespace loti::routing {

// Per-hop context a router may consult. It grows as later parts land (time range,
// request/response leg, visited set, accumulated looseness `g`, hop limit); Part 1
// needs none of it, but the seam carries it from the start so the signature is stable.
struct RouteContext {};

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

}  // namespace loti::routing
