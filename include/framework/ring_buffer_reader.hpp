#pragma once

#include <array>
#include <functional>
#include <memory>
#include "disruptorplus/ring_buffer.hpp"
#include "disruptorplus/single_threaded_claim_strategy.hpp"
#include "disruptorplus/spin_wait_strategy.hpp"
#include "disruptorplus/sequence_barrier.hpp"
#include "messages.hpp"

namespace reflex {

constexpr disruptorplus::sequence_t INITIAL = static_cast<disruptorplus::sequence_t>(-1);

template<typename MessageSlot>
class RingBufferReader {
public:
    RingBufferReader(
        std::shared_ptr<disruptorplus::ring_buffer<MessageSlot>> buffer,
        std::shared_ptr<disruptorplus::single_threaded_claim_strategy<disruptorplus::spin_wait_strategy>> claim_strategy,
        std::shared_ptr<disruptorplus::sequence_barrier<disruptorplus::spin_wait_strategy>> barrier = nullptr)
        : buffer_(std::move(buffer))
        , claim_strategy_(std::move(claim_strategy))
        , barrier_(std::move(barrier))
        , next_(INITIAL + 1) {
    }

    disruptorplus::sequence_t get_next_sequence() const {
        return next_;
    }

    void reset_to_sequence(disruptorplus::sequence_t sequence) {
        next_ = sequence;
    }

protected:
    ~RingBufferReader() = default;

    template<typename Dispatch>
    int drain(Dispatch&& dispatch) {
        const disruptorplus::sequence_t available = claim_strategy_->last_published();
        if (disruptorplus::difference(available, next_) < 0) {
            return 0;
        }

        int work = 0;
        while (disruptorplus::difference(next_, available) <= 0) {
            dispatch((*buffer_)[next_]);
            ++next_;
            ++work;
        }

        if (barrier_) {
            barrier_->publish(available);
        }

        return work;
    }

private:
    std::shared_ptr<disruptorplus::ring_buffer<MessageSlot>> buffer_;
    std::shared_ptr<disruptorplus::single_threaded_claim_strategy<disruptorplus::spin_wait_strategy>> claim_strategy_;
    std::shared_ptr<disruptorplus::sequence_barrier<disruptorplus::spin_wait_strategy>> barrier_;
    disruptorplus::sequence_t next_;
};

class MessageSlotReader final : public RingBufferReader<MessageSlot> {
public:
    using MessageTypeHandler = std::function<void(const MessageSlot&)>;

    static constexpr std::size_t kMaxMessageTypes = 32;
    static_assert(static_cast<std::size_t>(MessageType::LiquidationEvent) < kMaxMessageTypes,
                  "kMaxMessageTypes must cover every MessageType value");

    MessageSlotReader(
        std::shared_ptr<disruptorplus::ring_buffer<MessageSlot>> buffer,
        std::shared_ptr<disruptorplus::single_threaded_claim_strategy<disruptorplus::spin_wait_strategy>> claim_strategy,
        std::shared_ptr<disruptorplus::sequence_barrier<disruptorplus::spin_wait_strategy>> barrier = nullptr)
        : RingBufferReader<MessageSlot>(std::move(buffer), std::move(claim_strategy), std::move(barrier)) {
    }

    void register_handler(MessageType type, MessageTypeHandler handler) {
        const auto idx = static_cast<std::size_t>(type);
        if (idx < kMaxMessageTypes) {
            handlers_[idx] = std::move(handler);
        }
    }

    void set_default_handler(MessageTypeHandler handler) {
        default_handler_ = std::move(handler);
    }

    int process_messages() {
        return drain([this](const MessageSlot& slot) { dispatch_message(slot); });
    }

private:
    void dispatch_message(const MessageSlot& slot) {
        const auto idx = static_cast<std::size_t>(slot.get_type());
        if (idx < kMaxMessageTypes && handlers_[idx]) {
            handlers_[idx](slot);
        } else if (default_handler_) {
            default_handler_(slot);
        }
    }

    std::array<MessageTypeHandler, kMaxMessageTypes> handlers_{};
    MessageTypeHandler default_handler_;
};

}
