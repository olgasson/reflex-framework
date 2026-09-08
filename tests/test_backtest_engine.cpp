
#include "backtest/backtest_engine.hpp"
#include "backtest/backtest_order_writer.hpp"
#include "backtest/exchange_simulator.hpp"
#include "asset_info_manager.hpp"
#include "file_header.hpp"
#include "framework/strategy.hpp"
#include "messages.hpp"
#include "timer_manager.hpp"
#include "utils/codec_utils.hpp"

#include <gtest/gtest.h>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

using namespace reflex;
using namespace reflex::backtest;

namespace {

constexpr int32_t kInstr = 10301;
const int64_t kBase = 1'700'000'000'000'000'000LL;

MessageSlot l1_slot(int64_t ts, double bid, double ask) {
  MessageSlot slot;
  auto* e = new (slot.raw_data()) L1UpdateEvent();
  e->timestamp_ns_ = ts;
  e->instrument_id_ = kInstr;
  e->bid_price_ = CodecUtils::encode_price(bid);
  e->bid_size_ = CodecUtils::encode_quantity(5.0);
  e->offer_price_ = CodecUtils::encode_price(ask);
  e->offer_size_ = CodecUtils::encode_quantity(5.0);
  return slot;
}

MessageSlot trade_slot(int64_t ts, Side aggressor, double price, double size) {
  MessageSlot slot;
  auto* e = new (slot.raw_data()) TradeEvent();
  e->timestamp_ns_ = ts;
  e->instrument_id_ = kInstr;
  e->side_ = aggressor;
  e->price_ = CodecUtils::encode_price(price);
  e->size_ = CodecUtils::encode_quantity(size);
  return slot;
}

std::string write_tape(const std::string& name, const std::vector<MessageSlot>& slots) {
  const std::string path = (std::filesystem::temp_directory_path() / name).string();
  std::FILE* f = std::fopen(path.c_str(), "wb");
  EXPECT_NE(f, nullptr);
  FileHeader header{};
  header.message_slot_size_ = sizeof(MessageSlot);
  EXPECT_EQ(std::fwrite(&header, sizeof(header), 1, f), 1u);
  for (const auto& slot : slots) {
    EXPECT_EQ(std::fwrite(slot.raw_data(), sizeof(MessageSlot), 1, f), 1u);
  }
  std::fclose(f);
  return path;
}

std::string write_header_only(const std::string& name) {
  return write_tape(name, {});
}

class RaceStrategy final : public Strategy {
 public:
  RaceStrategy(ClockInterface* clock, TimerManager* tm, AlgoOrderManagement* om)
      : Strategy(clock, tm, om) {
    register_as_listener();
  }

  void on_l1_update(const L1UpdateEvent& e) override {
    if (e.instrument_id_ != kInstr) return;
    ++l1_count;
    if (l1_count == 1) {
      order_id = om_->send_pending(kInstr, Side::Buy, CodecUtils::encode_quantity(1.0),
                                   e.bid_price_, OrderType::Limit, TimeInForce::Gtc, 0,
                                   ExecInst::ParticipateDontInitiate);
    } else if (l1_count == 2 && order_id != 0) {
      om_->send_pending_cancel(order_id);
    }
  }
  void on_l2_update(const L2UpdateEvent&) override {}
  void on_trade(const TradeEvent&) override {}

  void on_accepted(const Order&, const AcceptedEvent&) override { ++accepted; }
  void on_rejected(const Order&, const RejectedEvent&) override { ++rejected; }
  void on_replace_accepted(const Order&, const ReplaceAcceptedEvent&) override {}
  void on_replace_rejected(const Order&, const ReplaceRejectedEvent&) override {}
  void on_cancel_accepted(const Order&, const CancelAcceptedEvent&) override { ++cancel_accepted; }
  void on_cancel_rejected(const Order&, const CancelRejectedEvent&) override { ++cancel_rejected; }
  void on_executed(const Order&, const ExecutedEvent& e) override { filled += e.last_quantity_; }

  int64_t order_id = 0;
  int l1_count = 0;
  int accepted = 0;
  int rejected = 0;
  int cancel_accepted = 0;
  int cancel_rejected = 0;
  int64_t filled = 0;
};

class SinkStrategy final : public Strategy {
 public:
  SinkStrategy(ClockInterface* clock, TimerManager* tm) : Strategy(clock, tm, nullptr) {}
  void on_l1_update(const L1UpdateEvent&) override { ++l1_count; }
  void on_l2_update(const L2UpdateEvent&) override {}
  void on_trade(const TradeEvent&) override { ++trade_count; }
  void on_accepted(const Order&, const AcceptedEvent&) override {}
  void on_rejected(const Order&, const RejectedEvent&) override {}
  void on_replace_accepted(const Order&, const ReplaceAcceptedEvent&) override {}
  void on_replace_rejected(const Order&, const ReplaceRejectedEvent&) override {}
  void on_cancel_accepted(const Order&, const CancelAcceptedEvent&) override {}
  void on_cancel_rejected(const Order&, const CancelRejectedEvent&) override {}
  void on_executed(const Order&, const ExecutedEvent&) override {}
  int l1_count = 0;
  int trade_count = 0;
};

struct RaceHarness {
  explicit RaceHarness(const BackTestEngineConfig& cfg, const std::vector<std::string>& files) {
    reflex::AssetInfoManager::initialize();
    engine = std::make_unique<BackTestEngine>(cfg);
    engine->set_data_files(files);
    writer = std::make_shared<BacktestOrderWriter>(engine.get());
    om = std::make_shared<AlgoOrderManagement>(writer, true);
    strategy = std::make_shared<RaceStrategy>(engine->get_clock().get(), &tm, om.get());
    engine->set_strategy(strategy);
    exchange = std::make_shared<ExchangeSimulator>(engine->get_clock(), engine.get());
    exchange->set_queue_init_fraction(0.0);
    engine->set_exchange_simulator(exchange);
  }

  TimerManager tm;
  std::unique_ptr<BackTestEngine> engine;
  std::shared_ptr<BacktestOrderWriter> writer;
  std::shared_ptr<AlgoOrderManagement> om;
  std::shared_ptr<RaceStrategy> strategy;
  std::shared_ptr<ExchangeSimulator> exchange;
};

std::vector<MessageSlot> race_tape() {
  return {
      l1_slot(kBase, 100.0, 101.0),
      l1_slot(kBase + 2'500'000, 100.0, 101.0),
      trade_slot(kBase + 3'000'000, Side::Sell, 99.0, 5.0),
  };
}

}

TEST(BackTestEngineRaceTest, FillWinsWhenCancelIsStillInFlight) {
  BackTestEngineConfig cfg;
  cfg.strategy_to_exchange_latency_ns_ = 1'000'000;
  cfg.exchange_to_strategy_latency_ns_ = 1'000'000;
  const auto tape = write_tape("reflex_race_fill_wins.bin", race_tape());

  RaceHarness h(cfg, {tape});
  h.engine->run_backtest();

  EXPECT_EQ(h.strategy->accepted, 1);
  EXPECT_EQ(h.strategy->filled, CodecUtils::encode_quantity(1.0));
  EXPECT_EQ(h.strategy->cancel_accepted, 0);
  EXPECT_EQ(h.strategy->cancel_rejected, 0);
  const auto& diag = h.om->request_correlation_diagnostics();
  EXPECT_EQ(diag.terminal_fill_order_unknown_cancel_rejects, 1u);
  EXPECT_EQ(diag.uncorrelated_cancel_responses, 0u);
  EXPECT_EQ(diag.stale_cancel_responses, 0u);
}

TEST(BackTestEngineRaceTest, CancelWinsWhenItReachesTheExchangeFirst) {
  BackTestEngineConfig cfg;
  cfg.strategy_to_exchange_latency_ns_ = 100'000;
  cfg.exchange_to_strategy_latency_ns_ = 100'000;
  const auto tape = write_tape("reflex_race_cancel_wins.bin", race_tape());

  RaceHarness h(cfg, {tape});
  h.engine->run_backtest();

  EXPECT_EQ(h.strategy->accepted, 1);
  EXPECT_EQ(h.strategy->filled, 0);
  EXPECT_EQ(h.strategy->cancel_accepted, 1);
  EXPECT_EQ(h.strategy->cancel_rejected, 0);
}

TEST(BackTestEngineDataTest, OutOfOrderCaptureFileAbortsTheRun) {
  reflex::AssetInfoManager::initialize();
  const auto tape = write_tape("reflex_out_of_order.bin",
                               {l1_slot(kBase + 1'000'000, 100.0, 101.0),
                                l1_slot(kBase, 100.0, 101.0)});

  auto engine = std::make_unique<BackTestEngine>(BackTestEngineConfig{});
  engine->set_data_files({tape});
  TimerManager tm;
  auto strategy = std::make_shared<SinkStrategy>(engine->get_clock().get(), &tm);
  engine->set_strategy(strategy);

  EXPECT_THROW(engine->run_backtest(), std::runtime_error);
}

TEST(BackTestEngineDataTest, TrailingHeaderOnlyFileEndsRunCleanly) {
  reflex::AssetInfoManager::initialize();
  const auto tape = write_tape("reflex_valid_head.bin",
                               {l1_slot(kBase, 100.0, 101.0),
                                trade_slot(kBase + 100'000, Side::Sell, 99.0, 1.0),
                                l1_slot(kBase + 200'000, 100.0, 101.0)});
  const auto empty = write_header_only("reflex_header_only_tail.bin");

  auto engine = std::make_unique<BackTestEngine>(BackTestEngineConfig{});
  engine->set_data_files({tape, empty});
  TimerManager tm;
  auto strategy = std::make_shared<SinkStrategy>(engine->get_clock().get(), &tm);
  engine->set_strategy(strategy);

  ASSERT_NO_THROW(engine->run_backtest());
  EXPECT_EQ(strategy->l1_count, 2);
  EXPECT_EQ(strategy->trade_count, 1);
  const auto& results = engine->get_results();
  EXPECT_GE(results.simulation_end_time_ns_, results.simulation_start_time_ns_);
  EXPECT_GE(results.simulation_start_time_ns_, kBase);
}

TEST(BackTestEngineDataTest, TrailingUnreadableFileEndsRunCleanly) {
  reflex::AssetInfoManager::initialize();
  const auto tape = write_tape("reflex_valid_head2.bin",
                               {l1_slot(kBase, 100.0, 101.0),
                                l1_slot(kBase + 100'000, 100.0, 101.0)});
  const std::string missing =
      (std::filesystem::temp_directory_path() / "reflex_does_not_exist.bin").string();

  auto engine = std::make_unique<BackTestEngine>(BackTestEngineConfig{});
  engine->set_data_files({tape, missing});
  TimerManager tm;
  auto strategy = std::make_shared<SinkStrategy>(engine->get_clock().get(), &tm);
  engine->set_strategy(strategy);

  ASSERT_NO_THROW(engine->run_backtest());
  EXPECT_EQ(strategy->l1_count, 2);
  EXPECT_GE(engine->get_results().simulation_start_time_ns_, kBase);
}
