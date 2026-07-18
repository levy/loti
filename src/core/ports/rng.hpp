// Rng port — supplies the random salt mixed into every event/clock event.
// The simulation uses the seeded OMNeT++ RNG (so runs replay exactly);
// production uses a CSPRNG (salts and keys are security-critical). The core
// just asks for a salt and does not know or care which.
#pragma once

#include "domain/types.hpp"

namespace loti::ports {

class Rng {
 public:
  virtual ~Rng() = default;
  virtual domain::Salt next_salt() = 0;
};

}  // namespace loti::ports
