#include "framework/ring_buffer_writer.hpp"

#include "spdlog/fmt/bundled/ostream.h"

namespace reflex {
namespace {

template <typename Event>
inline void log_publish_if_debug_enabled(spdlog::logger* logger, const Event& event) {
    if (logger != nullptr && logger->should_log(spdlog::level::debug)) {
        logger->debug("Publishing {}", fmt::streamed(event));
    }
}

}

RingBufferWriter::RingBufferWriter(
    const std::shared_ptr<disruptorplus::ring_buffer<MessageSlot>>& buffer,
    const std::shared_ptr<disruptorplus::single_threaded_claim_strategy<disruptorplus::spin_wait_strategy>>&
        claim_strategy,
    const ClockInterface* clock)
    : buffer_(buffer),
      claim_strategy_(claim_strategy),
      owned_clock_(clock ? nullptr : std::make_unique<OffsetEpochNanoClock>()),
      clock_(clock ? clock : owned_clock_.get()) {

    logger_ = LoggerFactory::getLogger("RingBufferWriter");

}

std::atomic<uint64_t> RingBufferWriter::ring_full_stalls{0};

bool RingBufferWriter::claim_with_retry(disruptorplus::sequence_range& range) const {
    for (int spin = 0; spin < 20000; ++spin) {
        if (claim_strategy_->try_claim(1, range)) return true;
    }
    ring_full_stalls.fetch_add(1, std::memory_order_relaxed);
    return false;
}

template <typename Event, typename FillFn>
void RingBufferWriter::send_event(FillFn&& fill) {
    disruptorplus::sequence_range range;
    disruptorplus::sequence_t seq;
    if (claim_with_retry(range)) {
        seq = range.first();
    } else {
        logger_->warn("Ring buffer full while publishing {} event; spinning until a slot is free",
                      to_string(Event::MESSAGE_TYPE));
        seq = claim_strategy_->claim_one();
    }

    auto* const event = new ((*buffer_)[seq].raw_data()) Event();
    event->timestamp_ns_ = clock_->epoch_nanos();
    fill(*event);

    log_publish_if_debug_enabled(logger_.get(), *event);

    claim_strategy_->publish(seq);
}

void RingBufferWriter::send_pending(const Order& order) {
    send_event<PendingEvent>([&order](PendingEvent& event) {
        event.order_id_ = order.order_id_;
        event.instrument_id_ = order.instrument_id_;
        event.side_ = order.side_;
        event.quantity_ = order.quantity_;
        event.price_ = order.price_;
        event.order_type_ = order.order_type_;
        event.time_in_force_ = order.time_in_force_;
        event.exec_inst_ = order.exec_inst_;
        event.parent_id_ = order.parent_id_;
        event.request_id_ = order.request_id_;
        event.account_ = order.account_;
        if (order.timestamp_ns_ != 0) {
            event.timestamp_ns_ = order.timestamp_ns_;
        }
    });
}

void RingBufferWriter::send_accepted(const Order& order) {
    send_event<AcceptedEvent>([&order](AcceptedEvent& event) {
        event.order_id_ = order.order_id_;
        event.exchange_order_id_ = order.exchange_order_id_;
    });
}

void RingBufferWriter::send_rejected(const Order& order) {
    send_event<RejectedEvent>([&order](RejectedEvent& event) {
        event.order_id_ = order.order_id_;
    });
}

void RingBufferWriter::send_pending_cancel(const Order& order, const CancelPriority priority) {
    send_event<PendingCancelEvent>([&order, priority](PendingCancelEvent& event) {
        event.order_id_ = order.order_id_;
        event.request_id_ = order.request_id_;
        event.instrument_id_ = order.instrument_id_;
        event.priority_ = priority;
    });
}

void RingBufferWriter::send_cancel_accepted(const Order& order) {
    send_event<CancelAcceptedEvent>([&order](CancelAcceptedEvent& event) {
        event.order_id_ = order.order_id_;
        event.request_id_ = order.request_id_;
    });
}

void RingBufferWriter::send_cancel_rejected(const Order& order) {
    send_event<CancelRejectedEvent>([&order](CancelRejectedEvent& event) {
        event.order_id_ = order.order_id_;
        event.request_id_ = order.request_id_;
    });
}

void RingBufferWriter::send_pending_replace(const Order& order) {
    send_event<PendingReplaceEvent>([&order](PendingReplaceEvent& event) {
        event.order_id_ = order.order_id_;
        event.request_id_ = order.request_id_;
        event.quantity_ = order.quantity_;
        event.price_ = order.price_;
        event.instrument_id_ = order.instrument_id_;
    });
}

void RingBufferWriter::send_replace_accepted(const Order& order) {
    send_event<ReplaceAcceptedEvent>([&order](ReplaceAcceptedEvent& event) {
        event.order_id_ = order.order_id_;
        event.request_id_ = order.request_id_;
    });
}

void RingBufferWriter::send_replace_rejected(const Order& order) {
    send_event<ReplaceRejectedEvent>([&order](ReplaceRejectedEvent& event) {
        event.order_id_ = order.order_id_;
        event.request_id_ = order.request_id_;
    });
}

void RingBufferWriter::send_executed(const int64_t order_id, const int64_t last_quantity, const int64_t last_price) {
    send_event<ExecutedEvent>([order_id, last_quantity, last_price](ExecutedEvent& event) {
        event.order_id_ = order_id;
        event.last_quantity_ = last_quantity;
        event.last_price_ = last_price;
    });
}

}
