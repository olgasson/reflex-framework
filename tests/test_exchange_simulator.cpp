
#include "backtest/exchange_simulator.hpp"

#include "domain/order_book_manager.hpp"
#include "offset_epoch_nano_clock.hpp"
#include "backtest/mock_exchange_response_handler.hpp"
#include "utils/codec_utils.hpp"

#include <gtest/gtest.h>
#include <memory>

using namespace reflex;
using namespace reflex::backtest;

class ExchangeSimulatorTest : public ::testing::Test {
protected:
  void SetUp() override {
    clock = std::make_shared<SimulationClock>();
    handler = std::make_unique<MockExchangeResponseHandler>();
    sim = std::make_unique<ExchangeSimulator>(clock, handler.get());
  }

  // Helper to create individual L2 update messages
  void send_l2_update(uint32_t instrument_id, Side side, double price, double quantity) {
    L2UpdateEvent l2;
    l2.instrument_id_ = instrument_id;
    l2.exchange_ = Exchange::Okx;
    l2.side_ = side;
    l2.price_1_ = CodecUtils::encode_price(price);
    l2.size_1_ = CodecUtils::encode_quantity(quantity);
    l2.timestamp_ns_ = clock->epoch_nanos();
    l2.snapshot_ = BooleanEnum::FALSE;
    l2.num_levels_ = 1;
    // Book only becomes ready() at batch end; without this best bid/ask read as 0
    // and post-only crossing checks / queue-position lookups silently no-op.
    l2.is_last_batch_ = BooleanEnum::TRUE;
    sim->process_l2_update(l2);
  }

  // Helper to create an external trade print (size = fill budget for resting orders)
  void send_trade(uint32_t instrument_id, Side aggressor_side, double price, double size) {
    TradeEvent trade{};
    trade.instrument_id_ = instrument_id;
    trade.side_ = aggressor_side;
    trade.price_ = CodecUtils::encode_price(price);
    trade.size_ = CodecUtils::encode_quantity(size);
    trade.timestamp_ns_ = clock->epoch_nanos();
    sim->process_trade_event(trade);
  }

  void send_bid_update(uint32_t instrument_id, double price, double quantity) {
    send_l2_update(instrument_id, Side::Buy, price, quantity);
  }

  void send_ask_update(uint32_t instrument_id, double price, double quantity) {
    send_l2_update(instrument_id, Side::Sell, price, quantity);
  }

  std::shared_ptr<SimulationClock> clock;
  std::unique_ptr<MockExchangeResponseHandler> handler;
  std::unique_ptr<ExchangeSimulator> sim;
};

// --- 1. Basic post-only accept / reject ----------------------

TEST_F(ExchangeSimulatorTest, AcceptsPostOnlyLimitOrderWhenNotCrossing) {
  // Send individual L2 updates to build the book
  send_bid_update(1, 100.00, 1.0);   // Best bid
  send_bid_update(1, 99.50, 2.0);    // Second bid level
  send_ask_update(1, 101.00, 1.0);   // Best ask
  send_ask_update(1, 101.50, 3.0);   // Second ask level

  PendingEvent e{};
  e.order_id_ = 1;
  e.instrument_id_ = 1;
  e.side_ = Side::Buy;
  e.price_ = CodecUtils::encode_price(100.00); // equal to best bid, not crossing
  e.quantity_ = CodecUtils::encode_quantity(10.0);
  e.order_type_ = OrderType::Limit;
  e.exec_inst_ = ExecInst::ParticipateDontInitiate;

  sim->on_pending(e);

  ASSERT_TRUE(handler->has<AcceptedEvent>());
  ASSERT_FALSE(handler->has<RejectedEvent>());
}

TEST_F(ExchangeSimulatorTest, RejectsPostOnlyCrossingLimitOrder) {
  // Send individual L2 updates
  send_bid_update(1, 100.00, 1.0);
  send_ask_update(1, 101.00, 1.0);

  PendingEvent e{};
  e.order_id_ = 1;
  e.instrument_id_ = 1;
  e.side_ = Side::Buy;
  e.price_ = CodecUtils::encode_price(102.00); // crosses ask
  e.quantity_ = CodecUtils::encode_quantity(10.0);
  e.order_type_ = OrderType::Limit;
  e.exec_inst_ = ExecInst::ParticipateDontInitiate;

  sim->on_pending(e);

  ASSERT_TRUE(handler->has<RejectedEvent>());
  const auto rej = handler->last<RejectedEvent>();
  ASSERT_EQ(rej->reject_reason_, RejectReason::PostOnly);
}

// --- 2. Market order sweeps top of book ----------------------

TEST_F(ExchangeSimulatorTest, MarketBuySweepsTopOfBook) {
  // Build ask side of book with multiple levels
  send_ask_update(1, 101.00, 5.0);   // Best ask - should be hit first
  send_ask_update(1, 102.00, 10.0);  // Second level

  PendingEvent e{};
  e.order_id_ = 1;
  e.instrument_id_ = 1;
  e.side_ = Side::Buy;
  e.quantity_ = CodecUtils::encode_quantity(10.0);
  e.order_type_ = OrderType::Market;

  sim->on_pending(e);

  ASSERT_TRUE(handler->has<AcceptedEvent>());
  ASSERT_TRUE(handler->has<ExecutedEvent>());

  // Check that we have 2 executions
  EXPECT_EQ(handler->events.size(), 3); // 1 Accepted + 2 Executed

  // Get the first execution (should be at best price)
  ExecutedEvent first_exec = handler->events[1]->as<ExecutedEvent>(); // events[0] is Accepted
  EXPECT_EQ(first_exec.last_price_, CodecUtils::encode_price(101.00));
  EXPECT_EQ(first_exec.last_quantity_, CodecUtils::encode_quantity(5.0));

  // Check the second execution too
  ExecutedEvent second_exec = handler->events[2]->as<ExecutedEvent>();
  EXPECT_EQ(second_exec.last_price_, CodecUtils::encode_price(102.00));
  EXPECT_EQ(second_exec.last_quantity_, CodecUtils::encode_quantity(5.0));
}

TEST_F(ExchangeSimulatorTest, MarketSellSweepsTopOfBook) {
  // Build bid side of book
  send_bid_update(1, 100.00, 5.0);   // Best bid - should be hit first
  send_bid_update(1, 99.00, 10.0);   // Second level

  PendingEvent e{};
  e.order_id_ = 2;
  e.instrument_id_ = 1;
  e.side_ = Side::Sell;
  e.quantity_ = CodecUtils::encode_quantity(10.0);
  e.order_type_ = OrderType::Market;

  sim->on_pending(e);

  ASSERT_TRUE(handler->has<AcceptedEvent>());
  ASSERT_TRUE(handler->has<ExecutedEvent>());

  // Check that we have 2 executions
  EXPECT_EQ(handler->events.size(), 3); // 1 Accepted + 2 Executed

  // Get the first execution (should be at best bid price)
  ExecutedEvent first_exec = handler->events[1]->as<ExecutedEvent>(); // events[0] is Accepted
  EXPECT_EQ(first_exec.last_price_, CodecUtils::encode_price(100.00)); // Best bid
  EXPECT_EQ(first_exec.last_quantity_, CodecUtils::encode_quantity(5.0));

  // Check the second execution
  ExecutedEvent second_exec = handler->events[2]->as<ExecutedEvent>();
  EXPECT_EQ(second_exec.last_price_, CodecUtils::encode_price(99.00)); // Second bid
  EXPECT_EQ(second_exec.last_quantity_, CodecUtils::encode_quantity(5.0));
}

// --- 3. Resting orders and trade-based fills -----------------

TEST_F(ExchangeSimulatorTest, RestingBuyFilledWhenTradeCrossesBelow) {
  PendingEvent e{};
  e.order_id_ = 10;
  e.instrument_id_ = 1;
  e.side_ = Side::Buy;
  e.price_ = CodecUtils::encode_price(100.00);
  e.quantity_ = CodecUtils::encode_quantity(5.0);
  e.order_type_ = OrderType::Limit;
  e.exec_inst_ = ExecInst::ParticipateDontInitiate;

  sim->on_pending(e);
  ASSERT_TRUE(handler->has<AcceptedEvent>());
  handler->clear();

  // External sell of 5.0 at 99.00 (through our bid). No displayed depth at our
  // price -> we are first in line, so the full print is our fill budget.
  send_trade(1, Side::Sell, 99.00, 5.0);

  ASSERT_TRUE(handler->has<ExecutedEvent>());
  const auto ex = handler->last<ExecutedEvent>();
  EXPECT_EQ(ex->order_id_, e.order_id_);
  EXPECT_EQ(ex->last_quantity_, CodecUtils::encode_quantity(5.0));
  EXPECT_EQ(ex->last_price_, CodecUtils::encode_price(100.00));  // filled at OUR limit
}

TEST_F(ExchangeSimulatorTest, RestingSellFilledWhenTradeCrossesAbove) {
  PendingEvent e{};
  e.order_id_ = 20;
  e.instrument_id_ = 1;
  e.side_ = Side::Sell;
  e.price_ = CodecUtils::encode_price(101.00);
  e.quantity_ = CodecUtils::encode_quantity(5.0);
  e.order_type_ = OrderType::Limit;
  e.exec_inst_ = ExecInst::ParticipateDontInitiate;

  sim->on_pending(e);
  handler->clear();

  // External buy of 5.0 at 102.00 (through our ask); no depth ahead of us.
  send_trade(1, Side::Buy, 102.00, 5.0);

  ASSERT_TRUE(handler->has<ExecutedEvent>());
  const auto ex = handler->last<ExecutedEvent>();
  EXPECT_EQ(ex->order_id_, e.order_id_);
  EXPECT_EQ(ex->last_quantity_, CodecUtils::encode_quantity(5.0));
  EXPECT_EQ(ex->last_price_, CodecUtils::encode_price(101.00));  // filled at OUR limit
}

// --- 4. Cancels and Replaces --------------------------------

TEST_F(ExchangeSimulatorTest, CancelsRestingOrder) {
  PendingEvent e{};
  e.order_id_ = 30;
  e.instrument_id_ = 1;
  e.side_ = Side::Sell;
  e.price_ = CodecUtils::encode_price(101.00);
  e.quantity_ = CodecUtils::encode_quantity(5.0);
  e.order_type_ = OrderType::Limit;
  e.exec_inst_ = ExecInst::ParticipateDontInitiate;
  sim->on_pending(e);

  PendingCancelEvent c{};
  c.order_id_ = 30;
  sim->on_pending_cancel(c);

  ASSERT_TRUE(handler->has<CancelAcceptedEvent>());
}

TEST_F(ExchangeSimulatorTest, RejectsReplaceIfCrossingPostOnly) {
  // Build book state with individual L2 updates
  send_bid_update(1, 100.00, 1.0);
  send_ask_update(1, 101.00, 1.0);

  PendingEvent e{};
  e.order_id_ = 40;
  e.instrument_id_ = 1;
  e.side_ = Side::Buy;
  e.price_ = CodecUtils::encode_price(100.00);
  e.quantity_ = CodecUtils::encode_quantity(1.0);
  e.order_type_ = OrderType::Limit;
  e.exec_inst_ = ExecInst::ParticipateDontInitiate;
  sim->on_pending(e);

  PendingReplaceEvent r{};
  r.order_id_ = 40;
  r.price_ = CodecUtils::encode_price(102.00); // crosses ask
  r.quantity_ = CodecUtils::encode_quantity(1.0);
  sim->on_pending_replace(r);

  ASSERT_TRUE(handler->has<ReplaceRejectedEvent>());
  const auto rr = handler->last<ReplaceRejectedEvent>();
  EXPECT_EQ(rr->reject_reason_, RejectReason::PostOnly);
}

// --- 5. Test multiple L2 updates building the book -----------

TEST_F(ExchangeSimulatorTest, HandlesMultipleL2UpdatesCorrectly) {
  // Build book gradually with multiple individual updates
  send_bid_update(1, 100.00, 5.0);   // Best bid
  send_bid_update(1, 99.50, 10.0);   // Second bid level
  send_ask_update(1, 101.00, 8.0);   // Best ask
  send_ask_update(1, 101.50, 12.0);  // Second ask level

  // Update an existing level (should replace)
  send_bid_update(1, 100.05, 3.0);   // New best bid

  // Test that an order at the old best bid level gets accepted (not crossing)
  PendingEvent e{};
  e.order_id_ = 50;
  e.instrument_id_ = 1;
  e.side_ = Side::Buy;
  e.price_ = CodecUtils::encode_price(100.00); // Below new best bid, should be fine
  e.quantity_ = CodecUtils::encode_quantity(2.0);
  e.order_type_ = OrderType::Limit;
  e.exec_inst_ = ExecInst::ParticipateDontInitiate;

  sim->on_pending(e);

  ASSERT_TRUE(handler->has<AcceptedEvent>());
  ASSERT_FALSE(handler->has<RejectedEvent>());
}

// --- 5b. Queue position: displayed depth must drain before we fill ----------
// These pin the fix for the dead queue-position model (displayed_depth_at used to
// read the never-populated legacy book, so every order started at queue front).

TEST_F(ExchangeSimulatorTest, QueuePositionDelaysFillUntilDepthAheadDrains) {
  // 5.0 displayed at 100.00 -> our order joins BEHIND it (queue_init_fraction=1.0 default).
  send_bid_update(1, 100.00, 5.0);
  send_ask_update(1, 101.00, 5.0);

  PendingEvent e{};
  e.order_id_ = 100;
  e.instrument_id_ = 1;
  e.side_ = Side::Buy;
  e.price_ = CodecUtils::encode_price(100.00);
  e.quantity_ = CodecUtils::encode_quantity(2.0);
  e.order_type_ = OrderType::Limit;
  e.exec_inst_ = ExecInst::ParticipateDontInitiate;
  sim->on_pending(e);
  ASSERT_TRUE(handler->has<AcceptedEvent>());
  handler->clear();

  // A 3.0 print only drains 3.0 of the 5.0 queued ahead of us -> NO fill.
  send_trade(1, Side::Sell, 100.00, 3.0);
  ASSERT_FALSE(handler->has<ExecutedEvent>());

  // A further 4.0 print drains the remaining 2.0 ahead, then fills our full 2.0.
  send_trade(1, Side::Sell, 100.00, 4.0);
  ASSERT_TRUE(handler->has<ExecutedEvent>());
  const auto ex = handler->last<ExecutedEvent>();
  EXPECT_EQ(ex->order_id_, e.order_id_);
  EXPECT_EQ(ex->last_quantity_, CodecUtils::encode_quantity(2.0));
}

TEST_F(ExchangeSimulatorTest, QueueInitFractionZeroJoinsFrontOfQueue) {
  sim->set_queue_init_fraction(0.0);   // optimistic calibration knob: start at queue front
  send_bid_update(1, 100.00, 5.0);
  send_ask_update(1, 101.00, 5.0);

  PendingEvent e{};
  e.order_id_ = 110;
  e.instrument_id_ = 1;
  e.side_ = Side::Buy;
  e.price_ = CodecUtils::encode_price(100.00);
  e.quantity_ = CodecUtils::encode_quantity(2.0);
  e.order_type_ = OrderType::Limit;
  e.exec_inst_ = ExecInst::ParticipateDontInitiate;
  sim->on_pending(e);
  handler->clear();

  // Same 5.0 displayed depth, but with fraction=0 even a 1.0 print fills us immediately.
  send_trade(1, Side::Sell, 100.00, 1.0);
  ASSERT_TRUE(handler->has<ExecutedEvent>());
  EXPECT_EQ(handler->last<ExecutedEvent>()->last_quantity_, CodecUtils::encode_quantity(1.0));
}

TEST_F(ExchangeSimulatorTest, SizedPrintFillsPartiallyAcrossTrades) {
  // No displayed depth at our price (we improve the book) -> first in line,
  // but each print's SIZE caps how much of us it can fill.
  PendingEvent e{};
  e.order_id_ = 120;
  e.instrument_id_ = 1;
  e.side_ = Side::Buy;
  e.price_ = CodecUtils::encode_price(100.00);
  e.quantity_ = CodecUtils::encode_quantity(5.0);
  e.order_type_ = OrderType::Limit;
  e.exec_inst_ = ExecInst::ParticipateDontInitiate;
  sim->on_pending(e);
  handler->clear();

  send_trade(1, Side::Sell, 99.00, 2.0);   // partial: 2.0 of 5.0
  ASSERT_TRUE(handler->has<ExecutedEvent>());
  EXPECT_EQ(handler->last<ExecutedEvent>()->last_quantity_, CodecUtils::encode_quantity(2.0));
  handler->clear();

  send_trade(1, Side::Sell, 99.00, 3.0);   // remainder: 3.0 -> order fully done
  ASSERT_TRUE(handler->has<ExecutedEvent>());
  EXPECT_EQ(handler->last<ExecutedEvent>()->last_quantity_, CodecUtils::encode_quantity(3.0));
  handler->clear();

  // Order is gone: further prints can't fill it.
  send_trade(1, Side::Sell, 99.00, 1.0);
  ASSERT_FALSE(handler->has<ExecutedEvent>());
}

TEST_F(ExchangeSimulatorTest, ZeroSizedTradePrintFillsNothing) {
  PendingEvent e{};
  e.order_id_ = 130;
  e.instrument_id_ = 1;
  e.side_ = Side::Buy;
  e.price_ = CodecUtils::encode_price(100.00);
  e.quantity_ = CodecUtils::encode_quantity(5.0);
  e.order_type_ = OrderType::Limit;
  e.exec_inst_ = ExecInst::ParticipateDontInitiate;
  sim->on_pending(e);
  handler->clear();

  send_trade(1, Side::Sell, 99.00, 0.0);   // size 0 = no fill budget
  ASSERT_FALSE(handler->has<ExecutedEvent>());
}

// --- 6. Test removing levels with zero quantity --------------

TEST_F(ExchangeSimulatorTest, RemovesLevelWhenQuantityIsZero) {
  // Build initial book
  send_bid_update(1, 100.00, 5.0);
  send_ask_update(1, 101.00, 8.0);

  // Remove the bid level by sending zero quantity
  send_bid_update(1, 100.00, 0.0);

  // Now a post-only buy at 100.00 should be accepted since there's no crossing level
  PendingEvent e{};
  e.order_id_ = 60;
  e.instrument_id_ = 1;
  e.side_ = Side::Buy;
  e.price_ = CodecUtils::encode_price(100.00);
  e.quantity_ = CodecUtils::encode_quantity(2.0);
  e.order_type_ = OrderType::Limit;
  e.exec_inst_ = ExecInst::ParticipateDontInitiate;

  sim->on_pending(e);

  ASSERT_TRUE(handler->has<AcceptedEvent>());
  ASSERT_FALSE(handler->has<RejectedEvent>());
}

// --- 7. Test snapshot vs incremental updates ----------------

TEST_F(ExchangeSimulatorTest, HandlesSnapshotUpdates) {
  // Send a snapshot update (full book refresh)
  L2UpdateEvent snapshot;
  snapshot.instrument_id_ = 1;
  snapshot.exchange_ = Exchange::Okx;
  snapshot.side_ = Side::Buy;
  snapshot.price_1_ = CodecUtils::encode_price(100.00);
  snapshot.size_1_ = CodecUtils::encode_quantity(10.0);
  snapshot.timestamp_ns_ = clock->epoch_nanos();
  snapshot.snapshot_ = BooleanEnum::TRUE;  // This is a snapshot
  snapshot.num_levels_ = 1;
  snapshot.is_last_batch_ = BooleanEnum::FALSE;

  sim->process_l2_update(snapshot);

  // Test that the book was properly initialized
  PendingEvent e{};
  e.order_id_ = 70;
  e.instrument_id_ = 1;
  e.side_ = Side::Buy;
  e.price_ = CodecUtils::encode_price(99.50); // Below best bid
  e.quantity_ = CodecUtils::encode_quantity(1.0);
  e.order_type_ = OrderType::Limit;
  e.exec_inst_ = ExecInst::ParticipateDontInitiate;

  sim->on_pending(e);

  ASSERT_TRUE(handler->has<AcceptedEvent>());
  ASSERT_FALSE(handler->has<RejectedEvent>());
}

// --- 8. Test batch messages ----------------------------------

TEST_F(ExchangeSimulatorTest, HandlesBatchMessages) {
  // Send first message in a batch
  L2UpdateEvent batch1;
  batch1.instrument_id_ = 1;
  batch1.exchange_ = Exchange::Okx;
  batch1.side_ = Side::Buy;
  batch1.price_1_ = CodecUtils::encode_price(100.00);
  batch1.size_1_ = CodecUtils::encode_quantity(5.0);
  batch1.timestamp_ns_ = clock->epoch_nanos();
  batch1.snapshot_ = BooleanEnum::FALSE;
  batch1.num_levels_ = 1;
  batch1.is_last_batch_ = BooleanEnum::FALSE;

  sim->process_l2_update(batch1);

  // Send last message in batch
  L2UpdateEvent batch2;
  batch2.instrument_id_ = 1;
  batch2.exchange_ = Exchange::Okx;
  batch2.side_ = Side::Sell;
  batch2.price_1_ = CodecUtils::encode_price(101.00);
  batch2.size_1_ = CodecUtils::encode_quantity(8.0);
  batch2.timestamp_ns_ = clock->epoch_nanos();
  batch2.snapshot_ = BooleanEnum::FALSE;
  batch2.num_levels_ = 1;
  batch2.is_last_batch_ = BooleanEnum::TRUE;

  sim->process_l2_update(batch2);

  // Verify book is built correctly
  PendingEvent e{};
  e.order_id_ = 80;
  e.instrument_id_ = 1;
  e.side_ = Side::Buy;
  e.price_ = CodecUtils::encode_price(100.50); // Between bid and ask
  e.quantity_ = CodecUtils::encode_quantity(1.0);
  e.order_type_ = OrderType::Limit;
  e.exec_inst_ = ExecInst::ParticipateDontInitiate;

  sim->on_pending(e);

  ASSERT_TRUE(handler->has<AcceptedEvent>());
  ASSERT_FALSE(handler->has<RejectedEvent>());
}

// --- 9. Limit-price-bounded taker fills (non-post-only limits) ---------------
// A limit order must NEVER fill through its own limit price.

namespace {

PendingEvent make_limit(int64_t order_id, Side side, double price, double qty) {
  PendingEvent e{};
  e.order_id_ = order_id;
  e.instrument_id_ = 1;
  e.side_ = side;
  e.price_ = CodecUtils::encode_price(price);
  e.quantity_ = CodecUtils::encode_quantity(qty);
  e.order_type_ = OrderType::Limit;
  // exec_inst_ stays Undefined -> NOT post-only
  return e;
}

// Sum of executed quantity for one order id across the response stream.
int64_t executed_qty(const MockExchangeResponseHandler& h, int64_t order_id) {
  int64_t total = 0;
  for (const auto& ev : h.events) {
    if (ev->get_type() == MessageType::Executed && ev->as<ExecutedEvent>().order_id_ == order_id) {
      total += ev->as<ExecutedEvent>().last_quantity_;
    }
  }
  return total;
}

}  // namespace

TEST_F(ExchangeSimulatorTest, PassiveLimitBuyBelowMarketRestsInsteadOfFilling) {
  send_bid_update(1, 100.00, 5.0);
  send_ask_update(1, 101.00, 5.0);

  // Non-post-only GTC buy BELOW the market: nothing is marketable, so it must
  // rest — the old code swept the asks and filled it instantly at 101.
  sim->on_pending(make_limit(200, Side::Buy, 99.00, 2.0));

  ASSERT_TRUE(handler->has<AcceptedEvent>());
  ASSERT_FALSE(handler->has<ExecutedEvent>());
  ASSERT_FALSE(handler->has<CancelAcceptedEvent>());  // rested, not IOC-cancelled
  handler->clear();

  // It really rests: a sell print through our price fills it at OUR limit.
  send_trade(1, Side::Sell, 98.00, 10.0);
  ASSERT_TRUE(handler->has<ExecutedEvent>());
  const auto ex = handler->last<ExecutedEvent>();
  EXPECT_EQ(ex->order_id_, 200);
  EXPECT_EQ(ex->last_price_, CodecUtils::encode_price(99.00));
  EXPECT_EQ(ex->last_quantity_, CodecUtils::encode_quantity(2.0));
}

TEST_F(ExchangeSimulatorTest, MarketableLimitStopsAtLimitAndRestsRemainder) {
  send_bid_update(1, 100.00, 5.0);
  send_ask_update(1, 101.00, 5.0);
  send_ask_update(1, 102.00, 10.0);

  // Buy 10 limit 101: only the 5.0 displayed at 101 is at-or-better; the
  // 102 level must NOT be touched, and the 5.0 remainder rests at 101.
  sim->on_pending(make_limit(210, Side::Buy, 101.00, 10.0));

  ASSERT_TRUE(handler->has<AcceptedEvent>());
  EXPECT_EQ(executed_qty(*handler, 210), CodecUtils::encode_quantity(5.0));
  for (const auto& ev : handler->events) {
    if (ev->get_type() == MessageType::Executed) {
      EXPECT_LE(ev->as<ExecutedEvent>().last_price_, CodecUtils::encode_price(101.00));
    }
  }
  ASSERT_FALSE(handler->has<CancelAcceptedEvent>());  // GTC remainder rests
  handler->clear();

  // The resting remainder fills when a sell print reaches our price.
  send_trade(1, Side::Sell, 101.00, 5.0);
  ASSERT_TRUE(handler->has<ExecutedEvent>());
  EXPECT_EQ(executed_qty(*handler, 210), CodecUtils::encode_quantity(5.0));
  EXPECT_EQ(handler->last<ExecutedEvent>()->last_price_, CodecUtils::encode_price(101.00));
}

TEST_F(ExchangeSimulatorTest, BackToBackTakersCannotReuseDisplayedLiquidity) {
  send_bid_update(1, 100.00, 5.0);
  send_ask_update(1, 101.00, 5.0);
  send_ask_update(1, 102.00, 5.0);

  PendingEvent m1 = make_limit(220, Side::Buy, 0.0, 5.0);
  m1.order_type_ = OrderType::Market;
  sim->on_pending(m1);
  EXPECT_EQ(handler->last<ExecutedEvent>()->last_price_, CodecUtils::encode_price(101.00));
  handler->clear();

  // Second taker before any new market data: the 101 depth was consumed, so
  // this must fill at 102 — not reuse the same 101 liquidity.
  PendingEvent m2 = make_limit(221, Side::Buy, 0.0, 5.0);
  m2.order_type_ = OrderType::Market;
  sim->on_pending(m2);
  ASSERT_TRUE(handler->has<ExecutedEvent>());
  EXPECT_EQ(handler->last<ExecutedEvent>()->last_price_, CodecUtils::encode_price(102.00));
}

// --- 10. Replace leaves accounting (OKX amend semantics) ---------------------

TEST_F(ExchangeSimulatorTest, ReplaceAccountsForCumulativeFills) {
  // Post-only buy 10 @ 100 with an empty external book -> front of queue.
  PendingEvent e{};
  e.order_id_ = 300;
  e.instrument_id_ = 1;
  e.side_ = Side::Buy;
  e.price_ = CodecUtils::encode_price(100.00);
  e.quantity_ = CodecUtils::encode_quantity(10.0);
  e.order_type_ = OrderType::Limit;
  e.exec_inst_ = ExecInst::ParticipateDontInitiate;
  sim->on_pending(e);

  send_trade(1, Side::Sell, 99.00, 4.0);   // 4.0 of 10.0 filled
  EXPECT_EQ(executed_qty(*handler, 300), CodecUtils::encode_quantity(4.0));
  handler->clear();

  // Amend to total qty 10 at a new price: leaves must be 10 - 4 = 6, not 10.
  PendingReplaceEvent r{};
  r.order_id_ = 300;
  r.instrument_id_ = 1;
  r.price_ = CodecUtils::encode_price(99.00);
  r.quantity_ = CodecUtils::encode_quantity(10.0);
  sim->on_pending_replace(r);
  ASSERT_TRUE(handler->has<ReplaceAcceptedEvent>());
  handler->clear();

  send_trade(1, Side::Sell, 98.00, 100.0);  // budget far above leaves
  EXPECT_EQ(executed_qty(*handler, 300), CodecUtils::encode_quantity(6.0));  // NOT 10.0
}

TEST_F(ExchangeSimulatorTest, ReplaceToAlreadyFilledQuantityTakesOrderDown) {
  PendingEvent e{};
  e.order_id_ = 310;
  e.instrument_id_ = 1;
  e.side_ = Side::Buy;
  e.price_ = CodecUtils::encode_price(100.00);
  e.quantity_ = CodecUtils::encode_quantity(10.0);
  e.order_type_ = OrderType::Limit;
  e.exec_inst_ = ExecInst::ParticipateDontInitiate;
  sim->on_pending(e);

  send_trade(1, Side::Sell, 99.00, 4.0);   // 4.0 filled
  handler->clear();

  // Amend down to total qty 4 == already filled -> nothing left to work.
  PendingReplaceEvent r{};
  r.order_id_ = 310;
  r.instrument_id_ = 1;
  r.price_ = CodecUtils::encode_price(99.00);
  r.quantity_ = CodecUtils::encode_quantity(4.0);
  sim->on_pending_replace(r);
  ASSERT_TRUE(handler->has<ReplaceAcceptedEvent>());
  handler->clear();

  send_trade(1, Side::Sell, 98.00, 100.0);
  ASSERT_FALSE(handler->has<ExecutedEvent>());  // order is gone, no overfill

  // And it is unknown to cancel now.
  PendingCancelEvent c{};
  c.order_id_ = 310;
  sim->on_pending_cancel(c);
  ASSERT_TRUE(handler->has<CancelRejectedEvent>());
}

// --- 11. Same-level orders share one trade budget ----------------------------

TEST_F(ExchangeSimulatorTest, SameLevelOrdersShareConsumedQueueDepth) {
  send_bid_update(1, 100.00, 5.0);
  send_ask_update(1, 101.00, 5.0);

  // Two of our orders join behind the same 5.0 of displayed depth.
  PendingEvent a{};
  a.order_id_ = 400;
  a.instrument_id_ = 1;
  a.side_ = Side::Buy;
  a.price_ = CodecUtils::encode_price(100.00);
  a.quantity_ = CodecUtils::encode_quantity(2.0);
  a.order_type_ = OrderType::Limit;
  a.exec_inst_ = ExecInst::ParticipateDontInitiate;
  sim->on_pending(a);

  PendingEvent b = a;
  b.order_id_ = 401;
  sim->on_pending(b);
  handler->clear();

  // One 9.0 print: 5.0 drains the shared market depth, 2.0 fills A, 2.0 fills
  // B. Before the fix, B was charged the 5.0 market depth AGAIN and missed.
  send_trade(1, Side::Sell, 100.00, 9.0);
  EXPECT_EQ(executed_qty(*handler, 400), CodecUtils::encode_quantity(2.0));
  EXPECT_EQ(executed_qty(*handler, 401), CodecUtils::encode_quantity(2.0));
}

// --- 12. Pessimistic vs Optimistic queue models ------------------------------
// Run identical event sequences through both models: Optimistic must never fill
// later or less, and displayed-depth reductions must produce a genuine gap.

namespace {

struct ModelPair {
  std::shared_ptr<SimulationClock> clock = std::make_shared<SimulationClock>();
  MockExchangeResponseHandler pess_handler;
  MockExchangeResponseHandler opt_handler;
  ExchangeSimulator pess{clock, &pess_handler};
  ExchangeSimulator opt{clock, &opt_handler};

  ModelPair() {
    pess.set_queue_model(ExchangeSimulator::QueueModel::Pessimistic);
    opt.set_queue_model(ExchangeSimulator::QueueModel::Optimistic);
  }

  void l2(Side side, double price, double qty) {
    L2UpdateEvent e;
    e.instrument_id_ = 1;
    e.exchange_ = Exchange::Okx;
    e.side_ = side;
    e.price_1_ = CodecUtils::encode_price(price);
    e.size_1_ = CodecUtils::encode_quantity(qty);
    e.timestamp_ns_ = clock->epoch_nanos();
    e.snapshot_ = BooleanEnum::FALSE;
    e.num_levels_ = 1;
    e.is_last_batch_ = BooleanEnum::TRUE;
    pess.process_l2_update(e);
    opt.process_l2_update(e);
  }

  void l1(double bid, double bid_qty, double ask, double ask_qty) {
    L1UpdateEvent e;
    e.instrument_id_ = 1;
    e.bid_price_ = CodecUtils::encode_price(bid);
    e.bid_size_ = CodecUtils::encode_quantity(bid_qty);
    e.offer_price_ = CodecUtils::encode_price(ask);
    e.offer_size_ = CodecUtils::encode_quantity(ask_qty);
    e.timestamp_ns_ = clock->epoch_nanos();
    pess.process_l1_update(e);
    opt.process_l1_update(e);
  }

  void trade(Side aggressor, double price, double size) {
    TradeEvent t{};
    t.instrument_id_ = 1;
    t.side_ = aggressor;
    t.price_ = CodecUtils::encode_price(price);
    t.size_ = CodecUtils::encode_quantity(size);
    t.timestamp_ns_ = clock->epoch_nanos();
    pess.process_trade_event(t);
    opt.process_trade_event(t);
    // Invariant: Optimistic never fills less than Pessimistic.
    EXPECT_GE(executed_qty(opt_handler, 500), executed_qty(pess_handler, 500));
  }

  void place_buy(double price, double qty) {
    PendingEvent e{};
    e.order_id_ = 500;
    e.instrument_id_ = 1;
    e.side_ = Side::Buy;
    e.price_ = CodecUtils::encode_price(price);
    e.quantity_ = CodecUtils::encode_quantity(qty);
    e.order_type_ = OrderType::Limit;
    e.exec_inst_ = ExecInst::ParticipateDontInitiate;
    pess.on_pending(e);
    opt.on_pending(e);
  }
};

}  // namespace

TEST(ExchangeSimulatorQueueModelTest, OptimisticFillsWherePessimisticDoesNotOnL2Cancels) {
  ModelPair p;
  p.l2(Side::Buy, 100.00, 10.0);
  p.l2(Side::Sell, 101.00, 5.0);
  p.place_buy(100.00, 2.0);      // joins behind 10.0 displayed

  p.l2(Side::Buy, 100.00, 1.0);  // depth collapses to 1.0 -> cancels somewhere in the queue

  // 3.0 print: Optimistic (queue capped at 1.0) drains 1.0 then fills 2.0;
  // Pessimistic still has 10.0 ahead and gets nothing.
  p.trade(Side::Sell, 100.00, 3.0);
  EXPECT_EQ(executed_qty(p.opt_handler, 500), CodecUtils::encode_quantity(2.0));
  EXPECT_EQ(executed_qty(p.pess_handler, 500), 0);
}

TEST(ExchangeSimulatorQueueModelTest, OptimisticCapAlsoAppliesOnL1Updates) {
  ModelPair p;
  p.l2(Side::Buy, 100.00, 10.0);
  p.l2(Side::Sell, 101.00, 5.0);
  p.place_buy(100.00, 2.0);      // joins behind 10.0 displayed

  // Top-of-book L1 shows the bid depth collapsed to 1.0. Before the fix this
  // only worked via L2 updates, so L1-driven tapes degraded Optimistic to
  // Pessimistic.
  p.l1(100.00, 1.0, 101.00, 5.0);

  p.trade(Side::Sell, 100.00, 3.0);
  EXPECT_EQ(executed_qty(p.opt_handler, 500), CodecUtils::encode_quantity(2.0));
  EXPECT_EQ(executed_qty(p.pess_handler, 500), 0);
}