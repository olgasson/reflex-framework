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

void IncrementalOrderBook::add_listener(
    IncrementalOrderBookListener* listener) {
  if (listener == nullptr ||
      std::find(listeners_.begin(), listeners_.end(), listener) !=
          listeners_.end()) {
    return;
  }
  listeners_.push_back(listener);
}

void IncrementalOrderBook::notify_listeners() {
  if (!book_ready_ || in_snapshot_ || in_batch_) {
    return;
  }
  for (auto* listener : listeners_) {
    listener->on_order_book_update(*this);
  }
}

void IncrementalOrderBook::reset() {
  bids_.clear();
  asks_.clear();
  book_ready_ = false;
  in_snapshot_ = false;
  in_batch_ = false;
  snapshot_seen_bid_ = false;
  snapshot_seen_ask_ = false;
  last_update_ts_ns_ = 0;
  last_l2_ts_ns_ = 0;
  has_l2_provenance_ = false;
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
  snapshot_seen_bid_ = false;
  snapshot_seen_ask_ = false;
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

void IncrementalOrderBook::prune_crossed_levels(int64_t top_bid, int64_t top_ask) {
  if (bids_.empty() || asks_.empty() || top_bid <= 0 || top_ask <= 0 || top_bid >= top_ask) {
    return;
  }

  auto first_valid_ask = std::find_if(asks_.begin(), asks_.end(), [top_bid](const Level& level) {
    return level.price > top_bid;
  });
  if (first_valid_ask != asks_.begin()) {
    asks_.erase(asks_.begin(), first_valid_ask);
    mark_dirty();
  }

  auto first_valid_bid = std::find_if(bids_.begin(), bids_.end(), [top_ask](const Level& level) {
    return level.price < top_ask;
  });
  if (first_valid_bid != bids_.begin()) {
    bids_.erase(bids_.begin(), first_valid_bid);
    mark_dirty();
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
  if (is_snapshot) {
    snapshot_seen_bid_ = snapshot_seen_bid_ || is_bid;
    snapshot_seen_ask_ = snapshot_seen_ask_ || !is_bid;
  }

  // Process level 1 (always present)
  set_level(is_bid, event.price_1_, event.size_1_);

  // Process level 2 (if present)
  if (event.num_levels_ == 2) {
    set_level(is_bid, event.price_2_, event.size_2_);
  }

  // A single message is complete by itself. A multi-message snapshot or delta
  // becomes observable only at its last message.
  const bool is_batch = is_true(event.is_batch_message_);
  const bool is_last = is_true(event.is_last_batch_);
  const bool complete = !is_batch || is_last;
  if (is_snapshot) {
    // Some venues publish bids and asks as separate batches, each with its own
    // last marker. A snapshot replaces the whole book, so the bid-side last
    // marker cannot make the half-built book observable or let the first ask
    // message start a second snapshot that clears the bids again.
    if (complete && snapshot_seen_bid_ && snapshot_seen_ask_) {
      in_snapshot_ = false;
      in_batch_ = false;
      book_ready_ = !bids_.empty() && !asks_.empty();
    }
  } else {
    if (complete) {
      in_batch_ = false;
      book_ready_ = !bids_.empty() || !asks_.empty();
    } else {
      in_batch_ = true;
    }
  }
  if (complete && !bids_.empty() && !asks_.empty()) {
    has_l2_provenance_ = true;
  }
  if (complete) {
    notify_listeners();
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

  // A degraded touch proves every formerly better price is gone. Erase the
  // whole traversed range; retaining only the intermediate levels would leave
  // phantom queue depth behind the new touch.
  if (event.bid_price_ > 0 && prev_best_bid > 0 && prev_best_bid != event.bid_price_) {
    if (event.bid_price_ < prev_best_bid) {  // Bid degraded
      const auto new_best = std::find_if(
          bids_.begin(), bids_.end(), [&event](const Level& level) {
            return level.price <= event.bid_price_;
          });
      if (new_best != bids_.begin()) {
        bids_.erase(bids_.begin(), new_best);
        mark_dirty();
      }
    }
    // else: bid improved, keep old level
  }

  // Symmetrically, an ask degradation proves every formerly lower ask is gone.
  if (event.offer_price_ > 0 && prev_best_ask > 0 && prev_best_ask != event.offer_price_) {
    if (event.offer_price_ > prev_best_ask) {  // Ask degraded
      const auto new_best = std::find_if(
          asks_.begin(), asks_.end(), [&event](const Level& level) {
            return level.price >= event.offer_price_;
          });
      if (new_best != asks_.begin()) {
        asks_.erase(asks_.begin(), new_best);
        mark_dirty();
      }
    }
    // else: ask improved, keep old level
  }

  prune_crossed_levels(event.bid_price_, event.offer_price_);

  if (!book_ready_) {
    book_ready_ = (!bids_.empty() && !asks_.empty());
  }
  notify_listeners();
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
  notify_listeners();
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
