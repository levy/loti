// Scheduler port — arms one-shot timers. The core expresses all waiting by
// scheduling a callback (never by blocking). The simulation maps this onto
// scheduleAt()/self-messages; production onto a reactor/timerfd.
#pragma once

#include <cstdint>
#include <functional>

#include "domain/types.hpp"

namespace loti::ports {

using TimerId = std::uint64_t;

class Scheduler {
 public:
  virtual ~Scheduler() = default;

  // Invoke `callback` once, `delay` ticks from now. Returns a handle for cancel().
  virtual TimerId after(domain::Duration delay, std::function<void()> callback) = 0;

  // Cancel a pending timer (no-op if already fired or unknown).
  virtual void cancel(TimerId id) = 0;
};

}  // namespace loti::ports
