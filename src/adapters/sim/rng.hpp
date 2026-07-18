// Rng port, simulation adapter — draws the event/clock-event salt from the
// hosting module's seeded OMNeT++ RNG (stream 0), so runs replay exactly.
//
// Byte-for-byte the original Daemon::generateSalt(): two independent 31-bit
// intuniform(0, 0x7FFFFFFF) draws combined high<<32 | low into a 64-bit salt.
#pragma once

#include <omnetpp.h>

#include "ports/rng.hpp"

namespace loti::sim {

class SimRng final : public ports::Rng {
 public:
  explicit SimRng(omnetpp::cComponent* owner) : owner_(owner) {}

  domain::Salt next_salt() override {
    omnetpp::cRNG* rng = owner_->getRNG(0);
    const domain::Salt high = static_cast<domain::Salt>(omnetpp::intuniform(rng, 0, 0x7FFFFFFF));
    const domain::Salt low = static_cast<domain::Salt>(omnetpp::intuniform(rng, 0, 0x7FFFFFFF));
    return (high << 32) | low;
  }

 private:
  omnetpp::cComponent* owner_;
};

}  // namespace loti::sim
