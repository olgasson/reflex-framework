#pragma once

#include <algorithm>
#include <cstdint>
#include <functional>
#include <limits>
#include <stdexcept>
#include <unordered_set>
#include <vector>

#include "logger_factory.hpp"
#include "spdlog/logger.h"

namespace reflex {

class TimerManager {
public:
  using Callback = std::function<void()>;
  using TimerId = uint64_t;

  explicit TimerManager(const std::string& context) {
    logger_ = LoggerFactory::getLogger(context + ".TimerManager");
  }

  TimerManager() {
    logger_ = LoggerFactory::getLogger("TimerManager");
  }

  TimerId add_timer(uint64_t next_trigger_time, int64_t wait_time, Callback callback) {
    if (!callback) {
      logger_->error("TimerManager::add_timer - Callback is null");
      throw std::invalid_argument("Callback cannot be null");
    }

    const TimerId id = next_id_++;
    heap_.push_back(Timer{next_trigger_time, wait_time, id, std::move(callback)});
    std::push_heap(heap_.begin(), heap_.end(), TimerLater{});
    live_.insert(id);
    return id;
  }

  void remove_timer(TimerId id) {
    if (live_.count(id) != 0) {
      removed_.insert(id);
    }
  }

  void check_scheduled_timers(const uint64_t current_time) {
    latest_time_ = current_time;

    while (!heap_.empty()) {
      if (heap_.front().next_trigger_time_ > current_time) {
        break;
      }

      std::pop_heap(heap_.begin(), heap_.end(), TimerLater{});
      Timer timer = std::move(heap_.back());
      heap_.pop_back();

      if (!removed_.empty() && removed_.erase(timer.id_) != 0) {
        live_.erase(timer.id_);
        continue;
      }

      try {
        timer.callback_();
      } catch (const std::exception& e) {
        logger_->error("TimerManager::check_scheduled_timers - Exception in callback: {}", e.what());
      } catch (...) {
        logger_->error("TimerManager::check_scheduled_timers - Unknown exception in callback");
      }

      if (timer.wait_time_ != -1) {
        timer.next_trigger_time_ = current_time + static_cast<uint64_t>(timer.wait_time_);
        heap_.push_back(std::move(timer));
        std::push_heap(heap_.begin(), heap_.end(), TimerLater{});
      } else {
        live_.erase(timer.id_);
      }
    }
  }

  uint64_t get_latest_time() const {
    return latest_time_;
  }

  [[nodiscard]] uint64_t next_trigger_time() const noexcept {
    return heap_.empty() ? std::numeric_limits<uint64_t>::max()
                         : heap_.front().next_trigger_time_;
  }

private:
  struct Timer {
    uint64_t next_trigger_time_;
    int64_t wait_time_;
    TimerId id_;
    Callback callback_;
  };

  struct TimerLater {
    bool operator()(const Timer& a, const Timer& b) const noexcept {
      return a.next_trigger_time_ > b.next_trigger_time_;
    }
  };

  std::vector<Timer> heap_;
  std::unordered_set<TimerId> live_;
  std::unordered_set<TimerId> removed_;
  TimerId next_id_ = 1;
  uint64_t latest_time_ = 0;
  std::shared_ptr<spdlog::logger> logger_;
};

}
