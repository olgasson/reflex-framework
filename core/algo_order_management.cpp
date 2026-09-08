#include "domain/algo_order_management.hpp"

#include "logger_factory.hpp"
#include "spdlog/fmt/bundled/ostream.h"

#include <algorithm>

namespace reflex {

namespace {

constexpr size_t kMaxTerminalFillRequestTombstones = 4096;

bool is_terminal(OrderState state) noexcept {
  return state == OrderState::Rejected || state == OrderState::Filled ||
         state == OrderState::Cancelled;
}

size_t reject_reason_index(RejectReason reason) noexcept {
  const auto value = static_cast<int>(reason);
  return value >= static_cast<int>(RejectReason::Unknown) &&
                 value <= static_cast<int>(RejectReason::OrderUnknown)
             ? static_cast<size_t>(value)
             : static_cast<size_t>(RejectReason::Unknown);
}

}

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

  order_store_[order_id] = order;
  seen_execution_ids_.erase(order_id);

  logger_->debug("Sending pending: {}", fmt::streamed(*order));
  writer_->send_pending(*order);

  return order_id;
}

void AlgoOrderManagement::send_pending_cancel(const int64_t order_id, const CancelPriority priority) {
  Order* order = get_order(order_id);
  if (!order) return;
  if (is_terminal(order->order_state_)) {
    logger_->warn("Ignoring cancel request for terminal order {}", order_id);
    return;
  }

  release_replace_shadow(order_id);
  clear_request_id(*order);
  order->request_id_ = generate_order_id();
  register_request_id(order->request_id_, order_id);
  order->order_state_ = OrderState::Working;
  order->order_wait_state_ = OrderWaitState::PendingCancel;

  logger_->debug("Sending pending_cancel: {}", fmt::streamed(*order));
  writer_->send_pending_cancel(*order, priority);
}

bool AlgoOrderManagement::send_pending_replace(const int64_t order_id, const int32_t , const int64_t price,
                                               const int64_t quantity) {
  Order* original = get_order(order_id);
  if (!original) {
    logger_->error("Order not found in store for order_id: {}", order_id);
    return false;
  }
  if (is_terminal(original->order_state_)) {
    logger_->warn("Ignoring replace request for terminal order {}", order_id);
    return false;
  }

  release_replace_shadow(order_id);
  original->order_state_ = OrderState::Working;
  original->order_wait_state_ = OrderWaitState::PendingReplace;
  clear_request_id(*original);
  original->request_id_ = generate_order_id();

  const int64_t replacement_order_id = -order_id;

  Order* replacement = order_pool_->acquire();
  *replacement = *original;
  replacement->order_id_ = original->order_id_;
  replacement->request_id_ = original->request_id_;
  replacement->price_ = price;
  replacement->quantity_ = quantity;
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
  if (is_terminal(order->order_state_)) {
    logger_->warn("Ignoring late accepted response for terminal order {}",
                  event->order_id_);
    return;
  }
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
  if (is_terminal(order->order_state_)) {
    logger_->warn("Ignoring late rejected response for terminal order {}",
                  event->order_id_);
    return;
  }
  order->order_state_ = OrderState::Rejected;
  order->order_wait_state_ = OrderWaitState::None;
  logger_->debug("on_rejected {}", fmt::streamed(*event));

  for (const auto listener : listeners_) {
    listener->on_rejected(*order, *event);
  }

  clear_request_id(*order);
  release_replace_shadow(order->order_id_);

  if (!keep_filled_and_dead_) {
    order_store_.erase(order->order_id_);
    seen_execution_ids_.erase(order->order_id_);
    order_pool_->release(order);
  }
}

void AlgoOrderManagement::on_replace_accepted(const ReplaceAcceptedEvent* event) {
  discard_terminal_fill_request(event->order_id_, event->request_id_,
                                TerminalRequestOperation::Replace);
  Order* original = get_order(event->order_id_);
  Order* replacement = get_order(-event->order_id_);
  if (original && is_terminal(original->order_state_)) {
    logger_->warn(
        "Ignoring late replace-accepted response for terminal order {}",
        event->order_id_);
    record_replace_correlation_failure(event->request_id_);
    if (replacement && event->request_id_ != 0 &&
        replacement->request_id_ == event->request_id_) {
      release_replace_shadow(event->order_id_);
    }
    return;
  }
  if (!original || !replacement || event->request_id_ == 0 ||
      original->request_id_ != event->request_id_ ||
      replacement->request_id_ != event->request_id_ ||
      original->order_wait_state_ != OrderWaitState::PendingReplace) {
    record_replace_correlation_failure(event->request_id_);
    logger_->warn(
        "Ignoring stale or uncorrelated replace-accepted response for order "
        "{} request {}",
        event->order_id_, event->request_id_);
    return;
  }
  const int64_t original_order_id = original->order_id_;
  const int64_t cumulative_filled =
      original->quantity_ - original->leaves_quantity_;
  if (replacement->quantity_ <= cumulative_filled) {
    logger_->error(
        "Ignoring impossible replace-accepted response for order {}: total "
        "quantity {} does not exceed cumulative fill {}",
        event->order_id_, replacement->quantity_, cumulative_filled);
    original->order_wait_state_ = OrderWaitState::None;
    clear_request_id(*original);
    release_replace_shadow(original_order_id);
    return;
  }

  replacement->order_id_ = original_order_id;
  replacement->order_wait_state_ = OrderWaitState::None;
  replacement->order_state_ = OrderState::Working;

  replacement->leaves_quantity_ = replacement->quantity_ - cumulative_filled;
  remove_request_id(event->request_id_);
  replacement->request_id_ = 0;

  order_store_[original_order_id] = replacement;
  order_store_.erase(-original_order_id);
  order_pool_->release(original);

  logger_->debug("on_replace_accepted {}", fmt::streamed(*event));

  for (const auto listener : listeners_) {
    listener->on_replace_accepted(*replacement, *event);
  }
}

void AlgoOrderManagement::on_replace_rejected(const ReplaceRejectedEvent* event) {
  if (consume_terminal_fill_order_unknown_reject(
          event->order_id_, event->request_id_, event->reject_reason_,
          TerminalRequestOperation::Replace)) {
    logger_->debug(
        "Classified replace-rejected response after terminal fill for order "
        "{} request {} as a lifecycle race",
        event->order_id_, event->request_id_);
    return;
  }
  Order* original = get_order(event->order_id_);
  Order* replacement = get_order(-event->order_id_);
  if (original && is_terminal(original->order_state_)) {
    logger_->warn(
        "Ignoring late replace-rejected response for terminal order {}",
        event->order_id_);
    record_replace_reject_correlation_failure(event->request_id_,
                                              event->reject_reason_);
    if (replacement && event->request_id_ != 0 &&
        replacement->request_id_ == event->request_id_) {
      release_replace_shadow(event->order_id_);
    }
    return;
  }
  if (!original || !replacement || event->request_id_ == 0 ||
      original->request_id_ != event->request_id_ ||
      replacement->request_id_ != event->request_id_ ||
      original->order_wait_state_ != OrderWaitState::PendingReplace) {
    record_replace_reject_correlation_failure(event->request_id_,
                                              event->reject_reason_);
    logger_->warn(
        "Ignoring stale or uncorrelated replace-rejected response for order "
        "{} request {}",
        event->order_id_, event->request_id_);
    return;
  }
  original->order_wait_state_ = OrderWaitState::None;

  clear_request_id(*original);
  release_replace_shadow(event->order_id_);

  logger_->debug("on_replace_rejected {}", fmt::streamed(*event));

  for (const auto listener : listeners_) {
    listener->on_replace_rejected(*original, *event);
  }
}

void AlgoOrderManagement::on_cancel_accepted(const CancelAcceptedEvent* event) {
  discard_terminal_fill_request(event->order_id_, event->request_id_,
                                TerminalRequestOperation::Cancel);
  Order* order = get_order(event->order_id_);
  if (!order) {
    record_cancel_correlation_failure(event->request_id_);
    return;
  }
  const bool unsolicited_system_cancel =
      event->cancel_reason_ == CancelReason::System && event->request_id_ == 0 &&
      (order->order_type_ == OrderType::Market ||
       order->time_in_force_ == TimeInForce::Ioc);
  const bool unsolicited_exchange_cancel =
      event->cancel_reason_ == CancelReason::Exchange &&
      event->request_id_ == 0;
  if (is_terminal(order->order_state_)) {
    logger_->warn(
        "Ignoring late cancel-accepted response for terminal order {}",
        event->order_id_);
    record_cancel_correlation_failure(event->request_id_);
    return;
  }
  if (!unsolicited_system_cancel && !unsolicited_exchange_cancel &&
      (event->request_id_ == 0 || order->request_id_ != event->request_id_ ||
       order->order_wait_state_ != OrderWaitState::PendingCancel)) {
    record_cancel_correlation_failure(event->request_id_);
    logger_->warn(
        "Ignoring stale or uncorrelated cancel-accepted response for order {} "
        "request {}",
        event->order_id_, event->request_id_);
    return;
  }
  order->order_state_ = OrderState::Cancelled;
  order->order_wait_state_ = OrderWaitState::None;

  clear_request_id(*order);
  release_replace_shadow(order->order_id_);

  logger_->debug("on_cancel_accepted {}", fmt::streamed(*event));

  for (auto listener : listeners_) {
    listener->on_cancel_accepted(*order, *event);
  }

  if (!keep_filled_and_dead_) {
    order_store_.erase(order->order_id_);
    seen_execution_ids_.erase(order->order_id_);
    order_pool_->release(order);
  }
}

void AlgoOrderManagement::on_cancel_rejected(const CancelRejectedEvent* event) {
  if (consume_terminal_fill_order_unknown_reject(
          event->order_id_, event->request_id_, event->reject_reason_,
          TerminalRequestOperation::Cancel)) {
    logger_->debug(
        "Classified cancel-rejected response after terminal fill for order {} "
        "request {} as a lifecycle race",
        event->order_id_, event->request_id_);
    return;
  }
  Order* order = get_order(event->order_id_);
  if (order && is_terminal(order->order_state_)) {
    logger_->warn(
        "Ignoring late cancel-rejected response for terminal order {}",
        event->order_id_);
    record_cancel_reject_correlation_failure(event->request_id_,
                                             event->reject_reason_);
    return;
  }
  if (!order || event->request_id_ == 0 ||
      order->request_id_ != event->request_id_ ||
      order->order_wait_state_ != OrderWaitState::PendingCancel) {
    record_cancel_reject_correlation_failure(event->request_id_,
                                             event->reject_reason_);
    logger_->warn(
        "Ignoring stale or uncorrelated cancel-rejected response for order {} "
        "request {}",
        event->order_id_, event->request_id_);
    return;
  }
  order->order_state_ = OrderState::Working;
  order->order_wait_state_ = OrderWaitState::None;
  clear_request_id(*order);

  logger_->debug("on_cancel_rejected {}", fmt::streamed(*event));

  for (const auto listener : listeners_) {
    listener->on_cancel_rejected(*order, *event);
  }
}

void AlgoOrderManagement::on_executed(const ExecutedEvent* event) {
  Order* order = get_order(event->order_id_);
  if (!order) return;

  if (event->exec_id_ != 0) {
    const auto seen = seen_execution_ids_.find(event->order_id_);
    if (seen != seen_execution_ids_.end() &&
        seen->second.find(event->exec_id_) != seen->second.end()) {
      logger_->warn("Ignoring duplicate execution {} for order {}",
                    event->exec_id_, event->order_id_);
      return;
    }
  }

  if (event->last_quantity_ <= 0 ||
      event->last_quantity_ > order->leaves_quantity_) {
    logger_->error(
        "Ignoring invalid execution quantity {} for order {} with leaves {}",
        event->last_quantity_, event->order_id_, order->leaves_quantity_);
    return;
  }

  if (event->exec_id_ != 0) {
    seen_execution_ids_[event->order_id_].insert(event->exec_id_);
  }
  order->leaves_quantity_ -= event->last_quantity_;
  if (order->leaves_quantity_ == 0) {
    remember_terminal_fill_request(*order);
    order->order_state_ = OrderState::Filled;
    order->order_wait_state_ = OrderWaitState::None;
  }

  logger_->debug("on_executed {}", fmt::streamed(*event));

  for (const auto listener : listeners_) {
    listener->on_executed(*order, *event);
  }

  if (order->leaves_quantity_ == 0) {
    clear_request_id(*order);
    release_replace_shadow(order->order_id_);
    if (!keep_filled_and_dead_) {
      order_store_.erase(order->order_id_);
      seen_execution_ids_.erase(order->order_id_);
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

void AlgoOrderManagement::clear_request_id(Order& order) {
  if (order.request_id_ != 0) {
    remove_request_id(order.request_id_);
    order.request_id_ = 0;
  }
}

void AlgoOrderManagement::release_replace_shadow(int64_t order_id) {
  const auto shadow_it = order_store_.find(-order_id);
  if (shadow_it == order_store_.end()) return;
  Order* shadow = shadow_it->second;
  order_store_.erase(shadow_it);
  order_pool_->release(shadow);
}

void AlgoOrderManagement::record_cancel_correlation_failure(
    int64_t request_id) {
  if (request_id == 0) {
    ++request_correlation_diagnostics_.uncorrelated_cancel_responses;
  } else {
    ++request_correlation_diagnostics_.stale_cancel_responses;
  }
}

void AlgoOrderManagement::record_replace_correlation_failure(
    int64_t request_id) {
  if (request_id == 0) {
    ++request_correlation_diagnostics_.uncorrelated_replace_responses;
  } else {
    ++request_correlation_diagnostics_.stale_replace_responses;
  }
}

void AlgoOrderManagement::record_cancel_reject_correlation_failure(
    int64_t request_id, RejectReason reason) {
  ++request_correlation_diagnostics_
        .cancel_reject_correlation_failures_by_reason[reject_reason_index(
            reason)];
  record_cancel_correlation_failure(request_id);
}

void AlgoOrderManagement::record_replace_reject_correlation_failure(
    int64_t request_id, RejectReason reason) {
  ++request_correlation_diagnostics_
        .replace_reject_correlation_failures_by_reason[reject_reason_index(
            reason)];
  record_replace_correlation_failure(request_id);
}

void AlgoOrderManagement::remember_terminal_fill_request(const Order& order) {
  if (order.request_id_ == 0) return;

  TerminalRequestOperation operation{};
  if (order.order_wait_state_ == OrderWaitState::PendingCancel) {
    operation = TerminalRequestOperation::Cancel;
  } else if (order.order_wait_state_ == OrderWaitState::PendingReplace) {
    operation = TerminalRequestOperation::Replace;
  } else {
    return;
  }

  terminal_fill_requests_[order.request_id_] =
      TerminalRequestCompletion{order.order_id_, operation};
  terminal_fill_request_order_.push_back(order.request_id_);
  while (terminal_fill_request_order_.size() >
         kMaxTerminalFillRequestTombstones) {
    const int64_t oldest = terminal_fill_request_order_.front();
    terminal_fill_request_order_.pop_front();
    terminal_fill_requests_.erase(oldest);
  }
}

bool AlgoOrderManagement::consume_terminal_fill_order_unknown_reject(
    int64_t order_id, int64_t request_id, RejectReason reason,
    TerminalRequestOperation operation) {
  if (request_id == 0) return false;
  const auto it = terminal_fill_requests_.find(request_id);
  if (it == terminal_fill_requests_.end() ||
      it->second.order_id != order_id || it->second.operation != operation) {
    return false;
  }
  terminal_fill_requests_.erase(it);
  if (reason != RejectReason::OrderUnknown) return false;

  if (operation == TerminalRequestOperation::Cancel) {
    ++request_correlation_diagnostics_
          .terminal_fill_order_unknown_cancel_rejects;
  } else {
    ++request_correlation_diagnostics_
          .terminal_fill_order_unknown_replace_rejects;
  }
  return true;
}

void AlgoOrderManagement::discard_terminal_fill_request(
    int64_t order_id, int64_t request_id,
    TerminalRequestOperation operation) {
  if (request_id == 0) return;
  const auto it = terminal_fill_requests_.find(request_id);
  if (it != terminal_fill_requests_.end() &&
      it->second.order_id == order_id && it->second.operation == operation) {
    terminal_fill_requests_.erase(it);
  }
}
}
