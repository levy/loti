// The imperative shell: a single-threaded epoll reactor that drives the core.
//
// It owns the only loop in the daemon. Each iteration fires every due timer (which
// is how the core "waits" — never by blocking), then epoll_wait()s until the next
// timer or a watched fd (the UDP socket, stdin) becomes readable, then dispatches.
// ReactorScheduler is the core's Scheduler port expressed against it. Timers use
// CLOCK_REALTIME nanoseconds, the same clock as WallClock, so due times computed by
// the core (clock.now() + delay) line up with the reactor's comparisons.
#pragma once

#include <sys/epoll.h>
#include <unistd.h>

#include <cerrno>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <functional>
#include <map>
#include <stdexcept>
#include <utility>
#include <vector>

#include "ports/clock.hpp"
#include "ports/scheduler.hpp"

namespace loti::os {

class Reactor {
 public:
  Reactor() {
    epoll_fd_ = ::epoll_create1(0);
    if (epoll_fd_ < 0) throw std::runtime_error("epoll_create1 failed");
  }
  ~Reactor() {
    if (epoll_fd_ >= 0) ::close(epoll_fd_);
  }
  Reactor(const Reactor&) = delete;
  Reactor& operator=(const Reactor&) = delete;

  ports::TimerId add_timer(domain::Timestamp due_ns, std::function<void()> cb) {
    const auto id = ++next_timer_id_;
    timers_.emplace(due_ns, std::make_pair(id, std::move(cb)));
    timer_due_[id] = due_ns;
    return id;
  }

  void cancel_timer(ports::TimerId id) {
    auto it = timer_due_.find(id);
    if (it == timer_due_.end()) return;
    auto range = timers_.equal_range(it->second);
    for (auto j = range.first; j != range.second; ++j)
      if (j->second.first == id) {
        timers_.erase(j);
        break;
      }
    timer_due_.erase(it);
  }

  void add_reader(int fd, std::function<void()> on_readable) {
    readers_[fd] = std::move(on_readable);
    epoll_event ev{};
    ev.events = EPOLLIN;
    ev.data.fd = fd;
    ::epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, fd, &ev);
  }

  void remove_reader(int fd) {
    readers_.erase(fd);
    ::epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd, nullptr);
  }

  void stop() { running_ = false; }

  void run() {
    running_ = true;
    std::vector<epoll_event> events(16);
    while (running_) {
      fire_due_timers();
      if (!running_) break;
      const int timeout_ms = next_timeout_ms();
      const int n = ::epoll_wait(epoll_fd_, events.data(), static_cast<int>(events.size()), timeout_ms);
      if (n < 0) {
        if (errno == EINTR) continue;
        break;
      }
      for (int i = 0; i < n; ++i) {
        auto it = readers_.find(events[i].data.fd);
        if (it != readers_.end()) safe_call(it->second);
      }
    }
  }

 private:
  // Run one reactor callback, containing any exception it throws. A timer or fd handler
  // that fails (a bad packet, a transient I/O error) must never unwind out of the event
  // loop and terminate the daemon — it is logged and dropped (hardening plan, Phase 1.1).
  static void safe_call(const std::function<void()>& cb) {
    try {
      cb();
    } catch (const std::exception& e) {
      std::fprintf(stderr, "[lotid] reactor: dropped a failing callback: %s\n", e.what());
    } catch (...) {
      std::fprintf(stderr, "[lotid] reactor: dropped a failing callback (unknown)\n");
    }
  }

  static domain::Timestamp now_ns() {
    timespec ts{};
    clock_gettime(CLOCK_REALTIME, &ts);
    return static_cast<domain::Timestamp>(ts.tv_sec) * 1'000'000'000 + ts.tv_nsec;
  }

  void fire_due_timers() {
    for (;;) {
      auto it = timers_.begin();
      if (it == timers_.end() || it->first > now_ns()) break;
      auto cb = std::move(it->second.second);
      timer_due_.erase(it->second.first);
      timers_.erase(it);
      safe_call(cb);  // one-shot; may arm new timers
      if (!running_) break;
    }
  }

  [[nodiscard]] int next_timeout_ms() const {
    if (timers_.empty()) return -1;  // block until an fd is ready
    const auto diff = timers_.begin()->first - now_ns();
    if (diff <= 0) return 0;
    const auto ms = (diff + 999'999) / 1'000'000;  // ceil to ms
    return ms > INT_MAX ? INT_MAX : static_cast<int>(ms);
  }

  int epoll_fd_ = -1;
  bool running_ = false;
  ports::TimerId next_timer_id_ = 0;
  std::multimap<domain::Timestamp, std::pair<ports::TimerId, std::function<void()>>> timers_;
  std::map<ports::TimerId, domain::Timestamp> timer_due_;
  std::map<int, std::function<void()>> readers_;
};

// Scheduler port backed by the reactor's timer queue.
class ReactorScheduler final : public ports::Scheduler {
 public:
  ReactorScheduler(Reactor& reactor, ports::Clock& clock) : reactor_(reactor), clock_(clock) {}

  ports::TimerId after(domain::Duration delay, std::function<void()> callback) override {
    return reactor_.add_timer(clock_.now() + delay, std::move(callback));
  }
  void cancel(ports::TimerId id) override { reactor_.cancel_timer(id); }

 private:
  Reactor& reactor_;
  ports::Clock& clock_;
};

}  // namespace loti::os
