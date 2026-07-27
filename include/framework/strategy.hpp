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

  // Lifecycle methods
  virtual void on_start() {}
  virtual void on_stop() {}

  // Raw MarketData - PURE VIRTUAL (must be implemented)
  virtual void on_l1_update(const L1UpdateEvent& event) = 0;
  virtual void on_l2_update(const L2UpdateEvent& event) = 0;
  virtual void on_trade(const TradeEvent& event) = 0;
  virtual void on_mark_price(const MarkPriceEvent& /*event*/) {}
  virtual void on_funding_rate(const FundingRateEvent& /*event*/) {}
  virtual void on_open_interest(const OpenInterestEvent& /*event*/) {}

  // OM callbacks - PURE VIRTUAL (must be implemented)
  void on_accepted(const Order& /*order*/, const AcceptedEvent& event) override = 0;
  void on_rejected(const Order& /*order*/, const RejectedEvent& event) override = 0;
  void on_replace_accepted(const Order& /*replacement*/, const ReplaceAcceptedEvent& event) override = 0;
  void on_replace_rejected(const Order& /*original*/, const ReplaceRejectedEvent& event) override = 0;
  void on_cancel_accepted(const Order& /*order*/, const CancelAcceptedEvent& event) override = 0;
  void on_cancel_rejected(const Order& /*order*/, const CancelRejectedEvent& event) override = 0;
  void on_executed(const Order& /*order*/, const ExecutedEvent& event) override = 0;

  // Getter for AlgoOrderManagement
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

}  // namespace reflex
