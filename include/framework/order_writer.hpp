// include/framework/order_writer.hpp
#pragma once
#include "domain/order.hpp"

namespace reflex {

class OrderWriter {
public:
  virtual ~OrderWriter() = default;

  virtual void send_pending(const Order& order) = 0;
  virtual void send_pending_cancel(const Order& order) = 0;
  virtual void send_pending_replace(const Order& order) = 0;
};

} // namespace reflex