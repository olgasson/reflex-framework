
// include/backtest/backtest_engine.hpp
#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>
#include "binary_reader.hpp"
#include "exchange_response_handler.hpp"
#include "messages.hpp"
#include "framework/strategy.hpp"

namespace reflex::backtest {

// Forward declarations
class ExchangeSimulator;
class OrderBookManager;

// Scheduled event held in the latency queues.
struct DelayedEvent {
  int64_t delivery_timestamp_ns_;
  MessageSlot event_data_;
};

// Single-threaded FIFO ring buffer. Same FIFO semantics as the std::queue it
// replaces (push order == pop order); grows (never overwrites) when full and
// never shrinks, so steady-state operation is allocation-free. Capacity is
// always a power of two so the index wrap is a single mask.
template <typename T>
class EventRing {
 public:
  explicit EventRing(std::size_t initial_capacity = 1024) {
    std::size_t cap = 1;
    while (cap < initial_capacity) cap <<= 1;
    buf_.resize(cap);
    mask_ = cap - 1;
  }

  bool empty() const noexcept { return head_ == tail_; }
  std::size_t size() const noexcept { return static_cast<std::size_t>(tail_ - head_); }

  const T& front() const noexcept { return buf_[head_ & mask_]; }
  void pop() noexcept { ++head_; }

  // Returns a reference to a fresh slot at the back so callers can fill it
  // in place — avoids constructing a temporary and copying it in.
  T& emplace_back() {
    if (size() > mask_) {  // size() == capacity -> full
      grow();
    }
    T& slot = buf_[tail_ & mask_];
    ++tail_;
    return slot;
  }

 private:
  void grow() {
    const std::size_t n = size();
    std::vector<T> next(buf_.size() * 2);
    for (std::size_t i = 0; i < n; ++i) {
      next[i] = std::move(buf_[(head_ + i) & mask_]);  // preserve FIFO order
    }
    buf_.swap(next);
    mask_ = buf_.size() - 1;
    head_ = 0;
    tail_ = n;
  }

  std::vector<T> buf_;
  std::size_t mask_ = 0;
  std::uint64_t head_ = 0;  // monotonic counters; live index = counter & mask_
  std::uint64_t tail_ = 0;
};

// Configuration for backtest. Timestamp ties between the file stream and the
// latency queues are broken by the engine's fixed source order (see
// run_backtest), so no artificial +1ns offset is needed here.
struct BackTestEngineConfig {
  // One-way latency for the strategy -> exchange leg (order entry, cancels, replaces).
  int64_t strategy_to_exchange_latency_ns_ = 3'000'000;
  // One-way latency for the exchange -> strategy leg (market data and order responses).
  int64_t exchange_to_strategy_latency_ns_ = 3'000'000;
};

// Backtest results
struct BackTestResults {
  std::chrono::steady_clock::time_point start_time_;
  std::chrono::steady_clock::time_point end_time_;
  int64_t simulation_start_time_ns_ = 0;
  int64_t simulation_end_time_ns_ = 0;

  void print_summary() const;
  double get_elapsed_seconds() const;
};

class BackTestEngine : public ExchangeResponseHandler {
 public:
  explicit BackTestEngine(const BackTestEngineConfig& config = {});
  ~BackTestEngine();

  // Configuration
  void set_data_files(const std::vector<std::string>& files);
  void set_strategy(std::shared_ptr<reflex::Strategy> strategy);
  void set_exchange_simulator(std::shared_ptr<ExchangeSimulator> exchange);

  // Clock access
  std::shared_ptr<reflex::SimulationClock> get_clock() const { return clock_; }

  // Main execution
  void run_backtest();

  void process_file_data_event(const MessageSlot* message_slot);
  void process_strategy_market_data_event(const DelayedEvent& event);
  void process_strategy_order_event(const DelayedEvent& event);
  void process_exchange_response_event(const DelayedEvent& event);

  // Results
  const BackTestResults& get_results() const { return results_; }

  // Current simulation time access
  int64_t get_current_time_ns() const;

  void add_strategy_order_event(const MessageSlot& message_slot);

  void on_exchange_response(const MessageSlot& response_event) override;

 private:

  void advance_simulation_time(int64_t new_time_ns);

  // Strategy scheduling (with latency)
  void schedule_message_for_strategy(const MessageSlot& event);

  // Utility
  bool should_continue() const;
  bool has_more_data() const;
  int64_t get_next_market_data_time() const;
  void initialize_components();

  // Configuration
  BackTestEngineConfig config_;
  std::vector<std::string> data_files_;

  // Core components
  std::unique_ptr<MultiFileBinaryReader> data_reader_;
  std::shared_ptr<Strategy> strategy_;
  std::shared_ptr<ExchangeSimulator> exchange_simulator_;

  // Clock (owned by engine)
  std::shared_ptr<SimulationClock> clock_;

  // Event scheduling (preallocated FIFO rings; sized to typical in-flight
  // backlog so steady-state operation does not allocate).
  EventRing<DelayedEvent> market_data_queue_{8192};
  EventRing<DelayedEvent> strategy_to_exchange_queue_{1024};
  EventRing<DelayedEvent> exchange_to_strategy_queue_{1024};

  // Simulation state
  bool is_running_ = false;

  // Results tracking
  BackTestResults results_;
  std::shared_ptr<spdlog::logger> logger_;
};

}  // namespace reflex::backtest
