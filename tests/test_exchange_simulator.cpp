
#include "backtest/exchange_simulator.hpp"

#include "asset_info_manager.hpp"
#include "domain/order_book_manager.hpp"
#include "offset_epoch_nano_clock.hpp"
#include "backtest/mock_exchange_response_handler.hpp"
#include "utils/codec_utils.hpp"

#include <gtest/gtest.h>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unistd.h>
#include <vector>

using namespace reflex;
using namespace reflex::backtest;

namespace {

class TempLifecyclePath {
public:
  TempLifecyclePath() {
    std::string pattern =
        (std::filesystem::temp_directory_path() /
         "reflex_lifecycle_XXXXXX").string();
    const int fd = ::mkstemp(pattern.data());
    if (fd < 0) {
      throw std::runtime_error("failed to create lifecycle telemetry path");
    }
    ::close(fd);
    path_ = std::move(pattern);
  }

  ~TempLifecyclePath() {
    std::error_code ec;
    std::filesystem::remove(path_, ec);
  }

  const std::filesystem::path& path() const { return path_; }

private:
  std::filesystem::path path_;
};

using CsvRow = std::vector<std::string>;

std::vector<CsvRow> read_csv(const std::filesystem::path& path) {
  std::ifstream input(path);
  if (!input.is_open()) {
    throw std::runtime_error("failed to read lifecycle telemetry");
  }

  std::vector<CsvRow> rows;
  for (std::string line; std::getline(input, line);) {
    CsvRow row;
    std::istringstream fields(line);
    for (std::string field; std::getline(fields, field, ',');) {
      row.push_back(std::move(field));
    }
    rows.push_back(std::move(row));
  }
  return rows;
}

}

class ExchangeSimulatorTest : public ::testing::Test {
protected:
  void SetUp() override {
    clock = std::make_shared<SimulationClock>();
    handler = std::make_unique<MockExchangeResponseHandler>();
    sim = std::make_unique<ExchangeSimulator>(clock, handler.get());
  }

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
    l2.is_last_batch_ = BooleanEnum::TRUE;
    sim->process_l2_update(l2);
  }

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


TEST_F(ExchangeSimulatorTest, AcceptsPostOnlyLimitOrderWhenNotCrossing) {
  send_bid_update(1, 100.00, 1.0);
  send_bid_update(1, 99.50, 2.0);
  send_ask_update(1, 101.00, 1.0);
  send_ask_update(1, 101.50, 3.0);

  PendingEvent e{};
  e.order_id_ = 1;
  e.instrument_id_ = 1;
  e.side_ = Side::Buy;
  e.price_ = CodecUtils::encode_price(100.00);
  e.quantity_ = CodecUtils::encode_quantity(10.0);
  e.order_type_ = OrderType::Limit;
  e.exec_inst_ = ExecInst::ParticipateDontInitiate;

  sim->on_pending(e);

  ASSERT_TRUE(handler->has<AcceptedEvent>());
  ASSERT_FALSE(handler->has<RejectedEvent>());
}

TEST_F(ExchangeSimulatorTest, RateLimitRejectionPreservesOperationIdentity) {
  MessageSlot pending_slot{};
  auto* pending = new (pending_slot.raw_data()) PendingEvent();
  pending->order_id_ = 71;
  sim->on_rate_limited(pending_slot);
  ASSERT_TRUE(handler->has<RejectedEvent>());
  EXPECT_EQ(handler->last<RejectedEvent>()->order_id_, 71);
  EXPECT_EQ(handler->last<RejectedEvent>()->reject_reason_,
            RejectReason::RateLimit);

  MessageSlot replace_slot{};
  auto* replace = new (replace_slot.raw_data()) PendingReplaceEvent();
  replace->order_id_ = 72;
  replace->request_id_ = 7002;
  sim->on_rate_limited(replace_slot);
  ASSERT_TRUE(handler->has<ReplaceRejectedEvent>());
  EXPECT_EQ(handler->last<ReplaceRejectedEvent>()->order_id_, 72);
  EXPECT_EQ(handler->last<ReplaceRejectedEvent>()->request_id_, 7002);
  EXPECT_EQ(handler->last<ReplaceRejectedEvent>()->reject_reason_,
            RejectReason::RateLimit);

  MessageSlot cancel_slot{};
  auto* cancel = new (cancel_slot.raw_data()) PendingCancelEvent();
  cancel->order_id_ = 73;
  cancel->request_id_ = 7003;
  sim->on_rate_limited(cancel_slot);
  ASSERT_TRUE(handler->has<CancelRejectedEvent>());
  EXPECT_EQ(handler->last<CancelRejectedEvent>()->order_id_, 73);
  EXPECT_EQ(handler->last<CancelRejectedEvent>()->request_id_, 7003);
  EXPECT_EQ(handler->last<CancelRejectedEvent>()->reject_reason_,
            RejectReason::RateLimit);
}

TEST_F(ExchangeSimulatorTest, RejectsBehindTouchSimulationWithoutL2Provenance) {
  L1UpdateEvent l1{};
  l1.instrument_id_ = 1;
  l1.exchange_ = Exchange::Okx;
  l1.bid_price_ = CodecUtils::encode_price(100.00);
  l1.bid_size_ = CodecUtils::encode_quantity(10.0);
  l1.offer_price_ = CodecUtils::encode_price(101.00);
  l1.offer_size_ = CodecUtils::encode_quantity(10.0);
  l1.timestamp_ns_ = 1;
  sim->process_l1_update(l1);

  PendingEvent e{};
  e.order_id_ = 2;
  e.instrument_id_ = 1;
  e.side_ = Side::Buy;
  e.price_ = CodecUtils::encode_price(99.00);
  e.quantity_ = CodecUtils::encode_quantity(1.0);
  e.order_type_ = OrderType::Limit;
  e.exec_inst_ = ExecInst::ParticipateDontInitiate;

  EXPECT_THROW(sim->on_pending(e), std::runtime_error);
}

TEST_F(ExchangeSimulatorTest, RejectsBehindTouchReplaceWithoutL2Provenance) {
  L1UpdateEvent l1{};
  l1.instrument_id_ = 1;
  l1.exchange_ = Exchange::Okx;
  l1.bid_price_ = CodecUtils::encode_price(100.00);
  l1.bid_size_ = CodecUtils::encode_quantity(10.0);
  l1.offer_price_ = CodecUtils::encode_price(101.00);
  l1.offer_size_ = CodecUtils::encode_quantity(10.0);
  l1.timestamp_ns_ = 1;
  sim->process_l1_update(l1);

  PendingEvent e{};
  e.order_id_ = 3;
  e.instrument_id_ = 1;
  e.side_ = Side::Buy;
  e.price_ = CodecUtils::encode_price(100.00);
  e.quantity_ = CodecUtils::encode_quantity(1.0);
  e.order_type_ = OrderType::Limit;
  e.exec_inst_ = ExecInst::ParticipateDontInitiate;
  sim->on_pending(e);
  ASSERT_TRUE(handler->has<AcceptedEvent>());

  PendingReplaceEvent r{};
  r.order_id_ = 3;
  r.request_id_ = 300;
  r.instrument_id_ = 1;
  r.price_ = CodecUtils::encode_price(99.00);
  r.quantity_ = CodecUtils::encode_quantity(1.0);
  EXPECT_THROW(sim->on_pending_replace(r), std::runtime_error);
}

TEST_F(ExchangeSimulatorTest, RejectsOffTickGridLimitPrice) {
  reflex::AssetInfoManager::initialize();
  send_bid_update(10303, 16.48, 10.0);
  send_ask_update(10303, 16.49, 10.0);

  PendingEvent e{};
  e.order_id_ = 3;
  e.instrument_id_ = 10303;
  e.side_ = Side::Buy;
  e.price_ = CodecUtils::encode_price(16.475);
  e.quantity_ = CodecUtils::encode_quantity(1.0);
  e.order_type_ = OrderType::Limit;
  e.exec_inst_ = ExecInst::ParticipateDontInitiate;

  EXPECT_THROW(sim->on_pending(e), std::runtime_error);
}

TEST_F(ExchangeSimulatorTest, RejectsPostOnlyCrossingLimitOrder) {
  send_bid_update(1, 100.00, 1.0);
  send_ask_update(1, 101.00, 1.0);

  PendingEvent e{};
  e.order_id_ = 1;
  e.instrument_id_ = 1;
  e.side_ = Side::Buy;
  e.price_ = CodecUtils::encode_price(102.00);
  e.quantity_ = CodecUtils::encode_quantity(10.0);
  e.order_type_ = OrderType::Limit;
  e.exec_inst_ = ExecInst::ParticipateDontInitiate;

  sim->on_pending(e);

  ASSERT_TRUE(handler->has<RejectedEvent>());
  const auto rej = handler->last<RejectedEvent>();
  ASSERT_EQ(rej->reject_reason_, RejectReason::PostOnly);
}


TEST_F(ExchangeSimulatorTest, MarketBuySweepsTopOfBook) {
  send_ask_update(1, 101.00, 5.0);
  send_ask_update(1, 102.00, 10.0);

  PendingEvent e{};
  e.order_id_ = 1;
  e.instrument_id_ = 1;
  e.side_ = Side::Buy;
  e.quantity_ = CodecUtils::encode_quantity(10.0);
  e.order_type_ = OrderType::Market;

  sim->on_pending(e);

  ASSERT_TRUE(handler->has<AcceptedEvent>());
  ASSERT_TRUE(handler->has<ExecutedEvent>());

  EXPECT_EQ(handler->events.size(), 3);

  ExecutedEvent first_exec = handler->events[1]->as<ExecutedEvent>();
  EXPECT_EQ(first_exec.last_price_, CodecUtils::encode_price(101.00));
  EXPECT_EQ(first_exec.last_quantity_, CodecUtils::encode_quantity(5.0));

  ExecutedEvent second_exec = handler->events[2]->as<ExecutedEvent>();
  EXPECT_EQ(second_exec.last_price_, CodecUtils::encode_price(102.00));
  EXPECT_EQ(second_exec.last_quantity_, CodecUtils::encode_quantity(5.0));
}

TEST_F(ExchangeSimulatorTest, MarketSweepCarriesExactMetadataTakerFee) {
  reflex::AssetInfoManager::initialize();
  constexpr int32_t kInstrument = 10301;
  const auto* asset =
      reflex::AssetInfoManager::get_by_instrument_id(kInstrument);
  ASSERT_NE(asset, nullptr);
  send_bid_update(kInstrument, 100.00, 5.0);
  send_ask_update(kInstrument, 101.00, 5.0);

  PendingEvent event{};
  event.order_id_ = 101;
  event.instrument_id_ = kInstrument;
  event.side_ = Side::Buy;
  event.quantity_ = CodecUtils::encode_quantity(2.0);
  event.order_type_ = OrderType::Market;
  sim->on_pending(event);

  ASSERT_TRUE(handler->has<ExecutedEvent>());
  const auto execution = handler->last<ExecutedEvent>();
  EXPECT_EQ(execution->commission_valid_, 1);
  const double expected = 2.0 * asset->asset_size(1.0) * 101.0 *
                          asset->taker_fee_ppb_ / 1e9;
  EXPECT_NEAR(CodecUtils::to_double(execution->commission_), expected, 1e-8);
}

TEST_F(ExchangeSimulatorTest, MarketSellSweepsTopOfBook) {
  send_bid_update(1, 100.00, 5.0);
  send_bid_update(1, 99.00, 10.0);

  PendingEvent e{};
  e.order_id_ = 2;
  e.instrument_id_ = 1;
  e.side_ = Side::Sell;
  e.quantity_ = CodecUtils::encode_quantity(10.0);
  e.order_type_ = OrderType::Market;

  sim->on_pending(e);

  ASSERT_TRUE(handler->has<AcceptedEvent>());
  ASSERT_TRUE(handler->has<ExecutedEvent>());

  EXPECT_EQ(handler->events.size(), 3);

  ExecutedEvent first_exec = handler->events[1]->as<ExecutedEvent>();
  EXPECT_EQ(first_exec.last_price_, CodecUtils::encode_price(100.00));
  EXPECT_EQ(first_exec.last_quantity_, CodecUtils::encode_quantity(5.0));

  ExecutedEvent second_exec = handler->events[2]->as<ExecutedEvent>();
  EXPECT_EQ(second_exec.last_price_, CodecUtils::encode_price(99.00));
  EXPECT_EQ(second_exec.last_quantity_, CodecUtils::encode_quantity(5.0));
}

TEST_F(ExchangeSimulatorTest, IocLimitNeverSweepsBeyondItsContraTouchPrice) {
  send_bid_update(1, 100.00, 2.0);
  send_bid_update(1, 99.00, 5.0);

  PendingEvent e{};
  e.order_id_ = 3;
  e.instrument_id_ = 1;
  e.side_ = Side::Sell;
  e.price_ = CodecUtils::encode_price(100.00);
  e.quantity_ = CodecUtils::encode_quantity(4.0);
  e.order_type_ = OrderType::Limit;
  e.time_in_force_ = TimeInForce::Ioc;
  e.exec_inst_ = ExecInst::Default;

  sim->on_pending(e);

  ASSERT_EQ(handler->events.size(), 3U);
  EXPECT_EQ(handler->events[0]->get_type(), MessageType::Accepted);
  ASSERT_EQ(handler->events[1]->get_type(), MessageType::Executed);
  EXPECT_EQ(handler->events[1]->as<ExecutedEvent>().last_price_, e.price_);
  EXPECT_EQ(handler->events[1]->as<ExecutedEvent>().last_quantity_,
            CodecUtils::encode_quantity(2.0));
  ASSERT_EQ(handler->events[2]->get_type(), MessageType::CancelAccepted);
  EXPECT_EQ(handler->events[2]->as<CancelAcceptedEvent>().cancel_reason_,
            CancelReason::System);
}


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

  send_trade(1, Side::Sell, 99.00, 5.0);

  ASSERT_TRUE(handler->has<ExecutedEvent>());
  const auto ex = handler->last<ExecutedEvent>();
  EXPECT_EQ(ex->order_id_, e.order_id_);
  EXPECT_EQ(ex->last_quantity_, CodecUtils::encode_quantity(5.0));
  EXPECT_EQ(ex->last_price_, CodecUtils::encode_price(100.00));
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

  send_trade(1, Side::Buy, 102.00, 5.0);

  ASSERT_TRUE(handler->has<ExecutedEvent>());
  const auto ex = handler->last<ExecutedEvent>();
  EXPECT_EQ(ex->order_id_, e.order_id_);
  EXPECT_EQ(ex->last_quantity_, CodecUtils::encode_quantity(5.0));
  EXPECT_EQ(ex->last_price_, CodecUtils::encode_price(101.00));
}


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
  r.price_ = CodecUtils::encode_price(102.00);
  r.quantity_ = CodecUtils::encode_quantity(1.0);
  sim->on_pending_replace(r);

  ASSERT_TRUE(handler->has<ReplaceRejectedEvent>());
  const auto rr = handler->last<ReplaceRejectedEvent>();
  EXPECT_EQ(rr->reject_reason_, RejectReason::PostOnly);
}

TEST_F(ExchangeSimulatorTest, RejectsOffTickGridReplacementPrice) {
  reflex::AssetInfoManager::initialize();
  send_bid_update(10303, 16.48, 10.0);
  send_ask_update(10303, 16.49, 10.0);

  PendingEvent e{};
  e.order_id_ = 41;
  e.instrument_id_ = 10303;
  e.side_ = Side::Buy;
  e.price_ = CodecUtils::encode_price(16.48);
  e.quantity_ = CodecUtils::encode_quantity(1.0);
  e.order_type_ = OrderType::Limit;
  e.exec_inst_ = ExecInst::ParticipateDontInitiate;
  sim->on_pending(e);

  PendingReplaceEvent replacement{};
  replacement.order_id_ = 41;
  replacement.price_ = CodecUtils::encode_price(16.475);
  replacement.quantity_ = CodecUtils::encode_quantity(1.0);
  EXPECT_THROW(sim->on_pending_replace(replacement), std::runtime_error);
}


TEST_F(ExchangeSimulatorTest, HandlesMultipleL2UpdatesCorrectly) {
  send_bid_update(1, 100.00, 5.0);
  send_bid_update(1, 99.50, 10.0);
  send_ask_update(1, 101.00, 8.0);
  send_ask_update(1, 101.50, 12.0);

  send_bid_update(1, 100.05, 3.0);

  PendingEvent e{};
  e.order_id_ = 50;
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


TEST_F(ExchangeSimulatorTest, QueuePositionDelaysFillUntilDepthAheadDrains) {
  send_bid_update(1, 100.00, 5.0);
  send_bid_update(1, 99.00, 5.0);
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
  EXPECT_EQ(handler->last<AcceptedEvent>()->backtest_arrival_mid_x2_,
            CodecUtils::encode_price(201.00));
  handler->clear();

  send_trade(1, Side::Sell, 100.00, 3.0);
  ASSERT_FALSE(handler->has<ExecutedEvent>());

  send_trade(1, Side::Sell, 100.00, 4.0);
  ASSERT_TRUE(handler->has<ExecutedEvent>());
  const auto ex = handler->last<ExecutedEvent>();
  EXPECT_EQ(ex->order_id_, e.order_id_);
  EXPECT_EQ(ex->last_quantity_, CodecUtils::encode_quantity(2.0));
  EXPECT_EQ(ex->backtest_execution_mid_x2_,
            CodecUtils::encode_price(200.00));
}

TEST_F(ExchangeSimulatorTest, QueueInitFractionZeroJoinsFrontOfQueue) {
  sim->set_queue_init_fraction(0.0);
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

  send_trade(1, Side::Sell, 100.00, 1.0);
  ASSERT_TRUE(handler->has<ExecutedEvent>());
  EXPECT_EQ(handler->last<ExecutedEvent>()->last_quantity_, CodecUtils::encode_quantity(1.0));
}

TEST_F(ExchangeSimulatorTest, SizedPrintFillsPartiallyAcrossTrades) {
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

  send_trade(1, Side::Sell, 100.00, 2.0);
  ASSERT_TRUE(handler->has<ExecutedEvent>());
  EXPECT_EQ(handler->last<ExecutedEvent>()->last_quantity_, CodecUtils::encode_quantity(2.0));
  handler->clear();

  send_trade(1, Side::Sell, 100.00, 3.0);
  ASSERT_TRUE(handler->has<ExecutedEvent>());
  EXPECT_EQ(handler->last<ExecutedEvent>()->last_quantity_, CodecUtils::encode_quantity(3.0));
  handler->clear();

  send_trade(1, Side::Sell, 100.00, 1.0);
  ASSERT_FALSE(handler->has<ExecutedEvent>());
}

TEST_F(ExchangeSimulatorTest, TradeThroughPrintUsesFiniteVolumePastQueue) {
  send_bid_update(1, 100.00, 5.0);
  send_bid_update(1, 99.00, 5.0);
  send_ask_update(1, 101.00, 5.0);

  PendingEvent e{};
  e.order_id_ = 140;
  e.instrument_id_ = 1;
  e.side_ = Side::Buy;
  e.price_ = CodecUtils::encode_price(100.00);
  e.quantity_ = CodecUtils::encode_quantity(2.0);
  e.order_type_ = OrderType::Limit;
  e.exec_inst_ = ExecInst::ParticipateDontInitiate;
  sim->on_pending(e);
  handler->clear();

  send_trade(1, Side::Sell, 100.00, 1.0);
  ASSERT_FALSE(handler->has<ExecutedEvent>());

  send_trade(1, Side::Sell, 99.00, 1.0);
  ASSERT_TRUE(handler->has<ExecutedEvent>());
  const auto ex = handler->last<ExecutedEvent>();
  EXPECT_EQ(ex->order_id_, e.order_id_);
  EXPECT_EQ(ex->last_quantity_, CodecUtils::encode_quantity(1.0));
  EXPECT_EQ(ex->last_price_, CodecUtils::encode_price(100.00));

  handler->clear();
  send_trade(1, Side::Sell, 99.00, 5.0);
  ASSERT_TRUE(handler->has<ExecutedEvent>());
  EXPECT_EQ(handler->last<ExecutedEvent>()->last_quantity_,
            CodecUtils::encode_quantity(1.0));

  handler->clear();
  send_trade(1, Side::Sell, 99.00, 5.0);
  ASSERT_FALSE(handler->has<ExecutedEvent>());
}

TEST_F(ExchangeSimulatorTest, TradeThroughSharesBudgetWithAtPriceLevel) {
  send_bid_update(1, 100.00, 5.0);
  send_bid_update(1, 99.00, 3.0);
  send_ask_update(1, 101.00, 5.0);

  PendingEvent top{};
  top.order_id_ = 150;
  top.instrument_id_ = 1;
  top.side_ = Side::Buy;
  top.price_ = CodecUtils::encode_price(100.00);
  top.quantity_ = CodecUtils::encode_quantity(2.0);
  top.order_type_ = OrderType::Limit;
  top.exec_inst_ = ExecInst::ParticipateDontInitiate;
  sim->on_pending(top);

  PendingEvent lower{};
  lower.order_id_ = 151;
  lower.instrument_id_ = 1;
  lower.side_ = Side::Buy;
  lower.price_ = CodecUtils::encode_price(99.00);
  lower.quantity_ = CodecUtils::encode_quantity(2.0);
  lower.order_type_ = OrderType::Limit;
  lower.exec_inst_ = ExecInst::ParticipateDontInitiate;
  sim->on_pending(lower);
  handler->clear();

  send_trade(1, Side::Sell, 99.00, 3.0);
  ASSERT_TRUE(handler->has<ExecutedEvent>());
  EXPECT_EQ(handler->last<ExecutedEvent>()->order_id_, top.order_id_);
  EXPECT_EQ(handler->last<ExecutedEvent>()->last_quantity_,
            CodecUtils::encode_quantity(2.0));
  handler->clear();

  send_trade(1, Side::Sell, 99.00, 2.0);
  ASSERT_FALSE(handler->has<ExecutedEvent>());

  send_trade(1, Side::Sell, 99.00, 2.0);
  ASSERT_TRUE(handler->has<ExecutedEvent>());
  EXPECT_EQ(handler->last<ExecutedEvent>()->order_id_, lower.order_id_);
  EXPECT_EQ(handler->last<ExecutedEvent>()->last_quantity_,
            CodecUtils::encode_quantity(2.0));
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

  send_trade(1, Side::Sell, 99.00, 0.0);
  ASSERT_FALSE(handler->has<ExecutedEvent>());
}


TEST_F(ExchangeSimulatorTest, RemovesLevelWhenQuantityIsZero) {
  send_bid_update(1, 100.00, 5.0);
  send_ask_update(1, 101.00, 8.0);

  send_bid_update(1, 100.00, 0.0);

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


TEST_F(ExchangeSimulatorTest, HandlesSnapshotUpdates) {
  L2UpdateEvent snapshot;
  snapshot.instrument_id_ = 1;
  snapshot.exchange_ = Exchange::Okx;
  snapshot.side_ = Side::Buy;
  snapshot.price_1_ = CodecUtils::encode_price(100.00);
  snapshot.size_1_ = CodecUtils::encode_quantity(10.0);
  snapshot.timestamp_ns_ = clock->epoch_nanos();
  snapshot.snapshot_ = BooleanEnum::TRUE;
  snapshot.num_levels_ = 1;
  snapshot.is_batch_message_ = BooleanEnum::TRUE;
  snapshot.is_last_batch_ = BooleanEnum::FALSE;

  sim->process_l2_update(snapshot);

  snapshot.side_ = Side::Sell;
  snapshot.price_1_ = CodecUtils::encode_price(101.00);
  snapshot.size_1_ = CodecUtils::encode_quantity(10.0);
  snapshot.is_last_batch_ = BooleanEnum::TRUE;
  sim->process_l2_update(snapshot);

  PendingEvent e{};
  e.order_id_ = 70;
  e.instrument_id_ = 1;
  e.side_ = Side::Buy;
  e.price_ = CodecUtils::encode_price(99.50);
  e.quantity_ = CodecUtils::encode_quantity(1.0);
  e.order_type_ = OrderType::Limit;
  e.exec_inst_ = ExecInst::ParticipateDontInitiate;

  sim->on_pending(e);

  ASSERT_TRUE(handler->has<AcceptedEvent>());
  ASSERT_FALSE(handler->has<RejectedEvent>());
}


TEST_F(ExchangeSimulatorTest, HandlesBatchMessages) {
  L2UpdateEvent batch1;
  batch1.instrument_id_ = 1;
  batch1.exchange_ = Exchange::Okx;
  batch1.side_ = Side::Buy;
  batch1.price_1_ = CodecUtils::encode_price(100.00);
  batch1.size_1_ = CodecUtils::encode_quantity(5.0);
  batch1.timestamp_ns_ = clock->epoch_nanos();
  batch1.snapshot_ = BooleanEnum::FALSE;
  batch1.num_levels_ = 1;
  batch1.is_batch_message_ = BooleanEnum::TRUE;
  batch1.is_last_batch_ = BooleanEnum::FALSE;

  sim->process_l2_update(batch1);

  L2UpdateEvent batch2;
  batch2.instrument_id_ = 1;
  batch2.exchange_ = Exchange::Okx;
  batch2.side_ = Side::Sell;
  batch2.price_1_ = CodecUtils::encode_price(101.00);
  batch2.size_1_ = CodecUtils::encode_quantity(8.0);
  batch2.timestamp_ns_ = clock->epoch_nanos();
  batch2.snapshot_ = BooleanEnum::FALSE;
  batch2.num_levels_ = 1;
  batch2.is_batch_message_ = BooleanEnum::TRUE;
  batch2.is_last_batch_ = BooleanEnum::TRUE;

  sim->process_l2_update(batch2);

  PendingEvent e{};
  e.order_id_ = 80;
  e.instrument_id_ = 1;
  e.side_ = Side::Buy;
  e.price_ = CodecUtils::encode_price(100.50);
  e.quantity_ = CodecUtils::encode_quantity(1.0);
  e.order_type_ = OrderType::Limit;
  e.exec_inst_ = ExecInst::ParticipateDontInitiate;

  sim->on_pending(e);

  ASSERT_TRUE(handler->has<AcceptedEvent>());
  ASSERT_FALSE(handler->has<RejectedEvent>());
}

TEST_F(ExchangeSimulatorTest,
       UnknownAndRepeatedCancelTransitionsReturnOrderUnknown) {
  PendingCancelEvent cancel{};
  cancel.order_id_ = 900;

  sim->on_pending_cancel(cancel);
  ASSERT_TRUE(handler->has<CancelRejectedEvent>());
  EXPECT_EQ(handler->last<CancelRejectedEvent>()->reject_reason_,
            RejectReason::OrderUnknown);

  handler->clear();
  PendingEvent order{};
  order.order_id_ = cancel.order_id_;
  order.instrument_id_ = 1;
  order.side_ = Side::Buy;
  order.price_ = CodecUtils::encode_price(100.0);
  order.quantity_ = CodecUtils::encode_quantity(1.0);
  order.order_type_ = OrderType::Limit;
  order.exec_inst_ = ExecInst::ParticipateDontInitiate;
  sim->on_pending(order);
  ASSERT_TRUE(handler->has<AcceptedEvent>());

  handler->clear();
  sim->on_pending_cancel(cancel);
  ASSERT_TRUE(handler->has<CancelAcceptedEvent>());
  ASSERT_FALSE(handler->has<CancelRejectedEvent>());

  handler->clear();
  sim->on_pending_cancel(cancel);
  ASSERT_TRUE(handler->has<CancelRejectedEvent>());
  EXPECT_EQ(handler->last<CancelRejectedEvent>()->reject_reason_,
            RejectReason::OrderUnknown);
}

TEST_F(ExchangeSimulatorTest, CancelAfterFullFillReturnsOrderUnknown) {
  PendingEvent order{};
  order.order_id_ = 901;
  order.instrument_id_ = 1;
  order.side_ = Side::Buy;
  order.price_ = CodecUtils::encode_price(100.0);
  order.quantity_ = CodecUtils::encode_quantity(1.0);
  order.order_type_ = OrderType::Limit;
  order.exec_inst_ = ExecInst::ParticipateDontInitiate;
  sim->on_pending(order);
  handler->clear();

  send_trade(1, Side::Sell, 99.0, 1.0);
  ASSERT_TRUE(handler->has<ExecutedEvent>());

  handler->clear();
  PendingCancelEvent cancel{};
  cancel.order_id_ = order.order_id_;
  sim->on_pending_cancel(cancel);

  ASSERT_TRUE(handler->has<CancelRejectedEvent>());
  EXPECT_EQ(handler->last<CancelRejectedEvent>()->reject_reason_,
            RejectReason::OrderUnknown);
}

TEST_F(ExchangeSimulatorTest, CrossingReplaceRejectKeepsOriginalOrderResting) {
  send_bid_update(1, 100.0, 1.0);
  send_ask_update(1, 101.0, 1.0);

  PendingEvent order{};
  order.order_id_ = 902;
  order.instrument_id_ = 1;
  order.side_ = Side::Buy;
  order.price_ = CodecUtils::encode_price(100.0);
  order.quantity_ = CodecUtils::encode_quantity(1.0);
  order.order_type_ = OrderType::Limit;
  order.exec_inst_ = ExecInst::ParticipateDontInitiate;
  sim->on_pending(order);
  handler->clear();

  PendingReplaceEvent replacement{};
  replacement.order_id_ = order.order_id_;
  replacement.instrument_id_ = order.instrument_id_;
  replacement.price_ = CodecUtils::encode_price(101.0);
  replacement.quantity_ = order.quantity_;
  sim->on_pending_replace(replacement);

  ASSERT_TRUE(handler->has<ReplaceRejectedEvent>());
  EXPECT_EQ(handler->last<ReplaceRejectedEvent>()->reject_reason_,
            RejectReason::PostOnly);

  handler->clear();
  send_trade(1, Side::Sell, 100.0, 2.0);
  ASSERT_TRUE(handler->has<ExecutedEvent>());
  EXPECT_EQ(handler->last<ExecutedEvent>()->order_id_, order.order_id_);
  EXPECT_EQ(handler->last<ExecutedEvent>()->last_price_, order.price_);
  EXPECT_EQ(handler->last<ExecutedEvent>()->last_quantity_, order.quantity_);
}

TEST_F(ExchangeSimulatorTest, AcceptedReplaceMovesOrderAndLosesOldPrice) {
  PendingEvent order{};
  order.order_id_ = 903;
  order.instrument_id_ = 1;
  order.side_ = Side::Buy;
  order.price_ = CodecUtils::encode_price(100.0);
  order.quantity_ = CodecUtils::encode_quantity(1.0);
  order.order_type_ = OrderType::Limit;
  order.exec_inst_ = ExecInst::ParticipateDontInitiate;
  sim->on_pending(order);
  handler->clear();

  PendingReplaceEvent replacement{};
  replacement.order_id_ = order.order_id_;
  replacement.instrument_id_ = order.instrument_id_;
  replacement.price_ = CodecUtils::encode_price(99.0);
  replacement.quantity_ = order.quantity_;
  sim->on_pending_replace(replacement);
  ASSERT_TRUE(handler->has<ReplaceAcceptedEvent>());

  handler->clear();
  send_trade(1, Side::Sell, 100.0, 1.0);
  ASSERT_FALSE(handler->has<ExecutedEvent>());

  send_trade(1, Side::Sell, 99.0, 1.0);
  ASSERT_TRUE(handler->has<ExecutedEvent>());
  EXPECT_EQ(handler->last<ExecutedEvent>()->last_price_, replacement.price_);
}

TEST_F(ExchangeSimulatorTest,
       PartialFillThenCancelPreservesLeavesRemovesBookAndWritesTelemetry) {
  TempLifecyclePath telemetry;
  sim->set_quote_lifecycle_output_path(telemetry.path());
  clock->set_time(10);

  PendingEvent order{};
  order.order_id_ = 910;
  order.instrument_id_ = 1;
  order.side_ = Side::Buy;
  order.price_ = CodecUtils::encode_price(100.0);
  order.quantity_ = CodecUtils::encode_quantity(5.0);
  order.order_type_ = OrderType::Limit;
  order.exec_inst_ = ExecInst::ParticipateDontInitiate;
  sim->on_pending(order);
  handler->clear();

  clock->set_time(20);
  send_trade(1, Side::Sell, 100.0, 2.0);
  ASSERT_TRUE(handler->has<ExecutedEvent>());
  EXPECT_EQ(handler->last<ExecutedEvent>()->last_quantity_,
            CodecUtils::encode_quantity(2.0));
  handler->clear();

  clock->set_time(30);
  PendingCancelEvent cancel{};
  cancel.order_id_ = order.order_id_;
  sim->on_pending_cancel(cancel);
  ASSERT_TRUE(handler->has<CancelAcceptedEvent>());
  EXPECT_EQ(handler->last<CancelAcceptedEvent>()->cancel_reason_,
            CancelReason::UserRequest);

  handler->clear();
  send_trade(1, Side::Sell, 100.0, 5.0);
  EXPECT_FALSE(handler->has<ExecutedEvent>());

  sim->flush_quote_lifecycle();
  const auto rows = read_csv(telemetry.path());
  ASSERT_EQ(rows.size(), 4u);
  EXPECT_EQ(rows[1][0], "accepted");
  EXPECT_EQ(rows[1][7], std::to_string(CodecUtils::encode_quantity(5.0)));
  EXPECT_EQ(rows[2][0], "filled");
  EXPECT_EQ(rows[2][6], std::to_string(CodecUtils::encode_quantity(2.0)));
  EXPECT_EQ(rows[2][7], std::to_string(CodecUtils::encode_quantity(3.0)));
  EXPECT_EQ(rows[2][10], "0");
  EXPECT_EQ(rows[2][14], "");
  EXPECT_EQ(rows[3][0], "canceled");
  EXPECT_EQ(rows[3][7], std::to_string(CodecUtils::encode_quantity(3.0)));
  EXPECT_EQ(rows[3][14], "canceled");
}

TEST_F(ExchangeSimulatorTest,
       PartialFillThenRejectedReplacePreservesOriginalAndTelemetry) {
  TempLifecyclePath telemetry;
  sim->set_quote_lifecycle_output_path(telemetry.path());
  send_bid_update(1, 99.0, 10.0);
  send_ask_update(1, 101.0, 2.0);

  PendingEvent order{};
  order.order_id_ = 911;
  order.instrument_id_ = 1;
  order.side_ = Side::Buy;
  order.price_ = CodecUtils::encode_price(100.0);
  order.quantity_ = CodecUtils::encode_quantity(5.0);
  order.order_type_ = OrderType::Limit;
  order.exec_inst_ = ExecInst::ParticipateDontInitiate;
  sim->on_pending(order);
  handler->clear();

  send_trade(1, Side::Sell, 100.0, 2.0);
  ASSERT_TRUE(handler->has<ExecutedEvent>());
  handler->clear();

  PendingReplaceEvent replacement{};
  replacement.order_id_ = order.order_id_;
  replacement.instrument_id_ = order.instrument_id_;
  replacement.price_ = CodecUtils::encode_price(101.0);
  replacement.quantity_ = CodecUtils::encode_quantity(4.0);
  sim->on_pending_replace(replacement);
  ASSERT_TRUE(handler->has<ReplaceRejectedEvent>());
  EXPECT_EQ(handler->last<ReplaceRejectedEvent>()->reject_reason_,
            RejectReason::PostOnly);

  handler->clear();
  send_trade(1, Side::Sell, 100.0, 1.0);
  ASSERT_TRUE(handler->has<ExecutedEvent>());
  EXPECT_EQ(handler->last<ExecutedEvent>()->last_price_, order.price_);
  EXPECT_EQ(handler->last<ExecutedEvent>()->last_quantity_,
            CodecUtils::encode_quantity(1.0));

  sim->flush_quote_lifecycle();
  const auto rows = read_csv(telemetry.path());
  ASSERT_EQ(rows.size(), 5u);
  EXPECT_EQ(rows[3][0], "replace_rejected");
  EXPECT_EQ(rows[3][5], std::to_string(order.price_));
  EXPECT_EQ(rows[3][7], std::to_string(CodecUtils::encode_quantity(3.0)));
  EXPECT_EQ(rows[3][14], "post_only");
  EXPECT_EQ(rows[4][0], "filled");
  EXPECT_EQ(rows[4][7], std::to_string(CodecUtils::encode_quantity(2.0)));
}

TEST_F(ExchangeSimulatorTest,
       PartialFillThenAcceptedReplaceUsesTotalQuantityDelta) {
  TempLifecyclePath telemetry;
  sim->set_quote_lifecycle_output_path(telemetry.path());

  PendingEvent order{};
  order.order_id_ = 912;
  order.instrument_id_ = 1;
  order.side_ = Side::Buy;
  order.price_ = CodecUtils::encode_price(100.0);
  order.quantity_ = CodecUtils::encode_quantity(5.0);
  order.order_type_ = OrderType::Limit;
  order.exec_inst_ = ExecInst::ParticipateDontInitiate;
  sim->on_pending(order);
  handler->clear();

  send_trade(1, Side::Sell, 100.0, 2.0);
  ASSERT_TRUE(handler->has<ExecutedEvent>());
  handler->clear();

  PendingReplaceEvent replacement{};
  replacement.order_id_ = order.order_id_;
  replacement.instrument_id_ = order.instrument_id_;
  replacement.price_ = CodecUtils::encode_price(99.0);
  replacement.quantity_ = CodecUtils::encode_quantity(4.0);
  sim->on_pending_replace(replacement);
  ASSERT_TRUE(handler->has<ReplaceAcceptedEvent>());

  handler->clear();
  send_trade(1, Side::Sell, 99.0, 3.0);
  ASSERT_TRUE(handler->has<ExecutedEvent>());
  EXPECT_EQ(handler->last<ExecutedEvent>()->last_price_, replacement.price_);
  EXPECT_EQ(handler->last<ExecutedEvent>()->last_quantity_,
            CodecUtils::encode_quantity(2.0));

  handler->clear();
  PendingCancelEvent cancel{};
  cancel.order_id_ = order.order_id_;
  sim->on_pending_cancel(cancel);
  ASSERT_TRUE(handler->has<CancelRejectedEvent>());
  EXPECT_EQ(handler->last<CancelRejectedEvent>()->reject_reason_,
            RejectReason::OrderUnknown);

  sim->flush_quote_lifecycle();
  const auto rows = read_csv(telemetry.path());
  ASSERT_EQ(rows.size(), 6u);
  EXPECT_EQ(rows[3][0], "replace_accepted");
  EXPECT_EQ(rows[3][7], std::to_string(CodecUtils::encode_quantity(2.0)));
  EXPECT_EQ(rows[4][0], "fully_filled");
  EXPECT_EQ(rows[4][6], std::to_string(CodecUtils::encode_quantity(2.0)));
  EXPECT_EQ(rows[4][7], "0");
  EXPECT_EQ(rows[4][14], "filled");
  EXPECT_EQ(rows[5][0], "cancel_rejected");
  EXPECT_EQ(rows[5][14], "order_unknown");
}

TEST_F(ExchangeSimulatorTest,
       AcceptedReplaceResetsQueuePriorityAtDestinationDepth) {
  TempLifecyclePath telemetry;
  sim->set_quote_lifecycle_output_path(telemetry.path());
  send_bid_update(1, 100.0, 2.0);
  send_bid_update(1, 99.0, 4.0);
  send_ask_update(1, 101.0, 2.0);

  PendingEvent order{};
  order.order_id_ = 913;
  order.instrument_id_ = 1;
  order.side_ = Side::Buy;
  order.price_ = CodecUtils::encode_price(100.0);
  order.quantity_ = CodecUtils::encode_quantity(2.0);
  order.order_type_ = OrderType::Limit;
  order.exec_inst_ = ExecInst::ParticipateDontInitiate;
  sim->on_pending(order);
  handler->clear();

  PendingReplaceEvent replacement{};
  replacement.order_id_ = order.order_id_;
  replacement.instrument_id_ = order.instrument_id_;
  replacement.price_ = CodecUtils::encode_price(99.0);
  replacement.quantity_ = order.quantity_;
  sim->on_pending_replace(replacement);
  ASSERT_TRUE(handler->has<ReplaceAcceptedEvent>());
  handler->clear();

  send_trade(1, Side::Sell, 99.0, 4.0);
  EXPECT_FALSE(handler->has<ExecutedEvent>());
  send_trade(1, Side::Sell, 99.0, 1.0);
  ASSERT_TRUE(handler->has<ExecutedEvent>());
  EXPECT_EQ(handler->last<ExecutedEvent>()->last_quantity_,
            CodecUtils::encode_quantity(1.0));

  sim->flush_quote_lifecycle();
  const auto rows = read_csv(telemetry.path());
  ASSERT_EQ(rows.size(), 4u);
  EXPECT_EQ(rows[1][9], std::to_string(CodecUtils::encode_quantity(2.0)));
  EXPECT_EQ(rows[2][0], "replace_accepted");
  EXPECT_EQ(rows[2][9], std::to_string(CodecUtils::encode_quantity(4.0)));
  EXPECT_EQ(rows[2][10], std::to_string(CodecUtils::encode_quantity(4.0)));
  EXPECT_EQ(rows[3][0], "filled");
  EXPECT_EQ(rows[3][10], "0");
}

TEST_F(ExchangeSimulatorTest,
       ReplaceAfterFullFillRejectsUnknownAndWritesTerminalTelemetry) {
  TempLifecyclePath telemetry;
  sim->set_quote_lifecycle_output_path(telemetry.path());

  PendingEvent order{};
  order.order_id_ = 914;
  order.instrument_id_ = 1;
  order.side_ = Side::Buy;
  order.price_ = CodecUtils::encode_price(100.0);
  order.quantity_ = CodecUtils::encode_quantity(1.0);
  order.order_type_ = OrderType::Limit;
  order.exec_inst_ = ExecInst::ParticipateDontInitiate;
  sim->on_pending(order);
  handler->clear();

  send_trade(1, Side::Sell, 100.0, 1.0);
  ASSERT_TRUE(handler->has<ExecutedEvent>());
  handler->clear();

  PendingReplaceEvent replacement{};
  replacement.order_id_ = order.order_id_;
  replacement.instrument_id_ = order.instrument_id_;
  replacement.price_ = CodecUtils::encode_price(99.0);
  replacement.quantity_ = order.quantity_;
  sim->on_pending_replace(replacement);
  ASSERT_TRUE(handler->has<ReplaceRejectedEvent>());
  EXPECT_EQ(handler->last<ReplaceRejectedEvent>()->reject_reason_,
            RejectReason::OrderUnknown);

  handler->clear();
  send_trade(1, Side::Sell, 99.0, 1.0);
  EXPECT_FALSE(handler->has<ExecutedEvent>());

  sim->flush_quote_lifecycle();
  const auto rows = read_csv(telemetry.path());
  ASSERT_EQ(rows.size(), 4u);
  EXPECT_EQ(rows[2][0], "fully_filled");
  EXPECT_EQ(rows[2][7], "0");
  EXPECT_EQ(rows[2][14], "filled");
  EXPECT_EQ(rows[3][0], "replace_rejected");
  EXPECT_EQ(rows[3][14], "order_unknown");
}

TEST_F(ExchangeSimulatorTest,
       ReplaceTotalsAtOrBelowCumulativeFillRejectWithoutMovingOrder) {
  TempLifecyclePath telemetry;
  sim->set_quote_lifecycle_output_path(telemetry.path());
  send_bid_update(1, 100.0, 3.0);
  send_ask_update(1, 101.0, 3.0);

  PendingEvent order{};
  order.order_id_ = 915;
  order.instrument_id_ = 1;
  order.side_ = Side::Buy;
  order.price_ = CodecUtils::encode_price(100.0);
  order.quantity_ = CodecUtils::encode_quantity(5.0);
  order.order_type_ = OrderType::Limit;
  order.exec_inst_ = ExecInst::ParticipateDontInitiate;
  sim->on_pending(order);
  handler->clear();

  send_trade(1, Side::Sell, 100.0, 4.0);
  ASSERT_TRUE(handler->has<ExecutedEvent>());
  EXPECT_EQ(handler->last<ExecutedEvent>()->last_quantity_,
            CodecUtils::encode_quantity(1.0));

  for (const double invalid_total : {1.0, 0.0}) {
    handler->clear();
    PendingReplaceEvent replacement{};
    replacement.order_id_ = order.order_id_;
    replacement.instrument_id_ = order.instrument_id_;
    replacement.price_ = CodecUtils::encode_price(99.0);
    replacement.quantity_ = CodecUtils::encode_quantity(invalid_total);
    sim->on_pending_replace(replacement);
    ASSERT_TRUE(handler->has<ReplaceRejectedEvent>());
    EXPECT_EQ(handler->last<ReplaceRejectedEvent>()->reject_reason_,
              RejectReason::InvalidQuantity);
  }

  handler->clear();
  send_trade(1, Side::Sell, 100.0, 4.0);
  ASSERT_TRUE(handler->has<ExecutedEvent>());
  EXPECT_EQ(handler->last<ExecutedEvent>()->last_price_, order.price_);
  EXPECT_EQ(handler->last<ExecutedEvent>()->last_quantity_,
            CodecUtils::encode_quantity(4.0));

  sim->flush_quote_lifecycle();
  const auto rows = read_csv(telemetry.path());
  ASSERT_EQ(rows.size(), 6u);
  EXPECT_EQ(rows[1][0], "accepted");
  EXPECT_EQ(rows[1][9], std::to_string(CodecUtils::encode_quantity(3.0)));
  EXPECT_EQ(rows[2][0], "filled");
  EXPECT_EQ(rows[2][7], std::to_string(CodecUtils::encode_quantity(4.0)));
  EXPECT_EQ(rows[2][10], "0");
  for (std::size_t row_index = 3; row_index <= 4; ++row_index) {
    EXPECT_EQ(rows[row_index][0], "replace_rejected");
    EXPECT_EQ(rows[row_index][5], std::to_string(order.price_));
    EXPECT_EQ(rows[row_index][7],
              std::to_string(CodecUtils::encode_quantity(4.0)));
    EXPECT_EQ(rows[row_index][9],
              std::to_string(CodecUtils::encode_quantity(3.0)));
    EXPECT_EQ(rows[row_index][10], "0");
    EXPECT_EQ(rows[row_index][14], "invalid_quantity");
  }
  EXPECT_EQ(rows[3][6], std::to_string(CodecUtils::encode_quantity(1.0)));
  EXPECT_EQ(rows[4][6], "0");
  EXPECT_EQ(rows[5][0], "fully_filled");
  EXPECT_EQ(rows[5][6], std::to_string(CodecUtils::encode_quantity(4.0)));
  EXPECT_EQ(rows[5][7], "0");
}

TEST_F(ExchangeSimulatorTest, IocRemaindersUseSystemCancelReason) {
  TempLifecyclePath telemetry;
  sim->set_quote_lifecycle_output_path(telemetry.path());

  PendingEvent empty_book_order{};
  empty_book_order.order_id_ = 916;
  empty_book_order.instrument_id_ = 1;
  empty_book_order.side_ = Side::Buy;
  empty_book_order.quantity_ = CodecUtils::encode_quantity(1.0);
  empty_book_order.order_type_ = OrderType::Market;
  sim->on_pending(empty_book_order);

  ASSERT_EQ(handler->events.size(), 2u);
  EXPECT_EQ(handler->events[0]->get_type(), MessageType::Accepted);
  EXPECT_EQ(handler->events[1]->get_type(), MessageType::CancelAccepted);
  EXPECT_EQ(handler->events[1]->as<CancelAcceptedEvent>().cancel_reason_,
            CancelReason::System);
  EXPECT_EQ(handler->events[1]->as<CancelAcceptedEvent>().request_id_, 0);

  handler->clear();
  send_ask_update(1, 101.0, 2.0);
  PendingEvent thin_book_order{};
  thin_book_order.order_id_ = 917;
  thin_book_order.instrument_id_ = 1;
  thin_book_order.side_ = Side::Buy;
  thin_book_order.quantity_ = CodecUtils::encode_quantity(3.0);
  thin_book_order.order_type_ = OrderType::Market;
  sim->on_pending(thin_book_order);

  ASSERT_EQ(handler->events.size(), 3u);
  EXPECT_EQ(handler->events[0]->get_type(), MessageType::Accepted);
  EXPECT_EQ(handler->events[1]->get_type(), MessageType::Executed);
  EXPECT_EQ(handler->events[1]->as<ExecutedEvent>().last_quantity_,
            CodecUtils::encode_quantity(2.0));
  EXPECT_EQ(handler->events[2]->get_type(), MessageType::CancelAccepted);
  EXPECT_EQ(handler->events[2]->as<CancelAcceptedEvent>().cancel_reason_,
            CancelReason::System);
  EXPECT_EQ(handler->events[2]->as<CancelAcceptedEvent>().request_id_, 0);

  sim->flush_quote_lifecycle();
  const auto rows = read_csv(telemetry.path());
  ASSERT_EQ(rows.size(), 3u);
  EXPECT_EQ(rows[0][26], "request_id");
  for (std::size_t row_index = 1; row_index < rows.size(); ++row_index) {
    ASSERT_EQ(rows[row_index].size(), 35u);
    EXPECT_EQ(rows[row_index][0], "canceled");
    EXPECT_EQ(rows[row_index][14], "system_ioc_remainder");
    EXPECT_EQ(rows[row_index][26], "0");
  }
}

TEST_F(ExchangeSimulatorTest,
       CancelAndReplaceResponsesEchoTheExactRequestIdentity) {
  TempLifecyclePath telemetry;
  sim->set_quote_lifecycle_output_path(telemetry.path());

  PendingReplaceEvent unknown_replace{};
  unknown_replace.order_id_ = 920;
  unknown_replace.request_id_ = 7001;
  unknown_replace.instrument_id_ = 1;
  unknown_replace.price_ = CodecUtils::encode_price(99.0);
  unknown_replace.quantity_ = CodecUtils::encode_quantity(1.0);
  sim->on_pending_replace(unknown_replace);
  ASSERT_TRUE(handler->has<ReplaceRejectedEvent>());
  EXPECT_EQ(handler->last<ReplaceRejectedEvent>()->request_id_, 7001);

  handler->clear();
  PendingEvent order{};
  order.order_id_ = 921;
  order.instrument_id_ = 1;
  order.side_ = Side::Buy;
  order.price_ = CodecUtils::encode_price(100.0);
  order.quantity_ = CodecUtils::encode_quantity(1.0);
  order.order_type_ = OrderType::Limit;
  order.exec_inst_ = ExecInst::ParticipateDontInitiate;
  sim->on_pending(order);
  handler->clear();

  PendingReplaceEvent accepted_replace{};
  accepted_replace.order_id_ = order.order_id_;
  accepted_replace.request_id_ = 7002;
  accepted_replace.instrument_id_ = order.instrument_id_;
  accepted_replace.price_ = CodecUtils::encode_price(99.0);
  accepted_replace.quantity_ = order.quantity_;
  sim->on_pending_replace(accepted_replace);
  ASSERT_TRUE(handler->has<ReplaceAcceptedEvent>());
  EXPECT_EQ(handler->last<ReplaceAcceptedEvent>()->request_id_, 7002);

  handler->clear();
  PendingCancelEvent accepted_cancel{};
  accepted_cancel.order_id_ = order.order_id_;
  accepted_cancel.request_id_ = 7003;
  sim->on_pending_cancel(accepted_cancel);
  ASSERT_TRUE(handler->has<CancelAcceptedEvent>());
  EXPECT_EQ(handler->last<CancelAcceptedEvent>()->request_id_, 7003);
  EXPECT_EQ(handler->last<CancelAcceptedEvent>()->cancel_reason_,
            CancelReason::UserRequest);

  handler->clear();
  PendingCancelEvent unknown_cancel{};
  unknown_cancel.order_id_ = order.order_id_;
  unknown_cancel.request_id_ = 7004;
  sim->on_pending_cancel(unknown_cancel);
  ASSERT_TRUE(handler->has<CancelRejectedEvent>());
  EXPECT_EQ(handler->last<CancelRejectedEvent>()->request_id_, 7004);

  sim->flush_quote_lifecycle();
  const auto rows = read_csv(telemetry.path());
  ASSERT_EQ(rows.size(), 6u);
  for (const auto& row : rows) {
    ASSERT_EQ(row.size(), 35u);
  }
  const CsvRow expected_header{
      "event_type",
      "timestamp_ns",
      "order_id",
      "instrument_id",
      "side",
      "price",
      "quantity",
      "leaves",
      "quote_created_ns",
      "initial_queue_ahead",
      "queue_ahead",
      "displayed_depth_at_placement",
      "queue_init_fraction",
      "queue_model",
      "terminal_reason",
      "price_decoded",
      "quantity_decoded",
      "leaves_decoded",
      "initial_queue_ahead_decoded",
      "queue_ahead_decoded",
      "displayed_depth_at_placement_decoded",
      "bbo_bid",
      "bbo_ask",
      "trade_price",
      "trade_qty",
      "trade_side",
      "request_id",
      "touch_depth_at_placement",
      "traded_ahead",
      "credited_cancellation",
      "touch_depth_at_placement_decoded",
      "traded_ahead_decoded",
      "credited_cancellation_decoded",
      "capped_advancement",
      "capped_advancement_decoded",
  };
  EXPECT_EQ(rows[0], expected_header);
  EXPECT_EQ(rows[1][0], "replace_rejected");
  EXPECT_EQ(rows[1][26], "7001");
  EXPECT_EQ(rows[2][0], "accepted");
  EXPECT_EQ(rows[2][26], "0");
  EXPECT_EQ(rows[3][0], "replace_accepted");
  EXPECT_EQ(rows[3][26], "7002");
  EXPECT_EQ(rows[4][0], "canceled");
  EXPECT_EQ(rows[4][26], "7003");
  EXPECT_EQ(rows[5][0], "cancel_rejected");
  EXPECT_EQ(rows[5][26], "7004");
}


TEST_F(ExchangeSimulatorTest, RejectsInvalidCancelCreditAlpha) {
  EXPECT_THROW(sim->set_cancel_credit_alpha(-0.1), std::invalid_argument);
  EXPECT_THROW(sim->set_cancel_credit_alpha(
                   std::numeric_limits<double>::infinity()),
               std::invalid_argument);
  EXPECT_THROW(sim->set_cancel_credit_alpha(
                   std::numeric_limits<double>::quiet_NaN()),
               std::invalid_argument);
  EXPECT_THROW(sim->set_cancel_credit_alpha(1e6 + 1.0),
               std::invalid_argument);
  EXPECT_NO_THROW(sim->set_cancel_credit_alpha(0.0));
  EXPECT_NO_THROW(sim->set_cancel_credit_alpha(0.5));
}

TEST_F(ExchangeSimulatorTest, CancelCreditAdvancesQueueOnlyWhenArmed) {
  for (const double alpha : {0.0, 1.0}) {
    auto local_handler = std::make_unique<MockExchangeResponseHandler>();
    auto local_clock = std::make_shared<SimulationClock>();
    ExchangeSimulator local(local_clock, local_handler.get());
    local.set_cancel_credit_alpha(alpha);

    auto l2 = [&](Side side, double px, double qty) {
      L2UpdateEvent e{};
      e.instrument_id_ = 1;
      e.exchange_ = Exchange::Okx;
      e.side_ = side;
      e.price_1_ = CodecUtils::encode_price(px);
      e.size_1_ = CodecUtils::encode_quantity(qty);
      e.timestamp_ns_ = local_clock->epoch_nanos();
      e.snapshot_ = BooleanEnum::FALSE;
      e.num_levels_ = 1;
      e.is_last_batch_ = BooleanEnum::TRUE;
      local.process_l2_update(e);
    };
    auto trade = [&](Side aggressor, double px, double size) {
      TradeEvent t{};
      t.instrument_id_ = 1;
      t.side_ = aggressor;
      t.price_ = CodecUtils::encode_price(px);
      t.size_ = CodecUtils::encode_quantity(size);
      t.timestamp_ns_ = local_clock->epoch_nanos();
      local.process_trade_event(t);
    };

    l2(Side::Buy, 100.00, 10.0);
    l2(Side::Sell, 101.00, 10.0);

    PendingEvent e{};
    e.order_id_ = 1;
    e.instrument_id_ = 1;
    e.side_ = Side::Buy;
    e.price_ = CodecUtils::encode_price(100.00);
    e.quantity_ = CodecUtils::encode_quantity(1.0);
    e.order_type_ = OrderType::Limit;
    e.exec_inst_ = ExecInst::ParticipateDontInitiate;
    local.on_pending(e);
    ASSERT_TRUE(local_handler->has<AcceptedEvent>());

    l2(Side::Buy, 100.00, 5.0);
    trade(Side::Sell, 100.00, 6.0);

    if (alpha > 0.0) {
      EXPECT_TRUE(local_handler->has<ExecutedEvent>())
          << "cancel credit armed: 5 cancelled + 6 traded should reach us";
    } else {
      EXPECT_FALSE(local_handler->has<ExecutedEvent>())
          << "pessimistic default must ignore cancellations";
    }
  }
}

TEST_F(ExchangeSimulatorTest, CancelCreditAlphaChangesQueueLocationNotVolume) {
  for (const double alpha : {0.0, 1.0, 0.5}) {
    auto local_handler = std::make_unique<MockExchangeResponseHandler>();
    auto local_clock = std::make_shared<SimulationClock>();
    ExchangeSimulator local(local_clock, local_handler.get());
    local.set_queue_init_fraction(0.25);
    local.set_cancel_credit_alpha(alpha);

    auto l2 = [&](Side side, double px, double qty) {
      L2UpdateEvent update{};
      update.instrument_id_ = 1;
      update.exchange_ = Exchange::Okx;
      update.side_ = side;
      update.price_1_ = CodecUtils::encode_price(px);
      update.size_1_ = CodecUtils::encode_quantity(qty);
      update.timestamp_ns_ = local_clock->epoch_nanos();
      update.snapshot_ = BooleanEnum::FALSE;
      update.num_levels_ = 1;
      update.is_last_batch_ = BooleanEnum::TRUE;
      local.process_l2_update(update);
    };

    l2(Side::Buy, 100.00, 100.0);
    l2(Side::Sell, 101.00, 10.0);

    PendingEvent order{};
    order.order_id_ = 1;
    order.instrument_id_ = 1;
    order.side_ = Side::Buy;
    order.price_ = CodecUtils::encode_price(100.00);
    order.quantity_ = CodecUtils::encode_quantity(1.0);
    order.order_type_ = OrderType::Limit;
    order.exec_inst_ = ExecInst::ParticipateDontInitiate;
    local.on_pending(order);

    l2(Side::Buy, 100.00, 84.0);
    TradeEvent trade{};
    trade.instrument_id_ = 1;
    trade.side_ = Side::Sell;
    trade.price_ = CodecUtils::encode_price(100.00);
    trade.size_ = CodecUtils::encode_quantity(18.0);
    trade.timestamp_ns_ = local_clock->epoch_nanos();
    local.process_trade_event(trade);

    EXPECT_EQ(local_handler->has<ExecutedEvent>(), alpha == 0.5)
        << "alpha=" << alpha;
  }
}

TEST_F(ExchangeSimulatorTest, CancelCreditNeverExceedsObservedCancellation) {
  sim->set_cancel_credit_alpha(0.25);
  send_bid_update(1, 100.00, 100.0);
  send_ask_update(1, 101.00, 10.0);

  PendingEvent order{};
  order.order_id_ = 1;
  order.instrument_id_ = 1;
  order.side_ = Side::Buy;
  order.price_ = CodecUtils::encode_price(100.00);
  order.quantity_ = CodecUtils::encode_quantity(1.0);
  order.order_type_ = OrderType::Limit;
  order.exec_inst_ = ExecInst::ParticipateDontInitiate;
  sim->on_pending(order);

  send_bid_update(1, 100.00, 95.0);
  send_trade(1, Side::Sell, 100.00, 95.0);
  EXPECT_FALSE(handler->has<ExecutedEvent>())
      << "bounded attribution cannot credit more than five units";

  send_trade(1, Side::Sell, 100.00, 1.0);
  EXPECT_TRUE(handler->has<ExecutedEvent>());
}

TEST_F(ExchangeSimulatorTest, CancelCreditNeverDoubleCountsTradedVolume) {
  sim->set_cancel_credit_alpha(1.0);
  send_bid_update(1, 100.00, 10.0);
  send_ask_update(1, 101.00, 10.0);

  PendingEvent e{};
  e.order_id_ = 1;
  e.instrument_id_ = 1;
  e.side_ = Side::Buy;
  e.price_ = CodecUtils::encode_price(100.00);
  e.quantity_ = CodecUtils::encode_quantity(1.0);
  e.order_type_ = OrderType::Limit;
  e.exec_inst_ = ExecInst::ParticipateDontInitiate;
  sim->on_pending(e);
  ASSERT_TRUE(handler->has<AcceptedEvent>());

  send_trade(1, Side::Sell, 100.00, 4.0);
  send_bid_update(1, 100.00, 6.0);

  send_trade(1, Side::Sell, 100.00, 5.0);
  EXPECT_FALSE(handler->has<ExecutedEvent>());
}

TEST_F(ExchangeSimulatorTest,
       LifecycleSeparatesTradedAheadFromCreditedCancellation) {
  TempLifecyclePath telemetry;
  sim->set_quote_lifecycle_output_path(telemetry.path());
  sim->set_cancel_credit_alpha(1.0);
  send_bid_update(1, 100.00, 10.0);
  send_ask_update(1, 101.00, 10.0);

  PendingEvent order{};
  order.order_id_ = 1;
  order.instrument_id_ = 1;
  order.side_ = Side::Buy;
  order.price_ = CodecUtils::encode_price(100.00);
  order.quantity_ = CodecUtils::encode_quantity(1.0);
  order.order_type_ = OrderType::Limit;
  order.exec_inst_ = ExecInst::ParticipateDontInitiate;
  sim->on_pending(order);

  send_bid_update(1, 100.00, 5.0);
  send_trade(1, Side::Sell, 100.00, 6.0);
  ASSERT_TRUE(handler->has<ExecutedEvent>());

  sim->flush_quote_lifecycle();
  const auto rows = read_csv(telemetry.path());
  ASSERT_EQ(rows.size(), 3u);
  ASSERT_EQ(rows[2][0], "fully_filled");
  EXPECT_EQ(rows[2][27],
            std::to_string(CodecUtils::encode_quantity(10.0)));
  EXPECT_EQ(rows[2][28],
            std::to_string(CodecUtils::encode_quantity(5.0)));
  EXPECT_EQ(rows[2][29],
            std::to_string(CodecUtils::encode_quantity(5.0)));
}

TEST_F(ExchangeSimulatorTest, CancelCreditIgnoresDepthIncreases) {
  sim->set_cancel_credit_alpha(1.0);
  send_bid_update(1, 100.00, 10.0);
  send_ask_update(1, 101.00, 10.0);

  PendingEvent e{};
  e.order_id_ = 1;
  e.instrument_id_ = 1;
  e.side_ = Side::Buy;
  e.price_ = CodecUtils::encode_price(100.00);
  e.quantity_ = CodecUtils::encode_quantity(1.0);
  e.order_type_ = OrderType::Limit;
  e.exec_inst_ = ExecInst::ParticipateDontInitiate;
  sim->on_pending(e);
  ASSERT_TRUE(handler->has<AcceptedEvent>());

  send_bid_update(1, 100.00, 15.0);
  send_trade(1, Side::Sell, 100.00, 10.0);
  EXPECT_FALSE(handler->has<ExecutedEvent>());
}

TEST_F(ExchangeSimulatorTest, SnapshotResyncGrantsNoCancelCreditOrFills) {
  sim->set_cancel_credit_alpha(0.5);
  send_bid_update(1, 100.00, 10.0);
  send_ask_update(1, 101.00, 10.0);

  auto place = [&](int64_t id, Side side, double px) {
    PendingEvent e{};
    e.order_id_ = id;
    e.instrument_id_ = 1;
    e.side_ = side;
    e.price_ = CodecUtils::encode_price(px);
    e.quantity_ = CodecUtils::encode_quantity(1.0);
    e.order_type_ = OrderType::Limit;
    e.exec_inst_ = ExecInst::ParticipateDontInitiate;
    sim->on_pending(e);
  };
  place(1, Side::Buy, 100.00);
  place(2, Side::Sell, 101.00);
  ASSERT_TRUE(handler->has<AcceptedEvent>());

  auto snapshot_msg = [&](Side side, double px, double qty, bool last) {
    L2UpdateEvent l2{};
    l2.instrument_id_ = 1;
    l2.exchange_ = Exchange::Okx;
    l2.side_ = side;
    l2.price_1_ = CodecUtils::encode_price(px);
    l2.size_1_ = CodecUtils::encode_quantity(qty);
    l2.timestamp_ns_ = clock->epoch_nanos();
    l2.snapshot_ = BooleanEnum::TRUE;
    l2.num_levels_ = 1;
    l2.is_batch_message_ = BooleanEnum::TRUE;
    l2.is_last_batch_ = last ? BooleanEnum::TRUE : BooleanEnum::FALSE;
    sim->process_l2_update(l2);
  };
  snapshot_msg(Side::Buy, 100.00, 10.0, false);
  snapshot_msg(Side::Buy, 99.99, 5.0, false);
  snapshot_msg(Side::Buy, 99.98, 5.0, true);
  snapshot_msg(Side::Sell, 101.00, 10.0, false);
  snapshot_msg(Side::Sell, 101.01, 5.0, true);

  EXPECT_FALSE(handler->has<ExecutedEvent>())
      << "snapshot rebuild must never fill a resting order";

  send_trade(1, Side::Sell, 100.00, 10.0);
  EXPECT_FALSE(handler->has<ExecutedEvent>())
      << "phantom cancel credit was granted during the snapshot rebuild";

  send_ask_update(1, 101.00, 5.0);
  send_trade(1, Side::Buy, 101.00, 6.0);
  EXPECT_TRUE(handler->has<ExecutedEvent>())
      << "baselines must re-seed and resume crediting after a resync";
}


namespace {
PendingEvent post_only_bid(int64_t order_id, double price, double qty) {
  PendingEvent e{};
  e.order_id_ = order_id;
  e.instrument_id_ = 1;
  e.side_ = Side::Buy;
  e.price_ = CodecUtils::encode_price(price);
  e.quantity_ = CodecUtils::encode_quantity(qty);
  e.order_type_ = OrderType::Limit;
  e.exec_inst_ = ExecInst::ParticipateDontInitiate;
  return e;
}

L1UpdateEvent l1_touch(double bid_px, double bid_sz, double ask_px,
                       double ask_sz, int64_t ts) {
  L1UpdateEvent l1{};
  l1.instrument_id_ = 1;
  l1.exchange_ = Exchange::Okx;
  l1.bid_price_ = CodecUtils::encode_price(bid_px);
  l1.bid_size_ = CodecUtils::encode_quantity(bid_sz);
  l1.offer_price_ = CodecUtils::encode_price(ask_px);
  l1.offer_size_ = CodecUtils::encode_quantity(ask_sz);
  l1.timestamp_ns_ = ts;
  return l1;
}
}

TEST_F(ExchangeSimulatorTest, L1AccountingCapsQueueAtDisplayedTouch) {
  TempLifecyclePath telemetry;
  sim->set_quote_lifecycle_output_path(telemetry.path());
  sim->set_queue_model(ExchangeSimulator::QueueModel::Optimistic);
  sim->set_queue_accounting_on_l1(true);
  clock->set_time(1'000'000'000);
  send_bid_update(1, 100.00, 10.0);
  send_ask_update(1, 101.00, 10.0);
  sim->on_pending(post_only_bid(1, 100.00, 1.0));

  clock->set_time(1'050'000'000);
  sim->process_l1_update(l1_touch(100.00, 2.0, 101.00, 10.0, 1'050'000'000));
  clock->set_time(1'100'000'000);
  send_trade(1, Side::Sell, 100.00, 2.5);
  ASSERT_TRUE(handler->has<ExecutedEvent>());
  EXPECT_EQ(handler->last<ExecutedEvent>()->last_quantity_,
            CodecUtils::encode_quantity(0.5));

  sim->flush_quote_lifecycle();
  const auto rows = read_csv(telemetry.path());
  ASSERT_EQ(rows.size(), 3u);
  EXPECT_EQ(rows[0][33], "capped_advancement");
  EXPECT_EQ(rows[2][0], "filled");
  EXPECT_EQ(rows[2][28], std::to_string(CodecUtils::encode_quantity(2.0)));
  EXPECT_EQ(rows[2][33], std::to_string(CodecUtils::encode_quantity(8.0)));
}

TEST_F(ExchangeSimulatorTest, L1AccountingIsOffByDefault) {
  sim->set_queue_model(ExchangeSimulator::QueueModel::Optimistic);
  clock->set_time(1'000'000'000);
  send_bid_update(1, 100.00, 10.0);
  send_ask_update(1, 101.00, 10.0);
  sim->on_pending(post_only_bid(1, 100.00, 1.0));
  clock->set_time(1'050'000'000);
  sim->process_l1_update(l1_touch(100.00, 2.0, 101.00, 10.0, 1'050'000'000));
  clock->set_time(1'100'000'000);
  send_trade(1, Side::Sell, 100.00, 2.5);
  EXPECT_FALSE(handler->has<ExecutedEvent>());
}

TEST_F(ExchangeSimulatorTest,
       L1AccountingNetsPrintAlreadyReflectedInObservation) {
  sim->set_queue_model(ExchangeSimulator::QueueModel::Optimistic);
  sim->set_queue_accounting_on_l1(true);
  sim->set_trade_netting_window_ns(3'000'000);
  clock->set_time(1'000'000'000);
  send_bid_update(1, 100.00, 10.0);
  send_ask_update(1, 101.00, 10.0);
  sim->on_pending(post_only_bid(1, 100.00, 1.0));

  clock->set_time(1'050'000'000);
  sim->process_l1_update(l1_touch(100.00, 6.0, 101.00, 10.0, 1'050'000'000));
  clock->set_time(1'050'500'000);
  send_trade(1, Side::Sell, 100.00, 4.0);
  clock->set_time(1'200'000'000);
  send_trade(1, Side::Sell, 100.00, 5.0);
  EXPECT_FALSE(handler->has<ExecutedEvent>());
  send_trade(1, Side::Sell, 100.00, 1.5);
  ASSERT_TRUE(handler->has<ExecutedEvent>());
  EXPECT_EQ(handler->last<ExecutedEvent>()->last_quantity_,
            CodecUtils::encode_quantity(0.5));
}

TEST_F(ExchangeSimulatorTest, L1AccountingTradeFirstThenObservationIsConsistent) {
  sim->set_queue_model(ExchangeSimulator::QueueModel::Optimistic);
  sim->set_queue_accounting_on_l1(true);
  sim->set_trade_netting_window_ns(3'000'000);
  clock->set_time(1'000'000'000);
  send_bid_update(1, 100.00, 10.0);
  send_ask_update(1, 101.00, 10.0);
  sim->on_pending(post_only_bid(1, 100.00, 1.0));

  clock->set_time(1'050'000'000);
  send_trade(1, Side::Sell, 100.00, 4.0);
  clock->set_time(1'050'500'000);
  sim->process_l1_update(l1_touch(100.00, 6.0, 101.00, 10.0, 1'050'500'000));
  clock->set_time(1'200'000'000);
  send_trade(1, Side::Sell, 100.00, 5.0);
  EXPECT_FALSE(handler->has<ExecutedEvent>());
  send_trade(1, Side::Sell, 100.00, 1.5);
  ASSERT_TRUE(handler->has<ExecutedEvent>());
}

TEST_F(ExchangeSimulatorTest, NettedPrintStillFillsAnOrderNearTheFront) {
  sim->set_queue_model(ExchangeSimulator::QueueModel::Optimistic);
  sim->set_queue_accounting_on_l1(true);
  sim->set_trade_netting_window_ns(3'000'000);
  sim->set_queue_init_fraction(0.2);
  clock->set_time(1'000'000'000);
  send_bid_update(1, 100.00, 10.0);
  send_ask_update(1, 101.00, 10.0);
  sim->on_pending(post_only_bid(1, 100.00, 1.0));

  clock->set_time(1'050'000'000);
  sim->process_l1_update(l1_touch(100.00, 5.0, 101.00, 10.0, 1'050'000'000));
  clock->set_time(1'050'500'000);
  send_trade(1, Side::Sell, 100.00, 4.0);
  ASSERT_TRUE(handler->has<ExecutedEvent>());
  EXPECT_EQ(handler->last<ExecutedEvent>()->last_quantity_,
            CodecUtils::encode_quantity(1.0));
}

TEST_F(ExchangeSimulatorTest, NettingWindowExpires) {
  sim->set_queue_model(ExchangeSimulator::QueueModel::Optimistic);
  sim->set_queue_accounting_on_l1(true);
  sim->set_trade_netting_window_ns(3'000'000);
  clock->set_time(1'000'000'000);
  send_bid_update(1, 100.00, 10.0);
  send_ask_update(1, 101.00, 10.0);
  sim->on_pending(post_only_bid(1, 100.00, 1.0));
  clock->set_time(1'050'000'000);
  sim->process_l1_update(l1_touch(100.00, 6.0, 101.00, 10.0, 1'050'000'000));
  clock->set_time(1'060'000'000);
  send_trade(1, Side::Sell, 100.00, 4.0);
  send_trade(1, Side::Sell, 100.00, 2.5);
  ASSERT_TRUE(handler->has<ExecutedEvent>());
}

TEST_F(ExchangeSimulatorTest, TapeWithOwnOrdersExcludesOurOwnSizeFromTheBound) {
  sim->set_queue_model(ExchangeSimulator::QueueModel::Optimistic);
  sim->set_queue_accounting_on_l1(true);
  sim->set_tape_includes_own_orders(true);
  clock->set_time(1'000'000'000);
  send_bid_update(1, 100.00, 10.0);
  send_ask_update(1, 101.00, 10.0);
  sim->on_pending(post_only_bid(1, 100.00, 1.0));
  clock->set_time(1'050'000'000);
  sim->process_l1_update(l1_touch(100.00, 1.0, 101.00, 10.0, 1'050'000'000));
  clock->set_time(1'100'000'000);
  send_trade(1, Side::Sell, 100.00, 0.5);
  ASSERT_TRUE(handler->has<ExecutedEvent>());
}

TEST_F(ExchangeSimulatorTest,
       NettingEpisodeKeepsThePreObservationSnapshotAcrossObservations) {
  sim->set_queue_model(ExchangeSimulator::QueueModel::Optimistic);
  sim->set_queue_accounting_on_l1(true);
  sim->set_trade_netting_window_ns(3'000'000);
  clock->set_time(1'000'000'000);
  send_bid_update(1, 100.00, 10.0);
  send_ask_update(1, 101.00, 10.0);
  sim->on_pending(post_only_bid(1, 100.00, 1.0));
  clock->set_time(1'050'000'000);
  sim->process_l1_update(l1_touch(100.00, 8.0, 101.00, 10.0, 1'050'000'000));
  clock->set_time(1'050'400'000);
  sim->process_l1_update(l1_touch(100.00, 6.0, 101.00, 10.0, 1'050'400'000));
  clock->set_time(1'050'900'000);
  send_trade(1, Side::Sell, 100.00, 2.0);
  send_trade(1, Side::Sell, 100.00, 2.0);
  clock->set_time(1'200'000'000);
  send_trade(1, Side::Sell, 100.00, 5.0);
  EXPECT_FALSE(handler->has<ExecutedEvent>());
  send_trade(1, Side::Sell, 100.00, 1.5);
  ASSERT_TRUE(handler->has<ExecutedEvent>());
  EXPECT_EQ(handler->last<ExecutedEvent>()->last_quantity_,
            CodecUtils::encode_quantity(0.5));
}

TEST_F(ExchangeSimulatorTest,
       DelayedPrintDoesNotShrinkTheBookBelowTheObservedDepth) {
  sim->set_queue_model(ExchangeSimulator::QueueModel::Optimistic);
  sim->set_queue_accounting_on_l1(true);
  sim->set_trade_netting_window_ns(3'000'000);
  clock->set_time(1'000'000'000);
  send_bid_update(1, 100.00, 10.0);
  send_bid_update(1, 99.00, 10.0);
  send_ask_update(1, 101.00, 10.0);
  sim->on_pending(post_only_bid(1, 100.00, 1.0));
  clock->set_time(1'050'000'000);
  sim->process_l1_update(l1_touch(100.00, 6.0, 101.00, 10.0, 1'050'000'000));
  clock->set_time(1'050'500'000);
  send_trade(1, Side::Sell, 100.00, 4.0);
  clock->set_time(1'100'000'000);
  send_bid_update(1, 99.00, 12.0);
  sim->on_pending(post_only_bid(2, 100.00, 1.0));
  clock->set_time(1'200'000'000);
  send_trade(1, Side::Sell, 100.00, 5.0);
  EXPECT_FALSE(handler->has<ExecutedEvent>());
  send_trade(1, Side::Sell, 100.00, 1.5);
  ASSERT_TRUE(handler->has<ExecutedEvent>());
  EXPECT_EQ(handler->last<ExecutedEvent>()->order_id_, 1);
}


namespace {

PendingEvent make_limit(int64_t order_id, Side side, double price, double qty) {
  PendingEvent e{};
  e.order_id_ = order_id;
  e.instrument_id_ = 1;
  e.side_ = side;
  e.price_ = CodecUtils::encode_price(price);
  e.quantity_ = CodecUtils::encode_quantity(qty);
  e.order_type_ = OrderType::Limit;
  e.time_in_force_ = TimeInForce::Gtc;
  return e;
}

int64_t executed_qty(const MockExchangeResponseHandler& h, int64_t order_id) {
  int64_t total = 0;
  for (const auto& ev : h.events) {
    if (ev->get_type() == MessageType::Executed && ev->as<ExecutedEvent>().order_id_ == order_id) {
      total += ev->as<ExecutedEvent>().last_quantity_;
    }
  }
  return total;
}

}

TEST_F(ExchangeSimulatorTest, PassiveLimitBuyBelowMarketRestsInsteadOfFilling) {
  send_bid_update(1, 100.00, 5.0);
  send_ask_update(1, 101.00, 5.0);
  send_bid_update(1, 99.00, 1.0);

  sim->on_pending(make_limit(200, Side::Buy, 99.00, 2.0));

  ASSERT_TRUE(handler->has<AcceptedEvent>());
  ASSERT_FALSE(handler->has<ExecutedEvent>());
  ASSERT_FALSE(handler->has<CancelAcceptedEvent>());
  handler->clear();

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

  sim->on_pending(make_limit(210, Side::Buy, 101.00, 10.0));

  ASSERT_TRUE(handler->has<AcceptedEvent>());
  EXPECT_EQ(executed_qty(*handler, 210), CodecUtils::encode_quantity(5.0));
  for (const auto& ev : handler->events) {
    if (ev->get_type() == MessageType::Executed) {
      EXPECT_LE(ev->as<ExecutedEvent>().last_price_, CodecUtils::encode_price(101.00));
    }
  }
  ASSERT_FALSE(handler->has<CancelAcceptedEvent>());
  handler->clear();

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

  PendingEvent m2 = make_limit(221, Side::Buy, 0.0, 5.0);
  m2.order_type_ = OrderType::Market;
  sim->on_pending(m2);
  ASSERT_TRUE(handler->has<ExecutedEvent>());
  EXPECT_EQ(handler->last<ExecutedEvent>()->last_price_, CodecUtils::encode_price(102.00));
}


TEST_F(ExchangeSimulatorTest, ReplaceAccountsForCumulativeFills) {
  send_bid_update(1, 100.00, 1.0);
  send_ask_update(1, 101.00, 1.0);
  send_bid_update(1, 99.00, 1.0);
  send_bid_update(1, 98.00, 1.0);

  sim->set_queue_init_fraction(0.0);
  PendingEvent e{};
  e.order_id_ = 300;
  e.instrument_id_ = 1;
  e.side_ = Side::Buy;
  e.price_ = CodecUtils::encode_price(100.00);
  e.quantity_ = CodecUtils::encode_quantity(10.0);
  e.order_type_ = OrderType::Limit;
  e.exec_inst_ = ExecInst::ParticipateDontInitiate;
  sim->on_pending(e);

  send_trade(1, Side::Sell, 99.00, 4.0);
  EXPECT_EQ(executed_qty(*handler, 300), CodecUtils::encode_quantity(4.0));
  handler->clear();

  PendingReplaceEvent r{};
  r.order_id_ = 300;
  r.instrument_id_ = 1;
  r.price_ = CodecUtils::encode_price(99.00);
  r.quantity_ = CodecUtils::encode_quantity(10.0);
  sim->on_pending_replace(r);
  ASSERT_TRUE(handler->has<ReplaceAcceptedEvent>());
  handler->clear();

  send_trade(1, Side::Sell, 98.00, 100.0);
  EXPECT_EQ(executed_qty(*handler, 300), CodecUtils::encode_quantity(6.0));
}


TEST_F(ExchangeSimulatorTest, SameLevelOrdersShareConsumedQueueDepth) {
  send_bid_update(1, 100.00, 5.0);
  send_ask_update(1, 101.00, 5.0);

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

  send_trade(1, Side::Sell, 100.00, 9.0);
  EXPECT_EQ(executed_qty(*handler, 400), CodecUtils::encode_quantity(2.0));
  EXPECT_EQ(executed_qty(*handler, 401), CodecUtils::encode_quantity(2.0));
}


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

}

TEST(ExchangeSimulatorQueueModelTest, OptimisticFillsWherePessimisticDoesNotOnL2Cancels) {
  ModelPair p;
  p.l2(Side::Buy, 100.00, 10.0);
  p.l2(Side::Sell, 101.00, 5.0);
  p.place_buy(100.00, 2.0);

  p.l2(Side::Buy, 100.00, 1.0);

  p.trade(Side::Sell, 100.00, 3.0);
  EXPECT_EQ(executed_qty(p.opt_handler, 500), CodecUtils::encode_quantity(2.0));
  EXPECT_EQ(executed_qty(p.pess_handler, 500), 0);
}

TEST(ExchangeSimulatorQueueModelTest, OptimisticCapAlsoAppliesOnL1UpdatesWhenEnabled) {
  ModelPair p;
  p.pess.set_queue_accounting_on_l1(true);
  p.opt.set_queue_accounting_on_l1(true);
  p.l2(Side::Buy, 100.00, 10.0);
  p.l2(Side::Sell, 101.00, 5.0);
  p.place_buy(100.00, 2.0);

  p.l1(100.00, 1.0, 101.00, 5.0);

  p.trade(Side::Sell, 100.00, 3.0);
  EXPECT_EQ(executed_qty(p.opt_handler, 500), CodecUtils::encode_quantity(2.0));
  EXPECT_EQ(executed_qty(p.pess_handler, 500), 0);
}
