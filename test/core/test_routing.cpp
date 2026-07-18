// Unit tests for the pluggable discovery routers (routing/discovery_router.hpp): the
// forwarding policies the Node consults to decide where to send a discovery.
#include "doctest.h"

#include <map>
#include <set>

#include "harness/world.hpp"
#include "routing/discovery_router.hpp"

using namespace loti;

TEST_CASE("NeighborHistoryRouter returns the neighbors cross-linked within the window") {
  harness::World w;
  auto nodes = harness::build_path(w, 3);  // n1 — n2 — n3
  Node& n2 = *nodes[1];
  harness::gossip(w, nodes, 5);            // form cross-links across the line (timestamps 10,20,…)

  // n2's overlay neighbors are n1 and n3; give the router the same retained view.
  std::map<domain::NodeId, domain::Neighbor> neighbors{{1, domain::Neighbor{1, {}}},
                                                       {3, domain::Neighbor{3, {}}}};
  routing::NeighborHistoryRouter router(n2.id(), w.store_of(n2.id()), neighbors);

  SUBCASE("full range → both cross-linked neighbors (undirected)") {
    routing::RouteContext ctx;  // range defaults to all()
    const auto hops = router.next_hops(/*destination=*/1, ctx);
    const std::set<domain::NodeId> got(hops.begin(), hops.end());
    CHECK(got == std::set<domain::NodeId>{1, 3});
  }

  SUBCASE("a window before any cross-link existed → no candidates") {
    routing::RouteContext ctx;
    ctx.range = domain::TimeRange{-100, -1};  // all gossip timestamps are positive
    CHECK(router.next_hops(1, ctx).empty());
  }

  SUBCASE("a node that is no longer a known neighbor is not offered") {
    std::map<domain::NodeId, domain::Neighbor> only_n1{{1, domain::Neighbor{1, {}}}};
    routing::NeighborHistoryRouter r(n2.id(), w.store_of(n2.id()), only_n1);
    routing::RouteContext ctx;
    const auto hops = r.next_hops(1, ctx);
    CHECK(hops == std::vector<domain::NodeId>{1});  // n3 cross-linked but not a known neighbor
  }
}

TEST_CASE("RoutingTableRouter directs on a hit and falls back on a miss") {
  harness::World w;
  auto nodes = harness::build_path(w, 4);  // n1 — n2 — n3 — n4; n4 is >1 hop from n2
  Node& n2 = *nodes[1];
  harness::gossip(w, nodes, 6);            // n2 cross-links with n1 and n3

  std::map<domain::NodeId, domain::Neighbor> neighbors{{1, domain::Neighbor{1, {}}},
                                                       {3, domain::Neighbor{3, {}}}};
  routing::NeighborHistoryRouter history(n2.id(), w.store_of(n2.id()), neighbors);

  routing::TimedRouteTable table;
  table[4].push_back(routing::TimedRoute{domain::TimeRange::all(), {3}});  // toward n4, go via n3
  routing::RoutingTableRouter router(table, history);
  routing::RouteContext ctx;  // range = all()

  SUBCASE("hit → the directed next hop (which is also cross-linked)") {
    CHECK(router.next_hops(/*destination=*/4, ctx) == std::vector<domain::NodeId>{3});
  }
  SUBCASE("no route learned → undirected neighbor-history fallback") {
    const auto hops = router.next_hops(/*destination=*/1, ctx);
    const std::set<domain::NodeId> got(hops.begin(), hops.end());
    CHECK(got == std::set<domain::NodeId>{1, 3});
  }
  SUBCASE("directed hop not cross-linked in-window → fallback") {
    routing::TimedRouteTable bad;
    bad[4].push_back(routing::TimedRoute{domain::TimeRange::all(), {99}});  // 99 never cross-linked
    routing::RoutingTableRouter r(bad, history);
    const auto hops = r.next_hops(4, ctx);
    const std::set<domain::NodeId> got(hops.begin(), hops.end());
    CHECK(got == std::set<domain::NodeId>{1, 3});
  }
  SUBCASE("route validity outside the query window → identical to pure fallback") {
    routing::TimedRouteTable future;
    future[4].push_back(routing::TimedRoute{domain::TimeRange{100000, 200000}, {3}});
    routing::RoutingTableRouter r(future, history);
    routing::RouteContext narrow;
    narrow.range = domain::TimeRange{1, 100};
    CHECK(r.next_hops(4, narrow) == history.next_hops(4, narrow));
  }
}
