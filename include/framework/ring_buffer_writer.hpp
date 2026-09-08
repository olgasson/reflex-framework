
#pragma once

#include "order_writer.hpp"

#include <atomic>
#include <cstdint>
#include <memory>
#include <disruptorplus/ring_buffer.hpp>
#include <disruptorplus/single_threaded_claim_strategy.hpp>
#include <disruptorplus/spin_wait_strategy.hpp>
#include "../messages.hpp"
#include "../offset_epoch_nano_clock.hpp"
#include "domain/order.hpp"
#include "logger_factory.hpp"

namespace reflex {

class RingBufferWriter : public OrderWriter {
 public:
  static std::atomic<uint64_t> ring_full_stalls;

  RingBufferWriter(
      const std::shared_ptr<disruptorplus::ring_buffer<MessageSlot>>& buffer,
      const std::shared_ptr<disruptorplus::single_threaded_claim_strategy<disruptorplus::spin_wait_strategy>>&
          claim_strategy,
      const ClockInterface* clock = nullptr);

  void send_pending(const Order& order) override;
  void send_pending_cancel(const Order& order,
                           CancelPriority priority) override;
  void send_pending_replace(const Order& order) override;

  void send_accepted(const Order& order);
  void send_rejected(const Order& order);
  void send_replace_accepted(const Order& order);
  void send_replace_rejected(const Order& order);
  void send_cancel_accepted(const Order& order);
  void send_cancel_rejected(const Order& order);

  void send_executed(int64_t order_id, int64_t last_quantity, int64_t last_price);

 private:
  template <typename Event, typename FillFn>
  void send_event(FillFn&& fill);

  bool claim_with_retry(disruptorplus::sequence_range& range) const;

  std::shared_ptr<disruptorplus::ring_buffer<MessageSlot>> buffer_;
  std::shared_ptr<disruptorplus::single_threaded_claim_strategy<disruptorplus::spin_wait_strategy>> claim_strategy_;
  std::shared_ptr<spdlog::logger> logger_;
  std::unique_ptr<OffsetEpochNanoClock> owned_clock_;
  const ClockInterface* clock_;
};

}
