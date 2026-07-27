// test_algo_order_management.cpp
#include "gtest/gtest.h"
#include "domain/algo_order_management.hpp"
#include "../include/messages.hpp"
#include "framework/ring_buffer_writer.hpp"
#include <memory>
#include <vector>
#include <chrono>
#include <disruptorplus/ring_buffer.hpp>
#include <disruptorplus/single_threaded_claim_strategy.hpp>
#include <disruptorplus/spin_wait_strategy.hpp>

using namespace reflex;

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
  
  // Helper to check if messages were written to ring buffer
  bool has_pending_messages() {
    // Get the last published sequence from the claim strategy
    auto last_published = claim_strategy_->last_published();
    // Check if there are any published messages (sequence >= 0 means there are messages)
    return last_published >= 0;
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

TEST_F(AlgoOrderManagementTest, OverlappingReplaceIsRejected) {
  // GIVEN: Working order
  uint64_t order_id = algo_om_->send_pending(123, Side::Buy, 1000, 50000, OrderType::Limit, TimeInForce::Gtc, 456,
      ExecInst::Default);
  auto accepted = create_accepted_event(order_id);
  algo_om_->on_accepted(&accepted);

  int pool_size_before_first_replace = algo_om_->get_order_pool_size();

  // WHEN: First replace goes in flight
  EXPECT_TRUE(algo_om_->send_pending_replace(order_id, 123, 51000, 1200));
  auto* order = algo_om_->get_order(order_id);
  ASSERT_NE(order, nullptr);
  EXPECT_EQ(order->order_wait_state_, OrderWaitState::PendingReplace);

  // AND: A second replace is requested while the first is still pending
  int pool_size_after_first_replace = algo_om_->get_order_pool_size();
  EXPECT_FALSE(algo_om_->send_pending_replace(order_id, 123, 52000, 1500));

  // THEN: The overlapping replace neither leaked a pooled order nor touched
  // the in-flight replacement
  EXPECT_EQ(algo_om_->get_order_pool_size(), pool_size_after_first_replace);
  EXPECT_EQ(pool_size_before_first_replace - 1, pool_size_after_first_replace);
  auto* replacement = algo_om_->get_order(-static_cast<int64_t>(order_id));
  ASSERT_NE(replacement, nullptr);
  EXPECT_EQ(replacement->price_, 51000);
  EXPECT_EQ(replacement->quantity_, 1200);
}

TEST_F(AlgoOrderManagementTest, ReplaceLeavesReflectsFilledQuantity) {
  // GIVEN: Working order with a partial fill of 300
  uint64_t order_id = algo_om_->send_pending(123, Side::Buy, 1000, 50000, OrderType::Limit, TimeInForce::Gtc, 456,
      ExecInst::Default);
  auto accepted = create_accepted_event(order_id);
  algo_om_->on_accepted(&accepted);
  auto executed = create_executed_event(order_id, 300, 50000);
  algo_om_->on_executed(&executed);

  // WHEN: Replaced down to a quantity of 500
  ASSERT_TRUE(algo_om_->send_pending_replace(order_id, 123, 51000, 500));

  // THEN: Replacement leaves = new quantity - filled quantity
  auto* replacement = algo_om_->get_order(-static_cast<int64_t>(order_id));
  ASSERT_NE(replacement, nullptr);
  EXPECT_EQ(replacement->leaves_quantity_, 200);

  // WHEN: Replaced down below the already-filled quantity
  ReplaceAcceptedEvent replace_accepted;
  replace_accepted.order_id_ = order_id;
  algo_om_->on_replace_accepted(&replace_accepted);
  ASSERT_TRUE(algo_om_->send_pending_replace(order_id, 123, 51000, 200));

  // THEN: Leaves clamps at 0 (treated as fully filled)
  replacement = algo_om_->get_order(-static_cast<int64_t>(order_id));
  ASSERT_NE(replacement, nullptr);
  EXPECT_EQ(replacement->leaves_quantity_, 0);
}

TEST_F(AlgoOrderManagementTest, OverfillClampsLeavesToZero) {
  // GIVEN: Working order
  uint64_t order_id = algo_om_->send_pending(123, Side::Buy, 1000, 50000, OrderType::Limit, TimeInForce::Gtc, 456,
      ExecInst::Default);
  auto accepted = create_accepted_event(order_id);
  algo_om_->on_accepted(&accepted);

  // WHEN: Execution reports more than the order quantity
  auto executed = create_executed_event(order_id, 1500, 50000);
  algo_om_->on_executed(&executed);

  // THEN: Leaves clamps at 0 and the order is treated as filled
  auto* order = algo_om_->get_order(order_id);
  ASSERT_NE(order, nullptr);  // keepFilledAndDead = true
  EXPECT_EQ(order->leaves_quantity_, 0);
  EXPECT_EQ(order->order_state_, OrderState::Filled);
  EXPECT_EQ(order->order_wait_state_, OrderWaitState::None);
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
  cancel_accepted.timestamp_ns_ = std::chrono::high_resolution_clock::now().time_since_epoch().count();
  algo_om_->on_cancel_accepted(&cancel_accepted);
  
  // THEN: Order cancelled
  auto* order = algo_om_->get_order(order_id);
  ASSERT_NE(order, nullptr);
  EXPECT_EQ(order->order_state_, OrderState::Cancelled);
  EXPECT_EQ(order->order_wait_state_, OrderWaitState::None);
}