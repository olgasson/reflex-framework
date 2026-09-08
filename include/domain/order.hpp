#pragma once

#include <string>
#include <iostream>
#include <cstring>
#include "../messages.hpp"

namespace reflex {

struct Order {
  int64_t timestamp_ns_ = 0;
  int64_t order_id_ = 0;
  int64_t parent_id_ = 0;
  int64_t request_id_ = 0;
  int64_t price_ = 0;
  int64_t quantity_ = 0;
  int64_t leaves_quantity_ = 0;
  int64_t exchange_order_id_ = 0;
  int32_t instrument_id_ = 0;
  int32_t account_ = 0;
  Side side_ = Side::Undefined;
  OrderType order_type_ = OrderType::Undefined;
  TimeInForce time_in_force_ = TimeInForce::Undefined;
  ExecInst exec_inst_ = ExecInst::Undefined;
  OrderState order_state_ = OrderState::Undefined;
  OrderWaitState order_wait_state_ = OrderWaitState::Undefined;

  Order(const Order& other) = default;
  Order& operator=(const Order& other) = default;

  Order() = default;

  void reset() {
    timestamp_ns_ = 0;
    order_id_ = parent_id_ = request_id_ = exchange_order_id_ = 0;
    instrument_id_ = account_ = 0;
    price_ = quantity_ = leaves_quantity_ = 0;
    side_ = Side::Undefined;
    order_type_ = OrderType::Undefined;
    time_in_force_ = TimeInForce::Undefined;
    exec_inst_ = ExecInst::Undefined;
    order_state_ = OrderState::Undefined;
    order_wait_state_ = OrderWaitState::Undefined;
  }

  friend std::ostream& operator<<(std::ostream& os, const Order& o) {
    os << "Order{"
       << "timestamp=" << o.timestamp_ns_
       << "orderId=" << o.order_id_
       << ", parentId=" << o.parent_id_
       << ", requestId=" << o.request_id_
       << ", exchangeOrderId=" << o.exchange_order_id_
       << ", instrumentId=" << o.instrument_id_
       << ", account=" << o.account_
       << ", price=" << o.price_
       << ", quantity=" << o.quantity_
       << ", leavesQuantity=" << o.leaves_quantity_
       << ", side=" << o.side_
       << ", orderType=" << o.order_type_
       << ", timeInForce=" << o.time_in_force_
       << ", execInst=" << o.exec_inst_
       << ", orderState=" << o.order_state_
       << ", orderWaitState=" << o.order_wait_state_
       << "}";
    return os;
  }
};

}
