#pragma once
#include "event_listener.hpp"
#include "messages.hpp"
#include "offset_epoch_nano_clock.hpp"
#include "domain/algo_order_management.hpp"
#include "timer_manager.hpp"

#include <memory>
#include <utility>

namespace reflex {

class Strategy : public AlgoOrderManagementListener {
 public:
  explicit Strategy(ClockInterface* clock, TimerManager* timer_manager, AlgoOrderManagement* om) :
  clock_(clock),
  timer_manager_(timer_manager),
  om_(om) {}

  virtual ~Strategy() = default;

  virtual void on_start() {}
  virtual void on_stop() {}

  virtual void on_l1_update(const L1UpdateEvent& event) = 0;
  virtual void on_l2_update(const L2UpdateEvent& event) = 0;
  virtual void on_trade(const TradeEvent& event) = 0;
  virtual void on_mark_price(const MarkPriceEvent& ) {}
  virtual void on_funding_rate(const FundingRateEvent& ) {}
  virtual void on_open_interest(const OpenInterestEvent& ) {}
  virtual void on_liquidation(const LiquidationEvent& ) {}

  void on_accepted(const Order& , const AcceptedEvent& event) override = 0;
  void on_rejected(const Order& , const RejectedEvent& event) override = 0;
  void on_replace_accepted(const Order& , const ReplaceAcceptedEvent& event) override = 0;
  void on_replace_rejected(const Order& , const ReplaceRejectedEvent& event) override = 0;
  void on_cancel_accepted(const Order& , const CancelAcceptedEvent& event) override = 0;
  void on_cancel_rejected(const Order& , const CancelRejectedEvent& event) override = 0;
  void on_executed(const Order& , const ExecutedEvent& event) override = 0;

  AlgoOrderManagement* get_order_management() const { return om_; }

 protected:
  void register_as_listener() {
    if (om_) {
      om_->add_listener(this);
    }
  }

  ClockInterface* clock_;
  TimerManager* timer_manager_;
  AlgoOrderManagement* om_;


};

}
