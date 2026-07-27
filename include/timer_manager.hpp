#pragma once

#include <algorithm>
#include <cstdint>
#include <functional>
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

  // Schedule a timer. wait_time == -1 means one-shot; otherwise it reschedules
  // wait_time after each firing. Returns a stable id that survives reschedules
  // and can be passed to remove_timer().
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

  // Cancel a specific timer by id. O(1): the timer is tombstoned and dropped
  // when it next surfaces at the top of the heap. Unlike the previous
  // target_type()-based implementation, this removes exactly one timer rather
  // than every timer that happens to share a callable type.
  void remove_timer(TimerId id) {
    // Only tombstone ids that are still scheduled: a tombstone for an id that
    // already fired (one-shot) or was never scheduled would sit in removed_
    // forever, growing the set without bound.
    if (live_.count(id) != 0) {
      removed_.insert(id);
    }
  }

  void check_scheduled_timers(const uint64_t current_time) {
    latest_time_ = current_time;

    while (!heap_.empty()) {
      if (heap_.front().next_trigger_time_ > current_time) {
        break;  // earliest timer not due yet
      }

      std::pop_heap(heap_.begin(), heap_.end(), TimerLater{});
      Timer timer = std::move(heap_.back());  // move out — no std::function copy
      heap_.pop_back();

      if (!removed_.empty() && removed_.erase(timer.id_) != 0) {
        live_.erase(timer.id_);
        continue;  // cancelled before it fired
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
        live_.erase(timer.id_);  // one-shot expired - its id is dead now
      }
    }
  }

  uint64_t get_latest_time() const {
    return latest_time_;
  }

private:
  struct Timer {
    uint64_t next_trigger_time_;
    int64_t wait_time_;
    TimerId id_;
    Callback callback_;
  };

  // Comparator that turns std::push_heap / std::pop_heap (max-heap by default)
  // into a min-heap on trigger time, so heap_.front() is the earliest timer.
  struct TimerLater {
    bool operator()(const Timer& a, const Timer& b) const noexcept {
      return a.next_trigger_time_ > b.next_trigger_time_;
    }
  };

  std::vector<Timer> heap_;              // binary min-heap by next_trigger_time_
  std::unordered_set<TimerId> live_;     // ids currently scheduled in heap_
  std::unordered_set<TimerId> removed_;  // tombstones for cancelled timers (subset of live_)
  TimerId next_id_ = 1;
  uint64_t latest_time_ = 0;
  std::shared_ptr<spdlog::logger> logger_;
};

} // namespace reflex
