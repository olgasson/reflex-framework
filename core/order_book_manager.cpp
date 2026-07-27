
#include "domain/order_book_manager.hpp"

#include "asset_info_manager.hpp"
#include "logger_factory.hpp"

namespace reflex::backtest {

OrderBookManager::OrderBookManager(std::shared_ptr<AssetInfoManager> asset_manager)
    : asset_manager_(std::move(asset_manager)) {
  logger_ = LoggerFactory::getLogger("OrderBookManager");
}

void OrderBookManager::process_l1_update(const L1UpdateEvent& event) {
  auto& incremental = get_or_create_incremental_book(event.instrument_id_);
  incremental.apply_l1_update(event);
  ++stats_.l1_updates;
}

void OrderBookManager::process_l2_update(const L2UpdateEvent& event) {
  auto& incremental = get_or_create_incremental_book(event.instrument_id_);
  incremental.apply_l2_update(event);

  ++stats_.l2_updates;
}

void OrderBookManager::process_trade_event(const TradeEvent& event) {
  auto* incremental = get_incremental_book(event.instrument_id_);
  if (incremental) {
    incremental->apply_trade(event);
  }

  ++stats_.trade_events;
}

marketdata::IncrementalOrderBook* OrderBookManager::get_incremental_book(int32_t instrument_id) {
  auto it = incremental_books_.find(instrument_id);
  return (it != incremental_books_.end()) ? &it->second : nullptr;
}

const marketdata::IncrementalOrderBook* OrderBookManager::get_incremental_book(int32_t instrument_id) const {
  auto it = incremental_books_.find(instrument_id);
  return (it != incremental_books_.end()) ? &it->second : nullptr;
}

int64_t OrderBookManager::get_mid_price(int32_t instrument_id) const {
  const auto* incremental = get_incremental_book(instrument_id);
  if (incremental && incremental->ready()) {
    return incremental->mid_price();
  }
  return 0;
}

int64_t OrderBookManager::get_spread(int32_t instrument_id) const {
  const auto* incremental = get_incremental_book(instrument_id);
  if (incremental && incremental->ready()) {
    return incremental->spread();
  }
  return 0;
}

int64_t OrderBookManager::get_best_bid(int32_t instrument_id) const {
  const auto* incremental = get_incremental_book(instrument_id);
  if (incremental && incremental->ready()) {
    return incremental->best_bid_price();
  }
  return 0;
}

int64_t OrderBookManager::get_best_ask(int32_t instrument_id) const {
  const auto* incremental = get_incremental_book(instrument_id);
  if (incremental && incremental->ready()) {
    return incremental->best_ask_price();
  }
  return 0;
}

marketdata::IncrementalOrderBook& OrderBookManager::get_or_create_incremental_book(int32_t instrument_id) {
  auto [it, inserted] = incremental_books_.try_emplace(instrument_id);
  if (inserted) {
    it->second.set_instrument(instrument_id);
  }
  return it->second;
}

} // namespace reflex::backtest
