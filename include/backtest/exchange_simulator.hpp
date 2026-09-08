#pragma once

#include <cmath>
#include <stdexcept>

#include "event_listener.hpp"
#include "exchange_response_handler.hpp"

#include <memory>
#include <string>
#include <unordered_map>
#include <chrono>
#include "messages.hpp"
#include "offset_epoch_nano_clock.hpp"
#include "domain/order_book_manager.hpp"
#include "spdlog/logger.h"

#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <vector>

namespace reflex::backtest {

class ExchangeSimulator : public GatewayOrderEventListener {
public:
  explicit ExchangeSimulator(std::shared_ptr<reflex::ClockInterface> clock,
                             ExchangeResponseHandler* response_handler);
  ~ExchangeSimulator();

  void process_l1_update(const L1UpdateEvent& event);
  void process_l2_update(const L2UpdateEvent& event);
  void process_trade_event(const TradeEvent& event);

  enum class QueueModel { Pessimistic, Optimistic };
  void set_queue_model(QueueModel m) { queue_model_ = m; }

  void set_cancel_credit_alpha(double alpha) {
    if (!(alpha >= 0.0) || !std::isfinite(alpha) || alpha > 1e6) {
      throw std::invalid_argument(
          "cancel credit alpha must be finite and in [0, 1e6]");
    }
    cancel_credit_alpha_ = alpha;
  }

  void set_queue_init_fraction(double f) { queue_init_fraction_ = f; }

  void set_queue_accounting_on_l1(bool on) { queue_accounting_on_l1_ = on; }

  void set_tape_includes_own_orders(bool on) { tape_includes_own_orders_ = on; }

  void set_trade_netting_window_ns(int64_t ns) {
    if (ns < 0) throw std::invalid_argument("trade netting window must be >= 0");
    trade_netting_window_ns_ = ns;
  }

  void set_quote_lifecycle_output_path(std::filesystem::path output_path);
  void flush_quote_lifecycle();

  void on_pending(const PendingEvent& event) override;
  void on_pending_replace(const PendingReplaceEvent& event) override;
  void on_pending_cancel(const PendingCancelEvent& event) override;
  void on_rate_limited(const MessageSlot& event);

private:
  struct RestingOrder {
    int64_t order_id{0};
    int32_t instrument_id{0};
    Side     side{Side::Undefined};
    int64_t  price{0};
    int64_t  quantity{0};
    int64_t  leaves{0};
    bool     post_only{true};
    int64_t  ts_ns{0};
    int64_t  queue_ahead{0};
    int64_t  initial_queue_ahead{0};
    int64_t  displayed_depth_at_placement{0};
    int64_t  touch_depth_at_placement{0};
    int64_t  traded_ahead{0};
    int64_t  credited_cancellation{0};
    int64_t  capped_advancement{0};
    int64_t  queue_ahead_pre_obs{0};
    int32_t  next{-1};
    int32_t  prev{-1};
  };

  struct OrderArena {
    std::vector<RestingOrder> slots;
    std::vector<int32_t>      free_list;

    int32_t alloc() {
      if (!free_list.empty()) { const int32_t i = free_list.back(); free_list.pop_back(); return i; }
      slots.emplace_back();
      return static_cast<int32_t>(slots.size() - 1);
    }
    void release(int32_t i) { free_list.push_back(i); }
    RestingOrder&       operator[](int32_t i)       { return slots[static_cast<std::size_t>(i)]; }
    const RestingOrder& operator[](int32_t i) const { return slots[static_cast<std::size_t>(i)]; }
  };

  struct LevelFifo {
    int32_t head{-1};
    int32_t tail{-1};
    int64_t last_displayed{-1};
    int64_t traded_since_obs{0};
    int64_t last_obs_ts_ns{0};
    int64_t unexplained_recent{0};
  };

  using PriceMapAsc  = std::map<int64_t, LevelFifo>;
  using PriceMapDesc = std::map<int64_t, LevelFifo, std::greater<int64_t>>;

  struct Book {
    PriceMapDesc bids;
    PriceMapAsc  asks;
  };

  OrderArena arena_;
  std::unordered_map<int64_t, int32_t> rest_index_;

  void send_accepted(int64_t order_id, int32_t instrument_id,
                     int64_t now_ns);
  void send_rejected(int64_t order_id, RejectReason reason, int64_t now_ns);
  void send_replace_accepted(int64_t order_id, int64_t request_id,
                             int32_t instrument_id, int64_t now_ns);
  void send_replace_rejected(int64_t order_id, int64_t request_id,
                             RejectReason reason, int64_t now_ns);
  void send_cancel_accepted(int64_t order_id, int64_t request_id,
                            CancelReason reason, int64_t now_ns);
  void send_cancel_rejected(int64_t order_id, int64_t request_id,
                            RejectReason reason, int64_t now_ns);
  void send_executed(int64_t order_id, Side side, int32_t instrument_id,
                     int64_t px, int64_t qty, int64_t now_ns,
                     bool taker = false);

  void ensure_quote_lifecycle_stream_open();
  void write_quote_lifecycle_row(const char* event_type,
                                 int64_t timestamp_ns,
                                 int64_t order_id,
                                 int32_t instrument_id,
                                 Side side,
                                 int64_t price,
                                 int64_t quantity,
                                 int64_t leaves,
                                 int64_t quote_created_ns,
                                 int64_t initial_queue_ahead,
                                 int64_t queue_ahead,
                                 int64_t displayed_depth_at_placement,
                                 const char* terminal_reason,
                                 int64_t request_id = 0,
                                 int64_t bbo_bid = 0,
                                 int64_t bbo_ask = 0,
                                 int64_t trade_price = 0,
                                 int64_t trade_qty = 0,
                                 Side trade_side = Side::Undefined,
                                 int64_t touch_depth_at_placement = 0,
                                 int64_t traded_ahead = 0,
                                 int64_t credited_cancellation = 0,
                                 int64_t capped_advancement = 0);
  void write_quote_lifecycle_row(const char* event_type,
                                 int64_t timestamp_ns,
                                 const RestingOrder& order,
                                 int64_t quantity,
                                 const char* terminal_reason,
                                 int64_t request_id = 0,
                                 int64_t bbo_bid = 0,
                                 int64_t bbo_ask = 0,
                                 int64_t trade_price = 0,
                                 int64_t trade_qty = 0,
                                 Side trade_side = Side::Undefined);

  void handle_market_order(const PendingEvent& e, int64_t now_ns);
  void handle_limit_order(const PendingEvent& e, int64_t now_ns);
  void rest_post_only_limit(const PendingEvent& e, int64_t now_ns);
  void rest_order(const PendingEvent& e, int64_t leaves, bool post_only, int64_t now_ns);
  int64_t sweep_displayed_book(const PendingEvent& e, int64_t limit_price, int64_t now_ns);
  void send_ioc_remainder_cancel(const PendingEvent& e, int64_t qty_left, int64_t now_ns);

  void link_back(LevelFifo& level, int32_t idx);
  void unlink(LevelFifo& level, int32_t idx);

  void consume_level(LevelFifo& level, int64_t& remaining, int64_t now_ns, const TradeEvent& trigger);
  void consume_at_price(LevelFifo& level, int64_t& remaining, int64_t print_size,
                        int64_t now_ns, const TradeEvent& trigger);
  void consume_level_anticipated(LevelFifo& level, int64_t& anticipated,
                                 int64_t now_ns, const TradeEvent& trigger);
  void pop_front_filled(LevelFifo& level, int32_t idx);
  void observe_level(int32_t instrument_id, Side side, int64_t price,
                     LevelFifo& level, int64_t now_ns);
  void fill_level_through(LevelFifo& level, int64_t& remaining,
                          int64_t now_ns, const TradeEvent& trigger);
  void require_l2_provenance_for_behind_touch(int32_t instrument_id, Side side,
                                              int64_t price) const;
  int64_t displayed_depth_at(int32_t instrument_id, Side side, int64_t price);
  void apply_queue_accounting(int32_t instrument_id);
  int64_t initial_queue_ahead(int32_t instrument_id, Side side, int64_t price);


  std::shared_ptr<spdlog::logger> logger_;
  std::shared_ptr<ClockInterface> clock_;
  ExchangeResponseHandler* response_handler_;
  OrderBookManager order_book_manager_;
  QueueModel queue_model_{QueueModel::Pessimistic};
  double cancel_credit_alpha_{0.0};
  double queue_init_fraction_{1.0};
  bool queue_accounting_on_l1_{false};
  int64_t trade_netting_window_ns_{0};
  bool tape_includes_own_orders_{false};
  std::filesystem::path quote_lifecycle_path_;
  std::ofstream quote_lifecycle_stream_;
  uint64_t quote_lifecycle_count_{0};

  std::unordered_map<int32_t, Book> resting_;

  std::vector<marketdata::IncrementalOrderBook::Level> sweep_fills_;
};

}
