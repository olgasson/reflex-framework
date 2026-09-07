// tests/test_backtest_engine_replay.cpp
//
// BackTestEngine replay semantics: scoring window (pre-window priming), venue
// provenance filtering, reference-venue delivery, timestamp-merged inputs,
// the opt-in timer source, and EventRing growth.

#include "backtest/backtest_engine.hpp"
#include "asset_info_manager.hpp"
#include "backtest/backtest_order_writer.hpp"
#include "backtest/exchange_simulator.hpp"
#include "file_header.hpp"

#include <gtest/gtest.h>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {

class RecordingStrategy final : public reflex::Strategy {
 public:
  RecordingStrategy(reflex::ClockInterface* clock,
                    reflex::TimerManager* timer_manager,
                    std::vector<std::string>* delivery_order = nullptr)
      : Strategy(clock, timer_manager, nullptr),
        delivery_order_(delivery_order) {}

  void on_l1_update(const reflex::L1UpdateEvent& event) override {
    l1_timestamps.push_back(event.timestamp_ns_);
    record("l1:" + std::to_string(event.timestamp_ns_));
  }
  void on_l2_update(const reflex::L2UpdateEvent& event) override {
    l2_timestamps.push_back(event.timestamp_ns_);
    record("l2:" + std::to_string(event.timestamp_ns_));
  }
  void on_trade(const reflex::TradeEvent& event) override {
    trade_timestamps.push_back(event.timestamp_ns_);
    record("trade:" + std::to_string(event.timestamp_ns_));
  }
  void on_mark_price(const reflex::MarkPriceEvent&) override { ++mark_count; }
  void on_funding_rate(const reflex::FundingRateEvent&) override {
    ++funding_count;
  }
  void on_open_interest(const reflex::OpenInterestEvent&) override {
    ++open_interest_count;
  }
  void on_liquidation(const reflex::LiquidationEvent&) override {
    ++liquidation_count;
  }

  void on_accepted(const reflex::Order&,
                   const reflex::AcceptedEvent&) override {}
  void on_rejected(const reflex::Order&,
                   const reflex::RejectedEvent&) override {}
  void on_replace_accepted(const reflex::Order&,
                           const reflex::ReplaceAcceptedEvent&) override {}
  void on_replace_rejected(const reflex::Order&,
                           const reflex::ReplaceRejectedEvent&) override {}
  void on_cancel_accepted(const reflex::Order&,
                          const reflex::CancelAcceptedEvent&) override {}
  void on_cancel_rejected(const reflex::Order&,
                          const reflex::CancelRejectedEvent&) override {}
  void on_executed(const reflex::Order&,
                   const reflex::ExecutedEvent&) override {}
  void on_stop() override { ++stop_count; }

  std::vector<int64_t> l1_timestamps;
  std::vector<int64_t> l2_timestamps;
  std::vector<int64_t> trade_timestamps;
  int mark_count{0};
  int funding_count{0};
  int open_interest_count{0};
  int liquidation_count{0};
  int stop_count{0};

 private:
  void record(std::string event) {
    if (delivery_order_) delivery_order_->push_back(std::move(event));
  }

  std::vector<std::string>* delivery_order_{nullptr};
};

// Sends one post-only order on the first L1 and records when the venue
// response is stamped and when it is delivered.
class OrderTimingStrategy final : public reflex::Strategy {
 public:
  OrderTimingStrategy(reflex::ClockInterface* clock,
                      reflex::AlgoOrderManagement* order_management)
      : Strategy(clock, nullptr, order_management) {
    register_as_listener();
  }

  void on_l1_update(const reflex::L1UpdateEvent&) override {
    if (sent_) return;
    sent_ = true;
    om_->send_pending(10301, reflex::Side::Buy, 1'000'000,
                      10'000'000'000, reflex::OrderType::Limit,
                      reflex::TimeInForce::Gtc, 1,
                      reflex::ExecInst::ParticipateDontInitiate);
  }
  void on_l2_update(const reflex::L2UpdateEvent&) override {}
  void on_trade(const reflex::TradeEvent&) override {}
  void on_accepted(const reflex::Order&,
                   const reflex::AcceptedEvent& event) override {
    accepted_delivery_ns = clock_->epoch_nanos();
    accepted_event_ns = event.timestamp_ns_;
  }
  void on_rejected(const reflex::Order&,
                   const reflex::RejectedEvent& event) override {
    rejected_event_ns = event.timestamp_ns_;
  }
  void on_replace_accepted(const reflex::Order&,
                           const reflex::ReplaceAcceptedEvent&) override {}
  void on_replace_rejected(const reflex::Order&,
                           const reflex::ReplaceRejectedEvent&) override {}
  void on_cancel_accepted(const reflex::Order&,
                          const reflex::CancelAcceptedEvent&) override {}
  void on_cancel_rejected(const reflex::Order&,
                          const reflex::CancelRejectedEvent&) override {}
  void on_executed(const reflex::Order&,
                   const reflex::ExecutedEvent&) override {}

  int64_t accepted_delivery_ns{0};
  int64_t accepted_event_ns{0};
  int64_t rejected_event_ns{0};

 private:
  bool sent_{false};
};

template <typename Event>
reflex::MessageSlot market_slot(int64_t timestamp_ns,
                                reflex::Exchange exchange,
                                int32_t instrument_id) {
  reflex::MessageSlot slot;
  auto* event = new (slot.raw_data()) Event();
  event->timestamp_ns_ = timestamp_ns;
  event->exchange_ = exchange;
  event->instrument_id_ = instrument_id;
  return slot;
}

std::string write_replay_file(const std::string& name,
                              const std::vector<reflex::MessageSlot>& slots) {
  const std::string path = (std::filesystem::temp_directory_path() / name).string();
  std::FILE* f = std::fopen(path.c_str(), "wb");
  EXPECT_NE(f, nullptr);
  if (!f) return {};
  reflex::FileHeader header{};  // default ctor sets magic + version
  header.message_slot_size_ = sizeof(reflex::MessageSlot);
  EXPECT_EQ(std::fwrite(&header, sizeof(header), 1, f), 1u);
  for (const auto& slot : slots) {
    EXPECT_EQ(std::fwrite(slot.raw_data(), sizeof(reflex::MessageSlot), 1, f), 1u);
  }
  std::fclose(f);
  return path;
}

TEST(BackTestEngineReplayTest,
     WindowIsInclusiveAndScheduledDeliveriesDrainPastWindowEnd) {
  const auto before = market_slot<reflex::L1UpdateEvent>(
      9, reflex::Exchange::Okx, 10301);
  const auto at_start = market_slot<reflex::L1UpdateEvent>(
      10, reflex::Exchange::Okx, 10301);
  const auto at_end = market_slot<reflex::L1UpdateEvent>(
      20, reflex::Exchange::Okx, 10301);
  const auto after = market_slot<reflex::L1UpdateEvent>(
      21, reflex::Exchange::Okx, 10301);
  const std::string file_name =
      write_replay_file("reflex_replay_window.bin", {before, at_start, at_end, after});
  ASSERT_FALSE(file_name.empty());

  reflex::backtest::BackTestEngineConfig config{};
  config.exchange_to_strategy_latency_ns_ = 5;
  config.trading_exchange_ = reflex::Exchange::Okx;
  config.trading_instrument_id_ = 10301;
  config.fail_on_provenance_mismatch_ = true;
  config.window_start_ns_ = 10;
  config.window_end_ns_ = 20;
  reflex::backtest::BackTestEngine engine(config);
  auto strategy = std::make_shared<RecordingStrategy>(engine.get_clock().get(),
                                                       nullptr);
  engine.set_strategy(strategy);
  engine.set_data_files({file_name});
  engine.run_backtest();

  EXPECT_EQ(strategy->l1_timestamps, (std::vector<int64_t>{10, 20}));
  EXPECT_EQ(strategy->stop_count, 1);
  EXPECT_EQ(engine.get_market_data_provenance_stats().accepted_execution_events,
            2u);
  EXPECT_EQ(engine.get_market_data_provenance_stats().seen(
                reflex::Exchange::Okx),
            2u);
  EXPECT_EQ(engine.get_results().simulation_end_time_ns_, 25);

  std::filesystem::remove(file_name);
}

TEST(BackTestEngineReplayTest,
     PreWindowL2PrimesExchangeWithoutReachingStrategy) {
  constexpr int64_t kWindowStartNs = 10;
  constexpr int64_t kInstrumentId = 10301;

  reflex::MessageSlot snapshot_bid;
  auto* bid = new (snapshot_bid.raw_data()) reflex::L2UpdateEvent();
  bid->timestamp_ns_ = 5;
  bid->exchange_ = reflex::Exchange::Okx;
  bid->instrument_id_ = kInstrumentId;
  bid->side_ = reflex::Side::Buy;
  bid->num_levels_ = 1;
  bid->snapshot_ = reflex::BooleanEnum::TRUE;
  bid->is_batch_message_ = reflex::BooleanEnum::TRUE;
  bid->price_1_ = 10'000'000'000;
  bid->size_1_ = 100'000'000;

  reflex::MessageSlot snapshot_ask;
  auto* ask = new (snapshot_ask.raw_data()) reflex::L2UpdateEvent();
  ask->timestamp_ns_ = 6;
  ask->exchange_ = reflex::Exchange::Okx;
  ask->instrument_id_ = kInstrumentId;
  ask->side_ = reflex::Side::Sell;
  ask->num_levels_ = 1;
  ask->snapshot_ = reflex::BooleanEnum::TRUE;
  ask->is_batch_message_ = reflex::BooleanEnum::TRUE;
  ask->is_last_batch_ = reflex::BooleanEnum::TRUE;
  ask->price_1_ = 10'020'000'000;
  ask->size_1_ = 100'000'000;

  reflex::MessageSlot l1_at_start;
  auto* l1 = new (l1_at_start.raw_data()) reflex::L1UpdateEvent();
  l1->timestamp_ns_ = kWindowStartNs;
  l1->exchange_ = reflex::Exchange::Okx;
  l1->instrument_id_ = kInstrumentId;
  l1->bid_price_ = 10'000'000'000;
  l1->bid_size_ = 100'000'000;
  l1->offer_price_ = 10'020'000'000;
  l1->offer_size_ = 100'000'000;

  const std::string file_name = write_replay_file(
      "reflex_replay_prime.bin", {snapshot_bid, snapshot_ask, l1_at_start});
  ASSERT_FALSE(file_name.empty());

  reflex::AssetInfoManager::initialize();
  reflex::backtest::BackTestEngineConfig config{};
  config.exchange_to_strategy_latency_ns_ = 0;
  config.order_entry_latency_ns_ = 0;
  config.response_latency_ns_ = 0;
  config.trading_exchange_ = reflex::Exchange::Okx;
  config.trading_instrument_id_ = kInstrumentId;
  config.fail_on_provenance_mismatch_ = true;
  config.window_start_ns_ = kWindowStartNs;

  reflex::backtest::BackTestEngine engine(config);
  auto writer =
      std::make_shared<reflex::backtest::BacktestOrderWriter>(&engine);
  auto order_management =
      std::make_shared<reflex::AlgoOrderManagement>(writer, true);
  auto strategy = std::make_shared<OrderTimingStrategy>(
      engine.get_clock().get(), order_management.get());
  auto exchange = std::make_shared<reflex::backtest::ExchangeSimulator>(
      engine.get_clock(), &engine);
  engine.set_strategy(strategy);
  engine.set_exchange_simulator(exchange);
  engine.set_data_files({file_name});

  EXPECT_NO_THROW(engine.run_backtest());
  // The pre-window L2 snapshot primed the book, so the post-only order placed
  // on the first in-window L1 is accepted (not rejected against an empty book)
  // and stamped at window start.
  EXPECT_EQ(strategy->accepted_event_ns, kWindowStartNs);
  EXPECT_EQ(strategy->rejected_event_ns, 0);
  EXPECT_EQ(engine.get_results().simulation_start_time_ns_, kWindowStartNs);
  EXPECT_EQ(engine.get_market_data_provenance_stats().accepted_execution_events,
            1u);

  std::filesystem::remove(file_name);
}

TEST(BackTestEngineReplayTest, MarketDataWinsEqualTimestampTimerTie) {
  const auto first = market_slot<reflex::L1UpdateEvent>(
      100, reflex::Exchange::Okx, 10301);
  const auto second = market_slot<reflex::L1UpdateEvent>(
      200, reflex::Exchange::Okx, 10301);
  const std::string file_name =
      write_replay_file("reflex_replay_timer_tie.bin", {first, second});
  ASSERT_FALSE(file_name.empty());

  reflex::backtest::BackTestEngineConfig config{};
  config.exchange_to_strategy_latency_ns_ = 0;
  config.trading_exchange_ = reflex::Exchange::Okx;
  config.trading_instrument_id_ = 10301;
  config.fail_on_provenance_mismatch_ = true;
  reflex::backtest::BackTestEngine engine(config);
  auto timer_manager = std::make_shared<reflex::TimerManager>();
  std::vector<std::string> delivery_order;
  timer_manager->add_timer(100, -1,
                           [&delivery_order]() {
                             delivery_order.push_back("timer:100");
                           });
  auto strategy = std::make_shared<RecordingStrategy>(
      engine.get_clock().get(), timer_manager.get(), &delivery_order);
  engine.set_timer_manager(timer_manager);
  engine.set_strategy(strategy);
  engine.set_data_files({file_name});
  engine.run_backtest();

  EXPECT_EQ(delivery_order,
            (std::vector<std::string>{"l1:100", "timer:100", "l1:200"}));
  std::filesystem::remove(file_name);
}

TEST(BackTestEngineReplayTest, RoutesEachMarketDataTypeExactlyOnce) {
  const std::string file_name = write_replay_file("reflex_replay_route.bin", {
      market_slot<reflex::L1UpdateEvent>(1, reflex::Exchange::Okx, 10301),
      market_slot<reflex::L2UpdateEvent>(2, reflex::Exchange::Okx, 10301),
      market_slot<reflex::TradeEvent>(3, reflex::Exchange::Okx, 10301),
      market_slot<reflex::MarkPriceEvent>(4, reflex::Exchange::Okx, 10301),
      market_slot<reflex::FundingRateEvent>(5, reflex::Exchange::Okx, 10301),
      market_slot<reflex::OpenInterestEvent>(6, reflex::Exchange::Okx, 10301),
      market_slot<reflex::LiquidationEvent>(7, reflex::Exchange::Okx, 10301),
  });
  ASSERT_FALSE(file_name.empty());

  reflex::backtest::BackTestEngineConfig config{};
  config.exchange_to_strategy_latency_ns_ = 0;
  config.trading_exchange_ = reflex::Exchange::Okx;
  config.trading_instrument_id_ = 10301;
  config.fail_on_provenance_mismatch_ = true;
  reflex::backtest::BackTestEngine engine(config);
  auto strategy = std::make_shared<RecordingStrategy>(engine.get_clock().get(),
                                                       nullptr);
  engine.set_strategy(strategy);
  engine.set_data_files({file_name});
  engine.run_backtest();

  EXPECT_EQ(strategy->l1_timestamps, (std::vector<int64_t>{1}));
  EXPECT_EQ(strategy->l2_timestamps, (std::vector<int64_t>{2}));
  EXPECT_EQ(strategy->trade_timestamps, (std::vector<int64_t>{3}));
  EXPECT_EQ(strategy->mark_count, 1);
  EXPECT_EQ(strategy->funding_count, 1);
  EXPECT_EQ(strategy->open_interest_count, 1);
  EXPECT_EQ(strategy->liquidation_count, 1);
  EXPECT_EQ(engine.get_market_data_provenance_stats().accepted_execution_events,
            7u);
  std::filesystem::remove(file_name);
}

TEST(BackTestEngineReplayTest,
     ReferenceVenueCanReachStrategyWithoutBecomingExecutionVenueData) {
  const std::string file_name = write_replay_file("reflex_replay_reference.bin", {
      market_slot<reflex::L1UpdateEvent>(1, reflex::Exchange::Okx, 10301),
      market_slot<reflex::L1UpdateEvent>(2,
                                        reflex::Exchange::BinanceDerivatives,
                                        10301),
  });
  ASSERT_FALSE(file_name.empty());

  reflex::backtest::BackTestEngineConfig config{};
  config.exchange_to_strategy_latency_ns_ = 0;
  config.trading_exchange_ = reflex::Exchange::Okx;
  config.trading_instrument_id_ = 10301;
  config.fail_on_provenance_mismatch_ = false;
  config.deliver_reference_market_data_to_strategy_ = true;
  reflex::backtest::BackTestEngine engine(config);
  auto strategy = std::make_shared<RecordingStrategy>(engine.get_clock().get(),
                                                       nullptr);
  engine.set_strategy(strategy);
  engine.set_data_files({file_name});
  engine.run_backtest();

  EXPECT_EQ(strategy->l1_timestamps, (std::vector<int64_t>{1, 2}));
  const auto& provenance = engine.get_market_data_provenance_stats();
  EXPECT_EQ(provenance.accepted_execution_events, 1u);
  EXPECT_EQ(provenance.rejected_exchange_events, 1u);
  EXPECT_EQ(provenance.rejected_instrument_events, 0u);
  EXPECT_EQ(provenance.delivered_reference_events, 1u);
  EXPECT_EQ(provenance.seen(reflex::Exchange::Okx), 1u);
  EXPECT_EQ(provenance.seen(reflex::Exchange::BinanceDerivatives), 1u);

  std::filesystem::remove(file_name);
}

TEST(BackTestEngineReplayTest,
     ExactReferenceVenueReachesOnlyStrategyAndPreservesStrictProvenance) {
  const std::string file_name = write_replay_file("reflex_replay_exact_ref.bin", {
      market_slot<reflex::L1UpdateEvent>(1, reflex::Exchange::Okx, 10301),
      market_slot<reflex::L1UpdateEvent>(2,
                                        reflex::Exchange::BinanceDerivatives,
                                        10201),
  });
  ASSERT_FALSE(file_name.empty());

  reflex::backtest::BackTestEngineConfig config{};
  config.exchange_to_strategy_latency_ns_ = 0;
  config.trading_exchange_ = reflex::Exchange::Okx;
  config.trading_instrument_id_ = 10301;
  config.fail_on_provenance_mismatch_ = true;
  config.deliver_reference_market_data_to_strategy_ = true;
  config.reference_exchange_ = reflex::Exchange::BinanceDerivatives;
  config.reference_instrument_id_ = 10201;
  reflex::backtest::BackTestEngine engine(config);
  auto strategy = std::make_shared<RecordingStrategy>(engine.get_clock().get(),
                                                       nullptr);
  engine.set_strategy(strategy);
  engine.set_data_files({file_name});
  engine.run_backtest();

  EXPECT_EQ(strategy->l1_timestamps, (std::vector<int64_t>{1, 2}));
  const auto& provenance = engine.get_market_data_provenance_stats();
  EXPECT_EQ(provenance.accepted_execution_events, 1u);
  EXPECT_EQ(provenance.delivered_reference_events, 1u);
  EXPECT_EQ(provenance.rejected_exchange_events, 0u);
  EXPECT_EQ(provenance.rejected_instrument_events, 0u);

  std::filesystem::remove(file_name);
}

TEST(BackTestEngineReplayTest, TimestampMergeInterleavesPairedVenueFiles) {
  const std::string execution_file = write_replay_file("reflex_replay_merge_exec.bin", {
      market_slot<reflex::L1UpdateEvent>(1, reflex::Exchange::Okx, 10301),
      market_slot<reflex::L1UpdateEvent>(3, reflex::Exchange::Okx, 10301),
  });
  const std::string reference_file = write_replay_file("reflex_replay_merge_ref.bin", {
      market_slot<reflex::L1UpdateEvent>(2,
                                        reflex::Exchange::BinanceDerivatives,
                                        10201),
  });
  ASSERT_FALSE(execution_file.empty());
  ASSERT_FALSE(reference_file.empty());

  reflex::backtest::BackTestEngineConfig config{};
  config.exchange_to_strategy_latency_ns_ = 0;
  config.trading_exchange_ = reflex::Exchange::Okx;
  config.trading_instrument_id_ = 10301;
  config.fail_on_provenance_mismatch_ = true;
  config.deliver_reference_market_data_to_strategy_ = true;
  config.merge_data_files_by_timestamp_ = true;
  config.reference_exchange_ = reflex::Exchange::BinanceDerivatives;
  config.reference_instrument_id_ = 10201;
  reflex::backtest::BackTestEngine engine(config);
  auto strategy = std::make_shared<RecordingStrategy>(engine.get_clock().get(),
                                                       nullptr);
  engine.set_strategy(strategy);
  engine.set_data_files({execution_file, reference_file});
  engine.run_backtest();

  EXPECT_EQ(strategy->l1_timestamps, (std::vector<int64_t>{1, 2, 3}));
  const auto& provenance = engine.get_market_data_provenance_stats();
  EXPECT_EQ(provenance.accepted_execution_events, 2u);
  EXPECT_EQ(provenance.delivered_reference_events, 1u);
  EXPECT_EQ(provenance.rejected_exchange_events, 0u);
  EXPECT_EQ(provenance.rejected_instrument_events, 0u);

  std::filesystem::remove(execution_file);
  std::filesystem::remove(reference_file);
}

TEST(EventRingTest, GrowthPreservesFifoOrderAcrossWrappedHead) {
  reflex::backtest::EventRing<int> ring(2);
  ring.emplace_back() = 1;
  ring.emplace_back() = 2;
  EXPECT_EQ(ring.front(), 1);
  ring.pop();
  ring.emplace_back() = 3;
  ring.emplace_back() = 4;  // grows after the head has wrapped

  ASSERT_EQ(ring.size(), 3u);
  EXPECT_EQ(ring.front(), 2);
  ring.pop();
  EXPECT_EQ(ring.front(), 3);
  ring.pop();
  EXPECT_EQ(ring.front(), 4);
  ring.pop();
  EXPECT_TRUE(ring.empty());
}

}  // namespace
