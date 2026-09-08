#include "components/data_offload/logger_component.hpp"

#include "messages.hpp"
#include <chrono>

namespace reflex {

LoggerComponent::LoggerComponent(
    const ComponentConfig& config,
    std::shared_ptr<disruptorplus::ring_buffer<MessageSlot>> log_buffer,
    std::shared_ptr<disruptorplus::sequence_barrier<disruptorplus::spin_wait_strategy>> logger_barrier,
    std::shared_ptr<disruptorplus::single_threaded_claim_strategy<disruptorplus::spin_wait_strategy>> claim_strategy)
    : BaseComponent(config)
    , log_buffer_(std::move(log_buffer))
    , logger_barrier_(std::move(logger_barrier))
    , claim_strategy_(std::move(claim_strategy)) {

  reader_ = std::make_unique<MessageSlotReader>(log_buffer_, claim_strategy_, logger_barrier_);

  reader_->register_handler(MessageType::Pending, [this](const MessageSlot& slot) {
    const auto& event = slot.as<PendingEvent>();
    on_pending(&event);
  });

  reader_->register_handler(MessageType::Accepted, [this](const MessageSlot& slot) {
    const auto& event = slot.as<AcceptedEvent>();
    on_accepted(&event);
  });

  reader_->register_handler(MessageType::Rejected, [this](const MessageSlot& slot) {
    const auto& event = slot.as<RejectedEvent>();
    on_rejected(&event);
  });

  reader_->register_handler(MessageType::PendingReplace, [this](const MessageSlot& slot) {
    const auto& event = slot.as<PendingReplaceEvent>();
    on_pending_replace(&event);
  });

  reader_->register_handler(MessageType::ReplaceAccepted, [this](const MessageSlot& slot) {
    const auto& event = slot.as<ReplaceAcceptedEvent>();
    on_replace_accepted(&event);
  });

  reader_->register_handler(MessageType::ReplaceRejected, [this](const MessageSlot& slot) {
  const auto& event = slot.as<ReplaceRejectedEvent>();
  on_replace_rejected(&event);
});

  reader_->register_handler(MessageType::PendingCancel, [this](const MessageSlot& slot) {
    const auto& event = slot.as<PendingCancelEvent>();
    on_pending_cancel(&event);
  });

  reader_->register_handler(MessageType::CancelAccepted, [this](const MessageSlot& slot) {
    const auto& event = slot.as<CancelAcceptedEvent>();
    on_cancel_accepted(&event);
  });

  reader_->register_handler(MessageType::CancelRejected, [this](const MessageSlot& slot) {
    const auto& event = slot.as<CancelRejectedEvent>();
    on_cancel_rejected(&event);
  });

  reader_->register_handler(MessageType::Executed, [this](const MessageSlot& slot) {
    const auto& event = slot.as<ExecutedEvent>();
    on_executed(&event);
  });

  reader_->register_handler(MessageType::TradeEvent, [this](const MessageSlot& slot) {
      const auto& event = slot.as<TradeEvent>();
      on_trade(event);
  });

  reader_->register_handler(MessageType::L1UpdateEvent, [this](const MessageSlot& slot) {
      const auto& event = slot.as<L1UpdateEvent>();
      on_l1_update(event);
  });

  reader_->register_handler(MessageType::L2UpdateEvent, [this](const MessageSlot& slot) {
      const auto& event = slot.as<L2UpdateEvent>();
      on_l2_update(event);
  });

  reader_->register_handler(MessageType::MarkPriceEvent, [this](const MessageSlot& slot) {
      const auto& event = slot.as<MarkPriceEvent>();
      logger_->info("{}", fmt::streamed(event));
  });

  reader_->register_handler(MessageType::FundingRateEvent, [this](const MessageSlot& slot) {
      const auto& event = slot.as<FundingRateEvent>();
      logger_->info("{}", fmt::streamed(event));
  });

  reader_->register_handler(MessageType::OpenInterestEvent, [this](const MessageSlot& slot) {
      const auto& event = slot.as<OpenInterestEvent>();
      logger_->info("{}", fmt::streamed(event));
  });

  reader_->set_default_handler([this](const MessageSlot& slot) {
      logger_->error("Unknown message type: {}", static_cast<int>(slot.get_type()));
  });
}

int LoggerComponent::on_do_work() {
  return reader_->process_messages();
}


void LoggerComponent::on_heartbeat(const HeartbeatEvent* event) {
  logger_->info("{}", fmt::streamed(*event));
}

void LoggerComponent::on_pending(const PendingEvent* event) {
  logger_->info("{}", fmt::streamed(*event));
}

void LoggerComponent::on_accepted(const AcceptedEvent* event) {
  logger_->info("{}", fmt::streamed(*event));
}

void LoggerComponent::on_rejected(const RejectedEvent* event) {
  logger_->info("{}", fmt::streamed(*event));
}

void LoggerComponent::on_pending_replace(const PendingReplaceEvent* event) {
  logger_->info("{}", fmt::streamed(*event));
}

void LoggerComponent::on_replace_accepted(const ReplaceAcceptedEvent* event) {
  logger_->info("{}", fmt::streamed(*event));
}

void LoggerComponent::on_replace_rejected(const ReplaceRejectedEvent* event) {
  logger_->info("{}", fmt::streamed(*event));
}

void LoggerComponent::on_pending_cancel(const PendingCancelEvent* event) {
  logger_->info("{}", fmt::streamed(*event));
}

void LoggerComponent::on_cancel_accepted(const CancelAcceptedEvent* event) {
  logger_->info("{}", fmt::streamed(*event));
}

void LoggerComponent::on_cancel_rejected(const CancelRejectedEvent* event) {
  logger_->info("{}", fmt::streamed(*event));
}

void LoggerComponent::on_executed(const ExecutedEvent* event) {
  logger_->info("{}", fmt::streamed(*event));
}

void LoggerComponent::on_trade(const TradeEvent& event) {
  logger_->info("{}", fmt::streamed(event));
}

void LoggerComponent::on_l1_update(const L1UpdateEvent& event) {
  logger_->info("{}", fmt::streamed(event));
}

void LoggerComponent::on_l2_update(const L2UpdateEvent& event) {
  logger_->info("{}", fmt::streamed(event));
}

}
