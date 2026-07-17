// Clock port, simulation adapter — reads OMNeT++ simulation time.
//
// The core hashes a clock event's timestamp as its raw 64-bit tick
// (see core/hash/hashing.cpp and the original Daemon.cc's
// `writeUint64Be(clockEvent.getTimestamp().raw())`), so `now()` returns
// `simTime().raw()` — the picosecond tick count — verbatim.
#pragma once

#include <omnetpp.h>

#include "ports/clock.hpp"

namespace loti::sim {

class SimClock final : public ports::Clock {
 public:
  [[nodiscard]] domain::Timestamp now() const override {
    return omnetpp::simTime().raw();
  }
};

}  // namespace loti::sim
