#include "domain/incremental_order_book.hpp"

#include <gtest/gtest.h>

namespace rx = reflex;
using rx::Side;
using rx::BooleanEnum;
using rx::L1UpdateEvent;
using rx::L2UpdateEvent;
using rx::TradeEvent;
using reflex::marketdata::IncrementalOrderBook;

namespace {

L2UpdateEvent make_l2(int32_t instrument_id,
                      int64_t timestamp,
                      Side side,
                      int64_t price,
                      int64_t qty,
                      bool snapshot,
                      bool batch,
                      bool last) {
  L2UpdateEvent e{};
  e.instrument_id_ = instrument_id;
  e.timestamp_ns_ = timestamp;
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

L1UpdateEvent make_l1(int32_t instrument_id,
                      int64_t timestamp,
                      int64_t bid_px,
                      int64_t bid_qty,
                      int64_t ask_px,
                      int64_t ask_qty) {
  L1UpdateEvent e{};
  e.instrument_id_ = instrument_id;
  e.timestamp_ns_ = timestamp;
  e.bid_price_ = bid_px;
  e.bid_size_ = bid_qty;
  e.offer_price_ = ask_px;
  e.offer_size_ = ask_qty;
  return e;
}

TradeEvent make_trade(int32_t instrument_id,
                      int64_t timestamp,
                      Side aggressor_side,
                      int64_t price,
                      int64_t qty) {
  TradeEvent e{};
  e.instrument_id_ = instrument_id;
  e.timestamp_ns_ = timestamp;
  e.side_ = aggressor_side;
  e.price_ = price;  // TradeEvent still uses price_/size_, not changed
  e.size_ = qty;
  return e;
}

constexpr int32_t kInstrument = 42;

void seed_basic_snapshot(IncrementalOrderBook& book) {
  const bool snapshot = true;
  const bool batch = true;
  book.apply_l2_update(make_l2(kInstrument, 1'000, Side::Buy, 100'000, 5, snapshot, batch, false));
  book.apply_l2_update(make_l2(kInstrument, 1'000, Side::Buy, 99'900, 3, snapshot, batch, false));
  book.apply_l2_update(make_l2(kInstrument, 1'000, Side::Sell, 100'100, 4, snapshot, batch, false));
  book.apply_l2_update(make_l2(kInstrument, 1'000, Side::Sell, 100'200, 6, snapshot, batch, true));
}

} // namespace

TEST(IncrementalOrderBookTest, SeedsFromSnapshot) {
  IncrementalOrderBook book(4);
  seed_basic_snapshot(book);

  EXPECT_TRUE(book.ready());
  EXPECT_EQ(book.best_bid_price(), 100'000);
  EXPECT_EQ(book.best_bid_quantity(), 5);
  EXPECT_EQ(book.best_ask_price(), 100'100);
  EXPECT_EQ(book.best_ask_quantity(), 4);
  EXPECT_EQ(book.spread(), 100);
  EXPECT_EQ(book.mid_price(), 100'050);

  const auto& bids = book.bids();
  ASSERT_EQ(bids.size(), 2u);
  EXPECT_EQ(bids[0].price, 100'000);
  EXPECT_EQ(bids[1].price, 99'900);

  const auto& asks = book.asks();
  ASSERT_EQ(asks.size(), 2u);
  EXPECT_EQ(asks[0].price, 100'100);
  EXPECT_EQ(asks[1].price, 100'200);
}

TEST(IncrementalOrderBookTest, UpdatesTopOfBookFromL1) {
  IncrementalOrderBook book(8);
  seed_basic_snapshot(book);
  // Initial: bid=100'000@5, 99'900@3  |  ask=100'100@4, 100'200@6

  // L1 update: bid improves up to 100'050, ask degrades up to 100'150
  book.apply_l1_update(make_l1(kInstrument, 2'000, 100'050, 7, 100'150, 8));

  EXPECT_TRUE(book.ready());
  EXPECT_EQ(book.best_bid_price(), 100'050);
  EXPECT_EQ(book.best_bid_quantity(), 7);
  EXPECT_EQ(book.best_ask_price(), 100'150);
  EXPECT_EQ(book.best_ask_quantity(), 8);

  // Bid improved: old best (100'000@5) should be preserved as level 2
  const auto& bids = book.bids();
  ASSERT_GE(bids.size(), 3u);
  EXPECT_EQ(bids[0].price, 100'050);  // New best
  EXPECT_EQ(bids[1].price, 100'000);  // Old best (preserved!)
  EXPECT_EQ(bids[1].quantity, 5);
  EXPECT_EQ(bids[2].price, 99'900);   // Original level 2

  // Ask degraded: old best (100'100) should be erased, 100'200 becomes level 2
  const auto& asks = book.asks();
  ASSERT_EQ(asks.size(), 2u);
  EXPECT_EQ(asks[0].price, 100'150);  // New best
  EXPECT_EQ(asks[1].price, 100'200);  // Old level 2
}

TEST(IncrementalOrderBookTest, AppliesTradesToRestingLiquidity) {
  IncrementalOrderBook book(6);
  seed_basic_snapshot(book);

  book.apply_trade(make_trade(kInstrument, 3'000, Side::Buy, 100'100, 2));
  EXPECT_EQ(book.best_ask_price(), 100'100);
  EXPECT_EQ(book.best_ask_quantity(), 2);

  book.apply_trade(make_trade(kInstrument, 3'500, Side::Buy, 100'100, 3));
  EXPECT_EQ(book.best_ask_price(), 100'200);
  EXPECT_EQ(book.best_ask_quantity(), 6);

  book.apply_trade(make_trade(kInstrument, 4'000, Side::Sell, 100'000, 5));
  EXPECT_EQ(book.best_bid_price(), 99'900);
  EXPECT_EQ(book.best_bid_quantity(), 3);
}

TEST(IncrementalOrderBookTest, IgnoresMismatchedInstrument) {
  IncrementalOrderBook book(4);
  seed_basic_snapshot(book);

  const auto snapshot_before = book.snapshot();

  book.apply_l1_update(make_l1(999, 5'000, 101'000, 10, 101'100, 11));
  book.apply_trade(make_trade(999, 5'100, Side::Buy, 101'100, 1));

  const auto snapshot_after = book.snapshot();
  EXPECT_EQ(snapshot_before.bids, snapshot_after.bids);
  EXPECT_EQ(snapshot_before.asks, snapshot_after.asks);
}

TEST(IncrementalOrderBookTest, NewSnapshotResetsState) {
  IncrementalOrderBook book(4);
  seed_basic_snapshot(book);
  book.apply_l1_update(make_l1(kInstrument, 2'000, 100'050, 7, 100'150, 8));

  const bool snapshot = true;
  const bool batch = true;
  book.apply_l2_update(make_l2(kInstrument, 6'000, Side::Buy, 200'000, 9, snapshot, batch, false));
  book.apply_l2_update(make_l2(kInstrument, 6'000, Side::Sell, 200'100, 12, snapshot, batch, true));

  EXPECT_TRUE(book.ready());
  EXPECT_EQ(book.best_bid_price(), 200'000);
  EXPECT_EQ(book.best_bid_quantity(), 9);
  EXPECT_EQ(book.best_ask_price(), 200'100);
  EXPECT_EQ(book.best_ask_quantity(), 12);

  const auto& bids = book.bids();
  ASSERT_EQ(bids.size(), 1u);
  EXPECT_EQ(bids[0].price, 200'000);

  const auto& asks = book.asks();
  ASSERT_EQ(asks.size(), 1u);
  EXPECT_EQ(asks[0].price, 200'100);
}

TEST(IncrementalOrderBookTest, L1AloneGatesReadinessUntilBothSidesPresent) {
  IncrementalOrderBook book(4);

  book.apply_l1_update(make_l1(kInstrument, 1'000, 100'000, 5, 0, 0));
  EXPECT_FALSE(book.ready());

  book.apply_l1_update(make_l1(kInstrument, 1'100, 100'000, 5, 100'100, 4));
  EXPECT_TRUE(book.ready());
  EXPECT_EQ(book.best_bid_price(), 100'000);
  EXPECT_EQ(book.best_ask_price(), 100'100);
}

TEST(IncrementalOrderBookTest, StreamingBatchDelaysReadinessUntilLastMessage) {
  IncrementalOrderBook book(4);

  const bool batch = true;
  const bool snapshot = false;
  book.apply_l2_update(make_l2(kInstrument, 5'000, Side::Buy, 100'000, 5, snapshot, batch, false));

  EXPECT_FALSE(book.ready());
  EXPECT_TRUE(book.in_batch());
  EXPECT_EQ(book.best_bid_price(), 100'000);

  book.apply_l2_update(make_l2(kInstrument, 5'100, Side::Sell, 100'100, 4, snapshot, batch, true));

  EXPECT_TRUE(book.ready());
  EXPECT_FALSE(book.in_batch());
  EXPECT_EQ(book.best_ask_price(), 100'100);
}

// ===== New Tests for L1 Improvement/Degradation Scenarios =====

TEST(IncrementalOrderBookTest, L1BidImprovesKeepsOldLevel) {
  IncrementalOrderBook book(8);
  seed_basic_snapshot(book);
  // Initial: bid=100'000@5, 99'900@3

  // Bid improves: 100'000 → 100'050
  book.apply_l1_update(make_l1(kInstrument, 2'000, 100'050, 7, 100'100, 4));

  const auto& bids = book.bids();
  ASSERT_EQ(bids.size(), 3u);
  EXPECT_EQ(bids[0].price, 100'050);  // New best
  EXPECT_EQ(bids[0].quantity, 7);
  EXPECT_EQ(bids[1].price, 100'000);  // Old best preserved!
  EXPECT_EQ(bids[1].quantity, 5);
  EXPECT_EQ(bids[2].price, 99'900);   // Level 2 preserved
}

TEST(IncrementalOrderBookTest, L1BidDegradesErasesOldLevel) {
  IncrementalOrderBook book(8);
  seed_basic_snapshot(book);
  // Initial: bid=100'000@5, 99'900@3

  // Bid degrades: 100'000 → 99'900 (top level was consumed)
  book.apply_l1_update(make_l1(kInstrument, 2'000, 99'900, 3, 100'100, 4));

  const auto& bids = book.bids();
  ASSERT_EQ(bids.size(), 1u);
  EXPECT_EQ(bids[0].price, 99'900);   // New best
  EXPECT_EQ(bids[0].quantity, 3);
  // 100'000 was erased (consumed)
}

TEST(IncrementalOrderBookTest, L1AskImprovesKeepsOldLevel) {
  IncrementalOrderBook book(8);
  seed_basic_snapshot(book);
  // Initial: ask=100'100@4, 100'200@6

  // Ask improves: 100'100 → 100'050 (tighter)
  book.apply_l1_update(make_l1(kInstrument, 2'000, 100'000, 5, 100'050, 8));

  const auto& asks = book.asks();
  ASSERT_EQ(asks.size(), 3u);
  EXPECT_EQ(asks[0].price, 100'050);  // New best
  EXPECT_EQ(asks[0].quantity, 8);
  EXPECT_EQ(asks[1].price, 100'100);  // Old best preserved!
  EXPECT_EQ(asks[1].quantity, 4);
  EXPECT_EQ(asks[2].price, 100'200);  // Level 2 preserved
}

TEST(IncrementalOrderBookTest, L1AskDegradesErasesOldLevel) {
  IncrementalOrderBook book(8);
  seed_basic_snapshot(book);
  // Initial: ask=100'100@4, 100'200@6

  // Ask degrades: 100'100 → 100'200 (top level was consumed)
  book.apply_l1_update(make_l1(kInstrument, 2'000, 100'000, 5, 100'200, 6));

  const auto& asks = book.asks();
  ASSERT_EQ(asks.size(), 1u);
  EXPECT_EQ(asks[0].price, 100'200);  // New best
  EXPECT_EQ(asks[0].quantity, 6);
  // 100'100 was erased (consumed)
}

TEST(IncrementalOrderBookTest, L1BothSidesImprove) {
  IncrementalOrderBook book(8);
  seed_basic_snapshot(book);
  // Initial: bid=100'000@5, 99'900@3  |  ask=100'100@4, 100'200@6

  // Both improve: bid up to 100'050, ask down to 100'075
  book.apply_l1_update(make_l1(kInstrument, 2'000, 100'050, 7, 100'075, 8));

  // Both old bests should be preserved
  const auto& bids = book.bids();
  ASSERT_EQ(bids.size(), 3u);
  EXPECT_EQ(bids[0].price, 100'050);
  EXPECT_EQ(bids[1].price, 100'000);  // Preserved
  EXPECT_EQ(bids[2].price, 99'900);

  const auto& asks = book.asks();
  ASSERT_EQ(asks.size(), 3u);
  EXPECT_EQ(asks[0].price, 100'075);
  EXPECT_EQ(asks[1].price, 100'100);  // Preserved
  EXPECT_EQ(asks[2].price, 100'200);
}

TEST(IncrementalOrderBookTest, L1BothSidesDegrade) {
  IncrementalOrderBook book(8);
  seed_basic_snapshot(book);
  // Initial: bid=100'000@5, 99'900@3  |  ask=100'100@4, 100'200@6

  // Both degrade: bid down to 99'900, ask up to 100'200
  book.apply_l1_update(make_l1(kInstrument, 2'000, 99'900, 3, 100'200, 6));

  // Both old bests should be erased
  const auto& bids = book.bids();
  ASSERT_EQ(bids.size(), 1u);
  EXPECT_EQ(bids[0].price, 99'900);

  const auto& asks = book.asks();
  ASSERT_EQ(asks.size(), 1u);
  EXPECT_EQ(asks[0].price, 100'200);
}

TEST(IncrementalOrderBookTest, MultipleL1ImprovementsBuildDepth) {
  IncrementalOrderBook book(8);
  seed_basic_snapshot(book);
  // Initial: bid=100'000@5

  // Series of improvements building up depth
  book.apply_l1_update(make_l1(kInstrument, 2'000, 100'010, 2, 100'100, 4));
  book.apply_l1_update(make_l1(kInstrument, 2'100, 100'020, 3, 100'100, 4));
  book.apply_l1_update(make_l1(kInstrument, 2'200, 100'030, 1, 100'100, 4));

  // Should have accumulated depth
  const auto& bids = book.bids();
  ASSERT_GE(bids.size(), 5u);
  EXPECT_EQ(bids[0].price, 100'030);
  EXPECT_EQ(bids[1].price, 100'020);
  EXPECT_EQ(bids[2].price, 100'010);
  EXPECT_EQ(bids[3].price, 100'000);
  EXPECT_EQ(bids[4].price, 99'900);
}

TEST(IncrementalOrderBookTest, L1ImprovementThenL2SnapshotResyncs) {
  IncrementalOrderBook book(8);
  seed_basic_snapshot(book);

  // L1 improvement adds new level
  book.apply_l1_update(make_l1(kInstrument, 2'000, 100'050, 7, 100'100, 4));

  // Book should have intermediate state
  ASSERT_EQ(book.bids().size(), 3u);

  // L2 snapshot resyncs everything
  const bool snapshot = true;
  const bool batch = true;
  book.apply_l2_update(make_l2(kInstrument, 3'000, Side::Buy, 100'060, 10, snapshot, batch, false));
  book.apply_l2_update(make_l2(kInstrument, 3'000, Side::Buy, 100'040, 8, snapshot, batch, false));
  book.apply_l2_update(make_l2(kInstrument, 3'000, Side::Sell, 100'110, 5, snapshot, batch, false));
  book.apply_l2_update(make_l2(kInstrument, 3'000, Side::Sell, 100'120, 7, snapshot, batch, true));

  // After snapshot, book should have exact L2 state
  const auto& bids = book.bids();
  ASSERT_EQ(bids.size(), 2u);
  EXPECT_EQ(bids[0].price, 100'060);
  EXPECT_EQ(bids[1].price, 100'040);

  const auto& asks = book.asks();
  ASSERT_EQ(asks.size(), 2u);
  EXPECT_EQ(asks[0].price, 100'110);
  EXPECT_EQ(asks[1].price, 100'120);
}
