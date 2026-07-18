// Telemetry port, simulation adapter — turns the core's telemetry hooks into
// OMNeT++ signals for the @statistic recorders declared in Daemon.ned.
//
// The core computes the quantities (byte sizes via core/hash, discovery times and
// intervals from the discovery records' raw-tick timestamps); this adapter emits
// them as plain scalar signals. The clock/event "created" signals carry the object
// byte size, so a single signal feeds both a `count` recorder and a `sum(...)`
// file-length recorder — reproducing the original statistics without needing
// OMNeT++ message objects or result filters.
#pragma once

#include <omnetpp.h>

#include "domain/types.hpp"
#include "hash/hashing.hpp"
#include "ports/telemetry.hpp"

namespace loti::sim {

class SimTelemetry final : public ports::Telemetry {
 public:
  explicit SimTelemetry(omnetpp::cComponent* owner)
      : owner_(owner),
        clock_event_created_(reg("clockEventCreated")),
        event_created_(reg("eventCreated")),
        chain_started_(reg("eventChainDiscoveryStarted")),
        chain_aborted_(reg("eventChainDiscoveryAborted")),
        chain_completed_(reg("eventChainDiscoveryCompleted")),
        chain_time_(reg("eventChainDiscoveryTime")),
        chain_length_(reg("eventChainDiscoveryLength")),
        chain_interval_(reg("eventChainDiscoveryInterval")),
        bounds_started_(reg("eventBoundsDiscoveryStarted")),
        bounds_aborted_(reg("eventBoundsDiscoveryAborted")),
        bounds_completed_(reg("eventBoundsDiscoveryCompleted")),
        bounds_time_(reg("eventBoundsDiscoveryTime")),
        bounds_interval_(reg("eventBoundsDiscoveryInterval")),
        order_started_(reg("eventOrderDiscoveryStarted")),
        order_aborted_(reg("eventOrderDiscoveryAborted")),
        order_completed_(reg("eventOrderDiscoveryCompleted")),
        order_time_(reg("eventOrderDiscoveryTime")),
        order_result_(reg("eventOrderDiscoveryOrder")) {}

  void on_clock_event_created(const domain::ClockEvent& c) override {
    owner_->emit(clock_event_created_, static_cast<omnetpp::intval_t>(hash::clock_event_size_bytes(c)));
  }
  void on_event_created(const domain::Event& e) override {
    owner_->emit(event_created_, static_cast<omnetpp::intval_t>(hash::event_size_bytes(e)));
  }

  void on_chain_discovery_started(const domain::EventChainDiscovery&) override {
    owner_->emit(chain_started_, kOne);
  }
  void on_chain_discovery_aborted(const domain::EventChainDiscovery&) override {
    owner_->emit(chain_aborted_, kOne);
  }
  void on_chain_discovery_completed(const domain::EventChainDiscovery& d) override {
    owner_->emit(chain_completed_, kOne);
    owner_->emit(chain_time_, ticks(d.end_time - d.start_time));
    const auto length = d.chain.lower_bound.size() + d.chain.upper_bound.size() + 1;
    owner_->emit(chain_length_, static_cast<omnetpp::intval_t>(length));
    if (!d.chain.lower_bound.empty() && !d.chain.upper_bound.empty())
      owner_->emit(chain_interval_,
                   ticks(d.chain.upper_bound.back().timestamp - d.chain.lower_bound.front().timestamp));
  }

  void on_bounds_discovery_started(const domain::EventBoundsDiscovery&) override {
    owner_->emit(bounds_started_, kOne);
  }
  void on_bounds_discovery_aborted(const domain::EventBoundsDiscovery&) override {
    owner_->emit(bounds_aborted_, kOne);
  }
  void on_bounds_discovery_completed(const domain::EventBoundsDiscovery& d) override {
    owner_->emit(bounds_completed_, kOne);
    owner_->emit(bounds_time_, ticks(d.end_time - d.start_time));
    owner_->emit(bounds_interval_, ticks(d.upper_bound - d.lower_bound));
  }

  void on_order_discovery_started(const domain::EventOrderDiscovery&) override {
    owner_->emit(order_started_, kOne);
  }
  void on_order_discovery_aborted(const domain::EventOrderDiscovery&) override {
    owner_->emit(order_aborted_, kOne);
  }
  void on_order_discovery_completed(const domain::EventOrderDiscovery& d) override {
    owner_->emit(order_completed_, kOne);
    owner_->emit(order_time_, ticks(d.end_time - d.start_time));
    owner_->emit(order_result_, static_cast<omnetpp::intval_t>(d.order));
  }

 private:
  static omnetpp::simsignal_t reg(const char* name) {
    return omnetpp::cComponent::registerSignal(name);
  }
  static omnetpp::SimTime ticks(domain::Duration d) { return omnetpp::SimTime::fromRaw(d); }

  static constexpr omnetpp::intval_t kOne = 1;

  omnetpp::cComponent* owner_;
  omnetpp::simsignal_t clock_event_created_, event_created_;
  omnetpp::simsignal_t chain_started_, chain_aborted_, chain_completed_, chain_time_, chain_length_,
      chain_interval_;
  omnetpp::simsignal_t bounds_started_, bounds_aborted_, bounds_completed_, bounds_time_,
      bounds_interval_;
  omnetpp::simsignal_t order_started_, order_aborted_, order_completed_, order_time_, order_result_;
};

}  // namespace loti::sim
