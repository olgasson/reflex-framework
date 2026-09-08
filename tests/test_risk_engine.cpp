#include "gtest/gtest.h"

#include "asset_info_manager.hpp"
#include "messages.hpp"
#include "risk/risk_engine.hpp"
#include "utils/codec_utils.hpp"

#include <cmath>
#include <string>

using namespace reflex;

namespace {
inline void expect_near(double a, double b, double tol = 1e-8) {
  EXPECT_NEAR(a, b, tol);
}
}

class RiskEngineTest : public ::testing::Test {
protected:
  void SetUp() override {
    AssetInfoManager::initialize();
    ai_ = AssetInfoManager::get_by_instrument_id(10301);
    ASSERT_NE(ai_, nullptr);

    maker_fee_rate_ = static_cast<double>(ai_->maker_fee_ppb_) / 1e9;
    taker_fee_rate_ = static_cast<double>(ai_->taker_fee_ppb_) / 1e9;

    contract_base_ = CodecUtils::to_double(static_cast<int64_t>(ai_->contract_size_));

    re_ = std::make_unique<RiskEngine>(10301);
  }

  const AssetInfo* ai_{nullptr};
  double maker_fee_rate_{0.0};
  double taker_fee_rate_{0.0};
  double contract_base_{0.0};
  std::unique_ptr<RiskEngine> re_;
};

TEST_F(RiskEngineTest, ExactSignedExchangeFeeOverridesModeledTierFee) {
  re_->set_fees(10.0, 20.0);
  re_->on_fill(RiskEngine::Fill{
      .side = Side::Buy,
      .qty_contracts = 2.0,
      .px = 60'000.0,
      .taker = false,
      .ts_ns = 1,
      .fee_quote = 0.12345678,
      .fee_quote_valid = true,
  });
  expect_near(re_->fees_usd(), 0.12345678);

  re_->on_fill(RiskEngine::Fill{
      .side = Side::Sell,
      .qty_contracts = 1.0,
      .px = 60'100.0,
      .taker = true,
      .ts_ns = 2,
      .fee_quote = -0.01,
      .fee_quote_valid = true,
  });
  expect_near(re_->fees_usd(), 0.11345678);
}

TEST_F(RiskEngineTest, LongThenPartialSellFifoRealizedAndFees) {
  const double q_buy = 100.0;
  const double px_buy = 60000.0;
  re_->on_fill(RiskEngine::Fill{
      .side = Side::Buy,
      .qty_contracts = q_buy,
      .px = px_buy,
      .taker = false,
      .ts_ns = 1
  });

  re_->on_mark_price(60100.0);

  auto M = re_->snapshot();
  expect_near(M.position_contracts, 100.0);
  expect_near(M.position_base, q_buy * contract_base_);
  expect_near(M.unrealized_pnl_quote, (60100.0 - 60000.0) * (q_buy * contract_base_));
  const double buy_notional = q_buy * contract_base_ * px_buy;
  expect_near(M.fees_quote, std::fabs(buy_notional) * maker_fee_rate_);

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

  const double matched_base1 = q_sell1 * contract_base_;
  const double realized1 = (px_sell1 - px_buy) * matched_base1;
  expect_near(M.realized_pnl_quote, realized1);

  expect_near(M.position_contracts, 60.0);
  expect_near(M.avg_entry_quote, 60000.0);

  const double sell_notional1 = q_sell1 * contract_base_ * px_sell1;
  expect_near(M.fees_quote, std::fabs(buy_notional) * maker_fee_rate_ +
                            std::fabs(sell_notional1) * taker_fee_rate_);

  const double u = (60100.0 - 60000.0) * (60.0 * contract_base_);
  expect_near(M.unrealized_pnl_quote, u);
}

TEST_F(RiskEngineTest, ShortThenPartialCoverFifoRealizedAndFees) {
  re_->reset();

  const double q_sell = 50.0;
  const double px_sell = 60000.0;
  re_->on_fill(RiskEngine::Fill{
      .side = Side::Sell,
      .qty_contracts = q_sell,
      .px = px_sell,
      .taker = true,
      .ts_ns = 1
  });

  re_->on_mark_price(59800.0);
  auto M = re_->snapshot();

  expect_near(M.position_contracts, -50.0);
  expect_near(M.avg_entry_quote, 60000.0);

  const double u = (px_sell - 59800.0) * (q_sell * contract_base_);
  expect_near(M.unrealized_pnl_quote, u);

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

  const double matched_base = q_buy * contract_base_;
  const double realized = (px_sell - px_buy) * matched_base;
  expect_near(M.realized_pnl_quote, realized);

  expect_near(M.position_contracts, -30.0);
  expect_near(M.avg_entry_quote, 60000.0);

  const double sell_notional = q_sell * contract_base_ * px_sell;
  const double buy_notional  = q_buy * contract_base_ * px_buy;
  expect_near(M.fees_quote,
              std::fabs(sell_notional) * taker_fee_rate_ +
              std::fabs(buy_notional)  * maker_fee_rate_);
}

TEST_F(RiskEngineTest, ReversalLongToShortFifo) {
  re_->reset();

  re_->on_fill(RiskEngine::Fill{Side::Buy, 30.0, 60000.0, false, 1});
  re_->on_fill(RiskEngine::Fill{Side::Buy, 20.0, 60200.0, false, 2});

  auto M = re_->snapshot();
  expect_near(M.position_contracts, 50.0);
  const double base1 = 30.0 * contract_base_;
  const double base2 = 20.0 * contract_base_;
  const double avg_entry_long = (base1 * 60000.0 + base2 * 60200.0) / (base1 + base2);
  expect_near(M.avg_entry_quote, avg_entry_long);

  re_->on_fill(RiskEngine::Fill{Side::Sell, 70.0, 60100.0, true, 3});

  M = re_->snapshot();

  const double bpc = contract_base_;
  const double realized =
      (60100.0 - 60000.0) * (30.0 * bpc) +
      (60100.0 - 60200.0) * (20.0 * bpc);
  expect_near(M.realized_pnl_quote, realized);

  expect_near(M.position_contracts, -20.0);
  expect_near(M.avg_entry_quote, 60100.0);

  const double notional_b1 = 30.0 * bpc * 60000.0;
  const double notional_b2 = 20.0 * bpc * 60200.0;
  const double notional_s  = 70.0 * bpc * 60100.0;

  expect_near(M.fees_quote,
              std::fabs(notional_b1) * maker_fee_rate_ +
              std::fabs(notional_b2) * maker_fee_rate_ +
              std::fabs(notional_s)  * taker_fee_rate_);
}

TEST_F(RiskEngineTest, UnrealizedAndTotalPnl) {
  re_->reset();

  re_->on_fill(RiskEngine::Fill{Side::Buy, 10.0, 60000.0, true, 1});
  re_->on_mark_price(60500.0);

  auto M = re_->snapshot();
  const double u = (60500.0 - 60000.0) * (10.0 * contract_base_);
  expect_near(M.unrealized_pnl_quote, u);

  re_->on_fill(RiskEngine::Fill{Side::Sell, 10.0, 60400.0, false, 2});
  M = re_->snapshot();
  expect_near(M.unrealized_pnl_quote, 0.0);
  const double realized = (60400.0 - 60000.0) * (10.0 * contract_base_);
  expect_near(M.realized_pnl_quote, realized);

  const double notional_b = 10.0 * contract_base_ * 60000.0;
  const double notional_s = 10.0 * contract_base_ * 60400.0;
  const double fees = std::fabs(notional_b) * taker_fee_rate_ + std::fabs(notional_s) * maker_fee_rate_;
  expect_near(M.fees_quote, fees);

  const double total = re_->total_pnl_quote();
  expect_near(total, realized - fees);
}

TEST_F(RiskEngineTest, AverageEntryShortAccumulation) {
  re_->reset();

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

TEST_F(RiskEngineTest, SameDirectionFillRefreshesUnrealizedAtExistingMark) {
  re_->set_fees(0.0, 0.0);
  re_->on_mark_price(100.0);
  re_->on_fill(RiskEngine::Fill{Side::Buy, 1.0, 90.0, false, 1});
  expect_near(re_->snapshot().unrealized_pnl_quote, 10.0 * contract_base_);

  re_->on_fill(RiskEngine::Fill{Side::Buy, 1.0, 110.0, false, 2});

  expect_near(re_->snapshot().avg_entry_quote, 100.0);
  expect_near(re_->snapshot().unrealized_pnl_quote, 0.0);
  EXPECT_TRUE(re_->pnl_attribution().closes());
}

TEST_F(RiskEngineTest, PathAttributionClosesToAuthoritativeFifoTotal) {
  re_->set_fees(0.0, 0.0);
  re_->on_mark_price(100.0);
  re_->on_fill(RiskEngine::Fill{Side::Buy, 2.0, 99.0, false, 1});
  re_->on_mark_price(105.0);
  re_->on_fill(RiskEngine::Fill{Side::Sell, 1.0, 106.0, false, 2});
  re_->on_mark_price(103.0);
  re_->on_funding_payment(0.25);

  const auto attribution = re_->pnl_attribution();
  expect_near(attribution.execution_edge_quote, 3.0 * contract_base_);
  expect_near(attribution.inventory_carry_quote, 8.0 * contract_base_);
  expect_near(attribution.funding_pnl_quote, 0.25);
  expect_near(attribution.attributed_total_quote,
              11.0 * contract_base_ + 0.25);
  expect_near(attribution.fifo_total_quote, attribution.attributed_total_quote);
  expect_near(attribution.identity_residual_quote, 0.0);
  EXPECT_TRUE(attribution.valid);
  EXPECT_TRUE(attribution.closes());
}

TEST_F(RiskEngineTest, FillBeforeFirstMarkInvalidatesPathAttribution) {
  re_->on_fill(RiskEngine::Fill{Side::Buy, 1.0, 100.0, false, 1});
  re_->on_mark_price(101.0);

  const auto attribution = re_->pnl_attribution();
  EXPECT_EQ(attribution.fills_before_first_mark, 1U);
  EXPECT_FALSE(attribution.valid);
  EXPECT_FALSE(attribution.closes());
}

TEST_F(RiskEngineTest, DiagStringSmoke) {
  re_->reset();
  re_->on_fill(RiskEngine::Fill{Side::Buy, 3.0, 60000.0, false, 1});
  re_->on_mark_price(60010.0);

  const std::string d = re_->diag_string();
  EXPECT_FALSE(d.empty());
  EXPECT_NE(d.find("pos_contr="), std::string::npos);
  EXPECT_NE(d.find("RPnL="), std::string::npos);
  EXPECT_NE(d.find("UPnL="), std::string::npos);
  EXPECT_NE(d.find("fees="), std::string::npos);
}

TEST(RiskEngineSpotTest, TreatsSpotQuantityAsBaseAssetUnits) {
  AssetInfoManager::initialize();
  RiskEngine risk(301);
  EXPECT_DOUBLE_EQ(risk.contract_value_base(), 1.0);

  risk.on_fill(RiskEngine::Fill{Side::Buy, 0.001, 60'000.0, false, 1});
  EXPECT_NEAR(risk.snapshot().position_base, 0.001, 1e-12);
}

TEST(RiskEngineFeeNormalizationTest, RepricesMakerNotionalAtAssumedRate) {
  AssetInfoManager::initialize();
  RiskEngine risk(301);

  risk.on_fill(RiskEngine::Fill{Side::Buy, 1.0, 60'000.0, false, 1, 12.0,
                                true});
  risk.on_fill(RiskEngine::Fill{Side::Sell, 1.0, 60'000.0, true, 2, 30.0,
                                true});

  const double actual = risk.total_pnl_quote();
  const double normalized = risk.fee_normalized_pnl_quote(-0.10 / 10'000.0);
  EXPECT_NEAR(normalized - actual, 12.0 + 0.60, 1e-9);
  EXPECT_NEAR(risk.maker_notional_usd(), 60'000.0, 1e-9);
  EXPECT_NEAR(risk.taker_fees_usd(), 30.0, 1e-9);
}
