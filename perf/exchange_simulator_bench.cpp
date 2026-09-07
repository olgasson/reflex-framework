// perf/exchange_simulator_bench.cpp
//
// Micro-benchmark for the ExchangeSimulator order path (the resting-order book:
// place / cancel / replace / trade-fill). Drives a realistic churn of post-only
// quotes resting near the touch, with a rolling window so a steady population of
// orders stays resting across several price levels.
//
// The response-stream checksum is a determinism guard: any change that altered
// accepts/rejects/fills/cancels would change it. Use it to confirm an S-4
// refactor is behavior-neutral (identical checksum before vs after).
//
// Usage: exchange_simulator_bench [num_iters] [repeats]

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <vector>

#include "asset_info_manager.hpp"
#include "backtest/exchange_response_handler.hpp"
#include "backtest/exchange_simulator.hpp"
#include "messages.hpp"
#include "offset_epoch_nano_clock.hpp"

using namespace reflex;
using namespace reflex::backtest;

namespace {

class CountingHandler final : public ExchangeResponseHandler {
 public:
  uint64_t checksum = 1469598103934665603ULL;  // FNV-1a offset basis
  uint64_t accepts = 0, rejects = 0, fills = 0, cancels = 0, replaces = 0, other = 0;

  void on_exchange_response(const MessageSlot& slot) override {
    const auto type = slot.get_type();
    fold(static_cast<uint64_t>(type));
    switch (type) {
      case MessageType::Accepted:
        fold(static_cast<uint64_t>(slot.as<AcceptedEvent>().order_id_));
        ++accepts;
        break;
      case MessageType::Rejected:
        fold(static_cast<uint64_t>(slot.as<RejectedEvent>().order_id_));
        ++rejects;
        break;
      case MessageType::Executed: {
        const auto& e = slot.as<ExecutedEvent>();
        fold(static_cast<uint64_t>(e.order_id_));
        fold(static_cast<uint64_t>(e.last_price_) ^ static_cast<uint64_t>(e.last_quantity_));
        ++fills;
        break;
      }
      case MessageType::CancelAccepted:
        fold(static_cast<uint64_t>(slot.as<CancelAcceptedEvent>().order_id_));
        ++cancels;
        break;
      case MessageType::CancelRejected:
        fold(static_cast<uint64_t>(slot.as<CancelRejectedEvent>().order_id_));
        ++cancels;
        break;
      case MessageType::ReplaceAccepted:
        fold(static_cast<uint64_t>(slot.as<ReplaceAcceptedEvent>().order_id_));
        ++replaces;
        break;
      case MessageType::ReplaceRejected:
        fold(static_cast<uint64_t>(slot.as<ReplaceRejectedEvent>().order_id_));
        ++replaces;
        break;
      default:
        ++other;
        break;
    }
  }

 private:
  void fold(uint64_t v) { checksum = (checksum ^ v) * 1099511628211ULL; }
};

constexpr int32_t kInstr = 10301;        // BTC-USDT-SWAP
constexpr int64_t kTick = 10'000'000;                     // 0.1 USD in fixed-point 1e8
constexpr int64_t kBestBid = 50'000'00000000LL;            // 50,000.0 USD, on the tick grid
constexpr int64_t kBestAsk = kBestBid + 100 * kTick;       // wide spread so post-only never crosses
constexpr int64_t kLevelSize = 50;
constexpr int kBookLevels = 20;

void feed_l2_level(ExchangeSimulator& sim, int64_t ts, Side side, int64_t price, int64_t size, bool last) {
  L2UpdateEvent e;
  e.timestamp_ns_ = ts;
  e.instrument_id_ = kInstr;
  e.side_ = side;
  e.num_levels_ = 1;
  e.price_1_ = price;
  e.size_1_ = size;
  e.snapshot_ = BooleanEnum::TRUE;
  e.is_batch_message_ = BooleanEnum::TRUE;
  e.is_last_batch_ = last ? BooleanEnum::TRUE : BooleanEnum::FALSE;
  sim.process_l2_update(e);
}

void build_book(ExchangeSimulator& sim, int64_t ts) {
  for (int i = 0; i < kBookLevels; ++i) {
    feed_l2_level(sim, ts, Side::Buy, kBestBid - i * kTick, kLevelSize, false);
  }
  for (int i = 0; i < kBookLevels; ++i) {
    feed_l2_level(sim, ts, Side::Sell, kBestAsk + i * kTick, kLevelSize, i == kBookLevels - 1);
  }
}

double run_once(uint64_t iters, CountingHandler& handler) {
  auto clock = std::make_shared<SimulationClock>();
  ExchangeSimulator sim(clock, &handler);

  int64_t ts = 1'700'000'000'000'000'000LL;
  clock->set_time(ts);
  build_book(sim, ts);

  constexpr int W = 256;  // thick-level config to exercise the trade-fill path
  std::vector<int64_t> live(W, 0);
  int64_t next_id = 1;

  const auto t0 = std::chrono::steady_clock::now();
  for (uint64_t i = 0; i < iters; ++i) {
    ts += 1000;
    clock->set_time(ts);
    const int slot = static_cast<int>(i % W);

    // Cancel whatever currently occupies this slot (rolls the window).
    if (live[slot] != 0) {
      PendingCancelEvent c;
      c.order_id_ = live[slot];
      c.timestamp_ns_ = ts;
      sim.on_pending_cancel(c);
      live[slot] = 0;
    }

    // Occasionally aggress with a trade to fill resting orders.
    if ((i & 15u) == 0) {
      TradeEvent tr;
      tr.instrument_id_ = kInstr;
      tr.timestamp_ns_ = ts;
      tr.side_ = (i & 16u) ? Side::Buy : Side::Sell;
      tr.price_ = (tr.side_ == Side::Buy) ? kBestAsk : kBestBid;
      tr.size_ = 1000;
      sim.process_trade_event(tr);
    }

    // Place a fresh post-only quote across a few levels near the touch.
    const int64_t id = next_id++;
    const Side side = (i & 1u) ? Side::Buy : Side::Sell;
    const int64_t px =
        (side == Side::Buy) ? kBestBid - static_cast<int64_t>(i % 4) * kTick
                            : kBestAsk + static_cast<int64_t>(i % 4) * kTick;
    PendingEvent pe;
    pe.side_ = side;
    pe.order_type_ = OrderType::Limit;
    pe.exec_inst_ = ExecInst::ParticipateDontInitiate;  // post-only -> rests
    pe.order_id_ = id;
    pe.price_ = px;
    pe.quantity_ = 5;
    pe.instrument_id_ = kInstr;
    pe.timestamp_ns_ = ts;
    sim.on_pending(pe);
    live[slot] = id;

    // Occasionally re-price the just-placed order.
    if ((i & 7u) == 0) {
      PendingReplaceEvent re;
      re.order_id_ = id;
      re.instrument_id_ = kInstr;
      re.price_ = px + ((side == Side::Buy) ? -kTick : kTick);
      re.quantity_ = 6;
      re.timestamp_ns_ = ts;
      sim.on_pending_replace(re);
    }
  }
  const auto t1 = std::chrono::steady_clock::now();
  return std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count() / 1e6;
}

}  // namespace

int main(int argc, char** argv) {
  reflex::AssetInfoManager::initialize();

  const uint64_t iters = (argc > 1) ? std::strtoull(argv[1], nullptr, 10) : 2'000'000ULL;
  const int repeats = (argc > 2) ? std::atoi(argv[2]) : 7;

  std::printf("ExchangeSimulator order-path bench: %llu iters x %d repeats\n",
              static_cast<unsigned long long>(iters), repeats);

  double best_ms = 1e18;
  CountingHandler last{};
  for (int r = 0; r < repeats; ++r) {
    CountingHandler handler{};
    const double ms = run_once(iters, handler);
    if (ms < best_ms) best_ms = ms;
    last = handler;
    std::printf("  run %d: %8.2f ms  (%.2f M ops/s)\n", r, ms,
                static_cast<double>(iters) / (ms * 1000.0));
  }

  std::printf(
      "\nBEST: %.2f ms | accepts=%llu rejects=%llu fills=%llu cancels=%llu replaces=%llu | checksum=0x%016llx\n",
      best_ms, static_cast<unsigned long long>(last.accepts), static_cast<unsigned long long>(last.rejects),
      static_cast<unsigned long long>(last.fills), static_cast<unsigned long long>(last.cancels),
      static_cast<unsigned long long>(last.replaces), static_cast<unsigned long long>(last.checksum));
  return 0;
}
