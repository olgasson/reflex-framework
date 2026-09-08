# reflex-framework

A C++17 framework for recording crypto market data and backtesting
market-making strategies against the recordings. Built around OKX perpetual
swaps; the exchange-facing pieces are isolated so other venues can be added.

It has two halves:

- **Capture**: a websocket ingest component subscribes to OKX public channels
  (L1/L2 books, trades, mark price, funding, open interest) and writes the
  event stream to disk as fixed-size binary records.
- **Backtest**: an event-driven engine replays capture files through an
  exchange simulator, with strategies written against small,
  exchange-agnostic interfaces.

## Exchange simulator

The simulator models the mechanics of resting a passive order on a central
limit order book:

- **Queue position**: a resting order joins behind the depth displayed at its
  price (scaled by `queue_init_fraction`); trade prints drain the queue ahead
  of it before it can fill.
- **Trade-size-budgeted fills**: a print fills at most its traded size, shared
  across the price levels and orders it sweeps.
- **Latency modeling**: strategy→exchange and exchange→strategy delays are
  simulated with timestamp-ordered event queues, so an in-flight cancel and an
  incoming fill race each other.
- **Pessimistic / optimistic queue models**: two conventions for how cancels
  ahead of you are treated, since that is not observable from L2 data. The
  optimistic model is bounded by displayed depth (you can never be behind
  more than the level shows), with an optional, calibratable cancellation
  credit (`set_cancel_credit_alpha`, off by default) and a trade-netting
  window so a print is never counted twice against the queue.
- **L1-driven queue accounting** (`set_queue_accounting_on_l1`): the
  top-of-book stream is usually real-time while depth diffs are sampled, so
  touch observations can drive queue updates too. Resting behind the touch
  requires L2 provenance; the simulator refuses to fabricate queue position
  from an L1-only tape.
- Post-only, limit, and IOC orders, replaces with total-quantity (fill-aware)
  semantics, tick-grid validation, request-correlated cancel/replace
  responses, and rejects are simulated; fills feed FIFO position/PnL
  accounting with maker/taker fees expressed in parts-per-billion (negative
  maker fee = rebate).

## Architecture

Components communicate over a single-producer ring buffer (disruptor pattern)
carrying fixed 64-byte message slots; event layouts are pinned with
`static_assert`.

```
 capture:   OKX WS ──> OkxBlob ──> ring buffer ──> BinaryWriter ──> capture files
 backtest:  capture files ──> BackTestEngine (latency queues) ──> Strategy
                                    ▲                                │
                                    └── ExchangeSimulator <── AlgoOrderManagement
                                                                     │
                                                                     ▼
                                                           RiskEngine (FIFO PnL)
```

Strategies implement the `Strategy` interface (market-data and
order-management callbacks) and act through `AlgoOrderManagement`
(place / replace / cancel) and a `ClockInterface` that is the simulation clock
in backtests.

| Component | What it does |
|---|---|
| `include/framework/` | ring-buffer reader/writer, component/agent runtime, `Strategy` interface |
| `include/messages.hpp` | all events as 64-byte aligned structs (fixed-point 1e8 prices) |
| `core/incremental_order_book.cpp` | L1/L2/trade-replay order book |
| `backtest/exchange_simulator.cpp` | queue-position fill model |
| `backtest/backtest_engine.cpp` | file replay + latency queues |
| `components/blob/` | OKX public websocket ingest |
| `components/data_offload/` | binary capture writer |
| `include/risk/risk_engine.hpp` | FIFO position/PnL accounting with maker/taker fees |

## Quick start

Requires CMake ≥ 3.24, a C++17 compiler, and OpenSSL. All other dependencies
(spdlog, yyjson, libwebsockets, disruptorplus, googletest) are fetched by CMake.

```bash
cmake -S . -B build
cmake --build build -j

# Run the demo: a simple top-of-book quoter against a synthetic tape
./build/example_backtest

# Run the tests
./build/reflex_tests
```

The example strategy (`examples/example_strategy.hpp`) shows the API surface:
quoting, re-pricing, inventory caps, fill and reject handling, and PnL
tracking.

## Running on real data

1. Capture market data: `blob_and_writer_launcher` subscribes to OKX public
   channels and writes binary capture files (set `REFLEX_OUTPUT_PATH`).
   `REFLEX_OKX_BOOK_CHANNEL` selects `books` (default), `books-l2-tbt`, or
   `books50-l2-tbt`. Collectors idle with a backoff strategy (near-zero CPU
   when quiet); set `REFLEX_IDLE=spin` for busy-polling.
2. Split multi-instrument captures into per-instrument chunks:
   `./build/splitter_main -o ./split_by_asset capture*.bin`
3. Backtest against them: `./build/example_backtest split_by_asset/<inst>/*.bin`

## Build options

| Option | Default | Effect |
|---|---|---|
| `REFLEX_NATIVE_ARCH` | `ON` | `-march=native` on first-party targets |
| `REFLEX_ENABLE_SANITIZERS` | `OFF` | ASan + UBSan build (disables LTO) |
| `REFLEX_BUILD_DISRUPTOR_BENCHMARK` | `OFF` | Disruptor-cpp comparison bench (needs Boost) |

## Scope and caveats

- The repo covers market-data capture and backtesting. Live order execution
  is not included.
- Funding-rate accrual is not modeled in the simulator — relevant if you hold
  perpetual-swap inventory across funding windows.
- The simulator approximates what cannot be observed from public data (queue
  position, cancel ordering); treat its output accordingly.
- Developed and tested on macOS (Apple Silicon).

## License

MIT — see [LICENSE](LICENSE).
