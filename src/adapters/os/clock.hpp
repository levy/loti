// Clock port, production adapter — reads the wall clock as nanoseconds since the
// Unix epoch.
//
// The core hashes a clock event's timestamp as its raw 64-bit tick, so in
// production a "tick" is one nanosecond of real time (CLOCK_REALTIME). This is
// what makes a proof's bounds meaningful as actual dates/times. The core neither
// knows nor cares about the unit — the sim maps the same field onto simTime().raw().
#pragma once

#include <ctime>

#include "ports/clock.hpp"

namespace loti::os {

class WallClock final : public ports::Clock {
 public:
  [[nodiscard]] domain::Timestamp now() const override {
    timespec ts{};
    clock_gettime(CLOCK_REALTIME, &ts);
    return static_cast<domain::Timestamp>(ts.tv_sec) * 1'000'000'000 + ts.tv_nsec;
  }
};

}  // namespace loti::os
