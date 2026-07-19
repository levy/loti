// Telemetry port, production adapter — logs protocol activity to stderr.
//
// The production counterpart of the simulation's signal/statistic recorders: one
// human-readable line per notable event. A metrics/Prometheus sink can replace or
// wrap this later; for now it is what makes a running lotid observable.
#pragma once

#include <cstddef>
#include <cstdio>
#include <ctime>
#include <string>

#include "domain/types.hpp"
#include "ports/telemetry.hpp"

namespace loti::os {

class LogTelemetry final : public ports::Telemetry {
 public:
  explicit LogTelemetry(bool verbose = false) : verbose_(verbose) {}

  void on_event_created(const domain::Event& e) override {
    log("event created         %s (%zu bytes content)", hex(e.hash).c_str(), e.data.size());
  }
  void on_clock_event_created(const domain::ClockEvent& c) override {
    if (verbose_) log("clock event created   %s", hex(c.hash).c_str());
  }

  void on_chain_discovery_started(const domain::EventChainDiscovery& d) override {
    log("chain  discovery START %s", hex(d.event.hash).c_str());
  }
  void on_chain_discovery_completed(const domain::EventChainDiscovery& d) override {
    log("chain  discovery DONE  %s (%zu clock events)", hex(d.event.hash).c_str(),
        d.chain.lower_bound.size() + d.chain.upper_bound.size());
  }
  void on_chain_discovery_aborted(const domain::EventChainDiscovery& d) override {
    log("chain  discovery ABORT %s", hex(d.event.hash).c_str());
  }

  void on_bounds_discovery_started(const domain::EventBoundsDiscovery& d) override {
    log("bounds discovery START %s", hex(d.event.hash).c_str());
  }
  void on_bounds_discovery_completed(const domain::EventBoundsDiscovery& d) override {
    log("bounds discovery DONE  %s  [%lld, %lld] ns", hex(d.event.hash).c_str(),
        static_cast<long long>(d.lower_bound), static_cast<long long>(d.upper_bound));
  }
  void on_bounds_discovery_aborted(const domain::EventBoundsDiscovery& d) override {
    log("bounds discovery ABORT %s", hex(d.event.hash).c_str());
  }

  void on_order_discovery_started(const domain::EventOrderDiscovery& d) override {
    log("order  discovery START %s / %s", hex(d.event1.hash).c_str(), hex(d.event2.hash).c_str());
  }
  void on_order_discovery_completed(const domain::EventOrderDiscovery& d) override {
    log("order  discovery DONE  %s / %s  order=%d", hex(d.event1.hash).c_str(),
        hex(d.event2.hash).c_str(), static_cast<int>(d.order));
  }
  void on_order_discovery_aborted(const domain::EventOrderDiscovery& d) override {
    log("order  discovery ABORT %s / %s", hex(d.event1.hash).c_str(), hex(d.event2.hash).c_str());
  }

 private:
  template <class... Args>
  static void log(const char* fmt, Args... args) {
    // Prefix each line with a wall-clock timestamp so a long-running daemon's stderr is
    // diagnosable even when not captured by a timestamping supervisor (journald etc.).
    timespec ts{};
    ::clock_gettime(CLOCK_REALTIME, &ts);
    tm tmv{};
    ::localtime_r(&ts.tv_sec, &tmv);
    char when[24];
    std::strftime(when, sizeof(when), "%Y-%m-%d %H:%M:%S", &tmv);
    std::fprintf(stderr, "%s.%03ld [lotid] ", when, ts.tv_nsec / 1'000'000);
    std::fprintf(stderr, fmt, args...);
    std::fprintf(stderr, "\n");
  }
  static std::string hex(const domain::EventHash& h) {
    static const char* d = "0123456789abcdef";
    std::string s;
    for (std::size_t i = 0; i < h.size() && i < 4; ++i) {
      s.push_back(d[h[i] >> 4]);
      s.push_back(d[h[i] & 0xF]);
    }
    return s;
  }

  bool verbose_;
};

}  // namespace loti::os
