// examples/example_strategy.hpp
//
// SimpleQuoterStrategy — a minimal but complete strategy showing how to wire
// the framework pieces together:
//
//   * consume market data           (Strategy::on_l1_update / on_trade)
//   * place / replace / cancel      (AlgoOrderManagement::send_pending*)
//   * react to exchange responses   (on_accepted / on_executed / ...)
//   * track position and PnL        (RiskEngine, FIFO accounting)
//
// Logic: join the best bid and best ask with one post-only order each,
// re-pricing whenever the top of book moves, and stop quoting a side once
// inventory exceeds a cap. This is a demo of the framework API, not a
// profitable strategy.
#pragma once

#include <cmath>
#include <cstdint>
#include <cstdlib>

#include "framework/strategy.hpp"
#include "risk/risk_engine.hpp"

namespace reflex::examples {

struct SimpleQuoterConfig {
  int32_t instrument_id = 0;
  int32_t account = 0;
  int64_t quote_size = 1'00000000;        // 1 contract, fixed-point 1e8
  double max_position_contracts = 10.0;   // stop quoting a side beyond this
};

class SimpleQuoterStrategy final : public Strategy {
 public:
  SimpleQuoterStrategy(ClockInterface* clock, TimerManager* tm, AlgoOrderManagement* om,
                       const SimpleQuoterConfig& cfg)
      : Strategy(clock, tm, om), cfg_(cfg), risk_(cfg.instrument_id) {
    register_as_listener();
  }

  // --- Market data ----------------------------------------------------------

  void on_l1_update(const L1UpdateEvent& e) override {
    if (e.instrument_id_ != cfg_.instrument_id) return;
    best_bid_ = e.bid_price_;
    best_ask_ = e.offer_price_;
    if (best_bid_ <= 0 || best_ask_ <= 0) return;

    risk_.on_mark_price(static_cast<double>(best_bid_ + best_ask_) / 2.0 / 1e8);
    quote_side(bid_, Side::Buy, best_bid_);
    quote_side(ask_, Side::Sell, best_ask_);
  }

  void on_l2_update(const L2UpdateEvent&) override {}
  void on_trade(const TradeEvent&) override {}

  // --- Order-management callbacks -------------------------------------------

  void on_accepted(const Order&, const AcceptedEvent& e) override {
    if (bid_.order_id == e.order_id_) bid_.working = true;
    if (ask_.order_id == e.order_id_) ask_.working = true;
  }

  void on_rejected(const Order&, const RejectedEvent& e) override {
    clear_if_ours(e.order_id_);  // e.g. post-only would have crossed; requote on next tick
  }

  void on_executed(const Order&, const ExecutedEvent& e) override {
    const double qty = static_cast<double>(e.last_quantity_) / 1e8;
    const double px = static_cast<double>(e.last_price_) / 1e8;
    const double dir = (e.side_ == Side::Buy) ? 1.0 : -1.0;
    position_contracts_ += dir * qty;
    risk_.on_fill({e.side_, qty, px, /*taker=*/false, e.timestamp_ns_});
    ++fills_;

    Quote& q = (e.side_ == Side::Buy) ? bid_ : ask_;
    q.filled += e.last_quantity_;
    if (q.filled >= q.qty) q = Quote{};  // fully filled -> slot free to requote
  }

  void on_replace_accepted(const Order&, const ReplaceAcceptedEvent& e) override {
    if (bid_.order_id == e.order_id_) { bid_.price = bid_.pending_price; bid_.working = true; }
    if (ask_.order_id == e.order_id_) { ask_.price = ask_.pending_price; ask_.working = true; }
  }
  void on_replace_rejected(const Order&, const ReplaceRejectedEvent& e) override {
    // The exchange kept the ORIGINAL order resting (e.g. the new price would
    // have crossed post-only). Resume quoting at the old price — leaving
    // working=false here would silence that side forever.
    if (bid_.order_id == e.order_id_) bid_.working = true;
    if (ask_.order_id == e.order_id_) ask_.working = true;
  }
  void on_cancel_accepted(const Order&, const CancelAcceptedEvent& e) override {
    clear_if_ours(e.order_id_);
  }
  void on_cancel_rejected(const Order&, const CancelRejectedEvent&) override {}

  // --- Reporting -------------------------------------------------------------

  const RiskEngine& risk_engine() const { return risk_; }
  double position_contracts() const { return position_contracts_; }
  uint64_t fills() const { return fills_; }

 private:
  struct Quote {
    int64_t order_id = 0;
    int64_t price = 0;          // price the order is resting at
    int64_t pending_price = 0;  // price requested by an in-flight replace
    int64_t qty = 0;
    int64_t filled = 0;
    bool working = false;
  };

  // Keep one post-only order joined to `target_price`; replace it when the
  // book moves; withdraw it when inventory on that side is capped.
  void quote_side(Quote& q, Side side, int64_t target_price) {
    const bool capped = (side == Side::Buy)
        ? position_contracts_ >= cfg_.max_position_contracts
        : position_contracts_ <= -cfg_.max_position_contracts;

    if (capped) {
      if (q.order_id != 0 && q.working) {
        om_->send_pending_cancel(q.order_id);
        q.working = false;  // cancel in flight; slot cleared in on_cancel_accepted
      }
      return;
    }

    if (q.order_id == 0) {  // nothing resting -> place a fresh quote
      const int64_t oid = om_->send_pending(cfg_.instrument_id, side, cfg_.quote_size,
                                            target_price, OrderType::Limit, TimeInForce::Gtc,
                                            cfg_.account, ExecInst::ParticipateDontInitiate);
      if (oid != 0) q = Quote{oid, target_price, target_price, cfg_.quote_size, 0, false};
      return;
    }

    if (q.working && q.price != target_price) {  // book moved -> re-price
      // Amend semantics: quantity is the TOTAL order quantity, so leaves after
      // the replace = quote_size - filled. Keep `filled` as-is (resetting it
      // would double-count the earlier fills against the same order).
      om_->send_pending_replace(q.order_id, cfg_.instrument_id, target_price,
                                cfg_.quote_size);
      q.pending_price = target_price;  // q.price updates only on replace-accept
      q.working = false;  // becomes true again in on_replace_accepted/_rejected
    }
  }

  void clear_if_ours(int64_t order_id) {
    if (bid_.order_id == order_id) bid_ = Quote{};
    if (ask_.order_id == order_id) ask_ = Quote{};
  }

  SimpleQuoterConfig cfg_;
  RiskEngine risk_;
  Quote bid_;
  Quote ask_;
  int64_t best_bid_ = 0;
  int64_t best_ask_ = 0;
  double position_contracts_ = 0.0;
  uint64_t fills_ = 0;
};

}  // namespace reflex::examples
