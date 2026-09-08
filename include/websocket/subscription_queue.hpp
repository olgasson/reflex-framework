#pragma once

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
                    int64_t interval_ns = 500'000'000ULL);

  void enqueue(Task task);

  [[nodiscard]] size_t size() const noexcept { return queue_.size(); }

private:
  void arm_next_timer();
  void process_next();

  std::queue<Task>           queue_;
  TimerManager&              timer_manager_;
  const int64_t             interval_ns_;
  bool                      timer_armed_ = false;
  std::shared_ptr<spdlog::logger> logger_;
};

}
