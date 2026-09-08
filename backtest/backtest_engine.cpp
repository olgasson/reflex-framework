
#include "backtest/backtest_engine.hpp"

#include "logger_factory.hpp"
#include "backtest/binary_reader.hpp"
#include "backtest/exchange_simulator.hpp"
#include <algorithm>
#include <iomanip>
#include <iostream>
#include <string_view>
#include <utility>

namespace reflex::backtest {
namespace {

int64_t slot_timestamp_ns(const MessageSlot& slot) {
  return slot.as<HeartbeatEvent>().timestamp_ns_;
}

Exchange slot_exchange(const MessageSlot& slot) {
  switch (slot.get_type()) {
    case MessageType::L1UpdateEvent:
      return slot.as<L1UpdateEvent>().exchange_;
    case MessageType::L2UpdateEvent:
      return slot.as<L2UpdateEvent>().exchange_;
    case MessageType::TradeEvent:
      return slot.as<TradeEvent>().exchange_;
    case MessageType::MarkPriceEvent:
      return slot.as<MarkPriceEvent>().exchange_;
    case MessageType::FundingRateEvent:
      return slot.as<FundingRateEvent>().exchange_;
    case MessageType::OpenInterestEvent:
      return slot.as<OpenInterestEvent>().exchange_;
    case MessageType::LiquidationEvent:
      return slot.as<LiquidationEvent>().exchange_;
    default:
      return Exchange::Unknown;
  }
}

int32_t slot_instrument_id(const MessageSlot& slot) {
  switch (slot.get_type()) {
    case MessageType::L1UpdateEvent:
      return slot.as<L1UpdateEvent>().instrument_id_;
    case MessageType::L2UpdateEvent:
      return slot.as<L2UpdateEvent>().instrument_id_;
    case MessageType::TradeEvent:
      return slot.as<TradeEvent>().instrument_id_;
    case MessageType::MarkPriceEvent:
      return slot.as<MarkPriceEvent>().instrument_id_;
    case MessageType::FundingRateEvent:
      return slot.as<FundingRateEvent>().instrument_id_;
    case MessageType::OpenInterestEvent:
      return slot.as<OpenInterestEvent>().instrument_id_;
    case MessageType::LiquidationEvent:
      return slot.as<LiquidationEvent>().instrument_id_;
    default:
      return 0;
  }
}

const char* exchange_name(Exchange exchange) noexcept {
  switch (exchange) {
    case Exchange::Binance: return "binance";
    case Exchange::BinanceDerivatives: return "binance_derivatives";
    case Exchange::Okx: return "okx";
    default: return "unknown";
  }
}

}

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

void BackTestEngine::set_timer_manager(std::shared_ptr<reflex::TimerManager> timer_manager) {
  timer_manager_ = std::move(timer_manager);
}


void BackTestEngine::run_backtest() {
  if (data_files_.empty()) {
    throw std::runtime_error("No data files specified");
  }

  if (!strategy_) {
    throw std::runtime_error("No strategy specified");
  }

  initialize_components();

  results_.start_time_ = std::chrono::steady_clock::now();
  is_running_ = true;
  data_window_exhausted_ = false;

  while (should_continue()) {
    int64_t earliest_time = INT64_MAX;
    int source_to_process = -1;

    if (has_more_data()) {
      const int64_t file_time = get_next_market_data_time();
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
        source_to_process = 0;
      }
    }

    if (!market_data_queue_.empty() && market_data_queue_.front().delivery_timestamp_ns_ < earliest_time) {
      earliest_time = market_data_queue_.front().delivery_timestamp_ns_;
      source_to_process = 1;
    }

    if (!strategy_to_exchange_queue_.empty() &&
        strategy_to_exchange_queue_.front().delivery_timestamp_ns_ < earliest_time) {
      earliest_time = strategy_to_exchange_queue_.front().delivery_timestamp_ns_;
      source_to_process = 2;
    }

    if (!exchange_to_strategy_queue_.empty() &&
        exchange_to_strategy_queue_.front().delivery_timestamp_ns_ < earliest_time) {
      earliest_time = exchange_to_strategy_queue_.front().delivery_timestamp_ns_;
      source_to_process = 3;
    }

    if (timer_manager_) {
      const uint64_t timer_time = timer_manager_->next_trigger_time();
      if (timer_time <= static_cast<uint64_t>(INT64_MAX) &&
          static_cast<int64_t>(timer_time) < earliest_time) {
        earliest_time = static_cast<int64_t>(timer_time);
        source_to_process = 4;
      }
    }

    advance_simulation_time(earliest_time);

    switch (source_to_process) {
      case 0: {
        const MessageSlot* slot = data_reader_->read_next_message();
        if (!slot) {
          logger_->warn("File data source selected but reader returned no message");
          break;
        }
        const int64_t ts = slot_timestamp_ns(*slot);
        if (config_.window_start_ns_ > 0 && ts < config_.window_start_ns_) {
          prime_exchange_state(*slot);
          break;
        }
        if (config_.window_end_ns_ > 0 && ts > config_.window_end_ns_) {
          data_window_exhausted_ = true;
          break;
        }
        process_file_data_event(slot);
        break;
      }
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
      case 4:
        timer_manager_->check_scheduled_timers(static_cast<uint64_t>(earliest_time));
        break;
      default:
        logger_->warn("No events to process for invalid source");
        return;
    }
  }
  is_running_ = false;
  results_.end_time_ = std::chrono::steady_clock::now();
  strategy_->on_stop();
}

void BackTestEngine::process_file_data_event(const MessageSlot* slot) {
  if (!slot) {
    logger_->warn("File data source selected but reader returned no message");
    return;
  }

  const Exchange source_exchange = slot_exchange(*slot);
  const int32_t source_instrument = slot_instrument_id(*slot);
  const auto exchange_index = static_cast<std::size_t>(source_exchange);
  if (exchange_index < provenance_stats_.seen_by_exchange.size()) {
    ++provenance_stats_.seen_by_exchange[exchange_index];
  } else {
    ++provenance_stats_.seen_by_exchange[0];
  }

  const bool exchange_matches =
      config_.trading_exchange_ == Exchange::Unknown ||
      source_exchange == config_.trading_exchange_;
  const bool instrument_matches =
      config_.trading_instrument_id_ <= 0 ||
      source_instrument == config_.trading_instrument_id_;
  const bool reference_matches =
      config_.deliver_reference_market_data_to_strategy_ &&
      config_.reference_exchange_ != Exchange::Unknown &&
      config_.reference_instrument_id_ > 0 &&
      source_exchange == config_.reference_exchange_ &&
      source_instrument == config_.reference_instrument_id_;
  if (!exchange_matches || !instrument_matches) {
    if (reference_matches) {
      if (slot->get_type() == MessageType::L1UpdateEvent) {
        ++provenance_stats_.delivered_reference_events;
        schedule_message_for_strategy(*slot);
      }
      return;
    }
    if (!exchange_matches) ++provenance_stats_.rejected_exchange_events;
    if (!instrument_matches) ++provenance_stats_.rejected_instrument_events;
    if (config_.fail_on_provenance_mismatch_) {
      throw std::runtime_error(
          std::string{"Market-data provenance mismatch: expected exchange="} +
          exchange_name(config_.trading_exchange_) + " instrument=" +
          std::to_string(config_.trading_instrument_id_) + " but received exchange=" +
          exchange_name(source_exchange) + " instrument=" +
          std::to_string(source_instrument));
    }
    if (config_.deliver_reference_market_data_to_strategy_ &&
        config_.reference_exchange_ == Exchange::Unknown &&
        config_.reference_instrument_id_ <= 0) {
      ++provenance_stats_.delivered_reference_events;
      schedule_message_for_strategy(*slot);
    }
    return;
  }
  ++provenance_stats_.accepted_execution_events;

  switch (slot->get_type()) {
    case MessageType::L1UpdateEvent: {
      const auto& event = slot->as<L1UpdateEvent>();

      if (exchange_simulator_) {
        exchange_simulator_->process_l1_update(event);
      }

      schedule_message_for_strategy(*slot);
      break;
    }
    case MessageType::L2UpdateEvent: {
      const auto& event = slot->as<L2UpdateEvent>();

      if (exchange_simulator_) {
        exchange_simulator_->process_l2_update(event);
      }

      schedule_message_for_strategy(*slot);
      break;
    }
    case MessageType::TradeEvent: {
      const auto& event = slot->as<TradeEvent>();

      if (exchange_simulator_) {
        exchange_simulator_->process_trade_event(event);
      }

      schedule_message_for_strategy(*slot);
      break;
    }
    case MessageType::MarkPriceEvent:
    case MessageType::FundingRateEvent:
    case MessageType::OpenInterestEvent:
    case MessageType::LiquidationEvent: {
      schedule_message_for_strategy(*slot);
      break;
    }
    default:
      break;
  }
}

void BackTestEngine::prime_exchange_state(const MessageSlot& slot) {
  if (!exchange_simulator_) return;

  const Exchange source_exchange = slot_exchange(slot);
  const int32_t source_instrument = slot_instrument_id(slot);
  const bool exchange_matches =
      config_.trading_exchange_ == Exchange::Unknown ||
      source_exchange == config_.trading_exchange_;
  const bool instrument_matches =
      config_.trading_instrument_id_ <= 0 ||
      source_instrument == config_.trading_instrument_id_;
  if (!exchange_matches || !instrument_matches) return;

  switch (slot.get_type()) {
    case MessageType::L1UpdateEvent:
      exchange_simulator_->process_l1_update(slot.as<L1UpdateEvent>());
      break;
    case MessageType::L2UpdateEvent:
      exchange_simulator_->process_l2_update(slot.as<L2UpdateEvent>());
      break;
    case MessageType::TradeEvent:
      exchange_simulator_->process_trade_event(slot.as<TradeEvent>());
      break;
    default:
      break;
  }
}

void BackTestEngine::process_strategy_market_data_event(const DelayedEvent& event) {
  if (!strategy_) { return; }

  const auto& slot = event.event_data_;

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
    case MessageType::LiquidationEvent:
      strategy_->on_liquidation(slot.as<LiquidationEvent>());
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

  switch (event.event_data_.get_type()) {
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
  data_reader_ = std::make_unique<MultiFileBinaryReader>(
      data_files_, config_.merge_data_files_by_timestamp_);
}

int64_t BackTestEngine::get_current_time_ns() const {
  return clock_ ? clock_->epoch_nanos() : 0;
}

void BackTestEngine::add_strategy_order_event(const MessageSlot& message_slot) {
  const int64_t latency_ns = configured_outbound_latency_ns(message_slot.get_type());
  DelayedEvent& delayed = strategy_to_exchange_queue_.emplace_back();
  delayed.delivery_timestamp_ns_ =
      std::max(clock_->epoch_nanos() + latency_ns, last_outbound_delivery_ns_);
  last_outbound_delivery_ns_ = delayed.delivery_timestamp_ns_;
  delayed.event_data_ = message_slot;
}

int64_t BackTestEngine::configured_outbound_latency_ns(MessageType type) const noexcept {
  int64_t latency_ns = config_.strategy_to_exchange_latency_ns_;
  switch (type) {
    case MessageType::Pending:
      if (config_.order_entry_latency_ns_ >= 0) {
        latency_ns = config_.order_entry_latency_ns_;
      }
      break;
    case MessageType::PendingCancel:
      if (config_.cancel_latency_ns_ >= 0) {
        latency_ns = config_.cancel_latency_ns_;
      }
      break;
    case MessageType::PendingReplace:
      if (config_.replace_latency_ns_ >= 0) {
        latency_ns = config_.replace_latency_ns_;
      }
      break;
    default:
      break;
  }
  return latency_ns;
}

int64_t BackTestEngine::configured_response_latency_ns() const noexcept {
  return config_.response_latency_ns_ >= 0 ? config_.response_latency_ns_
                                           : config_.exchange_to_strategy_latency_ns_;
}

void BackTestEngine::on_exchange_response(const MessageSlot& response_event) {
  const int64_t latency_ns = configured_response_latency_ns();
  DelayedEvent& delayed = exchange_to_strategy_queue_.emplace_back();
  delayed.delivery_timestamp_ns_ =
      std::max(clock_->epoch_nanos() + latency_ns, last_inbound_delivery_ns_);
  last_inbound_delivery_ns_ = delayed.delivery_timestamp_ns_;
  delayed.event_data_ = response_event;
}


void BackTestEngine::schedule_message_for_strategy(const MessageSlot& event) {
  DelayedEvent& delayed = market_data_queue_.emplace_back();
  delayed.delivery_timestamp_ns_ = clock_->epoch_nanos() + config_.exchange_to_strategy_latency_ns_;
  delayed.event_data_ = event;
}


void BackTestEngine::advance_simulation_time(int64_t new_time_ns) {
  if (clock_) {
    new_time_ns = std::max(new_time_ns, clock_->epoch_nanos());
    clock_->set_time(new_time_ns);

    if (config_.window_start_ns_ > 0 && new_time_ns < config_.window_start_ns_) {
      return;
    }

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

bool BackTestEngine::has_more_data() const {
  return !data_window_exhausted_ && data_reader_ && data_reader_->has_more_data();
}

int64_t BackTestEngine::get_next_market_data_time() const {
  return data_reader_ ? data_reader_->peek_next_timestamp() : INT64_MAX;
}

}
