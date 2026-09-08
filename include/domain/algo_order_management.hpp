#pragma once

#include <array>
#include <cstdint>
#include <deque>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "../event_listener.hpp"
#include "../messages.hpp"
#include "order.hpp"
#include "order_pool.hpp"
#include "framework/order_writer.hpp"
#include "spdlog/logger.h"

namespace reflex {

struct RequestCorrelationDiagnostics {
  static constexpr size_t kRejectReasonCount = 7;

  uint64_t stale_cancel_responses{0};
  uint64_t uncorrelated_cancel_responses{0};
  uint64_t stale_replace_responses{0};
  uint64_t uncorrelated_replace_responses{0};
  uint64_t terminal_fill_order_unknown_cancel_rejects{0};
  uint64_t terminal_fill_order_unknown_replace_rejects{0};
  std::array<uint64_t, kRejectReasonCount>
      cancel_reject_correlation_failures_by_reason{};
  std::array<uint64_t, kRejectReasonCount>
      replace_reject_correlation_failures_by_reason{};
};

class AlgoOrderManagement {
 public:
  explicit AlgoOrderManagement(std::shared_ptr<OrderWriter> writer, bool keep_filled_and_dead = false);

  void add_listener(AlgoOrderManagementListener* listener);

  int64_t send_pending(int32_t instrument_id, Side side, int64_t quantity, int64_t price, OrderType order_type,TimeInForce time_in_force, int32_t account, ExecInst exec_inst);
  void send_pending_cancel(int64_t order_id,
                           CancelPriority priority = CancelPriority::RiskReducing);
  bool send_pending_replace(int64_t order_id, int32_t instrument_id, int64_t price, int64_t quantity);

  void on_accepted(const AcceptedEvent* event);
  void on_rejected(const RejectedEvent* event);
  void on_replace_accepted(const ReplaceAcceptedEvent* event);
  void on_replace_rejected(const ReplaceRejectedEvent* event);
  void on_cancel_accepted(const CancelAcceptedEvent* event);
  void on_cancel_rejected(const CancelRejectedEvent* event);
  void on_executed(const ExecutedEvent* event);

  Order* get_order(int64_t order_id);

  int get_order_pool_size() const { return order_pool_->available(); }
  [[nodiscard]] const RequestCorrelationDiagnostics&
  request_correlation_diagnostics() const noexcept {
    return request_correlation_diagnostics_;
  }

 private:
  enum class TerminalRequestOperation : uint8_t { Cancel, Replace };
  struct TerminalRequestCompletion {
    int64_t order_id{0};
    TerminalRequestOperation operation{TerminalRequestOperation::Cancel};
  };

  int64_t generate_order_id();
  void register_request_id(int64_t request_id, int64_t order_id);
  void remove_request_id(int64_t request_id);
  void clear_request_id(Order& order);
  void release_replace_shadow(int64_t order_id);
  void record_cancel_correlation_failure(int64_t request_id);
  void record_replace_correlation_failure(int64_t request_id);
  void record_cancel_reject_correlation_failure(int64_t request_id,
                                                RejectReason reason);
  void record_replace_reject_correlation_failure(int64_t request_id,
                                                 RejectReason reason);
  void remember_terminal_fill_request(const Order& order);
  bool consume_terminal_fill_order_unknown_reject(
      int64_t order_id, int64_t request_id, RejectReason reason,
      TerminalRequestOperation operation);
  void discard_terminal_fill_request(int64_t order_id, int64_t request_id,
                                     TerminalRequestOperation operation);

  std::shared_ptr<OrderWriter> writer_;

  std::unordered_map<int64_t, Order*> order_store_;
  std::unordered_map<int64_t, int64_t> request_id_to_order_id_;
  std::unordered_map<int64_t, TerminalRequestCompletion>
      terminal_fill_requests_;
  std::deque<int64_t> terminal_fill_request_order_;
  RequestCorrelationDiagnostics request_correlation_diagnostics_{};
  std::unordered_map<int64_t, std::unordered_set<int64_t>> seen_execution_ids_;
  std::unique_ptr<OrderPool> order_pool_;
  bool keep_filled_and_dead_;

  int64_t next_order_id_;

  std::shared_ptr<spdlog::logger> logger_;

  std::vector<AlgoOrderManagementListener*> listeners_;
};

}
