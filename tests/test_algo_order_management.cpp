// test_algo_order_management.cpp
#include "gtest/gtest.h"
#include "domain/algo_order_management.hpp"
#include "../include/messages.hpp"
#include "framework/ring_buffer_writer.hpp"
#include <array>
#include <cstddef>
#include <cstring>
#include <memory>
#include <limits>
#include <vector>
#include <chrono>
#include <disruptorplus/ring_buffer.hpp>
#include <disruptorplus/single_threaded_claim_strategy.hpp>
#include <disruptorplus/spin_wait_strategy.hpp>

using namespace reflex;

namespace {

template <typename Value>
void write_wire_value(std::array<std::byte, 64>& bytes, std::size_t offset,
                      const Value& value) {
  std::memcpy(bytes.data() + offset, &value, sizeof(value));
}

template <typename Value>
Value read_wire_value(const std::array<std::byte, 64>& bytes,
                      std::size_t offset) {
  Value value{};
  std::memcpy(&value, bytes.data() + offset, sizeof(value));
  return value;
}

template <typename Event>
Event decode_wire_bytes(const std::array<std::byte, 64>& bytes) {
  Event event{};
  std::memcpy(&event, bytes.data(), bytes.size());
  return event;
}

}  // namespace

TEST(OrderResponseSchemaTest, RequestIdsRoundTripInFixedSizeMessageSlots) {
  static_assert(sizeof(CancelAcceptedEvent) == 64);
  static_assert(sizeof(CancelRejectedEvent) == 64);
  static_assert(sizeof(ReplaceAcceptedEvent) == 64);
  static_assert(sizeof(ReplaceRejectedEvent) == 64);

  MessageSlot cancel_accepted_slot;
  auto* cancel_accepted =
      new (cancel_accepted_slot.raw_data()) CancelAcceptedEvent();
  cancel_accepted->order_id_ = 11;
  cancel_accepted->request_id_ = std::numeric_limits<int64_t>::max();
  EXPECT_EQ(cancel_accepted_slot.as<CancelAcceptedEvent>().request_id_,
            std::numeric_limits<int64_t>::max());

  MessageSlot cancel_rejected_slot;
  auto* cancel_rejected =
      new (cancel_rejected_slot.raw_data()) CancelRejectedEvent();
  cancel_rejected->order_id_ = 12;
  cancel_rejected->request_id_ = std::numeric_limits<int64_t>::min();
  EXPECT_EQ(cancel_rejected_slot.as<CancelRejectedEvent>().request_id_,
            std::numeric_limits<int64_t>::min());

  MessageSlot replace_accepted_slot;
  auto* replace_accepted =
      new (replace_accepted_slot.raw_data()) ReplaceAcceptedEvent();
  replace_accepted->order_id_ = 13;
  replace_accepted->request_id_ = 0x1234567890ABCDEFLL;
  EXPECT_EQ(replace_accepted_slot.as<ReplaceAcceptedEvent>().request_id_,
            0x1234567890ABCDEFLL);

  MessageSlot replace_rejected_slot;
  auto* replace_rejected =
      new (replace_rejected_slot.raw_data()) ReplaceRejectedEvent();
  replace_rejected->order_id_ = 14;
  replace_rejected->request_id_ = 0x0FEDCBA098765432LL;
  EXPECT_EQ(replace_rejected_slot.as<ReplaceRejectedEvent>().request_id_,
            0x0FEDCBA098765432LL);
}

TEST(OrderResponseSchemaTest, LegacyReasonOffsetsAndRawBytesRemainCompatible) {
  static_assert(offsetof(CancelAcceptedEvent, cancel_reason_) == 24);
  static_assert(offsetof(CancelAcceptedEvent, request_id_) == 32);
  static_assert(offsetof(CancelRejectedEvent, reject_reason_) == 24);
  static_assert(offsetof(CancelRejectedEvent, request_id_) == 32);
  static_assert(offsetof(ReplaceAcceptedEvent, request_id_) == 24);
  static_assert(offsetof(ReplaceRejectedEvent, reject_reason_) == 24);
  static_assert(offsetof(ReplaceRejectedEvent, request_id_) == 32);

  constexpr int64_t timestamp = 0x0102030405060708LL;
  constexpr int64_t order_id = 0x1112131415161718LL;
  constexpr int64_t request_id = 0x2122232425262728LL;

  CancelAcceptedEvent cancel_accepted{};
  cancel_accepted.timestamp_ns_ = timestamp;
  cancel_accepted.order_id_ = order_id;
  cancel_accepted.cancel_reason_ = CancelReason::UserRequest;
  cancel_accepted.request_id_ = request_id;
  std::array<std::byte, 64> cancel_accepted_golden{};
  std::memcpy(cancel_accepted_golden.data(), &cancel_accepted,
              cancel_accepted_golden.size());
  EXPECT_EQ(read_wire_value<CancelReason>(cancel_accepted_golden, 24),
            CancelReason::UserRequest);
  EXPECT_EQ(read_wire_value<int64_t>(cancel_accepted_golden, 32), request_id);

  CancelRejectedEvent cancel_rejected{};
  cancel_rejected.reject_reason_ = RejectReason::OrderUnknown;
  cancel_rejected.request_id_ = request_id;
  std::array<std::byte, 64> cancel_rejected_golden{};
  std::memcpy(cancel_rejected_golden.data(), &cancel_rejected,
              cancel_rejected_golden.size());
  EXPECT_EQ(read_wire_value<RejectReason>(cancel_rejected_golden, 24),
            RejectReason::OrderUnknown);
  EXPECT_EQ(read_wire_value<int64_t>(cancel_rejected_golden, 32), request_id);

  ReplaceAcceptedEvent replace_accepted{};
  replace_accepted.request_id_ = request_id;
  std::array<std::byte, 64> replace_accepted_golden{};
  std::memcpy(replace_accepted_golden.data(), &replace_accepted,
              replace_accepted_golden.size());
  EXPECT_EQ(read_wire_value<int64_t>(replace_accepted_golden, 24), request_id);

  ReplaceRejectedEvent replace_rejected{};
  replace_rejected.reject_reason_ = RejectReason::PostOnly;
  replace_rejected.request_id_ = request_id;
  std::array<std::byte, 64> replace_rejected_golden{};
  std::memcpy(replace_rejected_golden.data(), &replace_rejected,
              replace_rejected_golden.size());
  EXPECT_EQ(read_wire_value<RejectReason>(replace_rejected_golden, 24),
            RejectReason::PostOnly);
  EXPECT_EQ(read_wire_value<int64_t>(replace_rejected_golden, 32), request_id);

  std::array<std::byte, 64> legacy_cancel_accepted{};
  const MessageType cancel_accepted_type = MessageType::CancelAccepted;
  const CancelReason cancel_reason = CancelReason::System;
  write_wire_value(legacy_cancel_accepted, 0, cancel_accepted_type);
  write_wire_value(legacy_cancel_accepted, 8, timestamp);
  write_wire_value(legacy_cancel_accepted, 16, order_id);
  write_wire_value(legacy_cancel_accepted, 24, cancel_reason);
  const auto decoded_cancel_accepted =
      decode_wire_bytes<CancelAcceptedEvent>(legacy_cancel_accepted);
  EXPECT_EQ(decoded_cancel_accepted.cancel_reason_, CancelReason::System);
  EXPECT_EQ(decoded_cancel_accepted.request_id_, 0);

  std::array<std::byte, 64> legacy_cancel_rejected{};
  const MessageType cancel_rejected_type = MessageType::CancelRejected;
  const RejectReason cancel_reject_reason = RejectReason::InvalidQuantity;
  write_wire_value(legacy_cancel_rejected, 0, cancel_rejected_type);
  write_wire_value(legacy_cancel_rejected, 8, timestamp);
  write_wire_value(legacy_cancel_rejected, 16, order_id);
  write_wire_value(legacy_cancel_rejected, 24, cancel_reject_reason);
  const auto decoded_cancel_rejected =
      decode_wire_bytes<CancelRejectedEvent>(legacy_cancel_rejected);
  EXPECT_EQ(decoded_cancel_rejected.reject_reason_,
            RejectReason::InvalidQuantity);
  EXPECT_EQ(decoded_cancel_rejected.request_id_, 0);

  std::array<std::byte, 64> legacy_replace_rejected{};
  const MessageType replace_rejected_type = MessageType::ReplaceRejected;
  const RejectReason replace_reject_reason = RejectReason::PostOnly;
  write_wire_value(legacy_replace_rejected, 0, replace_rejected_type);
  write_wire_value(legacy_replace_rejected, 8, timestamp);
  write_wire_value(legacy_replace_rejected, 16, order_id);
  write_wire_value(legacy_replace_rejected, 24, replace_reject_reason);
  const auto decoded_replace_rejected =
      decode_wire_bytes<ReplaceRejectedEvent>(legacy_replace_rejected);
  EXPECT_EQ(decoded_replace_rejected.reject_reason_, RejectReason::PostOnly);
  EXPECT_EQ(decoded_replace_rejected.request_id_, 0);
}

class RecordingAlgoOrderListener final : public AlgoOrderManagementListener {
public:
  void on_accepted(const Order& order, const AcceptedEvent&) override {
    ++accepted_count;
    last_order = order;
  }

  void on_rejected(const Order& order, const RejectedEvent& event) override {
    ++rejected_count;
    last_reject_reason = event.reject_reason_;
    last_order = order;
  }

  void on_replace_accepted(const Order& order,
                           const ReplaceAcceptedEvent&) override {
    ++replace_accepted_count;
    last_order = order;
  }

  void on_replace_rejected(const Order& order,
                           const ReplaceRejectedEvent& event) override {
    ++replace_rejected_count;
    last_reject_reason = event.reject_reason_;
    last_order = order;
  }

  void on_cancel_accepted(const Order& order,
                          const CancelAcceptedEvent& event) override {
    ++cancel_accepted_count;
    last_cancel_reason = event.cancel_reason_;
    last_order = order;
  }

  void on_cancel_rejected(const Order& order,
                          const CancelRejectedEvent& event) override {
    ++cancel_rejected_count;
    last_reject_reason = event.reject_reason_;
    last_order = order;
  }

  void on_executed(const Order& order, const ExecutedEvent&) override {
    ++executed_count;
    last_order = order;
  }

  int accepted_count{0};
  int rejected_count{0};
  int replace_accepted_count{0};
  int replace_rejected_count{0};
  int cancel_accepted_count{0};
  int cancel_rejected_count{0};
  int executed_count{0};
  RejectReason last_reject_reason{RejectReason::Unknown};
  CancelReason last_cancel_reason{CancelReason::Unknown};
  Order last_order{};
};

class ReentrantRejectListener final : public AlgoOrderManagementListener {
 public:
  explicit ReentrantRejectListener(AlgoOrderManagement& order_management)
      : order_management_(order_management) {}

  void on_replace_rejected(const Order& original,
                           const ReplaceRejectedEvent&) override {
    order_management_.send_pending_cancel(original.order_id_);
    ++replace_rejected_count;
  }

  void on_cancel_rejected(const Order& order,
                          const CancelRejectedEvent&) override {
    order_management_.send_pending_replace(order.order_id_,
                                           order.instrument_id_,
                                           order.price_ - 1000,
                                           order.quantity_ - 100);
    ++cancel_rejected_count;
  }

  AlgoOrderManagement& order_management_;
  int replace_rejected_count{0};
  int cancel_rejected_count{0};
};

class AlgoOrderManagementTest : public ::testing::Test {
protected:
  void SetUp() override {
    // Create a real ring buffer for testing
    const size_t BUFFER_SIZE = 8192;  // Must be a power of 2
    
    // Create the ring buffer
    ring_buffer_ = std::make_shared<disruptorplus::ring_buffer<MessageSlot>>(BUFFER_SIZE);

    auto wait_strategy = std::make_shared<disruptorplus::spin_wait_strategy>();

    claim_strategy_ =
    std::make_shared<disruptorplus::single_threaded_claim_strategy<disruptorplus::spin_wait_strategy>>(
        BUFFER_SIZE, *wait_strategy);
    
    // Create a test consumer barrier and add it to the claim strategy
    test_consumer_barrier_ = std::make_shared<disruptorplus::sequence_barrier<disruptorplus::spin_wait_strategy>>(*wait_strategy);
    claim_strategy_->add_claim_barrier(*test_consumer_barrier_);

    // Create the real RingBufferWriter
    writer_ = std::make_shared<RingBufferWriter>(ring_buffer_, claim_strategy_);

    // Create AlgoOrderManagement with real writer
    algo_om_ = std::make_unique<AlgoOrderManagement>(writer_, true);
  }


  void TearDown() override {
    // Cleanup - order matters
    algo_om_.reset();
    writer_.reset();
    test_consumer_barrier_.reset();
    claim_strategy_.reset();
    ring_buffer_.reset();
  }



  std::shared_ptr<disruptorplus::ring_buffer<MessageSlot>> ring_buffer_;
  std::shared_ptr<disruptorplus::single_threaded_claim_strategy<disruptorplus::spin_wait_strategy>> claim_strategy_;
  std::shared_ptr<disruptorplus::sequence_barrier<disruptorplus::spin_wait_strategy>> test_consumer_barrier_;
  std::shared_ptr<RingBufferWriter> writer_;
  std::unique_ptr<AlgoOrderManagement> algo_om_;

  // Helper to create events  
  AcceptedEvent create_accepted_event(uint64_t order_id, uint64_t exchange_id = 50001) {
    AcceptedEvent event;
    event.order_id_ = order_id;
    event.exchange_order_id_ = exchange_id;
    event.timestamp_ns_ = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    return event;
  }

  RejectedEvent create_rejected_event(uint64_t order_id, RejectReason reason = RejectReason::PostOnly) {
    RejectedEvent event;
    event.order_id_ = order_id;
    event.reject_reason_ = reason;
    event.timestamp_ns_ = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    return event;
  }
  
  ExecutedEvent create_executed_event(uint64_t order_id, uint64_t quantity, int64_t price) {
    ExecutedEvent event;
    event.order_id_ = order_id;
    event.last_quantity_ = quantity;
    event.last_price_ = price;
    event.timestamp_ns_ = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    return event;
  }

  int64_t create_working_order(int64_t quantity = 1000,
                               int64_t price = 50000) {
    const int64_t order_id = algo_om_->send_pending(
        123, Side::Buy, quantity, price, OrderType::Limit, TimeInForce::Gtc,
        456, ExecInst::Default);
    auto accepted = create_accepted_event(order_id);
    algo_om_->on_accepted(&accepted);
    return order_id;
  }

  int64_t current_request_id(int64_t order_id) {
    const Order* order = algo_om_->get_order(order_id);
    EXPECT_NE(order, nullptr);
    return order == nullptr ? 0 : order->request_id_;
  }
  
  // Helper to check if messages were written to ring buffer
  bool has_pending_messages() {
    // Get the last published sequence from the claim strategy
    auto last_published = claim_strategy_->last_published();
    // Check if there are any published messages (sequence >= 0 means there are messages)
    return last_published >= 0;
  }

  int64_t published_message_count() const {
    const auto last_published = claim_strategy_->last_published();
    return last_published < 0 ? 0 : last_published + 1;
  }


  
  // Helper to consume messages from ring buffer (for verification)
  std::vector<MessageSlot> consume_messages() {
    std::vector<MessageSlot> messages;

    // Get the last published sequence
    auto last_published = claim_strategy_->last_published();
    if (last_published < 0) {
      return messages; // No messages published yet
    }

    // Read all available messages
    for (disruptorplus::sequence_t seq = 0; seq <= last_published; ++seq) {
      messages.push_back((*ring_buffer_)[seq]);
    }

    // Publish that we've consumed up to the last published sequence
    test_consumer_barrier_->publish(last_published);

    return messages;
  }



};

TEST_F(AlgoOrderManagementTest, SendPendingCreatesOrderAndCommand) {
  // WHEN: Send pending order
  uint64_t order_id = algo_om_->send_pending(
      /* instrument_id */ 123,
      /* side */ Side::Buy,
      /* quantity */ 1000,
      /* price */ 50000,
      /* order_type */ OrderType::Limit,
      /* time_in_force */ TimeInForce::Gtc,
      /* account */ 456,
      ExecInst::Default
  );

  // THEN: Order exists with correct state
  auto* order = algo_om_->get_order(order_id);  // First order ID
  ASSERT_NE(order, nullptr);

  EXPECT_EQ(order->order_id_, order_id);
  EXPECT_EQ(order->instrument_id_, 123);
  EXPECT_EQ(order->side_, Side::Buy);
  EXPECT_EQ(order->quantity_, 1000);
  EXPECT_EQ(order->price_, 50000);
  EXPECT_EQ(order->order_type_, OrderType::Limit);
  EXPECT_EQ(order->time_in_force_, TimeInForce::Gtc);
  EXPECT_EQ(order->account_, 456);
  EXPECT_EQ(order->order_state_, OrderState::New);
  EXPECT_EQ(order->order_wait_state_, OrderWaitState::Pending);
  EXPECT_EQ(order->leaves_quantity_, 1000);
  
  // AND: Message was written to ring buffer
  EXPECT_TRUE(has_pending_messages());
  
  // Optionally verify the message content
  auto messages = consume_messages();
  EXPECT_GE(messages.size(), 1);
  
  // First message should be a PendingEvent
  if (!messages.empty()) {
    auto* pending = reinterpret_cast<PendingEvent*>(messages[0].raw_data());
    EXPECT_EQ(pending->order_id_, order_id);
    EXPECT_EQ(pending->instrument_id_, 123);
    EXPECT_EQ(pending->quantity_, 1000);
    EXPECT_EQ(pending->price_, 50000);
    EXPECT_EQ(pending->side_, Side::Buy);
  }
}

TEST_F(AlgoOrderManagementTest, OnAcceptedUpdatesOrderState) {
  // GIVEN: Pending order
  uint64_t order_id = algo_om_->send_pending(123, Side::Buy, 1000, 50000, OrderType::Limit, TimeInForce::Gtc, 456,
      ExecInst::Default);
  auto* order = algo_om_->get_order(order_id);
  ASSERT_NE(order, nullptr);
  ASSERT_EQ(order->order_state_, OrderState::New);
  ASSERT_EQ(order->order_wait_state_, OrderWaitState::Pending);

  // WHEN: Accepted event received
  auto accepted = create_accepted_event(order_id, 50001);
  algo_om_->on_accepted(&accepted);

  // THEN: Order state updated
  EXPECT_EQ(order->order_state_, OrderState::Working);
  EXPECT_EQ(order->order_wait_state_, OrderWaitState::None);
  EXPECT_EQ(order->exchange_order_id_, 50001);
}

TEST_F(AlgoOrderManagementTest, OnRejectedWithKeepOrder) {
  // GIVEN: Pending order
  int initial_pool_size = algo_om_->get_order_pool_size();
  uint64_t order_id = algo_om_->send_pending(123, Side::Buy, 1000, 50000, OrderType::Limit, TimeInForce::Gtc, 456,
      ExecInst::Default);
  
  // WHEN: Rejected event received
  auto rejected = create_rejected_event(order_id);
  algo_om_->on_rejected(&rejected);
  
  // THEN: Order still exists (keepFilledAndDead = true)
  auto* order = algo_om_->get_order(order_id);
  ASSERT_NE(order, nullptr);
  EXPECT_EQ(order->order_state_, OrderState::Rejected);
  EXPECT_EQ(order->order_wait_state_, OrderWaitState::None);
  
  // Order pool should be unchanged (order not released)
  EXPECT_EQ(algo_om_->get_order_pool_size(), initial_pool_size - 1);
}

TEST_F(AlgoOrderManagementTest, PartialExecution) {
  // GIVEN: Working order
  uint64_t order_id = algo_om_->send_pending(123, Side::Buy, 1000, 50000, OrderType::Limit, TimeInForce::Gtc, 456,
      ExecInst::Default);
  auto accepted = create_accepted_event(order_id);
  algo_om_->on_accepted(&accepted);
  
  auto* order = algo_om_->get_order(order_id);
  ASSERT_EQ(order->leaves_quantity_, 1000);
  
  // WHEN: Partial execution
  auto executed = create_executed_event(order_id, 300, 50000);
  algo_om_->on_executed(&executed);
  
  // THEN: Leaves quantity updated, still working
  EXPECT_EQ(order->leaves_quantity_, 700);
  EXPECT_EQ(order->order_state_, OrderState::Working);
}

TEST_F(AlgoOrderManagementTest, CompleteExecution) {
  // GIVEN: Working order
  uint64_t order_id = algo_om_->send_pending(123, Side::Buy, 1000, 50000, OrderType::Limit, TimeInForce::Gtc, 456,
      ExecInst::Default);
  auto accepted = create_accepted_event(order_id);
  algo_om_->on_accepted(&accepted);
  
  // WHEN: Complete execution
  auto executed = create_executed_event(order_id, 1000, 50000);
  algo_om_->on_executed(&executed);
  
  // THEN: Order filled
  auto* order = algo_om_->get_order(order_id);
  ASSERT_NE(order, nullptr);  // Should still exist (keepFilledAndDead = true)
  EXPECT_EQ(order->leaves_quantity_, 0);
  EXPECT_EQ(order->order_state_, OrderState::Filled);
  EXPECT_EQ(order->order_wait_state_, OrderWaitState::None);
}

TEST_F(AlgoOrderManagementTest, CancelRequest) {
  // GIVEN: Working order
  uint64_t order_id = algo_om_->send_pending(123, Side::Buy, 1000, 50000, OrderType::Limit, TimeInForce::Gtc, 456,
      ExecInst::Default);
  auto accepted = create_accepted_event(order_id);
  algo_om_->on_accepted(&accepted);
  
  auto* order = algo_om_->get_order(order_id);
  ASSERT_EQ(order->order_state_, OrderState::Working);
  
  // WHEN: Cancel request
  algo_om_->send_pending_cancel(order_id);
  
  // THEN: Order in pending cancel state
  EXPECT_EQ(order->order_state_, OrderState::Working);
  EXPECT_EQ(order->order_wait_state_, OrderWaitState::PendingCancel);
  
  // AND: Cancel message sent to ring buffer
  auto messages = consume_messages();
  EXPECT_GE(messages.size(), 2); // Original pending + cancel
}

TEST_F(AlgoOrderManagementTest,
       CancelAndReplaceRequestsMintFreshIdsAndWritersPreserveThem) {
  const int64_t order_id = create_working_order();
  Order* order = algo_om_->get_order(order_id);
  ASSERT_NE(order, nullptr);
  EXPECT_EQ(order->request_id_, 0);

  algo_om_->send_pending_cancel(order_id);
  const int64_t cancel_request_id = order->request_id_;
  EXPECT_GT(cancel_request_id, 0);
  EXPECT_EQ(order->order_wait_state_, OrderWaitState::PendingCancel);

  algo_om_->send_pending_replace(order_id, 123, 49000, 800);
  const int64_t replace_request_id = order->request_id_;
  EXPECT_GT(replace_request_id, 0);
  EXPECT_NE(replace_request_id, cancel_request_id);
  EXPECT_EQ(order->order_wait_state_, OrderWaitState::PendingReplace);
  const Order* shadow = algo_om_->get_order(-order_id);
  ASSERT_NE(shadow, nullptr);
  EXPECT_EQ(shadow->request_id_, replace_request_id);

  const auto messages = consume_messages();
  ASSERT_EQ(messages.size(), 3u);
  ASSERT_EQ(messages[1].get_type(), MessageType::PendingCancel);
  EXPECT_EQ(messages[1].as<PendingCancelEvent>().request_id_,
            cancel_request_id);
  ASSERT_EQ(messages[2].get_type(), MessageType::PendingReplace);
  EXPECT_EQ(messages[2].as<PendingReplaceEvent>().request_id_,
            replace_request_id);
}

TEST_F(AlgoOrderManagementTest,
       ResponseRingWriterPreservesRequestIdentityForEveryResponseType) {
  Order order{};
  order.order_id_ = 77;
  order.request_id_ = 88;

  writer_->send_cancel_accepted(order);
  writer_->send_cancel_rejected(order);
  writer_->send_replace_accepted(order);
  writer_->send_replace_rejected(order);

  const auto messages = consume_messages();
  ASSERT_EQ(messages.size(), 4u);
  EXPECT_EQ(messages[0].as<CancelAcceptedEvent>().request_id_, 88);
  EXPECT_EQ(messages[1].as<CancelRejectedEvent>().request_id_, 88);
  EXPECT_EQ(messages[2].as<ReplaceAcceptedEvent>().request_id_, 88);
  EXPECT_EQ(messages[3].as<ReplaceRejectedEvent>().request_id_, 88);
}

TEST_F(AlgoOrderManagementTest,
       StaleReplaceResponseCannotResolveNewerOverlappingReplace) {
  RecordingAlgoOrderListener listener;
  algo_om_->add_listener(&listener);
  const int64_t order_id = create_working_order();
  const int pool_with_original = algo_om_->get_order_pool_size();

  algo_om_->send_pending_replace(order_id, 123, 49000, 800);
  EXPECT_EQ(algo_om_->get_order_pool_size(), pool_with_original - 1);
  const int64_t first_request_id = current_request_id(order_id);
  algo_om_->send_pending_replace(order_id, 123, 48000, 700);
  EXPECT_EQ(algo_om_->get_order_pool_size(), pool_with_original - 1);
  const int64_t second_request_id = current_request_id(order_id);
  ASSERT_NE(first_request_id, second_request_id);

  ReplaceAcceptedEvent stale{};
  stale.order_id_ = order_id;
  stale.request_id_ = first_request_id;
  algo_om_->on_replace_accepted(&stale);

  const Order* order = algo_om_->get_order(order_id);
  const Order* shadow = algo_om_->get_order(-order_id);
  ASSERT_NE(order, nullptr);
  ASSERT_NE(shadow, nullptr);
  EXPECT_EQ(order->order_wait_state_, OrderWaitState::PendingReplace);
  EXPECT_EQ(order->request_id_, second_request_id);
  EXPECT_EQ(shadow->request_id_, second_request_id);
  EXPECT_EQ(shadow->price_, 48000);
  EXPECT_EQ(listener.replace_accepted_count, 0);
  EXPECT_EQ(algo_om_->request_correlation_diagnostics()
                .stale_replace_responses,
            1u);

  ReplaceAcceptedEvent current{};
  current.order_id_ = order_id;
  current.request_id_ = second_request_id;
  algo_om_->on_replace_accepted(&current);

  order = algo_om_->get_order(order_id);
  ASSERT_NE(order, nullptr);
  EXPECT_EQ(order->order_wait_state_, OrderWaitState::None);
  EXPECT_EQ(order->request_id_, 0);
  EXPECT_EQ(order->price_, 48000);
  EXPECT_EQ(order->quantity_, 700);
  EXPECT_EQ(algo_om_->get_order(-order_id), nullptr);
  EXPECT_EQ(algo_om_->get_order_pool_size(), pool_with_original);
  EXPECT_EQ(listener.replace_accepted_count, 1);

  algo_om_->on_replace_accepted(&current);
  EXPECT_EQ(listener.replace_accepted_count, 1);
  EXPECT_EQ(algo_om_->request_correlation_diagnostics()
                .stale_replace_responses,
            2u);
}

TEST_F(AlgoOrderManagementTest,
       RejectCallbacksCanSubmitTheNextCorrelatedRequest) {
  const int64_t order_id = create_working_order();
  ReentrantRejectListener listener(*algo_om_);
  algo_om_->add_listener(&listener);

  algo_om_->send_pending_replace(order_id, 123, 49000, 800);
  const int64_t replace_request_id = current_request_id(order_id);
  ReplaceRejectedEvent replace_rejected{};
  replace_rejected.order_id_ = order_id;
  replace_rejected.request_id_ = replace_request_id;
  replace_rejected.reject_reason_ = RejectReason::PostOnly;
  algo_om_->on_replace_rejected(&replace_rejected);

  const Order* order = algo_om_->get_order(order_id);
  ASSERT_NE(order, nullptr);
  ASSERT_EQ(listener.replace_rejected_count, 1);
  ASSERT_EQ(order->order_wait_state_, OrderWaitState::PendingCancel);
  const int64_t cancel_request_id = order->request_id_;
  EXPECT_GT(cancel_request_id, 0);
  EXPECT_NE(cancel_request_id, replace_request_id);
  EXPECT_EQ(algo_om_->get_order(-order_id), nullptr);

  CancelRejectedEvent cancel_rejected{};
  cancel_rejected.order_id_ = order_id;
  cancel_rejected.request_id_ = cancel_request_id;
  cancel_rejected.reject_reason_ = RejectReason::OrderUnknown;
  algo_om_->on_cancel_rejected(&cancel_rejected);

  order = algo_om_->get_order(order_id);
  ASSERT_NE(order, nullptr);
  EXPECT_EQ(listener.cancel_rejected_count, 1);
  EXPECT_EQ(order->order_wait_state_, OrderWaitState::PendingReplace);
  EXPECT_GT(order->request_id_, 0);
  EXPECT_NE(order->request_id_, cancel_request_id);
  const Order* shadow = algo_om_->get_order(-order_id);
  ASSERT_NE(shadow, nullptr);
  EXPECT_EQ(shadow->request_id_, order->request_id_);
}

TEST_F(AlgoOrderManagementTest,
       CrossOperationStaleResponsesCannotResolveNewerRequest) {
  RecordingAlgoOrderListener listener;
  algo_om_->add_listener(&listener);
  const int64_t order_id = create_working_order();

  algo_om_->send_pending_cancel(order_id);
  const int64_t cancel_request_id = current_request_id(order_id);
  algo_om_->send_pending_replace(order_id, 123, 49000, 800);
  const int64_t replace_request_id = current_request_id(order_id);

  CancelAcceptedEvent stale_cancel{};
  stale_cancel.order_id_ = order_id;
  stale_cancel.request_id_ = cancel_request_id;
  stale_cancel.cancel_reason_ = CancelReason::UserRequest;
  algo_om_->on_cancel_accepted(&stale_cancel);

  const Order* order = algo_om_->get_order(order_id);
  ASSERT_NE(order, nullptr);
  EXPECT_EQ(order->order_wait_state_, OrderWaitState::PendingReplace);
  EXPECT_EQ(order->request_id_, replace_request_id);
  EXPECT_EQ(listener.cancel_accepted_count, 0);
  EXPECT_EQ(algo_om_->request_correlation_diagnostics()
                .stale_cancel_responses,
            1u);

  algo_om_->send_pending_cancel(order_id);
  const int64_t current_cancel_request_id = current_request_id(order_id);
  EXPECT_EQ(algo_om_->get_order(-order_id), nullptr);

  ReplaceRejectedEvent stale_replace{};
  stale_replace.order_id_ = order_id;
  stale_replace.request_id_ = replace_request_id;
  stale_replace.reject_reason_ = RejectReason::OrderUnknown;
  algo_om_->on_replace_rejected(&stale_replace);

  order = algo_om_->get_order(order_id);
  ASSERT_NE(order, nullptr);
  EXPECT_EQ(order->order_wait_state_, OrderWaitState::PendingCancel);
  EXPECT_EQ(order->request_id_, current_cancel_request_id);
  EXPECT_EQ(listener.replace_rejected_count, 0);
  EXPECT_EQ(algo_om_->request_correlation_diagnostics()
                .stale_replace_responses,
            1u);

  CancelRejectedEvent current_cancel{};
  current_cancel.order_id_ = order_id;
  current_cancel.request_id_ = current_cancel_request_id;
  current_cancel.reject_reason_ = RejectReason::OrderUnknown;
  algo_om_->on_cancel_rejected(&current_cancel);
  EXPECT_EQ(order->order_wait_state_, OrderWaitState::None);
  EXPECT_EQ(order->request_id_, 0);
  EXPECT_EQ(listener.cancel_rejected_count, 1);
}

TEST_F(AlgoOrderManagementTest,
       ZeroRequestResponsesFailClosedAndAreCountedSeparately) {
  RecordingAlgoOrderListener listener;
  algo_om_->add_listener(&listener);

  const int64_t cancel_order_id = create_working_order();
  algo_om_->send_pending_cancel(cancel_order_id);
  CancelAcceptedEvent cancel{};
  cancel.order_id_ = cancel_order_id;
  cancel.cancel_reason_ = CancelReason::UserRequest;
  algo_om_->on_cancel_accepted(&cancel);
  CancelRejectedEvent cancel_rejected{};
  cancel_rejected.order_id_ = cancel_order_id;
  cancel_rejected.reject_reason_ = RejectReason::OrderUnknown;
  algo_om_->on_cancel_rejected(&cancel_rejected);
  const Order* cancel_order = algo_om_->get_order(cancel_order_id);
  ASSERT_NE(cancel_order, nullptr);
  EXPECT_EQ(cancel_order->order_wait_state_, OrderWaitState::PendingCancel);
  EXPECT_EQ(listener.cancel_accepted_count, 0);

  const int64_t replace_order_id = create_working_order();
  algo_om_->send_pending_replace(replace_order_id, 123, 49000, 800);
  ReplaceAcceptedEvent replace{};
  replace.order_id_ = replace_order_id;
  algo_om_->on_replace_accepted(&replace);
  ReplaceRejectedEvent replace_rejected{};
  replace_rejected.order_id_ = replace_order_id;
  replace_rejected.reject_reason_ = RejectReason::OrderUnknown;
  algo_om_->on_replace_rejected(&replace_rejected);
  const Order* replace_order = algo_om_->get_order(replace_order_id);
  ASSERT_NE(replace_order, nullptr);
  EXPECT_EQ(replace_order->order_wait_state_, OrderWaitState::PendingReplace);
  EXPECT_EQ(listener.replace_accepted_count, 0);

  const auto& diagnostics = algo_om_->request_correlation_diagnostics();
  EXPECT_EQ(diagnostics.uncorrelated_cancel_responses, 2u);
  EXPECT_EQ(diagnostics.uncorrelated_replace_responses, 2u);
}

TEST_F(AlgoOrderManagementTest,
       ZeroRequestSystemCancelIsLimitedToMarketOrIocLifecycle) {
  RecordingAlgoOrderListener listener;
  algo_om_->add_listener(&listener);

  const int64_t limit_order_id = create_working_order();
  algo_om_->send_pending_replace(limit_order_id, 123, 49000, 800);
  const int64_t limit_request_id = current_request_id(limit_order_id);

  CancelAcceptedEvent system_cancel{};
  system_cancel.order_id_ = limit_order_id;
  system_cancel.request_id_ = 0;
  system_cancel.cancel_reason_ = CancelReason::System;
  algo_om_->on_cancel_accepted(&system_cancel);

  const Order* limit_order = algo_om_->get_order(limit_order_id);
  ASSERT_NE(limit_order, nullptr);
  EXPECT_EQ(limit_order->order_state_, OrderState::Working);
  EXPECT_EQ(limit_order->order_wait_state_, OrderWaitState::PendingReplace);
  EXPECT_EQ(limit_order->request_id_, limit_request_id);
  EXPECT_NE(algo_om_->get_order(-limit_order_id), nullptr);
  EXPECT_EQ(listener.cancel_accepted_count, 0);

  const int64_t market_order_id = algo_om_->send_pending(
      123, Side::Buy, 1000, 0, OrderType::Market, TimeInForce::Ioc, 456,
      ExecInst::Default);
  auto accepted = create_accepted_event(market_order_id);
  algo_om_->on_accepted(&accepted);
  system_cancel.order_id_ = market_order_id;
  algo_om_->on_cancel_accepted(&system_cancel);

  const Order* market_order = algo_om_->get_order(market_order_id);
  ASSERT_NE(market_order, nullptr);
  EXPECT_EQ(market_order->order_state_, OrderState::Cancelled);
  EXPECT_EQ(market_order->order_wait_state_, OrderWaitState::None);
  EXPECT_EQ(market_order->request_id_, 0);
  EXPECT_EQ(listener.cancel_accepted_count, 1);
  EXPECT_EQ(listener.last_cancel_reason, CancelReason::System);
  const auto& diagnostics = algo_om_->request_correlation_diagnostics();
  EXPECT_EQ(diagnostics.uncorrelated_cancel_responses, 1u);
  EXPECT_EQ(diagnostics.stale_cancel_responses, 0u);
}

TEST_F(AlgoOrderManagementTest,
       ZeroRequestExchangeCancelTerminatesLiveGtcLifecycle) {
  RecordingAlgoOrderListener listener;
  algo_om_->add_listener(&listener);
  const int64_t order_id = create_working_order();
  algo_om_->send_pending_replace(order_id, 123, 49000, 800);
  ASSERT_NE(algo_om_->get_order(-order_id), nullptr);

  CancelAcceptedEvent exchange_cancel{};
  exchange_cancel.order_id_ = order_id;
  exchange_cancel.request_id_ = 0;
  exchange_cancel.cancel_reason_ = CancelReason::Exchange;
  algo_om_->on_cancel_accepted(&exchange_cancel);

  const Order* order = algo_om_->get_order(order_id);
  ASSERT_NE(order, nullptr);
  EXPECT_EQ(order->order_state_, OrderState::Cancelled);
  EXPECT_EQ(order->order_wait_state_, OrderWaitState::None);
  EXPECT_EQ(order->request_id_, 0);
  EXPECT_EQ(algo_om_->get_order(-order_id), nullptr);
  EXPECT_EQ(listener.cancel_accepted_count, 1);
  EXPECT_EQ(listener.last_cancel_reason, CancelReason::Exchange);
  EXPECT_EQ(algo_om_->request_correlation_diagnostics()
                .uncorrelated_cancel_responses,
            0u);
}

TEST_F(AlgoOrderManagementTest,
       DuplicateSystemCancelIsCountedRegardlessOfRetentionPolicy) {
  for (const bool keep_filled_and_dead : {false, true}) {
    AlgoOrderManagement order_management(writer_, keep_filled_and_dead);
    RecordingAlgoOrderListener listener;
    order_management.add_listener(&listener);

    const int64_t order_id = order_management.send_pending(
        123, Side::Buy, 1000, 0, OrderType::Market, TimeInForce::Ioc, 456,
        ExecInst::Default);
    AcceptedEvent accepted{};
    accepted.order_id_ = order_id;
    order_management.on_accepted(&accepted);

    CancelAcceptedEvent system_cancel{};
    system_cancel.order_id_ = order_id;
    system_cancel.request_id_ = 0;
    system_cancel.cancel_reason_ = CancelReason::System;
    order_management.on_cancel_accepted(&system_cancel);
    EXPECT_EQ(listener.cancel_accepted_count, 1);
    EXPECT_EQ(order_management.request_correlation_diagnostics()
                  .uncorrelated_cancel_responses,
              0u);

    const Order* retained = order_management.get_order(order_id);
    if (keep_filled_and_dead) {
      ASSERT_NE(retained, nullptr);
      EXPECT_EQ(retained->order_state_, OrderState::Cancelled);
      EXPECT_EQ(retained->order_wait_state_, OrderWaitState::None);
    } else {
      EXPECT_EQ(retained, nullptr);
    }

    order_management.on_cancel_accepted(&system_cancel);
    EXPECT_EQ(listener.cancel_accepted_count, 1);
    EXPECT_EQ(order_management.request_correlation_diagnostics()
                  .uncorrelated_cancel_responses,
              1u);

    retained = order_management.get_order(order_id);
    if (keep_filled_and_dead) {
      ASSERT_NE(retained, nullptr);
      EXPECT_EQ(retained->order_state_, OrderState::Cancelled);
      EXPECT_EQ(retained->order_wait_state_, OrderWaitState::None);
    } else {
      EXPECT_EQ(retained, nullptr);
    }
  }
}

TEST_F(AlgoOrderManagementTest, ReplaceLeavesReflectsFilledQuantity) {
  // GIVEN: Working order with a partial fill of 300
  const int64_t order_id = create_working_order();
  auto executed = create_executed_event(order_id, 300, 50000);
  executed.exec_id_ = 300;
  algo_om_->on_executed(&executed);

  // WHEN: Replaced down to a quantity of 500
  ASSERT_TRUE(algo_om_->send_pending_replace(order_id, 123, 51000, 500));

  // THEN: Replacement leaves = new quantity - filled quantity
  auto* replacement = algo_om_->get_order(-order_id);
  ASSERT_NE(replacement, nullptr);
  EXPECT_EQ(replacement->leaves_quantity_, 200);

  ReplaceAcceptedEvent replace_accepted{};
  replace_accepted.order_id_ = order_id;
  replace_accepted.request_id_ = current_request_id(order_id);
  algo_om_->on_replace_accepted(&replace_accepted);
  const Order* order = algo_om_->get_order(order_id);
  ASSERT_NE(order, nullptr);
  EXPECT_EQ(order->quantity_, 500);
  EXPECT_EQ(order->leaves_quantity_, 200);

  // WHEN: Replaced down below the already-filled quantity
  ASSERT_TRUE(algo_om_->send_pending_replace(order_id, 123, 51000, 200));

  // THEN: Leaves clamps at 0 on the in-flight replacement (treated as fully
  // filled); an acceptance of such a total is rejected as impossible.
  replacement = algo_om_->get_order(-order_id);
  ASSERT_NE(replacement, nullptr);
  EXPECT_EQ(replacement->leaves_quantity_, 0);
}

TEST_F(AlgoOrderManagementTest, ReplaceAndCancelOfUnknownOrderAreRejectedLocally) {
  const int64_t messages_before = published_message_count();
  EXPECT_FALSE(algo_om_->send_pending_replace(424242, 123, 51000, 500));
  algo_om_->send_pending_cancel(424242);
  EXPECT_EQ(published_message_count(), messages_before);
}

TEST_F(AlgoOrderManagementTest, CancelAccepted) {
  // GIVEN: Order in pending cancel
  uint64_t order_id = algo_om_->send_pending(123, Side::Buy, 1000, 50000, OrderType::Limit, TimeInForce::Gtc, 456,
      ExecInst::Default);
  auto accepted = create_accepted_event(order_id);
  algo_om_->on_accepted(&accepted);
  algo_om_->send_pending_cancel(order_id);
  
  // WHEN: Cancel accepted
  CancelAcceptedEvent cancel_accepted;
  cancel_accepted.order_id_ = order_id;
  cancel_accepted.request_id_ = current_request_id(order_id);
  cancel_accepted.timestamp_ns_ = std::chrono::high_resolution_clock::now().time_since_epoch().count();
  algo_om_->on_cancel_accepted(&cancel_accepted);
  
  // THEN: Order cancelled
  auto* order = algo_om_->get_order(order_id);
  ASSERT_NE(order, nullptr);
  EXPECT_EQ(order->order_state_, OrderState::Cancelled);
  EXPECT_EQ(order->order_wait_state_, OrderWaitState::None);
}

TEST_F(AlgoOrderManagementTest,
       PartialFillDuringPendingCancelPreservesResidualUntilCancelAccepted) {
  RecordingAlgoOrderListener listener;
  algo_om_->add_listener(&listener);
  const int64_t order_id = create_working_order();
  algo_om_->send_pending_cancel(order_id);

  auto partial_fill = create_executed_event(order_id, 300, 50000);
  partial_fill.exec_id_ = 101;
  algo_om_->on_executed(&partial_fill);

  const Order* order = algo_om_->get_order(order_id);
  ASSERT_NE(order, nullptr);
  EXPECT_EQ(order->order_state_, OrderState::Working);
  EXPECT_EQ(order->order_wait_state_, OrderWaitState::PendingCancel);
  EXPECT_EQ(order->leaves_quantity_, 700);
  EXPECT_EQ(listener.executed_count, 1);
  EXPECT_EQ(listener.last_order.leaves_quantity_, 700);

  CancelAcceptedEvent canceled{};
  canceled.order_id_ = order_id;
  canceled.request_id_ = current_request_id(order_id);
  canceled.cancel_reason_ = CancelReason::UserRequest;
  algo_om_->on_cancel_accepted(&canceled);

  order = algo_om_->get_order(order_id);
  ASSERT_NE(order, nullptr);
  EXPECT_EQ(order->order_state_, OrderState::Cancelled);
  EXPECT_EQ(order->order_wait_state_, OrderWaitState::None);
  EXPECT_EQ(order->leaves_quantity_, 700);
  EXPECT_EQ(listener.cancel_accepted_count, 1);
  EXPECT_EQ(listener.last_cancel_reason, CancelReason::UserRequest);
}

TEST_F(AlgoOrderManagementTest,
       PartialFillDuringPendingReplaceThenRejectPreservesOriginal) {
  RecordingAlgoOrderListener listener;
  algo_om_->add_listener(&listener);
  const int64_t order_id = create_working_order();
  algo_om_->send_pending_replace(order_id, 123, 49000, 800);

  auto partial_fill = create_executed_event(order_id, 300, 50000);
  partial_fill.exec_id_ = 102;
  algo_om_->on_executed(&partial_fill);

  ReplaceRejectedEvent rejected{};
  rejected.order_id_ = order_id;
  rejected.request_id_ = current_request_id(order_id);
  rejected.reject_reason_ = RejectReason::PostOnly;
  algo_om_->on_replace_rejected(&rejected);

  const Order* order = algo_om_->get_order(order_id);
  ASSERT_NE(order, nullptr);
  EXPECT_EQ(order->order_state_, OrderState::Working);
  EXPECT_EQ(order->order_wait_state_, OrderWaitState::None);
  EXPECT_EQ(order->quantity_, 1000);
  EXPECT_EQ(order->leaves_quantity_, 700);
  EXPECT_EQ(order->price_, 50000);
  EXPECT_EQ(algo_om_->get_order(-order_id), nullptr);
  EXPECT_EQ(listener.replace_rejected_count, 1);
  EXPECT_EQ(listener.last_reject_reason, RejectReason::PostOnly);
  EXPECT_EQ(listener.last_order.leaves_quantity_, 700);
}

TEST_F(AlgoOrderManagementTest,
       PartialFillDuringPendingReplaceThenAcceptUsesTotalQuantityDelta) {
  RecordingAlgoOrderListener listener;
  algo_om_->add_listener(&listener);
  const int64_t order_id = create_working_order();
  algo_om_->send_pending_replace(order_id, 123, 49000, 800);

  auto partial_fill = create_executed_event(order_id, 300, 50000);
  partial_fill.exec_id_ = 103;
  algo_om_->on_executed(&partial_fill);

  ReplaceAcceptedEvent accepted{};
  accepted.order_id_ = order_id;
  accepted.request_id_ = current_request_id(order_id);
  algo_om_->on_replace_accepted(&accepted);

  const Order* order = algo_om_->get_order(order_id);
  ASSERT_NE(order, nullptr);
  EXPECT_EQ(order->order_state_, OrderState::Working);
  EXPECT_EQ(order->order_wait_state_, OrderWaitState::None);
  EXPECT_EQ(order->quantity_, 800);
  EXPECT_EQ(order->leaves_quantity_, 500);
  EXPECT_EQ(order->price_, 49000);
  EXPECT_EQ(algo_om_->get_order(-order_id), nullptr);
  EXPECT_EQ(listener.replace_accepted_count, 1);
  EXPECT_EQ(listener.last_order.leaves_quantity_, 500);
}

TEST_F(AlgoOrderManagementTest,
       LateCancelRejectAfterFullFillDoesNotResurrectTerminalOrder) {
  RecordingAlgoOrderListener listener;
  algo_om_->add_listener(&listener);
  const int64_t order_id = create_working_order();
  algo_om_->send_pending_cancel(order_id);
  const int64_t cancel_request_id = current_request_id(order_id);

  auto full_fill = create_executed_event(order_id, 1000, 50000);
  full_fill.exec_id_ = 104;
  algo_om_->on_executed(&full_fill);
  EXPECT_EQ(current_request_id(order_id), 0);

  CancelRejectedEvent late_reject{};
  late_reject.order_id_ = order_id;
  late_reject.request_id_ = cancel_request_id;
  late_reject.reject_reason_ = RejectReason::OrderUnknown;
  algo_om_->on_cancel_rejected(&late_reject);

  const Order* order = algo_om_->get_order(order_id);
  ASSERT_NE(order, nullptr);
  EXPECT_EQ(order->order_state_, OrderState::Filled);
  EXPECT_EQ(order->order_wait_state_, OrderWaitState::None);
  EXPECT_EQ(order->leaves_quantity_, 0);
  EXPECT_EQ(listener.cancel_rejected_count, 0);
  EXPECT_EQ(algo_om_->request_correlation_diagnostics()
                .stale_cancel_responses,
            0u);
  EXPECT_EQ(algo_om_->request_correlation_diagnostics()
                .terminal_fill_order_unknown_cancel_rejects,
            1u);
}

TEST_F(AlgoOrderManagementTest,
       FillBeforeOrderUnknownCancelRejectIsClassifiedWithEitherRetentionPolicy) {
  for (const bool keep_filled_and_dead : {false, true}) {
    AlgoOrderManagement order_management(writer_, keep_filled_and_dead);
    const int64_t order_id = order_management.send_pending(
        123, Side::Buy, 1000, 50000, OrderType::Limit, TimeInForce::Gtc,
        456, ExecInst::Default);
    AcceptedEvent accepted{};
    accepted.order_id_ = order_id;
    order_management.on_accepted(&accepted);
    order_management.send_pending_cancel(order_id);
    const int64_t request_id =
        order_management.get_order(order_id)->request_id_;

    ExecutedEvent fill{};
    fill.order_id_ = order_id;
    fill.last_quantity_ = 1000;
    fill.last_price_ = 50000;
    fill.exec_id_ = keep_filled_and_dead ? 108 : 109;
    order_management.on_executed(&fill);

    CancelRejectedEvent reject{};
    reject.order_id_ = order_id;
    reject.request_id_ = request_id;
    reject.reject_reason_ = RejectReason::OrderUnknown;
    order_management.on_cancel_rejected(&reject);

    const auto& diagnostics =
        order_management.request_correlation_diagnostics();
    EXPECT_EQ(diagnostics.stale_cancel_responses, 0U);
    EXPECT_EQ(diagnostics.uncorrelated_cancel_responses, 0U);
    EXPECT_EQ(diagnostics.terminal_fill_order_unknown_cancel_rejects, 1U);
  }
}

TEST_F(AlgoOrderManagementTest,
       LateReplaceOrderUnknownRejectAfterFullFillIsLifecycleRace) {
  RecordingAlgoOrderListener listener;
  algo_om_->add_listener(&listener);
  const int64_t order_id = create_working_order();
  algo_om_->send_pending_replace(order_id, 123, 49000, 800);
  const int64_t replace_request_id = current_request_id(order_id);

  auto full_fill = create_executed_event(order_id, 1000, 50000);
  full_fill.exec_id_ = 105;
  algo_om_->on_executed(&full_fill);

  ReplaceRejectedEvent late_reject{};
  late_reject.order_id_ = order_id;
  late_reject.request_id_ = replace_request_id;
  late_reject.reject_reason_ = RejectReason::OrderUnknown;
  algo_om_->on_replace_rejected(&late_reject);

  const Order* order = algo_om_->get_order(order_id);
  ASSERT_NE(order, nullptr);
  EXPECT_EQ(order->order_state_, OrderState::Filled);
  EXPECT_EQ(order->leaves_quantity_, 0);
  EXPECT_EQ(listener.replace_rejected_count, 0);
  const auto& diagnostics = algo_om_->request_correlation_diagnostics();
  EXPECT_EQ(diagnostics.stale_replace_responses, 0u);
  EXPECT_EQ(diagnostics.uncorrelated_replace_responses, 0u);
  EXPECT_EQ(diagnostics.terminal_fill_order_unknown_replace_rejects, 1u);
}

TEST_F(AlgoOrderManagementTest,
       LateCancelRejectWithUnexpectedReasonRemainsFailClosed) {
  const int64_t order_id = create_working_order();
  algo_om_->send_pending_cancel(order_id);
  const int64_t cancel_request_id = current_request_id(order_id);
  auto full_fill = create_executed_event(order_id, 1000, 50000);
  full_fill.exec_id_ = 106;
  algo_om_->on_executed(&full_fill);

  CancelRejectedEvent late_reject{};
  late_reject.order_id_ = order_id;
  late_reject.request_id_ = cancel_request_id;
  late_reject.reject_reason_ = RejectReason::RateLimit;
  algo_om_->on_cancel_rejected(&late_reject);

  const auto& diagnostics = algo_om_->request_correlation_diagnostics();
  EXPECT_EQ(diagnostics.stale_cancel_responses, 1u);
  EXPECT_EQ(diagnostics.terminal_fill_order_unknown_cancel_rejects, 0u);
  EXPECT_EQ(diagnostics.cancel_reject_correlation_failures_by_reason[
                static_cast<size_t>(RejectReason::RateLimit)],
            1u);
}

TEST_F(AlgoOrderManagementTest,
       MismatchedOrderUnknownCancelRejectRemainsFailClosed) {
  const int64_t order_id = create_working_order();
  algo_om_->send_pending_cancel(order_id);
  const int64_t cancel_request_id = current_request_id(order_id);
  auto full_fill = create_executed_event(order_id, 1000, 50000);
  full_fill.exec_id_ = 107;
  algo_om_->on_executed(&full_fill);

  CancelRejectedEvent mismatched{};
  mismatched.order_id_ = order_id + 999;
  mismatched.request_id_ = cancel_request_id;
  mismatched.reject_reason_ = RejectReason::OrderUnknown;
  algo_om_->on_cancel_rejected(&mismatched);

  const auto& diagnostics = algo_om_->request_correlation_diagnostics();
  EXPECT_EQ(diagnostics.stale_cancel_responses, 1u);
  EXPECT_EQ(diagnostics.terminal_fill_order_unknown_cancel_rejects, 0u);
  EXPECT_EQ(diagnostics.cancel_reject_correlation_failures_by_reason[
                static_cast<size_t>(RejectReason::OrderUnknown)],
            1u);
}

TEST_F(AlgoOrderManagementTest,
       SameIdentityWrongOperationCannotConsumeTerminalFillTombstone) {
  const int64_t order_id = create_working_order();
  algo_om_->send_pending_cancel(order_id);
  const int64_t cancel_request_id = current_request_id(order_id);
  auto full_fill = create_executed_event(order_id, 1000, 50000);
  full_fill.exec_id_ = 110;
  algo_om_->on_executed(&full_fill);

  ReplaceRejectedEvent wrong_operation{};
  wrong_operation.order_id_ = order_id;
  wrong_operation.request_id_ = cancel_request_id;
  wrong_operation.reject_reason_ = RejectReason::OrderUnknown;
  algo_om_->on_replace_rejected(&wrong_operation);

  auto diagnostics = algo_om_->request_correlation_diagnostics();
  EXPECT_EQ(diagnostics.stale_replace_responses, 1U);
  EXPECT_EQ(diagnostics.terminal_fill_order_unknown_cancel_rejects, 0U);
  EXPECT_EQ(diagnostics.terminal_fill_order_unknown_replace_rejects, 0U);
  EXPECT_EQ(diagnostics.replace_reject_correlation_failures_by_reason[
                static_cast<size_t>(RejectReason::OrderUnknown)],
            1U);

  CancelRejectedEvent matching_operation{};
  matching_operation.order_id_ = order_id;
  matching_operation.request_id_ = cancel_request_id;
  matching_operation.reject_reason_ = RejectReason::OrderUnknown;
  algo_om_->on_cancel_rejected(&matching_operation);

  diagnostics = algo_om_->request_correlation_diagnostics();
  EXPECT_EQ(diagnostics.stale_cancel_responses, 0U);
  EXPECT_EQ(diagnostics.terminal_fill_order_unknown_cancel_rejects, 1U);
}

TEST_F(AlgoOrderManagementTest,
       LateReplaceAcceptAfterFullFillDoesNotResurrectTerminalOrder) {
  RecordingAlgoOrderListener listener;
  algo_om_->add_listener(&listener);
  const int64_t order_id = create_working_order();
  algo_om_->send_pending_replace(order_id, 123, 49000, 800);
  const int64_t replace_request_id = current_request_id(order_id);

  auto full_fill = create_executed_event(order_id, 1000, 50000);
  full_fill.exec_id_ = 105;
  algo_om_->on_executed(&full_fill);
  EXPECT_EQ(current_request_id(order_id), 0);
  EXPECT_EQ(algo_om_->get_order(-order_id), nullptr);

  ReplaceAcceptedEvent late_accept{};
  late_accept.order_id_ = order_id;
  late_accept.request_id_ = replace_request_id;
  algo_om_->on_replace_accepted(&late_accept);

  const Order* order = algo_om_->get_order(order_id);
  ASSERT_NE(order, nullptr);
  EXPECT_EQ(order->order_state_, OrderState::Filled);
  EXPECT_EQ(order->order_wait_state_, OrderWaitState::None);
  EXPECT_EQ(order->quantity_, 1000);
  EXPECT_EQ(order->leaves_quantity_, 0);
  EXPECT_EQ(order->price_, 50000);
  EXPECT_EQ(algo_om_->get_order(-order_id), nullptr);
  EXPECT_EQ(listener.replace_accepted_count, 0);
  EXPECT_EQ(algo_om_->request_correlation_diagnostics()
                .stale_replace_responses,
            1u);
}

TEST_F(AlgoOrderManagementTest,
       DuplicateExecutionIdIsIdempotentAndNeverMakesLeavesNegative) {
  RecordingAlgoOrderListener listener;
  algo_om_->add_listener(&listener);
  const int64_t order_id = create_working_order();

  auto fill = create_executed_event(order_id, 1000, 50000);
  fill.exec_id_ = 106;
  algo_om_->on_executed(&fill);
  algo_om_->on_executed(&fill);

  const Order* order = algo_om_->get_order(order_id);
  ASSERT_NE(order, nullptr);
  EXPECT_EQ(order->order_state_, OrderState::Filled);
  EXPECT_EQ(order->order_wait_state_, OrderWaitState::None);
  EXPECT_EQ(order->leaves_quantity_, 0);
  EXPECT_EQ(listener.executed_count, 1);
}

TEST_F(AlgoOrderManagementTest,
       SameExecutionIdOnDifferentOrdersIsAcceptedPerOrder) {
  RecordingAlgoOrderListener listener;
  algo_om_->add_listener(&listener);
  const int64_t first_order_id = create_working_order();
  const int64_t second_order_id = create_working_order();

  auto first_fill = create_executed_event(first_order_id, 400, 50000);
  first_fill.exec_id_ = 108;
  algo_om_->on_executed(&first_fill);
  auto second_fill = create_executed_event(second_order_id, 400, 50000);
  second_fill.exec_id_ = first_fill.exec_id_;
  algo_om_->on_executed(&second_fill);

  const Order* first = algo_om_->get_order(first_order_id);
  const Order* second = algo_om_->get_order(second_order_id);
  ASSERT_NE(first, nullptr);
  ASSERT_NE(second, nullptr);
  EXPECT_EQ(first->leaves_quantity_, 600);
  EXPECT_EQ(second->leaves_quantity_, 600);
  EXPECT_EQ(first->order_state_, OrderState::Working);
  EXPECT_EQ(second->order_state_, OrderState::Working);
  EXPECT_EQ(listener.executed_count, 2);
}

TEST_F(AlgoOrderManagementTest,
       NonPositiveAndOversizedExecutionsFailClosed) {
  RecordingAlgoOrderListener listener;
  algo_om_->add_listener(&listener);
  const int64_t order_id = create_working_order();

  ExecutedEvent zero{};
  zero.order_id_ = order_id;
  zero.last_quantity_ = 0;
  zero.last_price_ = 50000;
  zero.exec_id_ = 109;
  algo_om_->on_executed(&zero);

  ExecutedEvent negative = zero;
  negative.last_quantity_ = -1;
  negative.exec_id_ = 110;
  algo_om_->on_executed(&negative);

  ExecutedEvent oversized = zero;
  oversized.last_quantity_ = 1001;
  oversized.exec_id_ = 111;
  algo_om_->on_executed(&oversized);

  const Order* order = algo_om_->get_order(order_id);
  ASSERT_NE(order, nullptr);
  EXPECT_EQ(order->order_state_, OrderState::Working);
  EXPECT_EQ(order->order_wait_state_, OrderWaitState::None);
  EXPECT_EQ(order->leaves_quantity_, 1000);
  EXPECT_EQ(listener.executed_count, 0);
}

TEST_F(AlgoOrderManagementTest,
       TerminalOrdersSuppressOutboundCancelAndReplaceRequests) {
  const int64_t rejected_id = algo_om_->send_pending(
      123, Side::Buy, 1000, 50000, OrderType::Limit, TimeInForce::Gtc, 456,
      ExecInst::Default);
  auto rejected = create_rejected_event(rejected_id);
  algo_om_->on_rejected(&rejected);

  const int64_t filled_id = create_working_order();
  auto full_fill = create_executed_event(filled_id, 1000, 50000);
  full_fill.exec_id_ = 112;
  algo_om_->on_executed(&full_fill);

  const int64_t canceled_id = create_working_order();
  algo_om_->send_pending_cancel(canceled_id);
  CancelAcceptedEvent canceled{};
  canceled.order_id_ = canceled_id;
  canceled.request_id_ = current_request_id(canceled_id);
  canceled.cancel_reason_ = CancelReason::UserRequest;
  algo_om_->on_cancel_accepted(&canceled);

  struct ExpectedTerminal {
    int64_t order_id;
    OrderState state;
    int64_t leaves;
  };
  const std::vector<ExpectedTerminal> terminals{
      {rejected_id, OrderState::Rejected, 1000},
      {filled_id, OrderState::Filled, 0},
      {canceled_id, OrderState::Cancelled, 1000},
  };

  for (const auto& expected : terminals) {
    const int64_t messages_before = published_message_count();
    algo_om_->send_pending_cancel(expected.order_id);
    algo_om_->send_pending_replace(expected.order_id, 123, 49000, 800);

    const Order* order = algo_om_->get_order(expected.order_id);
    ASSERT_NE(order, nullptr);
    EXPECT_EQ(order->order_state_, expected.state);
    EXPECT_EQ(order->order_wait_state_, OrderWaitState::None);
    EXPECT_EQ(order->leaves_quantity_, expected.leaves);
    EXPECT_EQ(algo_om_->get_order(-expected.order_id), nullptr);
    EXPECT_EQ(published_message_count(), messages_before);
  }
}

TEST_F(AlgoOrderManagementTest,
       LateNewAndCancelResponsesPreserveTerminalStateWithoutCallbacks) {
  const int64_t order_id = create_working_order();
  auto full_fill = create_executed_event(order_id, 1000, 50000);
  full_fill.exec_id_ = 113;
  algo_om_->on_executed(&full_fill);

  RecordingAlgoOrderListener listener;
  algo_om_->add_listener(&listener);

  auto accepted = create_accepted_event(order_id, 70001);
  algo_om_->on_accepted(&accepted);
  auto rejected = create_rejected_event(order_id, RejectReason::PostOnly);
  algo_om_->on_rejected(&rejected);
  CancelAcceptedEvent cancel_accepted{};
  cancel_accepted.order_id_ = order_id;
  cancel_accepted.cancel_reason_ = CancelReason::System;
  algo_om_->on_cancel_accepted(&cancel_accepted);
  CancelRejectedEvent cancel_rejected{};
  cancel_rejected.order_id_ = order_id;
  cancel_rejected.reject_reason_ = RejectReason::OrderUnknown;
  algo_om_->on_cancel_rejected(&cancel_rejected);

  const Order* order = algo_om_->get_order(order_id);
  ASSERT_NE(order, nullptr);
  EXPECT_EQ(order->order_state_, OrderState::Filled);
  EXPECT_EQ(order->order_wait_state_, OrderWaitState::None);
  EXPECT_EQ(order->leaves_quantity_, 0);
  EXPECT_EQ(order->quantity_, 1000);
  EXPECT_EQ(order->price_, 50000);
  EXPECT_EQ(order->exchange_order_id_, 50001);
  EXPECT_EQ(listener.accepted_count, 0);
  EXPECT_EQ(listener.rejected_count, 0);
  EXPECT_EQ(listener.cancel_accepted_count, 0);
  EXPECT_EQ(listener.cancel_rejected_count, 0);
}

TEST_F(AlgoOrderManagementTest,
       LateReplaceResponsesPreserveTerminalStateAndReleaseShadows) {
  const int64_t accept_order_id = create_working_order();
  algo_om_->send_pending_replace(accept_order_id, 123, 49000, 800);
  const int64_t accept_request_id = current_request_id(accept_order_id);
  auto accept_fill = create_executed_event(accept_order_id, 1000, 50000);
  accept_fill.exec_id_ = 114;
  algo_om_->on_executed(&accept_fill);

  const int64_t reject_order_id = create_working_order();
  algo_om_->send_pending_replace(reject_order_id, 123, 48000, 700);
  const int64_t reject_request_id = current_request_id(reject_order_id);
  auto reject_fill = create_executed_event(reject_order_id, 1000, 50000);
  reject_fill.exec_id_ = 115;
  algo_om_->on_executed(&reject_fill);

  RecordingAlgoOrderListener listener;
  algo_om_->add_listener(&listener);

  ReplaceAcceptedEvent late_accept{};
  late_accept.order_id_ = accept_order_id;
  late_accept.request_id_ = accept_request_id;
  algo_om_->on_replace_accepted(&late_accept);
  ReplaceRejectedEvent late_reject{};
  late_reject.order_id_ = reject_order_id;
  late_reject.request_id_ = reject_request_id;
  late_reject.reject_reason_ = RejectReason::OrderUnknown;
  algo_om_->on_replace_rejected(&late_reject);

  for (const int64_t order_id : {accept_order_id, reject_order_id}) {
    const Order* order = algo_om_->get_order(order_id);
    ASSERT_NE(order, nullptr);
    EXPECT_EQ(order->order_state_, OrderState::Filled);
    EXPECT_EQ(order->order_wait_state_, OrderWaitState::None);
    EXPECT_EQ(order->leaves_quantity_, 0);
    EXPECT_EQ(order->quantity_, 1000);
    EXPECT_EQ(order->price_, 50000);
    EXPECT_EQ(algo_om_->get_order(-order_id), nullptr);
  }
  EXPECT_EQ(listener.replace_accepted_count, 0);
  EXPECT_EQ(listener.replace_rejected_count, 0);
}

TEST_F(AlgoOrderManagementTest,
       ImpossibleReplaceAcceptPreservesOriginalAndSuppressesCallback) {
  RecordingAlgoOrderListener listener;
  algo_om_->add_listener(&listener);

  for (const int64_t replacement_total : {700, 600}) {
    const int64_t order_id = create_working_order();
    auto partial_fill = create_executed_event(order_id, 700, 50000);
    partial_fill.exec_id_ = 200 + order_id;
    algo_om_->on_executed(&partial_fill);
    const int callback_count_before = listener.replace_accepted_count;

    algo_om_->send_pending_replace(order_id, 123, 49000,
                                   replacement_total);
    ASSERT_NE(algo_om_->get_order(-order_id), nullptr);
    ReplaceAcceptedEvent impossible{};
    impossible.order_id_ = order_id;
    impossible.request_id_ = current_request_id(order_id);
    algo_om_->on_replace_accepted(&impossible);

    const Order* order = algo_om_->get_order(order_id);
    ASSERT_NE(order, nullptr);
    EXPECT_EQ(order->order_state_, OrderState::Working);
    EXPECT_EQ(order->order_wait_state_, OrderWaitState::None);
    EXPECT_EQ(order->quantity_, 1000);
    EXPECT_EQ(order->leaves_quantity_, 300);
    EXPECT_EQ(order->price_, 50000);
    EXPECT_EQ(algo_om_->get_order(-order_id), nullptr);
    EXPECT_EQ(listener.replace_accepted_count, callback_count_before);
  }
}

TEST_F(AlgoOrderManagementTest,
       ResponsesForStaleOrderIdLeaveLiveOrderUnchanged) {
  RecordingAlgoOrderListener listener;
  algo_om_->add_listener(&listener);
  const int64_t live_order_id = create_working_order();

  ReplaceRejectedEvent stale_replace{};
  stale_replace.order_id_ = 999999;
  stale_replace.reject_reason_ = RejectReason::OrderUnknown;
  algo_om_->on_replace_rejected(&stale_replace);
  CancelRejectedEvent stale_cancel{};
  stale_cancel.order_id_ = stale_replace.order_id_;
  stale_cancel.reject_reason_ = RejectReason::OrderUnknown;
  algo_om_->on_cancel_rejected(&stale_cancel);
  auto stale_fill = create_executed_event(stale_replace.order_id_, 100, 50000);
  stale_fill.exec_id_ = 107;
  algo_om_->on_executed(&stale_fill);

  const Order* order = algo_om_->get_order(live_order_id);
  ASSERT_NE(order, nullptr);
  EXPECT_EQ(order->order_state_, OrderState::Working);
  EXPECT_EQ(order->order_wait_state_, OrderWaitState::None);
  EXPECT_EQ(order->leaves_quantity_, 1000);
  EXPECT_EQ(listener.replace_rejected_count, 0);
  EXPECT_EQ(listener.cancel_rejected_count, 0);
  EXPECT_EQ(listener.executed_count, 0);
}
