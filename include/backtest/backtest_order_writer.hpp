#pragma once
#include "framework/ring_buffer_writer.hpp"
#include "domain/order.hpp"
#include "messages.hpp"
#include "backtest/backtest_engine.hpp"

namespace reflex::backtest {


class BacktestOrderWriter : public OrderWriter {
public:
  explicit BacktestOrderWriter(BackTestEngine* engine);

  void send_pending(const Order& order) override;
  void send_pending_cancel(const Order& order, CancelPriority priority) override;
  void send_pending_replace(const Order& order) override;

private:
  BackTestEngine* engine_;
};

}