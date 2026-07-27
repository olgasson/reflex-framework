#include "domain/incremental_order_book.hpp"

#include <algorithm>
#include <iterator>

namespace reflex::marketdata {

namespace {
inline bool is_true(BooleanEnum flag) {
  return flag == BooleanEnum::TRUE;
}

constexpr int64_t clamp_non_negative(int64_t value) {
  return value >= 0 ? value : 0;
}
} // namespace

IncrementalOrderBook::IncrementalOrderBook(std::size_t max_depth)
  : max_depth_(max_depth ? max_depth : 200) {}

void IncrementalOrderBook::reset() {
  bids_.clear();
  asks_.clear();
  book_ready_ = false;
  in_snapshot_ = false;
  in_batch_ = false;
  last_update_ts_ns_ = 0;
  last_l2_ts_ns_ = 0;
  mark_dirty();
}

void IncrementalOrderBook::set_instrument(int32_t instrument_id) {
  instrument_id_ = instrument_id;
  reset();
}

bool IncrementalOrderBook::ensure_instrument(int32_t instrument_id) {
  if (!instrument_id_.has_value()) {
    instrument_id_ = instrument_id;
    return true;
  }
  return instrument_id_.value() == instrument_id;
}

void IncrementalOrderBook::start_snapshot() {
  bids_.clear();
  asks_.clear();
  book_ready_ = false;
  in_snapshot_ = true;
  in_batch_ = false;
  mark_dirty();
}

namespace {
template <typename Side>
auto lower_bound_price(Side& levels, bool is_bid, int64_t price) {
  auto comp = [is_bid](const reflex::marketdata::IncrementalOrderBook::Level& level,
                       int64_t value) {
    return is_bid ? level.price > value : level.price < value;
  };
  return std::lower_bound(levels.begin(), levels.end(), price, comp);
}
}  // namespace

void IncrementalOrderBook::set_level(bool is_bid, int64_t price, int64_t qty) {
  if (price <= 0) {
    return;
  }

  auto& side = is_bid ? static_cast<SideLevels&>(bids_) : static_cast<SideLevels&>(asks_);
  const int64_t sanitized_qty = clamp_non_negative(qty);
  auto it = lower_bound_price(side, is_bid, price);

  if (sanitized_qty == 0) {
    if (it != side.end() && it->price == price) {
      side.erase(it);
      mark_dirty();
    }
    return;
  }

  if (it != side.end() && it->price == price) {
    if (it->quantity != sanitized_qty) {
      it->quantity = sanitized_qty;
      mark_dirty();
    }
    return;
  }

  side.insert(it, Level{price, sanitized_qty});
  mark_dirty();
  prune_side(side);
}

void IncrementalOrderBook::prune_side(SideLevels& side) {
  if (side.size() > max_depth_) {
    side.resize(max_depth_);
  }
}

void IncrementalOrderBook::apply_l2_update(const L2UpdateEvent& event) {
  if (!ensure_instrument(event.instrument_id_)) {
    return;
  }

  last_update_ts_ns_ = std::max(last_update_ts_ns_, event.timestamp_ns_);
  last_l2_ts_ns_ = std::max(last_l2_ts_ns_, event.timestamp_ns_);

  const bool is_snapshot = is_true(event.snapshot_);
  if (is_snapshot && !in_snapshot_) {
    start_snapshot();
  }

  const bool is_bid = (event.side_ == Side::Buy);

  // Process level 1 (always present)
  set_level(is_bid, event.price_1_, event.size_1_);

  // Process level 2 (if present)
  if (event.num_levels_ == 2) {
    set_level(is_bid, event.price_2_, event.size_2_);
  }

  // Update book ready state based on last batch flag
  const bool is_last = is_true(event.is_last_batch_);
  if (is_snapshot) {
    if (is_last) {
      in_snapshot_ = false;
      in_batch_ = false;
      book_ready_ = !bids_.empty() || !asks_.empty();
    }
  } else {
    if (is_last) {
      in_batch_ = false;
      book_ready_ = !bids_.empty() || !asks_.empty();
    } else {
      in_batch_ = true;
    }
  }
}

void IncrementalOrderBook::apply_l1_update(const L1UpdateEvent& event) {
  if (!ensure_instrument(event.instrument_id_)) {
    return;
  }

  const int64_t prev_best_bid = best_bid_price();
  const int64_t prev_best_ask = best_ask_price();

  last_update_ts_ns_ = std::max(last_update_ts_ns_, event.timestamp_ns_);

  set_level(true, event.bid_price_, event.bid_size_);
  set_level(false, event.offer_price_, event.offer_size_);

  // Only erase old best bid if it DEGRADED (moved down)
  // If bid improved (moved up), keep old level - it becomes level 2
  if (event.bid_price_ > 0 && prev_best_bid > 0 && prev_best_bid != event.bid_price_) {
    if (event.bid_price_ < prev_best_bid) {  // Bid degraded
      auto it = lower_bound_price(bids_, true, prev_best_bid);
      if (it != bids_.end() && it->price == prev_best_bid) {
        bids_.erase(it);
        mark_dirty();
      }
    }
    // else: bid improved, keep old level
  }

  // Only erase old best ask if it DEGRADED (moved up)
  // If ask improved (moved down), keep old level - it becomes level 2
  if (event.offer_price_ > 0 && prev_best_ask > 0 && prev_best_ask != event.offer_price_) {
    if (event.offer_price_ > prev_best_ask) {  // Ask degraded
      auto it = lower_bound_price(asks_, false, prev_best_ask);
      if (it != asks_.end() && it->price == prev_best_ask) {
        asks_.erase(it);
        mark_dirty();
      }
    }
    // else: ask improved, keep old level
  }

  if (!book_ready_) {
    book_ready_ = (!bids_.empty() && !asks_.empty());
  }
}

void IncrementalOrderBook::apply_trade_to_side(bool hit_bid, int64_t price, int64_t qty) {
  if (qty <= 0) {
    return;
  }

  auto& side = hit_bid ? bids_ : asks_;
  const bool is_bid_side = hit_bid;
  auto it = lower_bound_price(side, is_bid_side, price);
  if (it != side.end() && it->price == price) {
    if (it->quantity > qty) {
      it->quantity -= qty;
    } else {
      side.erase(it);
    }
    mark_dirty();
    return;
  }

  if (side.empty()) {
    return;
  }

  auto& best = side.front();
  if (price > 0) {
    if (hit_bid && price < best.price) {
      return;
    }
    if (!hit_bid && price > best.price) {
      return;
    }
  }

  if (best.quantity > qty) {
    best.quantity -= qty;
  } else {
    side.erase(side.begin());
  }
  mark_dirty();
}

void IncrementalOrderBook::apply_trade(const TradeEvent& event) {
  if (!ensure_instrument(event.instrument_id_)) {
    return;
  }

  last_update_ts_ns_ = std::max(last_update_ts_ns_, event.timestamp_ns_);

  if (!book_ready_) {
    return;
  }

  const int64_t qty = clamp_non_negative(event.size_);
  if (qty == 0) {
    return;
  }

  switch (event.side_) {
    case Side::Buy:
      apply_trade_to_side(false, event.price_, qty);
      break;
    case Side::Sell:
      apply_trade_to_side(true, event.price_, qty);
      break;
    default:
      break;
  }

  if (book_ready_) {
    book_ready_ = (!bids_.empty() && !asks_.empty());
  }
}

void IncrementalOrderBook::ensure_cache_initialized() const {
  if (cached_bids_.capacity() < max_depth_) {
    cached_bids_.reserve(max_depth_);
  }
  if (cached_asks_.capacity() < max_depth_) {
    cached_asks_.reserve(max_depth_);
  }
}

void IncrementalOrderBook::rebuild_cache() const {
  if (!cache_dirty_) {
    return;
  }

  ensure_cache_initialized();

  cached_bids_.clear();
  cached_asks_.clear();

  auto fill_cache = [&](const auto& source, auto& target) {
    std::size_t copied = 0;
    for (const auto& [price, qty] : source) {
      target.emplace_back(Level{price, qty});
      if (++copied >= max_depth_) {
        break;
      }
    }
  };

  fill_cache(bids_, cached_bids_);
  fill_cache(asks_, cached_asks_);

  cache_dirty_ = false;
}

const std::vector<IncrementalOrderBook::Level>& IncrementalOrderBook::bids() const {
  rebuild_cache();
  return cached_bids_;
}

const std::vector<IncrementalOrderBook::Level>& IncrementalOrderBook::asks() const {
  rebuild_cache();
  return cached_asks_;
}

int64_t IncrementalOrderBook::best_bid_price() const {
  if (bids_.empty()) {
    return 0;
  }
  return bids_.front().price;
}

int64_t IncrementalOrderBook::best_ask_price() const {
  if (asks_.empty()) {
    return 0;
  }
  return asks_.front().price;
}

int64_t IncrementalOrderBook::best_bid_quantity() const {
  if (bids_.empty()) {
    return 0;
  }
  return bids_.front().quantity;
}

int64_t IncrementalOrderBook::best_ask_quantity() const {
  if (asks_.empty()) {
    return 0;
  }
  return asks_.front().quantity;
}

int64_t IncrementalOrderBook::spread() const {
  const auto bid = best_bid_price();
  const auto ask = best_ask_price();
  if (bid == 0 || ask == 0) {
    return 0;
  }
  return ask - bid;
}

int64_t IncrementalOrderBook::mid_price() const {
  const auto bid = best_bid_price();
  const auto ask = best_ask_price();
  if (bid == 0 || ask == 0) {
    return 0;
  }
  return (bid + ask) / 2;
}

IncrementalOrderBook::Snapshot IncrementalOrderBook::snapshot() const {
  Snapshot snap;
  snap.timestamp_ns = last_update_ts_ns_;
  snap.ready = book_ready_;
  snap.bids = bids();
  snap.asks = asks();
  return snap;
}

} // namespace reflex::marketdata
