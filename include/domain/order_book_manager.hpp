#pragma once

#include "incremental_order_book.hpp"
#include "messages.hpp"
#include "spdlog/logger.h"

#include <unordered_map>
#include <memory>

namespace reflex::backtest {

// Forward declaration for AssetInfoManager
class AssetInfoManager;

class OrderBookManager {
public:
  // Constructor with optional asset manager for tick sizes
  explicit OrderBookManager(std::shared_ptr<AssetInfoManager> asset_manager = nullptr);

  // Process market data messages
  void process_l1_update(const L1UpdateEvent& event);
  void process_l2_update(const L2UpdateEvent& event);
  void process_trade_event(const TradeEvent& event);

  // Incremental order book access
  marketdata::IncrementalOrderBook* get_incremental_book(int32_t instrument_id);
  const marketdata::IncrementalOrderBook* get_incremental_book(int32_t instrument_id) const;

  // Market data queries
  int64_t get_mid_price(int32_t instrument_id) const;
  int64_t get_spread(int32_t instrument_id) const;
  int64_t get_best_bid(int32_t instrument_id) const;
  int64_t get_best_ask(int32_t instrument_id) const;

  // Stats
  size_t num_instruments() const { return incremental_books_.size(); }
  void clear_all_books() { incremental_books_.clear(); }

  // Performance stats
  struct UpdateStats {
    uint64_t l1_updates = 0;
    uint64_t l2_updates = 0;
    uint64_t trade_events = 0;
    uint64_t reanchors = 0;
  };

  const UpdateStats& get_stats() const { return stats_; }
  void reset_stats() { stats_ = UpdateStats{}; }

private:
  std::shared_ptr<spdlog::logger> logger_;
  std::unordered_map<int32_t, marketdata::IncrementalOrderBook> incremental_books_;
  std::shared_ptr<AssetInfoManager> asset_manager_;
  UpdateStats stats_;

  marketdata::IncrementalOrderBook& get_or_create_incremental_book(int32_t instrument_id);
};

} // namespace reflex::backtest
