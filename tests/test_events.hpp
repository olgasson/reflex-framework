#pragma once

#include <string>
#include <chrono>

#include "messages.hpp"
#include "../include/utils/codec_utils.hpp"

namespace reflex {

class TestEvents {
public:
  static PendingEvent create_pending_event(
      uint64_t timestamp,
      uint64_t order_id,
      uint64_t request_id,
      int32_t instrument_id,
      int64_t price,
      int64_t quantity,
      OrderType order_type,
      TimeInForce time_in_force,
      ExecInst exec_inst,
      Side side
  ) {
    PendingEvent event{};
    event.timestamp_ns_ = timestamp;
    event.order_id_ = order_id;
    event.request_id_ = request_id;
    event.instrument_id_ = instrument_id;
    event.price_ = CodecUtils::encode_price(price);
    event.quantity_ = CodecUtils::encode_price(quantity);
    event.order_type_ = order_type;
    event.time_in_force_ = time_in_force;
    event.exec_inst_ = exec_inst;
    event.side_ = side;
    return event;
  }

  static AcceptedEvent create_accepted_event(uint64_t timestamp, uint64_t order_id) {
    AcceptedEvent event{};
    event.timestamp_ns_ = timestamp;
    event.order_id_ = order_id;
    return event;
  }

  static RejectedEvent create_rejected_event(uint64_t timestamp, uint64_t order_id, RejectReason reason) {
    RejectedEvent event{};
    event.timestamp_ns_ = timestamp;
    event.order_id_ = order_id;
    event.reject_reason_ = reason;
    return event;
  }

  static PendingCancelEvent create_pending_cancel_event(uint64_t timestamp,uint64_t order_id,uint64_t request_id) {
    PendingCancelEvent event{};
    event.timestamp_ns_ = timestamp;
    event.order_id_ = order_id;
    event.request_id_ = request_id;
    return event;
  }

  static CancelAcceptedEvent create_cancel_accepted_event(
      int64_t timestamp, int64_t order_id, int64_t request_id) {
    CancelAcceptedEvent event{};
    event.timestamp_ns_ = timestamp;
    event.order_id_ = order_id;
    event.request_id_ = request_id;
    return event;
  }

  static CancelRejectedEvent create_cancel_rejected_event(
      uint64_t timestamp, int64_t order_id, int64_t request_id,
      RejectReason reason) {
    CancelRejectedEvent event{};
    event.timestamp_ns_ = timestamp;
    event.order_id_ = order_id;
    event.request_id_ = request_id;
    event.reject_reason_ = reason;
    return event;
  }

  static PendingReplaceEvent create_pending_replace_event(
    uint64_t timestamp,
    int64_t order_id,
    int64_t request_id,
    int32_t instrument_id,
    double price,
    double quantity,
    const std::string& source
) {
    PendingReplaceEvent event{};
    event.timestamp_ns_ = timestamp;
    event.order_id_ = order_id;
    event.request_id_ = request_id;
    event.instrument_id_ = instrument_id;
    event.price_ = CodecUtils::encode_price(price);
    event.quantity_ = CodecUtils::encode_price(quantity);
    return event;
  }

  static ReplaceAcceptedEvent create_replace_accepted_event(
      uint64_t timestamp, int64_t order_id, int64_t request_id) {
    ReplaceAcceptedEvent event{};
    event.timestamp_ns_ = timestamp;
    event.order_id_ = order_id;
    event.request_id_ = request_id;
    return event;
  }

  static ReplaceRejectedEvent create_replace_rejected_event(
    uint64_t timestamp,
    int64_t order_id,
    int64_t request_id,
    RejectReason reason
) {
    ReplaceRejectedEvent event{};
    event.timestamp_ns_ = timestamp;
    event.order_id_ = order_id;
    event.request_id_ = request_id;
    event.reject_reason_ = reason;
    return event;
  }


  static uint64_t now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::system_clock::now().time_since_epoch()).count();
  }
};

}
