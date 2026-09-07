// tests/test_backtest_engine.cpp
//
// End-to-end BackTestEngine tests: the latency-race machinery (order/cancel in
// flight vs market prints at the exchange), the market-data monotonicity guard,
// and EOF handling for degenerate trailing capture files.

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

constexpr int32_t kInstr = 10301;  // BTC-USDT-SWAP
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
  FileHeader header{};  // default ctor sets magic + version
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

// Places a post-only buy at the bid on the FIRST L1, sends a cancel for it on
// the SECOND L1, and records every OM callback. Whether the cancel beats the
// fill is then purely a function of engine latencies vs file timestamps.
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

// Minimal strategy for tests that only exercise the data path.
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

// Full engine wiring for one race run.
struct RaceHarness {
  explicit RaceHarness(const BackTestEngineConfig& cfg, const std::vector<std::string>& files) {
    reflex::AssetInfoManager::initialize();
    engine = std::make_unique<BackTestEngine>(cfg);
    engine->set_data_files(files);
    writer = std::make_shared<BacktestOrderWriter>(engine.get());
    // keep_filled_and_dead=true so a CancelRejected arriving AFTER the full
    // fill still reaches the strategy (the order stays in the OM store).
    om = std::make_shared<AlgoOrderManagement>(writer, true);
    strategy = std::make_shared<RaceStrategy>(engine->get_clock().get(), &tm, om.get());
    engine->set_strategy(strategy);
    exchange = std::make_shared<ExchangeSimulator>(engine->get_clock(), engine.get());
    exchange->set_queue_init_fraction(0.0);  // front of queue -> prints fill us deterministically
    engine->set_exchange_simulator(exchange);
  }

  TimerManager tm;
  std::unique_ptr<BackTestEngine> engine;
  std::shared_ptr<BacktestOrderWriter> writer;
  std::shared_ptr<AlgoOrderManagement> om;
  std::shared_ptr<RaceStrategy> strategy;
  std::shared_ptr<ExchangeSimulator> exchange;
};

// Tape shared by both race tests:
//   T0        : L1 100/101  -> strategy quotes the bid (order in flight)
//   T0+2.5ms  : L1 100/101  -> strategy sends the cancel
//   T0+3.0ms  : SELL print through the bid -> would fill the resting order
std::vector<MessageSlot> race_tape() {
  return {
      l1_slot(kBase, 100.0, 101.0),
      l1_slot(kBase + 2'500'000, 100.0, 101.0),
      trade_slot(kBase + 3'000'000, Side::Sell, 99.0, 5.0),
  };
}

}  // namespace

TEST(BackTestEngineRaceTest, FillWinsWhenCancelIsStillInFlight) {
  // 1ms per leg: order rests at T0+2ms; cancel (sent T0+3.5ms) reaches the
  // exchange at T0+4.5ms — AFTER the print at T0+3ms. The fill must win and
  // the late cancel must never be honored. The venue answers OrderUnknown;
  // order management classifies that reject as the expected lifecycle race
  // (the strategy already saw the fill) and counts it instead of surfacing
  // a cancel-rejected callback for a terminal order.
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
  // 100us per leg: cancel (sent T0+2.6ms) reaches the exchange at T0+2.7ms —
  // BEFORE the print at T0+3ms. The cancel must win and no fill may occur.
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
  // Second event steps BACKWARDS in time — a silently time-warped simulation
  // is worse than a failed one, so the engine must abort loudly.
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
  // A header-only trailing chunk used to make has_more_data() promise phantom
  // data, warp the clock to -1 via the UINT64_MAX sentinel, and dereference a
  // null slot. Now it must simply end the run.
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
  EXPECT_GE(results.simulation_start_time_ns_, kBase);  // clock never warped backwards
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
