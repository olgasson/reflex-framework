
#pragma once
#include "messages.hpp"
#include "domain/order.hpp"

namespace reflex {

class EventListener {

  public:

  virtual ~EventListener() = default;


};

class HeartbeatListener {
  public:
  virtual ~HeartbeatListener() = default;
  virtual void on_heartbeat(const HeartbeatEvent* event) = 0;
};

class OrderEventListener {
  public:
  virtual ~OrderEventListener() = default;

  virtual void on_pending(const PendingEvent* event) = 0;
  virtual void on_accepted(const AcceptedEvent* event) = 0;
  virtual void on_rejected(const RejectedEvent* event) = 0;
  virtual void on_pending_replace(const PendingReplaceEvent* event) = 0;
  virtual void on_replace_accepted(const ReplaceAcceptedEvent* event) = 0;
  virtual void on_replace_rejected(const ReplaceRejectedEvent* event) = 0;
  virtual void on_pending_cancel(const PendingCancelEvent* event) = 0;
  virtual void on_cancel_accepted(const CancelAcceptedEvent* event) = 0;
  virtual void on_cancel_rejected(const CancelRejectedEvent* event) = 0;
  virtual void on_executed(const ExecutedEvent* event) = 0;
};

class OrderManagementListener {
public:
  virtual ~OrderManagementListener() = default;

  virtual void on_pending(const Order& /*order*/, const PendingEvent& /*event*/) {}
  virtual void on_accepted(const Order& /*order*/, const AcceptedEvent& /*event*/) {}
  virtual void on_rejected(const Order& /*order*/, const RejectedEvent& /*event*/) {}

  virtual void on_pending_replace(const Order& /*original*/,
                                  const Order& /*replacement*/,
                                  const PendingReplaceEvent& /*event*/) {}

  virtual void on_replace_accepted(const Order& /*replacement*/,
                                   const ReplaceAcceptedEvent& /*event*/) {}

  virtual void on_replace_rejected(const Order& /*original*/,
                                   const ReplaceRejectedEvent& /*event*/) {}

  virtual void on_pending_cancel(const Order& /*order*/,
                                 const PendingCancelEvent& /*event*/) {}

  virtual void on_cancel_accepted(const Order& /*order*/,
                                  const CancelAcceptedEvent& /*event*/) {}

  virtual void on_cancel_rejected(const Order& /*order*/,
                                  const CancelRejectedEvent& /*event*/) {}

  virtual void on_executed(const Order& /*order*/, const ExecutedEvent& /*event*/) {}
};

class AlgoOrderManagementListener {
public:
  virtual ~AlgoOrderManagementListener() = default;

  virtual void on_accepted(const Order& /*order*/, const AcceptedEvent& /*event*/) {}
  virtual void on_rejected(const Order& /*order*/, const RejectedEvent& /*event*/) {}

  virtual void on_replace_accepted(const Order& /*replacement*/,
                                   const ReplaceAcceptedEvent& /*event*/) {}

  virtual void on_replace_rejected(const Order& /*original*/,
                                   const ReplaceRejectedEvent& /*event*/) {}

  virtual void on_cancel_accepted(const Order& /*order*/,
                                  const CancelAcceptedEvent& /*event*/) {}

  virtual void on_cancel_rejected(const Order& /*order*/,
                                  const CancelRejectedEvent& /*event*/) {}

  virtual void on_executed(const Order& /*order*/, const ExecutedEvent& /*event*/) {}
};

class GatewayOrderEventListener {
public:
  virtual ~GatewayOrderEventListener() = default;

  virtual void on_pending(const PendingEvent& /*event*/) {}

  virtual void on_pending_replace(const PendingReplaceEvent& /*event*/) {}

  virtual void on_pending_cancel(const PendingCancelEvent& /*event*/) {}
};

// Market data event listeners
class MarketDataListener {
public:
  virtual ~MarketDataListener() = default;
  virtual void on_trade(const TradeEvent& event) = 0;
  virtual void on_l1_update(const L1UpdateEvent& event) = 0;
  virtual void on_l2_update(const L2UpdateEvent& event) = 0;
};


}