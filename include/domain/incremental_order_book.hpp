#pragma once

#include "messages.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace reflex::marketdata {

/**
 * IncrementalOrderBook rebuilds the book between coarse-grained L2 snapshots by
 * replaying higher-frequency L1 and trade events. Keeping this state inside the
 * trading stack lets live trading, telemetry capture, and offline calibration
 * share the same view of the market.
 */
class IncrementalOrderBook {
public:
  struct Level {
    int64_t price = 0;
    int64_t quantity = 0;

    bool operator==(const Level& other) const noexcept {
      return price == other.price && quantity == other.quantity;
    }

    bool operator!=(const Level& other) const noexcept {
      return !(*this == other);
    }
  };

  struct Snapshot {
    int64_t timestamp_ns = 0;
    bool ready = false;
    std::vector<Level> bids;
    std::vector<Level> asks;
  };

  explicit IncrementalOrderBook(std::size_t max_depth = 200);

  void reset();
  void set_instrument(int32_t instrument_id);

  void apply_l2_update(const L2UpdateEvent& event);
  void apply_l1_update(const L1UpdateEvent& event);
  void apply_trade(const TradeEvent& event);

  [[nodiscard]] bool has_instrument() const noexcept { return instrument_id_.has_value(); }
  [[nodiscard]] std::optional<int32_t> instrument_id() const noexcept { return instrument_id_; }
  [[nodiscard]] bool ready() const noexcept { return book_ready_; }
  [[nodiscard]] bool in_snapshot() const noexcept { return in_snapshot_; }
  [[nodiscard]] bool in_batch() const noexcept { return in_batch_; }

  [[nodiscard]] const std::vector<Level>& bids() const;
  [[nodiscard]] const std::vector<Level>& asks() const;

  [[nodiscard]] int64_t best_bid_price() const;
  [[nodiscard]] int64_t best_ask_price() const;
  [[nodiscard]] int64_t best_bid_quantity() const;
  [[nodiscard]] int64_t best_ask_quantity() const;
  [[nodiscard]] int64_t spread() const;
  [[nodiscard]] int64_t mid_price() const;

  [[nodiscard]] int64_t last_update_timestamp() const noexcept { return last_update_ts_ns_; }
  [[nodiscard]] int64_t last_l2_timestamp() const noexcept { return last_l2_ts_ns_; }

  [[nodiscard]] Snapshot snapshot() const;

private:
  using SideLevels = std::vector<Level>;
  using BidSide = SideLevels;
  using AskSide = SideLevels;

  void start_snapshot();
  bool ensure_instrument(int32_t instrument_id);
  void set_level(bool is_bid, int64_t price, int64_t qty);
  void prune_side(SideLevels& side);
  void mark_dirty() const noexcept { cache_dirty_ = true; }
  void rebuild_cache() const;
  [[nodiscard]] bool cache_dirty() const noexcept { return cache_dirty_; }
  void ensure_cache_initialized() const;
  void apply_trade_to_side(bool hit_bid, int64_t price, int64_t qty);

  static constexpr std::size_t kPruneSlack = 16;

  std::optional<int32_t> instrument_id_;
  std::size_t max_depth_;
  BidSide bids_;
  AskSide asks_;
  bool book_ready_{false};
  bool in_snapshot_{false};
  bool in_batch_{false};

  int64_t last_update_ts_ns_{0};
  int64_t last_l2_ts_ns_{0};

  mutable bool cache_dirty_{true};
  mutable std::vector<Level> cached_bids_;
  mutable std::vector<Level> cached_asks_;
};

} // namespace reflex::marketdata
