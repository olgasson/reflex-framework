// examples/example_backtest_main.cpp
//
// End-to-end demo of the backtest stack:
//
//   synthetic market data -> BackTestEngine (latency queues)
//                         -> SimpleQuoterStrategy (examples/example_strategy.hpp)
//                         -> ExchangeSimulator (queue-position fill model)
//                         -> RiskEngine (FIFO PnL)
//
// With no arguments it generates a mean-reverting synthetic tape and runs the
// quoter against it, so the repo has a runnable demo without any data files.
// Pass one or more capture files (see backtest/splitter_main.cpp) to run on
// real recorded market data instead:
//
//   ./example_backtest [capture_file.bin ...]
//
// Without capture-file arguments it generates and runs against a synthetic
// random-walk tape. Note that synthetic flow carries no information, so
// results on it say nothing about how a strategy fares on real data.

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include "asset_info_manager.hpp"
#include "backtest/backtest_engine.hpp"
#include "backtest/backtest_order_writer.hpp"
#include "backtest/exchange_simulator.hpp"
#include "example_strategy.hpp"
#include "file_header.hpp"
#include "messages.hpp"
#include "timer_manager.hpp"

using namespace reflex;
using namespace reflex::backtest;

namespace {

constexpr int32_t kInstrumentId = 10301;  // BTC-USDT-SWAP
constexpr int64_t kTick = 10'000'000;     // 0.1 USD in fixed-point 1e8

// Write a synthetic capture: a mean-reverting tick-grid mid with L1 quotes,
// a small L2 depth snapshot whenever the touch moves, and trade prints hitting
// alternating sides. ~10 events per millisecond. The depth matters: the
// simulator refuses to rest an order behind the touch on an L1-only book,
// because it could not know how much queue is ahead of it.
std::string write_synthetic_tape(uint64_t num_steps) {
  const std::string path =
      (std::filesystem::temp_directory_path() / "reflex_example_tape.bin").string();
  std::FILE* f = std::fopen(path.c_str(), "wb");
  if (!f) {
    std::perror("fopen");
    std::exit(1);
  }

  const auto write_or_die = [f, &path](const void* data, size_t size) {
    if (std::fwrite(data, size, 1, f) != 1) {
      std::fprintf(stderr, "Failed to write synthetic tape: %s\n", path.c_str());
      std::fclose(f);
      std::exit(1);
    }
  };

  FileHeader header{};
  header.magic_number_ = 0x52454658;  // "REFX"
  header.version_ = 1;
  header.message_slot_size_ = sizeof(MessageSlot);
  write_or_die(&header, sizeof(header));

  std::mt19937_64 rng(42);
  std::uniform_int_distribution<int> coin(0, 99);
  std::uniform_int_distribution<int64_t> trade_qty(2, 8);

  const int64_t base_ts = 1'700'000'000'000'000'000LL;
  const int64_t anchor = 50'000'00000000LL;  // 50,000.0 USD
  int64_t bid = anchor;

  MessageSlot slot;
  constexpr int kDepthLevels = 6;  // per side, two levels per L2 message
  // Snapshot both sides of the book as a batch of L2 messages: touch shows
  // 5 contracts, every deeper level 10.
  const auto write_depth_snapshot = [&](int64_t ts, int64_t bid_px, int64_t ask_px) {
    for (int side = 0; side < 2; ++side) {
      for (int lvl = 0; lvl < kDepthLevels; lvl += 2) {
        auto* e = new (slot.raw_data()) L2UpdateEvent();
        e->timestamp_ns_ = ts;
        e->instrument_id_ = kInstrumentId;
        e->side_ = (side == 0) ? Side::Buy : Side::Sell;
        e->num_levels_ = 2;
        const int64_t dir = (side == 0) ? -1 : 1;
        const int64_t base = (side == 0) ? bid_px : ask_px;
        e->price_1_ = base + dir * kTick * lvl;
        e->size_1_ = (lvl == 0) ? 5'00000000 : 10'00000000;
        e->price_2_ = base + dir * kTick * (lvl + 1);
        e->size_2_ = 10'00000000;
        e->snapshot_ = BooleanEnum::TRUE;
        e->is_batch_message_ = BooleanEnum::TRUE;
        e->is_last_batch_ =
            (side == 1 && lvl + 2 >= kDepthLevels) ? BooleanEnum::TRUE : BooleanEnum::FALSE;
        write_or_die(slot.raw_data(), sizeof(MessageSlot));
      }
    }
  };

  for (uint64_t i = 0; i < num_steps; ++i) {
    const int64_t ts = base_ts + static_cast<int64_t>(i) * 100'000;  // 100us apart

    // Mean-reverting random walk on the tick grid.
    const int p = coin(rng);
    const int64_t drift = (bid > anchor) ? -1 : (bid < anchor ? 1 : 0);
    const int64_t prev_bid = bid;
    if (p < 20) bid += kTick * ((p < 10) ? 1 : -1);
    else if (p < 25) bid += kTick * drift;
    const int64_t ask = bid + kTick;

    if (i == 0 || bid != prev_bid) write_depth_snapshot(ts, bid, ask);

    {
      auto* e = new (slot.raw_data()) L1UpdateEvent();
      e->timestamp_ns_ = ts;
      e->instrument_id_ = kInstrumentId;
      e->bid_price_ = bid;
      e->bid_size_ = 5'00000000;  // 5 contracts displayed
      e->offer_price_ = ask;
      e->offer_size_ = 5'00000000;
      write_or_die(slot.raw_data(), sizeof(MessageSlot));
    }

    // Every other step, a trade print hits one side of the book.
    if (i % 2 == 1) {
      auto* e = new (slot.raw_data()) TradeEvent();
      e->timestamp_ns_ = ts + 50'000;
      e->instrument_id_ = kInstrumentId;
      const bool sell_aggressor = (i % 4 == 1);
      e->side_ = sell_aggressor ? Side::Sell : Side::Buy;
      e->price_ = sell_aggressor ? bid : ask;
      e->size_ = trade_qty(rng) * 1'00000000;
      write_or_die(slot.raw_data(), sizeof(MessageSlot));
    }
  }
  if (std::fclose(f) != 0) {
    std::perror("fclose");
    std::exit(1);
  }
  return path;
}

}  // namespace

int main(int argc, char** argv) {
  reflex::AssetInfoManager::initialize();

  std::vector<std::string> data_files;
  const bool synthetic = (argc <= 1);
  if (argc > 1) {
    for (int i = 1; i < argc; ++i) data_files.emplace_back(argv[i]);
    std::printf("Running on %zu capture file(s)\n", data_files.size());
  } else {
    const uint64_t steps = 2'000'000;
    std::printf("No capture files given - generating %llu-step synthetic tape...\n",
                static_cast<unsigned long long>(steps));
    data_files.push_back(write_synthetic_tape(steps));
  }

  // --- Wiring: engine + strategy + exchange simulator -----------------------
  BackTestEngineConfig engine_cfg;  // default: 3ms one-way latencies
  auto engine = std::make_unique<BackTestEngine>(engine_cfg);
  engine->set_data_files(data_files);

  auto clock = engine->get_clock();
  TimerManager timer_manager;
  auto writer = std::make_shared<BacktestOrderWriter>(engine.get());
  auto om = std::make_shared<AlgoOrderManagement>(writer, false);

  examples::SimpleQuoterConfig cfg;
  cfg.instrument_id = kInstrumentId;
  auto strategy = std::make_shared<examples::SimpleQuoterStrategy>(
      clock.get(), &timer_manager, om.get(), cfg);
  engine->set_strategy(strategy);

  auto exchange = std::make_shared<ExchangeSimulator>(clock, engine.get());
  exchange->set_queue_model(ExchangeSimulator::QueueModel::Pessimistic);
  // The L1 stream is the real-time one on most venues (depth is sampled), so
  // let touch observations drive queue accounting too.
  exchange->set_queue_accounting_on_l1(true);
  engine->set_exchange_simulator(exchange);

  const auto t0 = std::chrono::steady_clock::now();
  engine->run_backtest();
  const auto t1 = std::chrono::steady_clock::now();
  const double sec = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count() / 1000.0;

  // --- Results ---------------------------------------------------------------
  const auto snap = strategy->risk_engine().snapshot();
  const double total = snap.realized_pnl_quote + snap.unrealized_pnl_quote - snap.fees_quote;
  std::printf("\n--- RESULTS (%.1fs wall) ---\n", sec);
  std::printf("Fills:          %llu\n", static_cast<unsigned long long>(strategy->fills()));
  std::printf("Position:       %.4f contracts\n", strategy->position_contracts());
  std::printf("Realized PnL:   %+.2f USD\n", snap.realized_pnl_quote);
  std::printf("Unrealized PnL: %+.2f USD\n", snap.unrealized_pnl_quote);
  std::printf("Fees:           %.2f USD\n", snap.fees_quote);
  std::printf("Total PnL:      %+.2f USD\n", total);
  if (synthetic) {
    std::printf("\nNote: this run used a synthetic random-walk tape; results on it are not\n"
                "indicative of performance on real recorded data.\n");
  }
  return 0;
}
