// include/backtest/exchange_simulator.hpp
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

  // Market data processing (called by BackTestEngine)
  void process_l1_update(const L1UpdateEvent& event);
  void process_l2_update(const L2UpdateEvent& event);
  void process_trade_event(const TradeEvent& event);

  // Fill-model configuration: how cancellations move our queue position.
  // Aggregate L2 can't tell whether a cancel was ahead of or behind us, so we offer the two bounds:
  //   Pessimistic = assume cancels are BEHIND us; queue drains only via trades -> slowest, conservative fills.
  //   Optimistic  = assume cancels are AHEAD of us; queue_ahead capped at displayed depth -> fastest fills.
  // Truth is between them; run both as a sensitivity band.
  enum class QueueModel { Pessimistic, Optimistic };
  void set_queue_model(QueueModel m) { queue_model_ = m; }

  // Bounded cancellation attribution for market-by-price replay. Aggregate
  // L2 reveals how much depth disappeared, but not whether it was ahead of
  // us. For queue percentile x = queue_ahead / displayed_before, credit the
  // observed cancellation by p(x) = x^alpha. The credit is therefore always
  // in [0, observed cancellation] and can never manufacture queue removal.
  //   0             = disabled/pessimistic behavior (default)
  //   0 < alpha < 1 = cancellations biased ahead of us
  //   alpha == 1    = uniform cancellation attribution
  //   alpha > 1     = cancellations biased behind us
  // Alpha should be calibrated against observed order lifecycles, never
  // against PnL.
  void set_cancel_credit_alpha(double alpha) {
    if (!(alpha >= 0.0) || !std::isfinite(alpha) || alpha > 1e6) {
      throw std::invalid_argument(
          "cancel credit alpha must be finite and in [0, 1e6]");
    }
    cancel_credit_alpha_ = alpha;
  }

  // Initial queue position of a freshly-placed order, as a fraction of displayed depth at our price:
  //   1.0 = back of queue (price-time priority; correct, conservative default)
  //   0.5 = halfway up,  0.0 = front of queue.
  // This is the knob to CALIBRATE against real fill rates (it cannot be derived from aggregate L2).
  void set_queue_init_fraction(double f) { queue_init_fraction_ = f; }

  // Also run queue accounting (optimistic cap / cancel credit) on every L1
  // (top-of-book) observation, not only on L2 diffs. The L1 stream is
  // typically real-time while depth diffs are sampled, so this exposes
  // intra-interval touch churn. It is also REQUIRED on L1-only tapes:
  // without it the Optimistic model never sees a displayed-depth reduction
  // and silently degrades to Pessimistic. When enabled, the L1 feed is
  // treated as authoritative for the touch and trade prints are no longer
  // subtracted from the book heuristically (see process_trade_event).
  void set_queue_accounting_on_l1(bool on) { queue_accounting_on_l1_ = on; }

  // Captures taken while our own orders were live display OUR OWN resting
  // quantity inside the level size. The physical bound then has to exclude
  // our resting quantity at that level, otherwise a level that is only us
  // caps queue_ahead at our own size instead of zero.
  void set_tape_includes_own_orders(bool on) { tape_includes_own_orders_ = on; }

  // Trade netting window for queue accounting. A public print and the book
  // observation that already reflects it arrive within ~1 ms of each other in
  // either order. A print arriving within this window after an observation
  // that showed an unexplained reduction is applied to each order's
  // PRE-observation position (it consumed the front of the level, which the
  // observation already removed) instead of draining the capped/credited
  // position a second time. 0 disables netting (double-count hazard).
  void set_trade_netting_window_ns(int64_t ns) {
    if (ns < 0) throw std::invalid_argument("trade netting window must be >= 0");
    trade_netting_window_ns_ = ns;
  }

  // Optional per-order lifecycle CSV (accept / fill / cancel / replace rows
  // with queue-position provenance) for offline fill-model diagnostics.
  void set_quote_lifecycle_output_path(std::filesystem::path output_path);
  void flush_quote_lifecycle();

  // Order flow from Algo OM
  void on_pending(const PendingEvent& event) override;
  void on_pending_replace(const PendingReplaceEvent& event) override;
  void on_pending_cancel(const PendingCancelEvent& event) override;
  // An operation the local gateway model refused to send (rate limit): the
  // matching reject is produced here so the request identity is preserved.
  void on_rate_limited(const MessageSlot& event);

private:
  // ---- Resting order state ----
  // Orders live in an index-stable arena and are threaded into per-price FIFO
  // lists via next/prev indices (intrusive). This drops the per-order heap node
  // that the std::deque levels allocated, and makes cancel/replace O(1) (the
  // order is located directly through rest_index_) instead of scanning a level.
  struct RestingOrder {
    int64_t order_id{0};
    int32_t instrument_id{0};
    Side     side{Side::Undefined};
    int64_t  price{0};
    int64_t  quantity{0};          // current total order quantity (cum filled = quantity - leaves)
    int64_t  leaves{0};
    bool     post_only{true};
    int64_t  ts_ns{0};
    int64_t  queue_ahead{0};   // est. market volume ahead of us in the FIFO at our price (aggregate-L2 proxy)
    int64_t  initial_queue_ahead{0};
    int64_t  displayed_depth_at_placement{0};
    int64_t  touch_depth_at_placement{0};
    int64_t  traded_ahead{0};
    int64_t  credited_cancellation{0};
    int64_t  capped_advancement{0};    // advancement from the displayed-depth cap (provenance)
    int64_t  queue_ahead_pre_obs{0};   // position before the latest observation (trade netting)
    int32_t  next{-1};         // next order at the same price level (-1 = none)
    int32_t  prev{-1};         // previous order at the same price level (-1 = none)
  };

  // Slab of RestingOrder with a freelist. Orders are referenced by index, so a
  // vector reallocation on growth does not invalidate outstanding handles.
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

  // Head/tail arena indices for one price level's FIFO (-1 = empty).
  // last_displayed / traded_since_obs carry the cancel-credit accounting:
  // the displayed depth when this level was last observed, and the volume
  // that has actually TRADED at it since. -1 = never observed.
  struct LevelFifo {
    int32_t head{-1};
    int32_t tail{-1};
    int64_t last_displayed{-1};
    int64_t traded_since_obs{0};
    int64_t last_obs_ts_ns{0};        // time of the last observation with an unexplained reduction
    int64_t unexplained_recent{0};    // that reduction, not yet explained by a later print
  };

  using PriceMapAsc  = std::map<int64_t, LevelFifo>;                        // asks: low->high
  using PriceMapDesc = std::map<int64_t, LevelFifo, std::greater<int64_t>>; // bids: high->low

  struct Book {
    PriceMapDesc bids;
    PriceMapAsc  asks;
  };

  OrderArena arena_;
  std::unordered_map<int64_t, int32_t> rest_index_; // order_id -> arena index

  // Helpers
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

  // Matching helpers
  void handle_market_order(const PendingEvent& e, int64_t now_ns);
  void handle_limit_order(const PendingEvent& e, int64_t now_ns);
  void rest_post_only_limit(const PendingEvent& e, int64_t now_ns);
  void rest_order(const PendingEvent& e, int64_t leaves, bool post_only, int64_t now_ns);
  // Taker sweep of the displayed book, bounded by limit_price (pass INT64_MAX
  // for a market buy, INT64_MIN for a market sell). Returns the unfilled remainder.
  int64_t sweep_displayed_book(const PendingEvent& e, int64_t limit_price, int64_t now_ns);
  void send_ioc_remainder_cancel(const PendingEvent& e, int64_t qty_left, int64_t now_ns);

  // Intrusive FIFO helpers
  void link_back(LevelFifo& level, int32_t idx);  // append to tail
  void unlink(LevelFifo& level, int32_t idx);      // detach from list

  // Queue-aware fill helpers
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
  double queue_init_fraction_{1.0};   // 1.0 = join at back of queue (price-time priority)
  bool queue_accounting_on_l1_{false};
  int64_t trade_netting_window_ns_{0};
  bool tape_includes_own_orders_{false};
  std::filesystem::path quote_lifecycle_path_;
  std::ofstream quote_lifecycle_stream_;
  uint64_t quote_lifecycle_count_{0};

  // Our own resting orders per instrument
  std::unordered_map<int32_t, Book> resting_;

  // Scratch buffer for taker sweeps (fills recorded during iteration, applied
  // to the displayed book afterwards) — member to avoid per-order allocation.
  std::vector<marketdata::IncrementalOrderBook::Level> sweep_fills_;
};

} // namespace reflex::backtest
