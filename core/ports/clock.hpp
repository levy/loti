// Clock port — reads the local clock. The simulation maps OMNeT++ simTime();
// production maps the wall clock. The core never reads time any other way.
#pragma once

#include "domain/types.hpp"

namespace loti::ports {

class Clock {
 public:
  virtual ~Clock() = default;
  [[nodiscard]] virtual domain::Timestamp now() const = 0;
};

}  // namespace loti::ports
