// backtest/backtest_engine.cpp

#include "backtest/backtest_engine.hpp"

#include "logger_factory.hpp"
#include "backtest/binary_reader.hpp"
#include "backtest/exchange_simulator.hpp"
#include <iomanip>
#include <iostream>
#include <string_view>
#include <utility>

namespace reflex::backtest {

void BackTestResults::print_summary() const {
  double elapsed_seconds = get_elapsed_seconds();

  std::cout << "\n=== BACKTEST SUMMARY ===" << '\n';
  std::cout << "Elapsed Time: " << elapsed_seconds << " seconds" << '\n';

  if (simulation_start_time_ns_ > 0 && simulation_end_time_ns_ > 0) {
    double sim_duration_ms = (simulation_end_time_ns_ - simulation_start_time_ns_) / 1e6;
    std::cout << "Simulation Duration: " << sim_duration_ms << " ms" << '\n';
  }
}

double BackTestResults::get_elapsed_seconds() const {
  auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(end_time_ - start_time_);
  return elapsed.count() / 1000.0;
}

// BackTestEngine implementation
BackTestEngine::BackTestEngine(const BackTestEngineConfig& config)
    : config_(config), clock_(std::make_shared<reflex::SimulationClock>()) {

  logger_ = LoggerFactory::getLogger("BacktestEngine");

}

BackTestEngine::~BackTestEngine() = default;

void BackTestEngine::set_data_files(const std::vector<std::string>& files) { data_files_ = files; }

void BackTestEngine::set_strategy(std::shared_ptr<reflex::Strategy> strategy) { strategy_ = std::move(strategy); }

void BackTestEngine::set_exchange_simulator(std::shared_ptr<ExchangeSimulator> exchange) {
  exchange_simulator_ = std::move(exchange);
}


void BackTestEngine::run_backtest() {
  if (data_files_.empty()) {
    throw std::runtime_error("No data files specified");
  }

  if (!strategy_) {
    throw std::runtime_error("No strategy specified");
  }

  // logger_->info("=== STARTING BACKTEST ===");
  // logger_->info("Loading {} data files", data_files_.size());

  // Initialize components
  initialize_components();

  // Start timing
  results_.start_time_ = std::chrono::steady_clock::now();
  is_running_ = true;

  // logger_->info("Processing market data...");

  while (should_continue()) {
    // k-way merge over the file stream + 3 latency queues. Strict-< comparisons
    // mean that on a timestamp tie the earliest-checked source wins
    // (file > market-data > order > response) — the original tie-break, kept
    // verbatim so delivery order is unchanged.
    int64_t earliest_time = INT64_MAX;
    int source_to_process = -1;

    if (has_more_data()) {
      const int64_t file_time = get_next_market_data_time();  // peek once (INT64_MAX = exhausted)
      // Monotonicity guard: the simulation clock only ever advances to the
      // minimum across all sources, so an unread file event earlier than the
      // clock means the capture files themselves are out of time order. A
      // silently time-warped simulation is worse than a failed one — abort.
      if (file_time != INT64_MAX && file_time < clock_->epoch_nanos()) {
        const std::string& file = data_reader_->current_file();
        logger_->error(
            "Out-of-order market data: next event at {} ns is behind simulation time {} ns "
            "(file: {}). Capture files must be sorted by timestamp — aborting run.",
            file_time, clock_->epoch_nanos(), file);
        throw std::runtime_error("Out-of-order market data in capture file: " + file);
      }
      if (file_time < earliest_time) {
        earliest_time = file_time;
        source_to_process = 0;  // File data
      }
    }

    if (!market_data_queue_.empty() && market_data_queue_.front().delivery_timestamp_ns_ < earliest_time) {
      earliest_time = market_data_queue_.front().delivery_timestamp_ns_;
      source_to_process = 1;  // Market data
    }

    if (!strategy_to_exchange_queue_.empty() &&
        strategy_to_exchange_queue_.front().delivery_timestamp_ns_ < earliest_time) {
      earliest_time = strategy_to_exchange_queue_.front().delivery_timestamp_ns_;
      source_to_process = 2;  // Strategy → Exchange
    }

    if (!exchange_to_strategy_queue_.empty() &&
        exchange_to_strategy_queue_.front().delivery_timestamp_ns_ < earliest_time) {
      earliest_time = exchange_to_strategy_queue_.front().delivery_timestamp_ns_;
      source_to_process = 3;  // Exchange → Strategy
    }

    advance_simulation_time(earliest_time);

    // Process the earliest event
    switch (source_to_process) {
      case 0:
        process_file_data_event(data_reader_->read_next_message());
        break;
      case 1:
        process_strategy_market_data_event(market_data_queue_.front());
        market_data_queue_.pop();
        break;
      case 2:
        process_strategy_order_event(strategy_to_exchange_queue_.front());
        strategy_to_exchange_queue_.pop();
        break;
      case 3:
        process_exchange_response_event(exchange_to_strategy_queue_.front());
        exchange_to_strategy_queue_.pop();
        break;
      default:
        logger_->warn("No events to process for invalid source");
        return;
    }
  }
  // Finalize
  is_running_ = false;
  results_.end_time_ = std::chrono::steady_clock::now();
  strategy_->on_stop();
}

void BackTestEngine::process_file_data_event(const MessageSlot* slot) {
  if (!slot) {
    // has_more_data()/peek promised a message the reader could not deliver;
    // never dereference — just drop the tick.
    logger_->warn("File data source selected but reader returned no message");
    return;
  }
  switch (slot->get_type()) {
    case MessageType::L1UpdateEvent: {
      const auto& event = slot->as<L1UpdateEvent>();

      // STEP 2: Send to exchange immediately (no latency)
      if (exchange_simulator_) {
        exchange_simulator_->process_l1_update(event);
      }

      // STEP 3: Schedule for strategy (with latency)
      schedule_message_for_strategy(*slot);
      break;
    }
    case MessageType::L2UpdateEvent: {
      const auto& event = slot->as<L2UpdateEvent>();

      // STEP 2: Send to exchange immediately (no latency)
      if (exchange_simulator_) {
        exchange_simulator_->process_l2_update(event);
      }

      // STEP 3: Schedule for strategy (with latency)
      schedule_message_for_strategy(*slot);
      break;
    }
    case MessageType::TradeEvent: {
      const auto& event = slot->as<TradeEvent>();

      // STEP 2: Send to exchange immediately (no latency)
      if (exchange_simulator_) {
        exchange_simulator_->process_trade_event(event);
      }

      // STEP 3: Schedule for strategy (with latency)
      schedule_message_for_strategy(*slot);
      break;
    }
    case MessageType::MarkPriceEvent:
    case MessageType::FundingRateEvent:
    case MessageType::OpenInterestEvent: {
      schedule_message_for_strategy(*slot);
      break;
    }
    default:
      break;
  }
}

void BackTestEngine::process_strategy_market_data_event(const DelayedEvent& event) {
  if (!strategy_) { return; }

  const auto& slot = event.event_data_;

  // Single type check when delivering to strategy
  switch (slot.get_type()) {
    case MessageType::L1UpdateEvent:
      strategy_->on_l1_update(slot.as<L1UpdateEvent>());
      break;
    case MessageType::L2UpdateEvent:
      strategy_->on_l2_update(slot.as<L2UpdateEvent>());
      break;
    case MessageType::TradeEvent:
      strategy_->on_trade(slot.as<TradeEvent>());
      break;
    case MessageType::MarkPriceEvent:
      strategy_->on_mark_price(slot.as<MarkPriceEvent>());
      break;
    case MessageType::FundingRateEvent:
      strategy_->on_funding_rate(slot.as<FundingRateEvent>());
      break;
    case MessageType::OpenInterestEvent:
      strategy_->on_open_interest(slot.as<OpenInterestEvent>());
      break;
    default:
      logger_->warn("Unexpected market data type in strategy delivery");
      break;
  }

}

void BackTestEngine::process_strategy_order_event(const DelayedEvent& event) {
  if (!exchange_simulator_) {
    logger_->error("No exchange simulator configured");
    return;
  }

  // Process the order event based on its type
  switch (event.event_data_.get_type()) {
    case MessageType::Pending: {
      const auto& order_event = event.event_data_.as<PendingEvent>();
      exchange_simulator_->on_pending(order_event);
      break;
    }
    case MessageType::PendingCancel: {
      const auto& cancel_event = event.event_data_.as<PendingCancelEvent>();
      exchange_simulator_->on_pending_cancel(cancel_event);
      break;
    }
    case MessageType::PendingReplace: {
      const auto& replace_event = event.event_data_.as<PendingReplaceEvent>();
      exchange_simulator_->on_pending_replace(replace_event);
      break;
    }
    default:
      logger_->warn("Unexpected event type in strategy order queue: {}",
                    static_cast<int>(event.event_data_.get_type()));
      break;
  }

}
void BackTestEngine::process_exchange_response_event(const DelayedEvent& event) {
  if (!strategy_) {
    logger_->error("No strategy configured");
    return;
  }

  // Route exchange responses to strategy based on event type
  switch (event.event_data_.get_type()) {
    // Order Management responses
    case MessageType::Accepted: {
      const auto& accepted_event = event.event_data_.as<AcceptedEvent>();
      strategy_->get_order_management()->on_accepted(&accepted_event);
      break;
    }
    case MessageType::Rejected: {
      const auto& rejected_event = event.event_data_.as<RejectedEvent>();
      strategy_->get_order_management()->on_rejected(&rejected_event);
      break;
    }
    case MessageType::ReplaceAccepted: {
      const auto& replace_accepted = event.event_data_.as<ReplaceAcceptedEvent>();
      strategy_->get_order_management()->on_replace_accepted(&replace_accepted);
      break;
    }
    case MessageType::ReplaceRejected: {
      const auto& replace_rejected = event.event_data_.as<ReplaceRejectedEvent>();
      strategy_->get_order_management()->on_replace_rejected(&replace_rejected);
      break;
    }
    case MessageType::CancelAccepted: {
      const auto& cancel_accepted = event.event_data_.as<CancelAcceptedEvent>();
      strategy_->get_order_management()->on_cancel_accepted(&cancel_accepted);
      break;
    }
    case MessageType::CancelRejected: {
      const auto& cancel_rejected = event.event_data_.as<CancelRejectedEvent>();
      strategy_->get_order_management()->on_cancel_rejected(&cancel_rejected);
      break;
    }
    case MessageType::Executed: {
      const auto& executed_event = event.event_data_.as<ExecutedEvent>();
      strategy_->get_order_management()->on_executed(&executed_event);
      break;
    }
    default:
      logger_->warn("Unexpected event type in exchange response queue: {}",
                    static_cast<int>(event.event_data_.get_type()));
      break;
  }
}


void BackTestEngine::initialize_components() {
  // Create data reader
  data_reader_ = std::make_unique<MultiFileBinaryReader>(data_files_);
  // logger_->info("Components initialized");
}

int64_t BackTestEngine::get_current_time_ns() const {
  return clock_ ? clock_->epoch_nanos() : 0;
}

void BackTestEngine::add_strategy_order_event(const MessageSlot& message_slot) {
  // Apply the strategy->exchange leg latency and add to the order queue.
  // Written straight into the ring slot — one copy of the 64-byte payload.
  DelayedEvent& delayed = strategy_to_exchange_queue_.emplace_back();
  delayed.delivery_timestamp_ns_ = clock_->epoch_nanos() + config_.strategy_to_exchange_latency_ns_;
  delayed.event_data_ = message_slot;
}

void BackTestEngine::on_exchange_response(const MessageSlot& response_event) {
  // Order responses travel the exchange->strategy leg.
  DelayedEvent& delayed = exchange_to_strategy_queue_.emplace_back();
  delayed.delivery_timestamp_ns_ = clock_->epoch_nanos() + config_.exchange_to_strategy_latency_ns_;
  delayed.event_data_ = response_event;
}


// Strategy scheduling methods (always with latency)
void BackTestEngine::schedule_message_for_strategy(const MessageSlot& event) {
  // Market data travels the same exchange->strategy leg as order responses.
  DelayedEvent& delayed = market_data_queue_.emplace_back();
  delayed.delivery_timestamp_ns_ = clock_->epoch_nanos() + config_.exchange_to_strategy_latency_ns_;
  delayed.event_data_ = event;
}



void BackTestEngine::advance_simulation_time(int64_t new_time_ns) {
  if (clock_) {
    clock_->set_time(new_time_ns);

    // Track simulation time range
    if (results_.simulation_start_time_ns_ == 0) {
      results_.simulation_start_time_ns_ = new_time_ns;
    }
    results_.simulation_end_time_ns_ = new_time_ns;
  }
}

bool BackTestEngine::should_continue() const {
  return is_running_ && (has_more_data() || !exchange_to_strategy_queue_.empty() ||
                         !strategy_to_exchange_queue_.empty() || !market_data_queue_.empty());
}

bool BackTestEngine::has_more_data() const { return data_reader_ && data_reader_->has_more_data(); }

int64_t BackTestEngine::get_next_market_data_time() const {
  return data_reader_ ? data_reader_->peek_next_timestamp() : INT64_MAX;
}

}  // namespace reflex::backtest
