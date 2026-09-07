#include "backtest/backtest_order_writer.hpp"

#include "backtest/backtest_engine.hpp"

namespace reflex::backtest {

BacktestOrderWriter::BacktestOrderWriter(BackTestEngine* engine) {
  engine_ = engine;
}
void BacktestOrderWriter::send_pending(const Order& order) {

  // Create MessageSlot and use placement new (like RingBufferWriter does)
  MessageSlot order_slot;

  // Use placement new to construct PendingEvent directly in the slot
  auto* const event = new (order_slot.raw_data()) PendingEvent();

  // Copy fields from Order to PendingEvent
  event->order_id_ = order.order_id_;
  event->parent_id_ = order.parent_id_;
  event->request_id_ = order.request_id_;
  event->price_ = order.price_;
  event->quantity_ = order.quantity_;
  event->instrument_id_ = order.instrument_id_;
  // event->account_ = order.account_id_;
  event->side_ = order.side_;
  event->order_type_ = order.order_type_;
  event->time_in_force_ = order.time_in_force_;
  event->exec_inst_ = order.exec_inst_;
  event->timestamp_ns_ = engine_->get_current_time_ns();

  // Pass directly to engine - it will handle the latency
  engine_->add_strategy_order_event(order_slot);

}

void BacktestOrderWriter::send_pending_cancel(const Order& order, CancelPriority priority) {
  MessageSlot cancel_slot;

  auto* const event = new (cancel_slot.raw_data()) PendingCancelEvent();

  event->order_id_ = order.order_id_;
  event->request_id_ = order.request_id_;
  event->priority_ = priority;
  event->timestamp_ns_ = engine_->get_current_time_ns();

  engine_->add_strategy_order_event(cancel_slot);

}

void BacktestOrderWriter::send_pending_replace(const Order& order) {
  MessageSlot replace_slot;

  auto* const event = new (replace_slot.raw_data()) PendingReplaceEvent();

  event->order_id_ = order.order_id_;
  event->request_id_ = order.request_id_;
  event->quantity_ = order.quantity_;
  event->price_ = order.price_;
  event->instrument_id_ = order.instrument_id_;
  event->timestamp_ns_ = engine_->get_current_time_ns();

  engine_->add_strategy_order_event(replace_slot);

}

} // namespace reflex