
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

class ExchangeSimulator;
class OrderBookManager;

struct DelayedEvent {
  int64_t delivery_timestamp_ns_;
  MessageSlot event_data_;
};

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

  T& emplace_back() {
    if (size() > mask_) {
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
      next[i] = std::move(buf_[(head_ + i) & mask_]);
    }
    buf_.swap(next);
    mask_ = buf_.size() - 1;
    head_ = 0;
    tail_ = n;
  }

  std::vector<T> buf_;
  std::size_t mask_ = 0;
  std::uint64_t head_ = 0;
  std::uint64_t tail_ = 0;
};

struct BackTestEngineConfig {
  int64_t strategy_to_exchange_latency_ns_ = 3'000'000;
  int64_t exchange_to_strategy_latency_ns_ = 3'000'000;
  int64_t order_entry_latency_ns_ = -1;
  int64_t cancel_latency_ns_ = -1;
  int64_t replace_latency_ns_ = -1;
  int64_t response_latency_ns_ = -1;
  Exchange trading_exchange_ = Exchange::Unknown;
  int32_t trading_instrument_id_ = 0;
  bool fail_on_provenance_mismatch_ = false;
  bool deliver_reference_market_data_to_strategy_ = false;
  bool merge_data_files_by_timestamp_ = false;
  Exchange reference_exchange_ = Exchange::Unknown;
  int32_t reference_instrument_id_ = 0;
  int64_t window_start_ns_ = 0;
  int64_t window_end_ns_ = 0;
};

struct MarketDataProvenanceStats {
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

  void set_data_files(const std::vector<std::string>& files);
  void set_strategy(std::shared_ptr<reflex::Strategy> strategy);
  void set_exchange_simulator(std::shared_ptr<ExchangeSimulator> exchange);
  void set_timer_manager(std::shared_ptr<reflex::TimerManager> timer_manager);

  std::shared_ptr<reflex::SimulationClock> get_clock() const { return clock_; }

  void run_backtest();

  void process_file_data_event(const MessageSlot* message_slot);
  void process_strategy_market_data_event(const DelayedEvent& event);
  void process_strategy_order_event(const DelayedEvent& event);
  void process_exchange_response_event(const DelayedEvent& event);

  const BackTestResults& get_results() const { return results_; }
  const MarketDataProvenanceStats& get_market_data_provenance_stats() const noexcept {
    return provenance_stats_;
  }
  int64_t configured_outbound_latency_ns(MessageType type) const noexcept;
  int64_t configured_response_latency_ns() const noexcept;

  int64_t get_current_time_ns() const;

  void add_strategy_order_event(const MessageSlot& message_slot);

  void on_exchange_response(const MessageSlot& response_event) override;

 private:

  void advance_simulation_time(int64_t new_time_ns);

  void schedule_message_for_strategy(const MessageSlot& event);

  bool should_continue() const;
  bool has_more_data() const;
  int64_t get_next_market_data_time() const;
  void initialize_components();
  void prime_exchange_state(const MessageSlot& event);

  BackTestEngineConfig config_;
  std::vector<std::string> data_files_;

  std::unique_ptr<MultiFileBinaryReader> data_reader_;
  std::shared_ptr<Strategy> strategy_;
  std::shared_ptr<ExchangeSimulator> exchange_simulator_;
  std::shared_ptr<TimerManager> timer_manager_;

  std::shared_ptr<SimulationClock> clock_;

  EventRing<DelayedEvent> market_data_queue_{8192};
  EventRing<DelayedEvent> strategy_to_exchange_queue_{1024};
  EventRing<DelayedEvent> exchange_to_strategy_queue_{1024};
  int64_t last_outbound_delivery_ns_ = 0;
  int64_t last_inbound_delivery_ns_ = 0;

  bool is_running_ = false;
  bool data_window_exhausted_ = false;

  BackTestResults results_;
  MarketDataProvenanceStats provenance_stats_;
  std::shared_ptr<spdlog::logger> logger_;
};

}
