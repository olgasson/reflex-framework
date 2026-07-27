// include/backtest/exchange_simulator.hpp
#pragma once

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
#include <functional>
#include <map>
#include <vector>

namespace reflex::backtest {

class ExchangeSimulator : public GatewayOrderEventListener {
public:
  explicit ExchangeSimulator(std::shared_ptr<reflex::ClockInterface> clock,
                             ExchangeResponseHandler* response_handler);

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

  // Initial queue position of a freshly-placed order, as a fraction of displayed depth at our price:
  //   1.0 = back of queue (price-time priority; correct, conservative default)
  //   0.5 = halfway up,  0.0 = front of queue.
  // This is the knob to CALIBRATE against real fill rates (it cannot be derived from aggregate L2).
  void set_queue_init_fraction(double f) { queue_init_fraction_ = f; }

  // Order flow from Algo OM
  void on_pending(const PendingEvent& event) override;
  void on_pending_replace(const PendingReplaceEvent& event) override;
  void on_pending_cancel(const PendingCancelEvent& event) override;

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
    int64_t  leaves{0};
    int64_t  cum_filled{0};    // lifetime filled quantity (drives OKX-style amend leaves accounting)
    bool     post_only{true};
    int64_t  ts_ns{0};
    int64_t  queue_ahead{0};   // est. market volume ahead of us in the FIFO at our price (aggregate-L2 proxy)
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
  struct LevelFifo { int32_t head{-1}; int32_t tail{-1}; };

  using PriceMapAsc  = std::map<int64_t, LevelFifo>;                        // asks: low->high
  using PriceMapDesc = std::map<int64_t, LevelFifo, std::greater<int64_t>>; // bids: high->low

  struct Book {
    PriceMapDesc bids;
    PriceMapAsc  asks;
  };

  OrderArena arena_;
  std::unordered_map<int64_t, int32_t> rest_index_; // order_id -> arena index

  // Helpers
  void send_accepted(int64_t order_id, int64_t now_ns);
  void send_rejected(int64_t order_id, RejectReason reason, int64_t now_ns);
  void send_replace_accepted(int64_t order_id, int64_t now_ns);
  void send_replace_rejected(int64_t order_id, RejectReason reason, int64_t now_ns);
  void send_cancel_accepted(int64_t order_id, int64_t now_ns);
  void send_cancel_rejected(int64_t order_id, RejectReason reason, int64_t now_ns);
  void send_executed(int64_t order_id, Side side, int32_t instrument_id,
                     int64_t px, int64_t qty, int64_t now_ns);

  // Matching helpers
  void handle_market_order(const PendingEvent& e, int64_t now_ns);
  void handle_limit_order(const PendingEvent& e, int64_t now_ns);
  void rest_post_only_limit(const PendingEvent& e, int64_t now_ns);
  void rest_order(const PendingEvent& e, int64_t leaves, bool post_only, int64_t now_ns);
  // Taker sweep of the displayed book, bounded by limit_price (pass INT64_MAX
  // for a market buy, INT64_MIN for a market sell). Returns the unfilled remainder.
  int64_t sweep_displayed_book(const PendingEvent& e, int64_t limit_price, int64_t now_ns);

  // Intrusive FIFO helpers
  void link_back(LevelFifo& level, int32_t idx);  // append to tail
  void unlink(LevelFifo& level, int32_t idx);      // detach from list

  // Queue-aware fill helpers
  void consume_level(LevelFifo& level, int64_t& remaining, int64_t now_ns);
  int64_t displayed_depth_at(int32_t instrument_id, Side side, int64_t price);
  int64_t initial_queue_ahead(int32_t instrument_id, Side side, int64_t price);


  std::shared_ptr<spdlog::logger> logger_;
  std::shared_ptr<ClockInterface> clock_;
  ExchangeResponseHandler* response_handler_;
  OrderBookManager order_book_manager_;
  QueueModel queue_model_{QueueModel::Pessimistic};
  double queue_init_fraction_{1.0};   // 1.0 = join at back of queue (price-time priority)

  // Our own resting orders per instrument
  std::unordered_map<int32_t, Book> resting_;

  // Scratch buffer for taker sweeps (fills recorded during iteration, applied
  // to the displayed book afterwards) — member to avoid per-order allocation.
  std::vector<marketdata::IncrementalOrderBook::Level> sweep_fills_;
};

} // namespace reflex::backtest