#include "domain/incremental_order_book.hpp"

#include <gtest/gtest.h>

#include <vector>

namespace rx = reflex;
using rx::Side;
using rx::BooleanEnum;
using rx::L1UpdateEvent;
using rx::L2UpdateEvent;
using rx::TradeEvent;
using reflex::marketdata::IncrementalOrderBook;
using reflex::marketdata::IncrementalOrderBookListener;

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
  e.price_ = price;
  e.size_ = qty;
  return e;
}

constexpr int32_t kInstrument = 42;

class RecordingBookListener final : public IncrementalOrderBookListener {
 public:
  void on_order_book_update(const IncrementalOrderBook& book) override {
    updates.push_back(book.snapshot());
  }

  std::vector<IncrementalOrderBook::Snapshot> updates;
};

void seed_basic_snapshot(IncrementalOrderBook& book) {
  const bool snapshot = true;
  const bool batch = true;
  book.apply_l2_update(make_l2(kInstrument, 1'000, Side::Buy, 100'000, 5, snapshot, batch, false));
  book.apply_l2_update(make_l2(kInstrument, 1'000, Side::Buy, 99'900, 3, snapshot, batch, false));
  book.apply_l2_update(make_l2(kInstrument, 1'000, Side::Sell, 100'100, 4, snapshot, batch, false));
  book.apply_l2_update(make_l2(kInstrument, 1'000, Side::Sell, 100'200, 6, snapshot, batch, true));
}

}

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

TEST(IncrementalOrderBookTest,
     NotifiesListenerOnlyAfterCompleteSnapshot) {
  IncrementalOrderBook book(4);
  RecordingBookListener listener;
  book.add_listener(&listener);

  const bool snapshot = true;
  const bool batch = true;
  book.apply_l2_update(make_l2(kInstrument, 1'000, Side::Buy, 100'000, 5,
                               snapshot, batch, false));
  book.apply_l2_update(make_l2(kInstrument, 1'000, Side::Buy, 99'900, 3,
                               snapshot, batch, false));
  book.apply_l2_update(make_l2(kInstrument, 1'000, Side::Sell, 100'100, 4,
                               snapshot, batch, false));
  EXPECT_TRUE(listener.updates.empty());

  book.apply_l2_update(make_l2(kInstrument, 1'000, Side::Sell, 100'200, 6,
                               snapshot, batch, true));

  ASSERT_EQ(listener.updates.size(), 1U);
  EXPECT_TRUE(listener.updates.front().ready);
  EXPECT_EQ(listener.updates.front().bids.front().price, 100'000);
  EXPECT_EQ(listener.updates.front().asks.front().price, 100'100);
}

TEST(IncrementalOrderBookTest,
     PerSideSnapshotLastMarkersStillProduceOneTwoSidedSnapshot) {
  IncrementalOrderBook book(4);
  RecordingBookListener listener;
  book.add_listener(&listener);

  const bool snapshot = true;
  const bool batch = true;
  book.apply_l2_update(make_l2(kInstrument, 1'000, Side::Buy, 100'000, 5,
                               snapshot, batch, false));
  book.apply_l2_update(make_l2(kInstrument, 1'000, Side::Buy, 99'900, 3,
                               snapshot, batch, true));
  EXPECT_TRUE(book.in_snapshot());
  EXPECT_FALSE(book.ready());
  EXPECT_TRUE(listener.updates.empty());

  book.apply_l2_update(make_l2(kInstrument, 1'000, Side::Sell, 100'100, 4,
                               snapshot, batch, false));
  book.apply_l2_update(make_l2(kInstrument, 1'000, Side::Sell, 100'200, 6,
                               snapshot, batch, true));

  ASSERT_EQ(listener.updates.size(), 1U);
  EXPECT_TRUE(book.ready());
  EXPECT_FALSE(book.in_snapshot());
  ASSERT_EQ(book.bids().size(), 2U);
  ASSERT_EQ(book.asks().size(), 2U);
  EXPECT_EQ(book.best_bid_price(), 100'000);
  EXPECT_EQ(book.best_ask_price(), 100'100);
}

TEST(IncrementalOrderBookTest,
     NotifiesListenerOnlyAfterCompleteDeltaBatch) {
  IncrementalOrderBook book(4);
  seed_basic_snapshot(book);
  RecordingBookListener listener;
  book.add_listener(&listener);

  const bool snapshot = false;
  const bool batch = true;
  book.apply_l2_update(make_l2(kInstrument, 2'000, Side::Buy, 100'000, 7,
                               snapshot, batch, false));
  EXPECT_TRUE(listener.updates.empty());

  book.apply_l2_update(make_l2(kInstrument, 2'000, Side::Sell, 100'100, 8,
                               snapshot, batch, true));

  ASSERT_EQ(listener.updates.size(), 1U);
  EXPECT_EQ(listener.updates.front().bids.front().quantity, 7);
  EXPECT_EQ(listener.updates.front().asks.front().quantity, 8);
}

TEST(IncrementalOrderBookTest, NotifiesListenerForSingleDelta) {
  IncrementalOrderBook book(4);
  seed_basic_snapshot(book);
  RecordingBookListener listener;
  book.add_listener(&listener);

  book.apply_l2_update(make_l2(kInstrument, 2'000, Side::Buy, 100'000, 9,
                               false, false,
                               false));

  ASSERT_EQ(listener.updates.size(), 1U);
  EXPECT_EQ(listener.updates.front().bids.front().quantity, 9);
}

TEST(IncrementalOrderBookTest, UpdatesTopOfBookFromL1) {
  IncrementalOrderBook book(8);
  seed_basic_snapshot(book);

  book.apply_l1_update(make_l1(kInstrument, 2'000, 100'050, 7, 100'150, 8));

  EXPECT_TRUE(book.ready());
  EXPECT_EQ(book.best_bid_price(), 100'050);
  EXPECT_EQ(book.best_bid_quantity(), 7);
  EXPECT_EQ(book.best_ask_price(), 100'150);
  EXPECT_EQ(book.best_ask_quantity(), 8);

  const auto& bids = book.bids();
  ASSERT_GE(bids.size(), 3u);
  EXPECT_EQ(bids[0].price, 100'050);
  EXPECT_EQ(bids[1].price, 100'000);
  EXPECT_EQ(bids[1].quantity, 5);
  EXPECT_EQ(bids[2].price, 99'900);

  const auto& asks = book.asks();
  ASSERT_EQ(asks.size(), 2u);
  EXPECT_EQ(asks[0].price, 100'150);
  EXPECT_EQ(asks[1].price, 100'200);
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

TEST(IncrementalOrderBookTest, L2ProvenanceRequiresCompletedTwoSidedBook) {
  IncrementalOrderBook book(4);
  const bool snapshot = true;
  const bool batch = true;

  book.apply_l2_update(
      make_l2(kInstrument, 5'000, Side::Buy, 100'000, 5, snapshot, batch, false));
  EXPECT_FALSE(book.ready());
  EXPECT_FALSE(book.has_l2_provenance());

  book.apply_l2_update(
      make_l2(kInstrument, 5'100, Side::Sell, 100'100, 4, snapshot, batch, true));
  EXPECT_TRUE(book.ready());
  EXPECT_TRUE(book.has_l2_provenance());
}


TEST(IncrementalOrderBookTest, L1BidImprovesKeepsOldLevel) {
  IncrementalOrderBook book(8);
  seed_basic_snapshot(book);

  book.apply_l1_update(make_l1(kInstrument, 2'000, 100'050, 7, 100'100, 4));

  const auto& bids = book.bids();
  ASSERT_EQ(bids.size(), 3u);
  EXPECT_EQ(bids[0].price, 100'050);
  EXPECT_EQ(bids[0].quantity, 7);
  EXPECT_EQ(bids[1].price, 100'000);
  EXPECT_EQ(bids[1].quantity, 5);
  EXPECT_EQ(bids[2].price, 99'900);
}

TEST(IncrementalOrderBookTest, L1BidDegradesErasesOldLevel) {
  IncrementalOrderBook book(8);
  seed_basic_snapshot(book);

  book.apply_l1_update(make_l1(kInstrument, 2'000, 99'900, 3, 100'100, 4));

  const auto& bids = book.bids();
  ASSERT_EQ(bids.size(), 1u);
  EXPECT_EQ(bids[0].price, 99'900);
  EXPECT_EQ(bids[0].quantity, 3);
}

TEST(IncrementalOrderBookTest, L1AskImprovesKeepsOldLevel) {
  IncrementalOrderBook book(8);
  seed_basic_snapshot(book);

  book.apply_l1_update(make_l1(kInstrument, 2'000, 100'000, 5, 100'050, 8));

  const auto& asks = book.asks();
  ASSERT_EQ(asks.size(), 3u);
  EXPECT_EQ(asks[0].price, 100'050);
  EXPECT_EQ(asks[0].quantity, 8);
  EXPECT_EQ(asks[1].price, 100'100);
  EXPECT_EQ(asks[1].quantity, 4);
  EXPECT_EQ(asks[2].price, 100'200);
}

TEST(IncrementalOrderBookTest, L1AskDegradesErasesOldLevel) {
  IncrementalOrderBook book(8);
  seed_basic_snapshot(book);

  book.apply_l1_update(make_l1(kInstrument, 2'000, 100'000, 5, 100'200, 6));

  const auto& asks = book.asks();
  ASSERT_EQ(asks.size(), 1u);
  EXPECT_EQ(asks[0].price, 100'200);
  EXPECT_EQ(asks[0].quantity, 6);
}

TEST(IncrementalOrderBookTest, L1BothSidesImprove) {
  IncrementalOrderBook book(8);
  seed_basic_snapshot(book);

  book.apply_l1_update(make_l1(kInstrument, 2'000, 100'050, 7, 100'075, 8));

  const auto& bids = book.bids();
  ASSERT_EQ(bids.size(), 3u);
  EXPECT_EQ(bids[0].price, 100'050);
  EXPECT_EQ(bids[1].price, 100'000);
  EXPECT_EQ(bids[2].price, 99'900);

  const auto& asks = book.asks();
  ASSERT_EQ(asks.size(), 3u);
  EXPECT_EQ(asks[0].price, 100'075);
  EXPECT_EQ(asks[1].price, 100'100);
  EXPECT_EQ(asks[2].price, 100'200);
}

TEST(IncrementalOrderBookTest, L1BothSidesDegrade) {
  IncrementalOrderBook book(8);
  seed_basic_snapshot(book);

  book.apply_l1_update(make_l1(kInstrument, 2'000, 99'900, 3, 100'200, 6));

  const auto& bids = book.bids();
  ASSERT_EQ(bids.size(), 1u);
  EXPECT_EQ(bids[0].price, 99'900);

  const auto& asks = book.asks();
  ASSERT_EQ(asks.size(), 1u);
  EXPECT_EQ(asks[0].price, 100'200);
}

TEST(IncrementalOrderBookTest, MultiTickL1DegradeErasesEveryTraversedLevel) {
  IncrementalOrderBook book(12);
  const bool snapshot = true;
  const bool batch = true;
  book.apply_l2_update(make_l2(kInstrument, 1'000, Side::Buy, 100'000, 5, snapshot, batch, false));
  book.apply_l2_update(make_l2(kInstrument, 1'000, Side::Buy, 99'900, 4, snapshot, batch, false));
  book.apply_l2_update(make_l2(kInstrument, 1'000, Side::Buy, 99'800, 3, snapshot, batch, false));
  book.apply_l2_update(make_l2(kInstrument, 1'000, Side::Buy, 99'700, 2, snapshot, batch, false));
  book.apply_l2_update(make_l2(kInstrument, 1'000, Side::Sell, 100'100, 5, snapshot, batch, false));
  book.apply_l2_update(make_l2(kInstrument, 1'000, Side::Sell, 100'200, 4, snapshot, batch, false));
  book.apply_l2_update(make_l2(kInstrument, 1'000, Side::Sell, 100'300, 3, snapshot, batch, false));
  book.apply_l2_update(make_l2(kInstrument, 1'000, Side::Sell, 100'400, 2, snapshot, batch, true));

  book.apply_l1_update(make_l1(kInstrument, 2'000, 99'700, 7, 100'400, 8));

  ASSERT_EQ(book.bids().size(), 1u);
  EXPECT_EQ(book.bids()[0], (IncrementalOrderBook::Level{99'700, 7}));
  ASSERT_EQ(book.asks().size(), 1u);
  EXPECT_EQ(book.asks()[0], (IncrementalOrderBook::Level{100'400, 8}));
}

TEST(IncrementalOrderBookTest, MultipleL1ImprovementsBuildDepth) {
  IncrementalOrderBook book(8);
  seed_basic_snapshot(book);

  book.apply_l1_update(make_l1(kInstrument, 2'000, 100'010, 2, 100'100, 4));
  book.apply_l1_update(make_l1(kInstrument, 2'100, 100'020, 3, 100'100, 4));
  book.apply_l1_update(make_l1(kInstrument, 2'200, 100'030, 1, 100'100, 4));

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

  book.apply_l1_update(make_l1(kInstrument, 2'000, 100'050, 7, 100'100, 4));

  ASSERT_EQ(book.bids().size(), 3u);

  const bool snapshot = true;
  const bool batch = true;
  book.apply_l2_update(make_l2(kInstrument, 3'000, Side::Buy, 100'060, 10, snapshot, batch, false));
  book.apply_l2_update(make_l2(kInstrument, 3'000, Side::Buy, 100'040, 8, snapshot, batch, false));
  book.apply_l2_update(make_l2(kInstrument, 3'000, Side::Sell, 100'110, 5, snapshot, batch, false));
  book.apply_l2_update(make_l2(kInstrument, 3'000, Side::Sell, 100'120, 7, snapshot, batch, true));

  const auto& bids = book.bids();
  ASSERT_EQ(bids.size(), 2u);
  EXPECT_EQ(bids[0].price, 100'060);
  EXPECT_EQ(bids[1].price, 100'040);

  const auto& asks = book.asks();
  ASSERT_EQ(asks.size(), 2u);
  EXPECT_EQ(asks[0].price, 100'110);
  EXPECT_EQ(asks[1].price, 100'120);
}

TEST(IncrementalOrderBookTest, L1BidImprovementPrunesCrossedAskLevels) {
  IncrementalOrderBook book(8);
  const bool snapshot = true;
  const bool batch = true;
  book.apply_l2_update(make_l2(kInstrument, 1'000, Side::Buy, 100'000, 5, snapshot, batch, false));
  book.apply_l2_update(make_l2(kInstrument, 1'000, Side::Sell, 100'100, 4, snapshot, batch, false));
  book.apply_l2_update(make_l2(kInstrument, 1'000, Side::Sell, 100'125, 5, snapshot, batch, false));
  book.apply_l2_update(make_l2(kInstrument, 1'000, Side::Sell, 100'200, 6, snapshot, batch, true));

  book.apply_l1_update(make_l1(kInstrument, 2'000, 100'150, 7, 100'200, 6));

  EXPECT_TRUE(book.ready());
  EXPECT_EQ(book.best_bid_price(), 100'150);
  EXPECT_EQ(book.best_ask_price(), 100'200);
  EXPECT_GT(book.best_ask_price(), book.best_bid_price());

  for (const auto& ask : book.asks()) {
    EXPECT_GT(ask.price, book.best_bid_price());
  }
}

TEST(IncrementalOrderBookTest, L1AskImprovementPrunesCrossedBidLevels) {
  IncrementalOrderBook book(8);
  const bool snapshot = true;
  const bool batch = true;
  book.apply_l2_update(make_l2(kInstrument, 1'000, Side::Buy, 100'000, 5, snapshot, batch, false));
  book.apply_l2_update(make_l2(kInstrument, 1'000, Side::Buy, 99'950, 4, snapshot, batch, false));
  book.apply_l2_update(make_l2(kInstrument, 1'000, Side::Buy, 99'900, 3, snapshot, batch, false));
  book.apply_l2_update(make_l2(kInstrument, 1'000, Side::Sell, 100'100, 4, snapshot, batch, true));

  book.apply_l1_update(make_l1(kInstrument, 2'000, 99'900, 3, 99'950, 8));

  EXPECT_TRUE(book.ready());
  EXPECT_EQ(book.best_bid_price(), 99'900);
  EXPECT_EQ(book.best_ask_price(), 99'950);
  EXPECT_LT(book.best_bid_price(), book.best_ask_price());

  for (const auto& bid : book.bids()) {
    EXPECT_LT(bid.price, book.best_ask_price());
  }
}
