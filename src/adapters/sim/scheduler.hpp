// Scheduler port, simulation adapter — arms one-shot timers as OMNeT++
// self-messages.
//
// `scheduleAt`/`cancelEvent` are protected members of the hosting cSimpleModule,
// so the module injects them as callbacks at construction. Each core timer is
// backed by one cMessage; the module's handleMessage routes a fired timer message
// back here via fire(). Timers are one-shot (the core re-arms by calling after()
// again), matching the harness FakeScheduler in test/harness/world.hpp.
#pragma once

#include <functional>
#include <map>
#include <utility>

#include <omnetpp.h>

#include "ports/scheduler.hpp"

namespace loti::sim {

class SimScheduler final : public ports::Scheduler {
 public:
  using ScheduleFn = std::function<void(omnetpp::cMessage*, omnetpp::simtime_t)>;
  using CancelFn = std::function<void(omnetpp::cMessage*)>;

  SimScheduler(ScheduleFn schedule, CancelFn cancel)
      : schedule_(std::move(schedule)), cancel_(std::move(cancel)) {}

  ~SimScheduler() override {
    for (auto& [id, timer] : timers_) {
      cancel_(timer.msg);
      delete timer.msg;
    }
  }

  ports::TimerId after(domain::Duration delay, std::function<void()> callback) override {
    const auto id = ++last_id_;
    auto* msg = new omnetpp::cMessage("loti-timer");
    by_msg_[msg] = id;
    timers_[id] = Timer{msg, std::move(callback)};
    schedule_(msg, omnetpp::simTime() + omnetpp::SimTime::fromRaw(delay));
    return id;
  }

  void cancel(ports::TimerId id) override {
    auto it = timers_.find(id);
    if (it == timers_.end()) return;
    cancel_(it->second.msg);
    by_msg_.erase(it->second.msg);
    delete it->second.msg;
    timers_.erase(it);
  }

  // Is this message one of ours? (asked by the host's handleMessage)
  [[nodiscard]] bool owns(omnetpp::cMessage* msg) const { return by_msg_.count(msg) != 0; }

  // Run the callback for a fired timer message and retire it (one-shot).
  void fire(omnetpp::cMessage* msg) {
    auto bit = by_msg_.find(msg);
    if (bit == by_msg_.end()) return;
    const auto id = bit->second;
    auto callback = std::move(timers_[id].cb);
    by_msg_.erase(bit);
    timers_.erase(id);
    delete msg;  // delivered self-message is consumed
    callback();  // may schedule a fresh timer
  }

 private:
  struct Timer {
    omnetpp::cMessage* msg = nullptr;
    std::function<void()> cb;
  };

  ScheduleFn schedule_;
  CancelFn cancel_;
  std::map<ports::TimerId, Timer> timers_;
  std::map<omnetpp::cMessage*, ports::TimerId> by_msg_;
  ports::TimerId last_id_ = 0;
};

}  // namespace loti::sim
