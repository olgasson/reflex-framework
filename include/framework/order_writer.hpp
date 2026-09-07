// include/framework/order_writer.hpp
#pragma once
#include "domain/order.hpp"
#include "messages.hpp"

namespace reflex {

class OrderWriter {
public:
  virtual ~OrderWriter() = default;

  virtual void send_pending(const Order& order) = 0;
  // `priority` travels with the cancel so a gateway can reserve
  // order-operation capacity for risk-reducing actions.
  virtual void send_pending_cancel(const Order& order,
                                   CancelPriority priority) = 0;
  virtual void send_pending_replace(const Order& order) = 0;
};

} // namespace reflex
