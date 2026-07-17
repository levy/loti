// Stage-0 smoke test: proves loti-core builds, links, and is reachable from a
// test binary that has no OMNeT++/INET/OS dependency. Replaced by real protocol
// tests as the core modules land.
#include "doctest.h"

#include "loti_core.hpp"

TEST_CASE("loti-core builds, links, and identifies itself") {
  CHECK(loti::core::library_id() == "loti-core");
  CHECK(loti::core::protocol_version == 1);
}
