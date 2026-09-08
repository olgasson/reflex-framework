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
  int64_t quote_size = 1'00000000;
  double max_position_contracts = 10.0;
};

class SimpleQuoterStrategy final : public Strategy {
 public:
  SimpleQuoterStrategy(ClockInterface* clock, TimerManager* tm, AlgoOrderManagement* om,
                       const SimpleQuoterConfig& cfg)
      : Strategy(clock, tm, om), cfg_(cfg), risk_(cfg.instrument_id) {
    register_as_listener();
  }


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


  void on_accepted(const Order&, const AcceptedEvent& e) override {
    if (bid_.order_id == e.order_id_) bid_.working = true;
    if (ask_.order_id == e.order_id_) ask_.working = true;
  }

  void on_rejected(const Order&, const RejectedEvent& e) override {
    clear_if_ours(e.order_id_);
  }

  void on_executed(const Order&, const ExecutedEvent& e) override {
    const double qty = static_cast<double>(e.last_quantity_) / 1e8;
    const double px = static_cast<double>(e.last_price_) / 1e8;
    const double dir = (e.side_ == Side::Buy) ? 1.0 : -1.0;
    position_contracts_ += dir * qty;
    risk_.on_fill({e.side_, qty, px, false, e.timestamp_ns_});
    ++fills_;

    Quote& q = (e.side_ == Side::Buy) ? bid_ : ask_;
    q.filled += e.last_quantity_;
    if (q.filled >= q.qty) q = Quote{};
  }

  void on_replace_accepted(const Order&, const ReplaceAcceptedEvent& e) override {
    if (bid_.order_id == e.order_id_) { bid_.price = bid_.pending_price; bid_.working = true; }
    if (ask_.order_id == e.order_id_) { ask_.price = ask_.pending_price; ask_.working = true; }
  }
  void on_replace_rejected(const Order&, const ReplaceRejectedEvent& e) override {
    if (bid_.order_id == e.order_id_) bid_.working = true;
    if (ask_.order_id == e.order_id_) ask_.working = true;
  }
  void on_cancel_accepted(const Order&, const CancelAcceptedEvent& e) override {
    clear_if_ours(e.order_id_);
  }
  void on_cancel_rejected(const Order&, const CancelRejectedEvent&) override {}


  const RiskEngine& risk_engine() const { return risk_; }
  double position_contracts() const { return position_contracts_; }
  uint64_t fills() const { return fills_; }

 private:
  struct Quote {
    int64_t order_id = 0;
    int64_t price = 0;
    int64_t pending_price = 0;
    int64_t qty = 0;
    int64_t filled = 0;
    bool working = false;
  };

  void quote_side(Quote& q, Side side, int64_t target_price) {
    const bool capped = (side == Side::Buy)
        ? position_contracts_ >= cfg_.max_position_contracts
        : position_contracts_ <= -cfg_.max_position_contracts;

    if (capped) {
      if (q.order_id != 0 && q.working) {
        om_->send_pending_cancel(q.order_id);
        q.working = false;
      }
      return;
    }

    if (q.order_id == 0) {
      const int64_t oid = om_->send_pending(cfg_.instrument_id, side, cfg_.quote_size,
                                            target_price, OrderType::Limit, TimeInForce::Gtc,
                                            cfg_.account, ExecInst::ParticipateDontInitiate);
      if (oid != 0) q = Quote{oid, target_price, target_price, cfg_.quote_size, 0, false};
      return;
    }

    if (q.working && q.price != target_price) {
      om_->send_pending_replace(q.order_id, cfg_.instrument_id, target_price,
                                cfg_.quote_size);
      q.pending_price = target_price;
      q.working = false;
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

}
