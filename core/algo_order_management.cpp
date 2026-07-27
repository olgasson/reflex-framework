// algo_order_management.cpp
#include "domain/algo_order_management.hpp"

#include "logger_factory.hpp"
#include "spdlog/fmt/bundled/ostream.h"

#include <algorithm>

namespace reflex {


AlgoOrderManagement::AlgoOrderManagement(std::shared_ptr<OrderWriter> writer, bool keep_filled_and_dead)
    : writer_(std::move(writer)),
      order_pool_(std::make_unique<OrderPool>(10000)),
      keep_filled_and_dead_(keep_filled_and_dead),
      next_order_id_(1) {

  logger_ = LoggerFactory::getLogger("AlgoOrderManagement");
}

void AlgoOrderManagement::add_listener(AlgoOrderManagementListener* listener) {
  listeners_.emplace_back(listener);
}

int64_t AlgoOrderManagement::send_pending(const int32_t instrument_id, const Side side, const int64_t quantity,
                                       const int64_t price,
                                       const OrderType order_type,
                                       const TimeInForce time_in_force,
                                       const int32_t account,
                                       const ExecInst exec_inst) {
  // Generate order ID and get order from pool
  const int64_t order_id = generate_order_id();
  Order* order = order_pool_->acquire();
  order->reset();

  order->order_id_ = order_id;
  order->instrument_id_ = instrument_id;
  order->account_ = account;
  order->side_ = side;
  order->quantity_ = quantity;
  order->leaves_quantity_ = quantity;
  order->price_ = price;
  order->order_type_ = order_type;
  order->time_in_force_ = time_in_force;
  order->exec_inst_ = exec_inst;
  order->order_state_ = OrderState::New;
  order->order_wait_state_ = OrderWaitState::Pending;

  // Store order
  order_store_[order_id] = order;

  logger_->debug("Sending pending: {}", fmt::streamed(*order));
  // Delegate to RingBufferWriter (transport layer)
  writer_->send_pending(*order);

  return order_id;
}

void AlgoOrderManagement::send_pending_cancel(const int64_t order_id) {
  Order* order = get_order(order_id);
  if (!order) return;

  order->order_state_ = OrderState::Working;
  order->order_wait_state_ = OrderWaitState::PendingCancel;

  logger_->debug("Sending pending_cancel: {}", fmt::streamed(*order));
  writer_->send_pending_cancel(*order);
}

bool AlgoOrderManagement::send_pending_replace(const int64_t order_id, const int32_t /*instrument_id*/, const int64_t price,
                                               const int64_t quantity) {
  Order* original = get_order(order_id);
  if (!original) {
    logger_->error("Order not found in store for order_id: {}", order_id);
    return false;
  }
  if (original->order_wait_state_ == OrderWaitState::PendingReplace) {
    // A second in-flight replace would overwrite the -order_id map slot and
    // leak the pooled replacement Order - reject it instead.
    logger_->warn("Rejecting overlapping replace for order_id {}: a replace is already pending", order_id);
    return false;
  }

  original->order_state_ = OrderState::Working;
  original->order_wait_state_ = OrderWaitState::PendingReplace;
  remove_request_id(original->request_id_);
  original->request_id_ = generate_order_id();

  const int64_t replacement_order_id = -order_id;

  Order* replacement = order_pool_->acquire();
  *replacement = *original;
  replacement->order_id_ = original->order_id_;
  replacement->request_id_ = original->request_id_;
  replacement->price_ = price;
  replacement->quantity_ = quantity;
  // Replacement leaves = new order quantity minus what has already filled on
  // the original. Clamped at 0: filled beyond the new quantity means the
  // replacement is effectively fully filled.
  const int64_t filled_quantity = original->quantity_ - original->leaves_quantity_;
  replacement->leaves_quantity_ = std::max<int64_t>(quantity - filled_quantity, 0);
  replacement->order_state_ = OrderState::Working;
  replacement->order_wait_state_ = OrderWaitState::PendingReplace;
  replacement->parent_id_ = original->parent_id_;

  order_store_[replacement_order_id] = replacement;
  register_request_id(replacement->request_id_, original->order_id_);

  logger_->debug("Sending pending_replace: {}", fmt::streamed(*replacement));

  writer_->send_pending_replace(*replacement);
  return true;
}

void AlgoOrderManagement::on_accepted(const AcceptedEvent* event) {
  Order* order = get_order(event->order_id_);
  if (!order) return;
  order->exchange_order_id_ = event->exchange_order_id_;
  order->order_state_ = OrderState::Working;
  order->order_wait_state_ = OrderWaitState::None;

  logger_->debug("on_accepted {}", fmt::streamed(*event));

  for (const auto listener : listeners_) {
    listener->on_accepted(*order, *event);
  }
}

void AlgoOrderManagement::on_rejected(const RejectedEvent* event) {
  Order* order = get_order(event->order_id_);
  if (!order) return;
  order->order_state_ = OrderState::Rejected;
  order->order_wait_state_ = OrderWaitState::None;
  // Handle order cleanup if not keeping filled/dead orders
  logger_->debug("on_rejected {}", fmt::streamed(*event));

  for (const auto listener : listeners_) {
    listener->on_rejected(*order, *event);
  }

  if (!keep_filled_and_dead_) {
    order_store_.erase(order->order_id_);
    order_pool_->release(order);
  }
}

void AlgoOrderManagement::on_replace_accepted(const ReplaceAcceptedEvent* event) {
  Order* original = get_order(event->order_id_);
  if (!original) return;
  Order* replacement = get_order(-1 * static_cast<int64_t>(event->order_id_));
  if (!replacement) return;

  const auto original_order_id = original->order_id_;

  replacement->order_id_ = original_order_id;
  replacement->order_wait_state_ = OrderWaitState::None;
  replacement->order_state_ = OrderState::Working;

  // Leaves = new order quantity minus what has filled on the original so far
  // (fills between the replace request and its acceptance land on the
  // original). Clamped at 0 for overfilled-beyond-new-quantity.
  const int64_t filled_quantity = original->quantity_ - original->leaves_quantity_;
  replacement->leaves_quantity_ = std::max<int64_t>(replacement->quantity_ - filled_quantity, 0);

  // Replace original with updated replacement
  order_store_[original_order_id] = replacement;
  order_store_.erase(-original_order_id);
  order_pool_->release(original);

  logger_->debug("on_replace_accepted {}", fmt::streamed(*event));

  for (const auto listener : listeners_) {
    listener->on_replace_accepted(*replacement, *event);
  }
}

void AlgoOrderManagement::on_replace_rejected(const ReplaceRejectedEvent* event) {
  Order* replacement = get_order(-1 * static_cast<int64_t>(event->order_id_));
  if (!replacement) return;
  Order* original = get_order(event->order_id_);
  if (!original) {
    if (replacement->order_wait_state_ == OrderWaitState::PendingReplace) {
      order_store_.erase(-replacement->order_id_);
      order_pool_->release(replacement);
    }
    return;
  }
  original->order_wait_state_ = OrderWaitState::None;

  logger_->debug("on_replace_rejected {}", fmt::streamed(*event));

  for (const auto listener : listeners_) {
    listener->on_replace_rejected(*original, *event);
  }

  order_store_.erase(-replacement->order_id_);
  order_pool_->release(replacement);
}

void AlgoOrderManagement::on_cancel_accepted(const CancelAcceptedEvent* event) {
  Order* order = get_order(event->order_id_);
  if (!order) return;
  order->order_state_ = OrderState::Cancelled;
  order->order_wait_state_ = OrderWaitState::None;

  logger_->debug("on_cancel_accepted {}", fmt::streamed(*event));

  for (auto listener : listeners_) {
    listener->on_cancel_accepted(*order, *event);
  }

  if (!keep_filled_and_dead_) {
    order_store_.erase(order->order_id_);
    order_pool_->release(order);
  }
}

void AlgoOrderManagement::on_cancel_rejected(const CancelRejectedEvent* event) {
  Order* order = get_order(event->order_id_);
  if (!order) return;
  order->order_state_ = OrderState::Working;
  order->order_wait_state_ = OrderWaitState::None;

  logger_->debug("on_cancel_rejected {}", fmt::streamed(*event));

  for (const auto listener : listeners_) {
    listener->on_cancel_rejected(*order, *event);
  }
}

void AlgoOrderManagement::on_executed(const ExecutedEvent* event) {
  Order* order = get_order(event->order_id_);
  if (!order) return;
  order->leaves_quantity_ -= event->last_quantity_;
  if (order->leaves_quantity_ < 0) {
    // Overfill: without the clamp the order never reaches the == 0 cleanup
    // below and leaks from the pool.
    logger_->error("Overfill on order {}: last_quantity {} exceeds leaves; clamping leaves to 0",
                   event->order_id_, event->last_quantity_);
    order->leaves_quantity_ = 0;
  }
  if (order->leaves_quantity_ == 0) {
    order->order_state_ = OrderState::Filled;
    order->order_wait_state_ = OrderWaitState::None;
  }

  logger_->debug("on_executed {}", fmt::streamed(*event));

  for (const auto listener : listeners_) {
    listener->on_executed(*order, *event);
  }

  if (order->leaves_quantity_ == 0) {
    if (!keep_filled_and_dead_) {
      order_store_.erase(order->order_id_);
      order_pool_->release(order);
    }
  }
}

Order* AlgoOrderManagement::get_order(const int64_t order_id) {
  const auto it = order_store_.find(order_id);
  return (it != order_store_.end()) ? it->second : nullptr;
}

int64_t AlgoOrderManagement::generate_order_id() { return next_order_id_++; }

void AlgoOrderManagement::register_request_id(int64_t request_id, int64_t order_id) {
  request_id_to_order_id_[request_id] = order_id;
}

void AlgoOrderManagement::remove_request_id(int64_t request_id) {
  request_id_to_order_id_.erase(request_id);
}
}  // namespace reflex
