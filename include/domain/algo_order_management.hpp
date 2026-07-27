// algo_order_management.hpp
#pragma once

#include <memory>
#include <unordered_map>
#include <vector>

#include "../event_listener.hpp"
#include "../messages.hpp"
#include "order.hpp"
#include "order_pool.hpp"
#include "framework/order_writer.hpp"
#include "spdlog/logger.h"

namespace reflex {

class AlgoOrderManagement {
 public:
  explicit AlgoOrderManagement(std::shared_ptr<OrderWriter> writer, bool keep_filled_and_dead = false);

  void add_listener(AlgoOrderManagementListener* listener);

  int64_t send_pending(int32_t instrument_id, Side side, int64_t quantity, int64_t price, OrderType order_type,TimeInForce time_in_force, int32_t account, ExecInst exec_inst);
  void send_pending_cancel(int64_t order_id);
  // Returns false if the request was rejected locally (unknown order, or a
  // replace is already in flight for this order).
  bool send_pending_replace(int64_t order_id, int32_t instrument_id, int64_t price, int64_t quantity);

  // Called when responses come back from ring buffer
  void on_accepted(const AcceptedEvent* event);
  void on_rejected(const RejectedEvent* event);
  void on_replace_accepted(const ReplaceAcceptedEvent* event);
  void on_replace_rejected(const ReplaceRejectedEvent* event);
  void on_cancel_accepted(const CancelAcceptedEvent* event);
  void on_cancel_rejected(const CancelRejectedEvent* event);
  void on_executed(const ExecutedEvent* event);

  // Query methods
  Order* get_order(int64_t order_id);

  int get_order_pool_size() const { return order_pool_->available(); }

 private:
  int64_t generate_order_id();
  void register_request_id(int64_t request_id, int64_t order_id);
  void remove_request_id(int64_t request_id);

  std::shared_ptr<OrderWriter> writer_;

  std::unordered_map<int64_t, Order*> order_store_;
  std::unordered_map<int64_t, int64_t> request_id_to_order_id_;
  std::unique_ptr<OrderPool> order_pool_;
  bool keep_filled_and_dead_;

  // Order ID generation
  int64_t next_order_id_;

  std::shared_ptr<spdlog::logger> logger_;

  std::vector<AlgoOrderManagementListener*> listeners_;
};

}  // namespace reflex
