// Telemetry port — the fixed set of hook points the core calls so a runtime can
// record statistics. One set of hooks, two sinks: the simulation translates them
// into OMNeT++ signals + result filters; production into logs/metrics.
//
// Methods are non-pure no-ops so a runtime overrides only what it cares about,
// and NoopTelemetry is just `struct NoopTelemetry : Telemetry {};`.
#pragma once

#include "domain/types.hpp"

namespace loti::ports {

class Telemetry {
 public:
  virtual ~Telemetry() = default;

  virtual void on_event_created(const domain::Event&) {}
  virtual void on_clock_event_created(const domain::ClockEvent&) {}

  virtual void on_chain_discovery_started(const domain::EventChainDiscovery&) {}
  virtual void on_chain_discovery_completed(const domain::EventChainDiscovery&) {}
  virtual void on_chain_discovery_aborted(const domain::EventChainDiscovery&) {}

  virtual void on_bounds_discovery_started(const domain::EventBoundsDiscovery&) {}
  virtual void on_bounds_discovery_completed(const domain::EventBoundsDiscovery&) {}
  virtual void on_bounds_discovery_aborted(const domain::EventBoundsDiscovery&) {}

  virtual void on_order_discovery_started(const domain::EventOrderDiscovery&) {}
  virtual void on_order_discovery_completed(const domain::EventOrderDiscovery&) {}
  virtual void on_order_discovery_aborted(const domain::EventOrderDiscovery&) {}
};

struct NoopTelemetry : Telemetry {};

}  // namespace loti::ports
