#pragma once
#include "domain/order.hpp"
#include "messages.hpp"

namespace reflex {

class OrderWriter {
public:
  virtual ~OrderWriter() = default;

  virtual void send_pending(const Order& order) = 0;
  virtual void send_pending_cancel(const Order& order,
                                   CancelPriority priority) = 0;
  virtual void send_pending_replace(const Order& order) = 0;
};

}
