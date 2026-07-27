// tests/test_risk_engine.cpp
#include "gtest/gtest.h"

#include "asset_info_manager.hpp"
#include "messages.hpp"          // for Side
#include "risk/risk_engine.hpp"
#include "utils/codec_utils.hpp"

#include <cmath>
#include <string>

using namespace reflex;

namespace {
// Helper: compare doubles with tolerance
inline void expect_near(double a, double b, double tol = 1e-8) {
  EXPECT_NEAR(a, b, tol);
}
} // namespace

class RiskEngineTest : public ::testing::Test {
protected:
  void SetUp() override {
    // Ensure instruments are initialized once.
    AssetInfoManager::initialize();
    ai_ = AssetInfoManager::get_by_instrument_id(10301); // OKX BTC-USDT-SWAP
    ASSERT_NE(ai_, nullptr);

    // Fee rates (bps -> fraction). Adjust field names if yours differ.
    maker_fee_rate_ = static_cast<double>(ai_->maker_fee_bps_) / 10'000.0;
    taker_fee_rate_ = static_cast<double>(ai_->taker_fee_bps_) / 10'000.0;

    // Contract size in base units (BTC) as double (ai_->contract_size_ is fp8)
    contract_base_ = CodecUtils::to_double(static_cast<int64_t>(ai_->contract_size_));

    // Create engine under test
    re_ = std::make_unique<RiskEngine>(10301);
  }

  const AssetInfo* ai_{nullptr};
  double maker_fee_rate_{0.0};
  double taker_fee_rate_{0.0};
  double contract_base_{0.0};
  std::unique_ptr<RiskEngine> re_;
};

// --- Basic long then partial sell (FIFO), fees (maker on buy, taker on sell) ---
TEST_F(RiskEngineTest, LongThenPartialSellFifoRealizedAndFees) {
  // Buy 100 contracts @ 60,000 (maker)
  const double q_buy = 100.0;
  const double px_buy = 60000.0;
  re_->on_fill(RiskEngine::Fill{
      .side = Side::Buy,
      .qty_contracts = q_buy,
      .px = px_buy,
      .taker = false,
      .ts_ns = 1
  });

  // Mark at 60,100
  re_->on_mark_price(60100.0);

  auto M = re_->snapshot();
  // Position checks
  expect_near(M.position_contracts, 100.0);
  expect_near(M.position_base, q_buy * contract_base_);
  // Unrealized: (60100-60000)*base
  expect_near(M.unrealized_pnl_quote, (60100.0 - 60000.0) * (q_buy * contract_base_));
  // Fees so far: maker on the buy
  const double buy_notional = q_buy * contract_base_ * px_buy;
  expect_near(M.fees_quote, std::fabs(buy_notional) * maker_fee_rate_);

  // Sell 40 contracts @ 60,500 (taker)
  const double q_sell1 = 40.0;
  const double px_sell1 = 60500.0;
  re_->on_fill(RiskEngine::Fill{
      .side = Side::Sell,
      .qty_contracts = q_sell1,
      .px = px_sell1,
      .taker = true,
      .ts_ns = 2
  });

  M = re_->snapshot();

  // Realized = (sell - buy) * matched_base
  const double matched_base1 = q_sell1 * contract_base_;
  const double realized1 = (px_sell1 - px_buy) * matched_base1;
  expect_near(M.realized_pnl_quote, realized1);

  // Remaining inventory = 60 contracts long at 60,000
  expect_near(M.position_contracts, 60.0);
  expect_near(M.avg_entry_quote, 60000.0);

  // Fees include taker fee on the sell as well
  const double sell_notional1 = q_sell1 * contract_base_ * px_sell1;
  expect_near(M.fees_quote, std::fabs(buy_notional) * maker_fee_rate_ +
                            std::fabs(sell_notional1) * taker_fee_rate_);

  // Unrealized now: mark still 60,100 on remaining 60 contracts
  const double u = (60100.0 - 60000.0) * (60.0 * contract_base_);
  expect_near(M.unrealized_pnl_quote, u);
}

// --- Short then partial buy-to-cover, FIFO, fees (taker on sell, maker on buy) ---
TEST_F(RiskEngineTest, ShortThenPartialCoverFifoRealizedAndFees) {
  re_->reset();

  // Open short: Sell 50 @ 60,000 (taker)
  const double q_sell = 50.0;
  const double px_sell = 60000.0;
  re_->on_fill(RiskEngine::Fill{
      .side = Side::Sell,
      .qty_contracts = q_sell,
      .px = px_sell,
      .taker = true,
      .ts_ns = 1
  });

  // Mark at 59,800 -> unrealized profit (entry - mark) * base
  re_->on_mark_price(59800.0);
  auto M = re_->snapshot();

  expect_near(M.position_contracts, -50.0);
  expect_near(M.avg_entry_quote, 60000.0);

  const double u = (px_sell - 59800.0) * (q_sell * contract_base_);
  expect_near(M.unrealized_pnl_quote, u);

  // Cover 20 @ 59,700 (maker)
  const double q_buy = 20.0;
  const double px_buy = 59700.0;
  re_->on_fill(RiskEngine::Fill{
      .side = Side::Buy,
      .qty_contracts = q_buy,
      .px = px_buy,
      .taker = false,
      .ts_ns = 2
  });

  M = re_->snapshot();

  // Realized on 20: (entry - cover) * base
  const double matched_base = q_buy * contract_base_;
  const double realized = (px_sell - px_buy) * matched_base;
  expect_near(M.realized_pnl_quote, realized);

  // Position now -30 contracts
  expect_near(M.position_contracts, -30.0);
  expect_near(M.avg_entry_quote, 60000.0); // remaining short lot still at 60k

  // Fees: taker on initial sell + maker on buy
  const double sell_notional = q_sell * contract_base_ * px_sell;
  const double buy_notional  = q_buy * contract_base_ * px_buy;
  expect_near(M.fees_quote,
              std::fabs(sell_notional) * taker_fee_rate_ +
              std::fabs(buy_notional)  * maker_fee_rate_);
}

// --- Reversal: start long, then sell more than position => flat + new short ---
TEST_F(RiskEngineTest, ReversalLongToShortFifo) {
  re_->reset();

  // Buy 30 @ 60,000 (maker)
  re_->on_fill(RiskEngine::Fill{Side::Buy, 30.0, 60000.0, false, 1});
  // Buy 20 @ 60,200 (maker)
  re_->on_fill(RiskEngine::Fill{Side::Buy, 20.0, 60200.0, false, 2});

  // Now long 50; average entry should be weighted
  auto M = re_->snapshot();
  expect_near(M.position_contracts, 50.0);
  const double base1 = 30.0 * contract_base_;
  const double base2 = 20.0 * contract_base_;
  const double avg_entry_long = (base1 * 60000.0 + base2 * 60200.0) / (base1 + base2);
  expect_near(M.avg_entry_quote, avg_entry_long);

  // Sell 70 @ 60,100 (taker) => realize on 50 long (FIFO: 30@60000, 20@60200), open short 20 @ 60100
  re_->on_fill(RiskEngine::Fill{Side::Sell, 70.0, 60100.0, true, 3});

  M = re_->snapshot();

  // Realized:
  // - First 30: (60100 - 60000)*30*base_per_contract
  // - Next 20:  (60100 - 60200)*20*base_per_contract
  const double bpc = contract_base_;
  const double realized =
      (60100.0 - 60000.0) * (30.0 * bpc) +
      (60100.0 - 60200.0) * (20.0 * bpc);
  expect_near(M.realized_pnl_quote, realized);

  // New position: -20 short at 60100
  expect_near(M.position_contracts, -20.0);
  expect_near(M.avg_entry_quote, 60100.0);

  // Fees: maker on two buys + taker on the large sell
  const double notional_b1 = 30.0 * bpc * 60000.0;
  const double notional_b2 = 20.0 * bpc * 60200.0;
  const double notional_s  = 70.0 * bpc * 60100.0;

  expect_near(M.fees_quote,
              std::fabs(notional_b1) * maker_fee_rate_ +
              std::fabs(notional_b2) * maker_fee_rate_ +
              std::fabs(notional_s)  * taker_fee_rate_);
}

// --- Unrealized PnL updates with mark and total PnL sanity ---
TEST_F(RiskEngineTest, UnrealizedAndTotalPnl) {
  re_->reset();

  // Long 10 @ 60,000 (taker)
  re_->on_fill(RiskEngine::Fill{Side::Buy, 10.0, 60000.0, true, 1});
  // Mark 60,500 => UPnL = 500 * 10 * base_per_contract
  re_->on_mark_price(60500.0);

  auto M = re_->snapshot();
  const double u = (60500.0 - 60000.0) * (10.0 * contract_base_);
  expect_near(M.unrealized_pnl_quote, u);

  // Now sell 10 @ 60,400 (maker) => realize (60400 - 60000) * 10 * base, position flat.
  re_->on_fill(RiskEngine::Fill{Side::Sell, 10.0, 60400.0, false, 2});
  M = re_->snapshot();
  expect_near(M.unrealized_pnl_quote, 0.0); // flat
  const double realized = (60400.0 - 60000.0) * (10.0 * contract_base_);
  expect_near(M.realized_pnl_quote, realized);

  // Total PnL = realized - fees (since flat)
  // Fees: taker on buy + maker on sell
  const double notional_b = 10.0 * contract_base_ * 60000.0;
  const double notional_s = 10.0 * contract_base_ * 60400.0;
  const double fees = std::fabs(notional_b) * taker_fee_rate_ + std::fabs(notional_s) * maker_fee_rate_;
  expect_near(M.fees_quote, fees);

  const double total = re_->total_pnl_quote();
  expect_near(total, realized - fees);
}

// --- Average entry price correctness for net short with multiple adds ---
TEST_F(RiskEngineTest, AverageEntryShortAccumulation) {
  re_->reset();

  // Sell 10 @ 61,000 (maker), then 15 @ 60,800 (maker)
  re_->on_fill(RiskEngine::Fill{Side::Sell, 10.0, 61000.0, false, 1});
  re_->on_fill(RiskEngine::Fill{Side::Sell, 15.0, 60800.0, false, 2});

  auto M = re_->snapshot();
  expect_near(M.position_contracts, -25.0);

  const double bpc = contract_base_;
  const double base_sum = (10.0 + 15.0) * bpc;
  const double cost_sum = (10.0 * bpc * 61000.0) + (15.0 * bpc * 60800.0);
  const double avg = cost_sum / base_sum;

  expect_near(M.avg_entry_quote, avg);
}

// --- diag_string smoke test (doesn't assert exact string, just non-empty & contains pieces) ---
TEST_F(RiskEngineTest, DiagStringSmoke) {
  re_->reset();
  re_->on_fill(RiskEngine::Fill{Side::Buy, 3.0, 60000.0, false, 1});
  re_->on_mark_price(60010.0);

  const std::string d = re_->diag_string();
  EXPECT_FALSE(d.empty());
  // A few key tokens
  EXPECT_NE(d.find("pos_contr="), std::string::npos);
  EXPECT_NE(d.find("RPnL="), std::string::npos);
  EXPECT_NE(d.find("UPnL="), std::string::npos);
  EXPECT_NE(d.find("fees="), std::string::npos);
}