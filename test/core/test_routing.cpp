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
