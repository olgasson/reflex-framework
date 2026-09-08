#pragma once
#include <utility>
#include <chrono>
#include <atomic>

#include "event_listener.hpp"
#include "messages.hpp"
#include "disruptorplus/ring_buffer.hpp"
#include "disruptorplus/sequence_barrier.hpp"
#include "disruptorplus/single_threaded_claim_strategy.hpp"
#include "disruptorplus/spin_wait_strategy.hpp"
#include "framework/base_component.hpp"
#include "framework/ring_buffer_reader.hpp"


namespace reflex {

class LoggerComponent : public BaseComponent, public HeartbeatListener, public OrderEventListener, MarketDataListener {
 public:
  LoggerComponent(
      const ComponentConfig& config, std::shared_ptr<disruptorplus::ring_buffer<MessageSlot>> log_buffer,
      std::shared_ptr<disruptorplus::sequence_barrier<disruptorplus::spin_wait_strategy>> logger_barrier,
      std::shared_ptr<disruptorplus::single_threaded_claim_strategy<disruptorplus::spin_wait_strategy>> claim_strategy);

  int on_do_work() override;

  void on_heartbeat(const HeartbeatEvent* event) override;
  void on_pending(const PendingEvent* event) override;
  void on_accepted(const AcceptedEvent* event) override;
  void on_rejected(const RejectedEvent* event) override;
  void on_pending_replace(const PendingReplaceEvent* event) override;
  void on_replace_accepted(const ReplaceAcceptedEvent* event) override;
  void on_replace_rejected(const ReplaceRejectedEvent* event) override;
  void on_pending_cancel(const PendingCancelEvent* event) override;
  void on_cancel_accepted(const CancelAcceptedEvent* event) override;
  void on_cancel_rejected(const CancelRejectedEvent* event) override;
  void on_executed(const ExecutedEvent* event) override;
  void on_trade(const TradeEvent& event) override;
  void on_l1_update(const L1UpdateEvent& event) override;
  void on_l2_update(const L2UpdateEvent& event) override;

 private:
  void log_stats();

  std::shared_ptr<disruptorplus::ring_buffer<reflex::MessageSlot>> log_buffer_;
  std::shared_ptr<disruptorplus::sequence_barrier<disruptorplus::spin_wait_strategy>> logger_barrier_;
  std::shared_ptr<disruptorplus::single_threaded_claim_strategy<disruptorplus::spin_wait_strategy>> claim_strategy_;
  disruptorplus::sequence_t next_{0};
  std::unique_ptr<MessageSlotReader> reader_;

  std::chrono::steady_clock::time_point last_stats_log_;
  std::atomic<uint64_t> l1_count_;
  std::atomic<uint64_t> l2_count_;
  std::atomic<uint64_t> trade_count_;
};

}
