#include "domain/order_book_manager.hpp"

#include <gtest/gtest.h>

namespace rx = reflex;
using rx::Side;
using rx::BooleanEnum;
using rx::L1UpdateEvent;
using rx::L2UpdateEvent;
using rx::TradeEvent;
using reflex::backtest::OrderBookManager;

namespace {

constexpr int32_t kInstrument = 4242;

L2UpdateEvent make_l2(int64_t ts,
                      Side side,
                      int64_t price,
                      int64_t qty,
                      bool snapshot,
                      bool batch,
                      bool last) {
  L2UpdateEvent e{};
  e.instrument_id_ = kInstrument;
  e.timestamp_ns_ = ts;
  e.side_ = side;
  e.price_1_ = price;
  e.size_1_ = qty;
  e.num_levels_ = 1;
  e.price_2_ = 0;
  e.size_2_ = 0;
  e.snapshot_ = snapshot ? BooleanEnum::TRUE : BooleanEnum::FALSE;
  e.is_batch_message_ = batch ? BooleanEnum::TRUE : BooleanEnum::FALSE;
  e.is_last_batch_ = last ? BooleanEnum::TRUE : BooleanEnum::FALSE;
  return e;
}

L1UpdateEvent make_l1(int64_t ts,
                      int64_t bid_px,
                      int64_t bid_qty,
                      int64_t ask_px,
                      int64_t ask_qty) {
  L1UpdateEvent e{};
  e.instrument_id_ = kInstrument;
  e.timestamp_ns_ = ts;
  e.bid_price_ = bid_px;
  e.bid_size_ = bid_qty;
  e.offer_price_ = ask_px;
  e.offer_size_ = ask_qty;
  return e;
}

TradeEvent make_trade(int64_t ts, Side aggressor, int64_t price, int64_t qty) {
  TradeEvent e{};
  e.instrument_id_ = kInstrument;
  e.timestamp_ns_ = ts;
  e.side_ = aggressor;
  e.price_ = price;
  e.size_ = qty;
  return e;
}

void seed_snapshot(OrderBookManager& mgr) {
  const bool snapshot = true;
  const bool batch = true;
  mgr.process_l2_update(make_l2(1'000, Side::Buy, 100'000, 5, snapshot, batch, false));
  mgr.process_l2_update(make_l2(1'000, Side::Buy, 99'900, 3, snapshot, batch, false));
  mgr.process_l2_update(make_l2(1'000, Side::Sell, 100'100, 4, snapshot, batch, false));
  mgr.process_l2_update(make_l2(1'000, Side::Sell, 100'200, 6, snapshot, batch, true));
}

} // namespace

TEST(OrderBookManagerTest, ProvidesIncrementalBookAfterSnapshot) {
  OrderBookManager mgr;
  seed_snapshot(mgr);

  const auto* inc = mgr.get_incremental_book(kInstrument);
  ASSERT_NE(inc, nullptr);
  EXPECT_TRUE(inc->ready());
  EXPECT_EQ(inc->best_bid_price(), 100'000);
  EXPECT_EQ(inc->best_ask_price(), 100'100);
  EXPECT_EQ(mgr.get_best_bid(kInstrument), 100'000);
  EXPECT_EQ(mgr.get_best_ask(kInstrument), 100'100);
  EXPECT_EQ(mgr.get_mid_price(kInstrument), 100'050);
  EXPECT_EQ(mgr.get_spread(kInstrument), 100);
}

TEST(OrderBookManagerTest, L1RefreshesTopOfBook) {
  OrderBookManager mgr;
  seed_snapshot(mgr);

  mgr.process_l1_update(make_l1(2'000, 100'050, 7, 100'150, 8));

  const auto* inc = mgr.get_incremental_book(kInstrument);
  ASSERT_NE(inc, nullptr);
  EXPECT_EQ(inc->best_bid_price(), 100'050);
  EXPECT_EQ(inc->best_bid_quantity(), 7);
  EXPECT_EQ(inc->best_ask_price(), 100'150);
  EXPECT_EQ(inc->best_ask_quantity(), 8);

  EXPECT_EQ(mgr.get_best_bid(kInstrument), 100'050);
  EXPECT_EQ(mgr.get_best_ask(kInstrument), 100'150);
}

TEST(OrderBookManagerTest, TradesDepleteUsingIncrementalBook) {
  OrderBookManager mgr;
  seed_snapshot(mgr);

  mgr.process_trade_event(make_trade(3'000, Side::Buy, 100'100, 2));

  const auto* inc = mgr.get_incremental_book(kInstrument);
  ASSERT_NE(inc, nullptr);
  EXPECT_EQ(inc->best_ask_price(), 100'100);
  EXPECT_EQ(inc->best_ask_quantity(), 2);

  mgr.process_trade_event(make_trade(3'500, Side::Buy, 100'100, 3));
  EXPECT_EQ(inc->best_ask_price(), 100'200);
  EXPECT_EQ(inc->best_ask_quantity(), 6);
}

