// perf/backtest_engine_bench.cpp
//
// Micro-benchmark for the BackTestEngine data path (file read -> latency queues
// -> strategy delivery). Uses a no-op strategy so we measure the engine
// plumbing rather than strategy/model cost, over synthetic data shaped to keep
// a realistic ~3ms backlog resident in the market-data queue.
//
// The order-sensitive checksum is a determinism guard: any change that altered
// delivery order/content would change it. Use it to prove B-1..B-4 are
// behavior-neutral (identical checksum before vs after).
//
// Usage: backtest_engine_bench [num_events] [repeats]

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

#include "asset_info_manager.hpp"
#include "backtest/backtest_engine.hpp"
#include "file_header.hpp"
#include "messages.hpp"
#include "framework/strategy.hpp"
#include "timer_manager.hpp"

using namespace reflex;
using namespace reflex::backtest;

namespace {

// No-op strategy: folds each delivered event into an order-sensitive checksum.
class NoopStrategy final : public Strategy {
 public:
  NoopStrategy(ClockInterface* clock, TimerManager* tm) : Strategy(clock, tm, nullptr) {}

  uint64_t checksum = 1469598103934665603ULL;  // FNV-1a offset basis
  uint64_t count = 0;

  inline void fold(uint64_t v) {
    checksum = (checksum ^ v) * 1099511628211ULL;  // FNV-1a step (order-sensitive)
    ++count;
  }

  void on_l1_update(const L1UpdateEvent& e) override {
    fold(static_cast<uint64_t>(e.timestamp_ns_));
    fold(static_cast<uint64_t>(e.bid_price_) ^ static_cast<uint64_t>(e.offer_price_));
  }
  void on_l2_update(const L2UpdateEvent& e) override {
    fold(static_cast<uint64_t>(e.timestamp_ns_));
    fold(static_cast<uint64_t>(e.price_1_));
  }
  void on_trade(const TradeEvent& e) override {
    fold(static_cast<uint64_t>(e.timestamp_ns_));
    fold(static_cast<uint64_t>(e.price_));
  }

  // OM callbacks never fire (the no-op strategy sends no orders).
  void on_accepted(const Order&, const AcceptedEvent&) override {}
  void on_rejected(const Order&, const RejectedEvent&) override {}
  void on_replace_accepted(const Order&, const ReplaceAcceptedEvent&) override {}
  void on_replace_rejected(const Order&, const ReplaceRejectedEvent&) override {}
  void on_cancel_accepted(const Order&, const CancelAcceptedEvent&) override {}
  void on_cancel_rejected(const Order&, const CancelRejectedEvent&) override {}
  void on_executed(const Order&, const ExecutedEvent&) override {}
};

// Write a synthetic REFX file: num_events MessageSlots (cycling L1/L2/trade)
// with timestamps spaced 1us apart, so at 3ms latency ~3000 events stay resident
// in the market-data queue.
std::string write_synthetic_file(uint64_t num_events) {
  std::string path = "/tmp/reflex_engine_bench.bin";
  std::FILE* f = std::fopen(path.c_str(), "wb");
  if (!f) {
    std::perror("fopen");
    std::exit(1);
  }

  reflex::FileHeader header{};
  header.magic_number_ = 0x52454658;  // "REFX"
  header.version_ = 1;
  header.message_slot_size_ = sizeof(MessageSlot);
  std::fwrite(&header, sizeof(header), 1, f);

  const int64_t base_ts = 1'700'000'000'000'000'000LL;  // ns
  const int32_t instrument_id = 10301;                  // BTC-USDT-SWAP

  MessageSlot slot;
  for (uint64_t i = 0; i < num_events; ++i) {
    const int64_t ts = base_ts + static_cast<int64_t>(i) * 1000;  // 1us apart
    switch (i % 3) {
      case 0: {
        auto* e = new (slot.raw_data()) L1UpdateEvent();
        e->timestamp_ns_ = ts;
        e->instrument_id_ = instrument_id;
        e->bid_price_ = 50'000'00000000LL + static_cast<int64_t>(i % 100);
        e->bid_size_ = 10;
        e->offer_price_ = 50'000'00000001LL + static_cast<int64_t>(i % 100);
        e->offer_size_ = 12;
        break;
      }
      case 1: {
        auto* e = new (slot.raw_data()) L2UpdateEvent();
        e->timestamp_ns_ = ts;
        e->instrument_id_ = instrument_id;
        e->side_ = (i & 1) ? Side::Buy : Side::Sell;
        e->num_levels_ = 1;
        e->price_1_ = 50'000'00000000LL + static_cast<int64_t>(i % 200);
        e->size_1_ = 7;
        break;
      }
      default: {
        auto* e = new (slot.raw_data()) TradeEvent();
        e->timestamp_ns_ = ts;
        e->instrument_id_ = instrument_id;
        e->side_ = (i & 1) ? Side::Buy : Side::Sell;
        e->price_ = 50'000'00000000LL + static_cast<int64_t>(i % 50);
        e->size_ = 3;
        break;
      }
    }
    std::fwrite(slot.raw_data(), sizeof(MessageSlot), 1, f);
  }
  std::fclose(f);
  return path;
}

}  // namespace

int main(int argc, char** argv) {
  AssetInfoManager::initialize();

  const uint64_t num_events = (argc > 1) ? std::strtoull(argv[1], nullptr, 10) : 2'000'000ULL;
  const int repeats = (argc > 2) ? std::atoi(argv[2]) : 7;

  std::printf("Generating %llu synthetic events...\n", static_cast<unsigned long long>(num_events));
  const std::string file = write_synthetic_file(num_events);

  double best_ms = 1e18;
  uint64_t last_checksum = 0;
  uint64_t last_count = 0;

  for (int r = 0; r < repeats; ++r) {
    BackTestEngineConfig cfg;  // default 3ms latencies
    auto engine = std::make_unique<BackTestEngine>(cfg);
    TimerManager tm;
    auto strat = std::make_shared<NoopStrategy>(engine->get_clock().get(), &tm);
    engine->set_data_files({file});
    engine->set_strategy(strat);

    const auto t0 = std::chrono::steady_clock::now();
    engine->run_backtest();
    const auto t1 = std::chrono::steady_clock::now();

    const double ms = std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count() / 1e6;
    if (ms < best_ms) best_ms = ms;
    last_checksum = strat->checksum;
    last_count = strat->count;

    std::printf("  run %d: %8.2f ms  (%.1f M events/s)\n", r, ms,
                static_cast<double>(strat->count) / (ms * 1000.0));
  }

  std::printf("\nBEST: %.2f ms over %d runs | delivered=%llu | checksum=0x%016llx\n", best_ms, repeats,
              static_cast<unsigned long long>(last_count), static_cast<unsigned long long>(last_checksum));
  return 0;
}
