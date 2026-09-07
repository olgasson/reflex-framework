
// include/backtest/backtest_engine.hpp
#pragma once

#include <array>
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
  // Optional action-specific one-way latencies. Negative values fall back to
  // the leg-wide value above.
  int64_t order_entry_latency_ns_ = -1;
  int64_t cancel_latency_ns_ = -1;
  int64_t replace_latency_ns_ = -1;
  int64_t response_latency_ns_ = -1;
  // A venue-safe replay must name both the execution venue and instrument.
  // When configured, non-matching market data never reaches the simulated
  // exchange. fail_on_provenance_mismatch_ turns accidental mixed inputs into
  // an immediate error instead of silently constructing a synthetic book.
  Exchange trading_exchange_ = Exchange::Unknown;
  int32_t trading_instrument_id_ = 0;
  bool fail_on_provenance_mismatch_ = false;
  // Optionally deliver a second (reference) venue's L1 to the strategy without
  // it ever touching the simulated execution book.
  bool deliver_reference_market_data_to_strategy_ = false;
  bool merge_data_files_by_timestamp_ = false;
  Exchange reference_exchange_ = Exchange::Unknown;
  int32_t reference_instrument_id_ = 0;
  // Market data before this timestamp primes the simulated execution book but
  // is not delivered to the strategy. Strategy delivery and scored simulation
  // time begin at the first event at or after the boundary.
  int64_t window_start_ns_ = 0;
  int64_t window_end_ns_ = 0;  // Stop ingesting market data after this timestamp when > 0
};

struct MarketDataProvenanceStats {
  // Exchange enum values currently occupy [1, 4]; index zero is reserved for
  // malformed/out-of-range values.
  std::array<uint64_t, 5> seen_by_exchange{};
  uint64_t accepted_execution_events{0};
  uint64_t rejected_exchange_events{0};
  uint64_t rejected_instrument_events{0};
  uint64_t delivered_reference_events{0};

  uint64_t seen(Exchange exchange) const noexcept {
    const auto index = static_cast<std::size_t>(exchange);
    return index < seen_by_exchange.size() ? seen_by_exchange[index]
                                           : seen_by_exchange[0];
  }
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
  // Opt-in: when set, timer deadlines become a fifth replay source. Runners
  // that leave this unset retain the original four-source ordering exactly.
  void set_timer_manager(std::shared_ptr<reflex::TimerManager> timer_manager);

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
  const MarketDataProvenanceStats& get_market_data_provenance_stats() const noexcept {
    return provenance_stats_;
  }
  int64_t configured_outbound_latency_ns(MessageType type) const noexcept;
  int64_t configured_response_latency_ns() const noexcept;

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
  void prime_exchange_state(const MessageSlot& event);

  // Configuration
  BackTestEngineConfig config_;
  std::vector<std::string> data_files_;

  // Core components
  std::unique_ptr<MultiFileBinaryReader> data_reader_;
  std::shared_ptr<Strategy> strategy_;
  std::shared_ptr<ExchangeSimulator> exchange_simulator_;
  std::shared_ptr<TimerManager> timer_manager_;

  // Clock (owned by engine)
  std::shared_ptr<SimulationClock> clock_;

  // Event scheduling (preallocated FIFO rings; sized to typical in-flight
  // backlog so steady-state operation does not allocate).
  EventRing<DelayedEvent> market_data_queue_{8192};
  EventRing<DelayedEvent> strategy_to_exchange_queue_{1024};
  EventRing<DelayedEvent> exchange_to_strategy_queue_{1024};
  // The rings are FIFO but per-type latencies differ, so a later-queued event
  // could otherwise carry an earlier delivery time — delivered late at its
  // stale timestamp, rewinding the simulation clock. Each direction models one
  // ordered connection: delivery times are clamped monotonic per queue.
  int64_t last_outbound_delivery_ns_ = 0;   // strategy -> exchange
  int64_t last_inbound_delivery_ns_ = 0;    // exchange -> strategy

  // Simulation state
  bool is_running_ = false;
  bool data_window_exhausted_ = false;

  // Results tracking
  BackTestResults results_;
  MarketDataProvenanceStats provenance_stats_;
  std::shared_ptr<spdlog::logger> logger_;
};

}  // namespace reflex::backtest
