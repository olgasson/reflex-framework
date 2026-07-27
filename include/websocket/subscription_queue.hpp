#pragma once
/**
 *  SubscriptionQueue
 *
 *  A very small paced task queue: each task is a `std::function<void()>`
 *  that typically sends one subscription message to an exchange websocket.
 *
 *  Behaviour:
 *  • When the queue goes from empty → non-empty we arm a single-shot timer.
 *  • After each task executes, if the queue is still non-empty we
 *    re-arm another timer `interval_ns` nanoseconds later.
 *
 *  With that invariant there is never more than one timer in flight, the
 *  chain can’t stall, and bursts of enqueues while the queue is active
 *  are handled naturally.
 */

#include <queue>
#include <functional>
#include <memory>
#include <cstdint>

#include "timer_manager.hpp"
#include "logger_factory.hpp"

namespace reflex {

class SubscriptionQueue {
public:
  using Task = std::function<void()>;

  SubscriptionQueue(TimerManager& tm,
                    const std::string& logger_ctx,
                    int64_t interval_ns = 500'000'000ULL);   // default 500 ms

  /** Enqueue another task. */
  void enqueue(Task task);

  [[nodiscard]] size_t size() const noexcept { return queue_.size(); }

private:
  void arm_next_timer();
  void process_next();

  std::queue<Task>           queue_;
  TimerManager&              timer_manager_;
  const int64_t             interval_ns_;
  bool                      timer_armed_ = false;  // guards against double-arming
                                                   // when a task enqueues more work
  std::shared_ptr<spdlog::logger> logger_;
};

} // namespace reflex