// backtest/exchange_simulator.cpp

#include "backtest/exchange_simulator.hpp"
#include "logger_factory.hpp"

#include <algorithm>
#include <climits>
#include <utility>

namespace reflex::backtest {

ExchangeSimulator::ExchangeSimulator(std::shared_ptr<ClockInterface> clock,
                   ExchangeResponseHandler* response_handler
)
    : clock_(std::move(clock)),
    response_handler_(response_handler)
{

  logger_ = LoggerFactory::getLogger("ExchangeSimulator");
  sweep_fills_.reserve(64);
}

void ExchangeSimulator::process_l1_update(const L1UpdateEvent& event) {
  order_book_manager_.process_l1_update(event);

  // Optimistic queue model: displayed-size reductions at the top of book are
  // treated as cancels AHEAD of us, exactly like the L2 path below. Without
  // this, L1-only tapes silently degrade Optimistic to Pessimistic.
  if (queue_model_ != QueueModel::Optimistic) return;
  auto it = resting_.find(event.instrument_id_);
  if (it == resting_.end()) return;
  Book& book = it->second;
  if (event.bid_price_ > 0) {
    auto lvl = book.bids.find(event.bid_price_);
    if (lvl != book.bids.end())
      for (int32_t i = lvl->second.head; i >= 0; i = arena_[i].next)
        arena_[i].queue_ahead = std::min(arena_[i].queue_ahead, event.bid_size_);
  }
  if (event.offer_price_ > 0) {
    auto lvl = book.asks.find(event.offer_price_);
    if (lvl != book.asks.end())
      for (int32_t i = lvl->second.head; i >= 0; i = arena_[i].next)
        arena_[i].queue_ahead = std::min(arena_[i].queue_ahead, event.offer_size_);
  }
}

void ExchangeSimulator::process_l2_update(const L2UpdateEvent& event) {
    // Maintain external book
  order_book_manager_.process_l2_update(event);

  // Optimistic queue model only: assume displayed-depth reductions (cancels) happened AHEAD
  // of us, so cap our queue position at the currently displayed depth. The Pessimistic default
  // does nothing here -> our queue position drains only via actual trades.
  if (queue_model_ != QueueModel::Optimistic) return;
  auto it = resting_.find(event.instrument_id_);
  if (it == resting_.end()) return;
  Book& book = it->second;
  for (auto& [px, lvl] : book.bids) {
    const int64_t depth = displayed_depth_at(event.instrument_id_, Side::Buy, px);
    for (int32_t i = lvl.head; i >= 0; i = arena_[i].next)
      arena_[i].queue_ahead = std::min(arena_[i].queue_ahead, depth);
  }
  for (auto& [px, lvl] : book.asks) {
    const int64_t depth = displayed_depth_at(event.instrument_id_, Side::Sell, px);
    for (int32_t i = lvl.head; i >= 0; i = arena_[i].next)
      arena_[i].queue_ahead = std::min(arena_[i].queue_ahead, depth);
  }
}


void ExchangeSimulator::process_trade_event(const TradeEvent& event) {
  order_book_manager_.process_trade_event(event);

  auto it = resting_.find(event.instrument_id_);
  if (it == resting_.end()) return;
  Book& book = it->second;  // reference, not copy

  const int64_t trade_price = event.price_;
  int64_t remaining = event.size_;   // contracts that actually traded in this print = our fill budget
  if (remaining <= 0) return;
  const int64_t now_ns = clock_->epoch_nanos();

  // event.side_ is the AGGRESSOR side. The printed size is shared across the levels it sweeps;
  // at each level the trade first drains the market depth queued ahead of us, then fills us.
  if (event.side_ == Side::Buy) {
    // Aggressive BUY lifts asks -> can fill our resting ASKS priced <= trade_price (best/lowest first).
    for (auto lvl = book.asks.begin(); lvl != book.asks.end() && remaining > 0; ) {
      if (lvl->first > trade_price) break;          // asks low->high: remaining levels are worse-priced
      consume_level(lvl->second, remaining, now_ns);
      if (lvl->second.head < 0) lvl = book.asks.erase(lvl);
      else ++lvl;
    }
  } else if (event.side_ == Side::Sell) {
    // Aggressive SELL hits bids -> can fill our resting BIDS priced >= trade_price (best/highest first).
    for (auto lvl = book.bids.begin(); lvl != book.bids.end() && remaining > 0; ) {
      if (lvl->first < trade_price) break;          // bids high->low: remaining levels are worse-priced
      consume_level(lvl->second, remaining, now_ns);
      if (lvl->second.head < 0) lvl = book.bids.erase(lvl);
      else ++lvl;
    }
  }
}


void ExchangeSimulator::on_pending(const PendingEvent& event) {
  const int64_t now_ns = clock_->epoch_nanos();

  if (event.order_type_ == OrderType::Market) {
    // immediate-or-reject semantics in many venues; here we fully sweep
    handle_market_order(event, now_ns);
    return;
  }

  const bool post_only = (event.exec_inst_ == ExecInst::ParticipateDontInitiate);
  // Limit
  if (post_only) {
    auto best_bid = order_book_manager_.get_best_bid(event.instrument_id_);
    auto best_ask = order_book_manager_.get_best_ask(event.instrument_id_);

    const bool crosses =
        (event.side_ == Side::Buy  && best_ask > 0 && event.price_ >= best_ask) ||
        (event.side_ == Side::Sell && best_bid > 0 && event.price_ <= best_bid);

    if (crosses) {
      send_rejected(event.order_id_, RejectReason::PostOnly, now_ns);
      return;
    }
    rest_post_only_limit(event, now_ns);
  } else {
    // Non post-only limit (GTC): fill the marketable part against the displayed
    // book — never through the limit price — and rest any remainder.
    handle_limit_order(event, now_ns);
  }
}


void ExchangeSimulator::on_pending_replace(const PendingReplaceEvent& e) {
  const auto now_ns = clock_->epoch_nanos();

  auto it = rest_index_.find(e.order_id_);
  if (it == rest_index_.end()) {
    send_replace_rejected(e.order_id_, RejectReason::OrderUnknown, now_ns);
    logger_->debug("Replace rejected: unknown order {}", e.order_id_);
    return;
  }

  const int32_t idx = it->second;
  const int32_t instrument_id = arena_[idx].instrument_id;
  const Side side = arena_[idx].side;
  const int64_t old_price = arena_[idx].price;
  auto& book = resting_[instrument_id];

  // Post-only crossing check against external TOB
  const int64_t best_bid = order_book_manager_.get_best_bid(instrument_id);
  const int64_t best_ask = order_book_manager_.get_best_ask(instrument_id);

  const bool crosses =
      (side == Side::Buy  && best_ask > 0 && e.price_ >= best_ask) ||
      (side == Side::Sell && best_bid > 0 && e.price_ <= best_bid);

  if (crosses) {
    send_replace_rejected(e.order_id_, RejectReason::PostOnly, now_ns);
    return; // keep original resting order + index intact
  }

  // Detach from the old price level (O(1) via the intrusive list).
  bool found = false;
  if (side == Side::Buy) {
    auto old_lvl_it = book.bids.find(old_price);
    if (old_lvl_it != book.bids.end()) {
      unlink(old_lvl_it->second, idx);
      if (old_lvl_it->second.head < 0) book.bids.erase(old_lvl_it);
      found = true;
    }
  } else {
    auto old_lvl_it = book.asks.find(old_price);
    if (old_lvl_it != book.asks.end()) {
      unlink(old_lvl_it->second, idx);
      if (old_lvl_it->second.head < 0) book.asks.erase(old_lvl_it);
      found = true;
    }
  }

  if (!found) {
    send_replace_rejected(e.order_id_, RejectReason::OrderUnknown, now_ns);
    logger_->debug("Replace rejected: indexed but not found in book, order {}", e.order_id_);
    rest_index_.erase(it);          // clean up bad index to avoid future confusion
    arena_.release(idx);
    return;
  }

  // OKX amend semantics: e.quantity_ is the new TOTAL order quantity, so
  // leaves = new_qty - cum_filled. Setting leaves = new_qty outright would let
  // an order fill its earlier executions all over again (overfill).
  const int64_t new_leaves = static_cast<int64_t>(e.quantity_) - arena_[idx].cum_filled;
  if (new_leaves <= 0) {
    // Already filled at least the amended quantity -> nothing left to work;
    // treat as fully filled and take the order down.
    rest_index_.erase(it);
    arena_.release(idx);
    send_replace_accepted(e.order_id_, now_ns);
    return;
  }

  // Re-price and re-queue at the back of the new level (loses time priority).
  RestingOrder& ro = arena_[idx];
  ro.price = e.price_;
  ro.leaves = new_leaves;
  ro.queue_ahead = initial_queue_ahead(instrument_id, side, ro.price);
  ro.next = ro.prev = -1;
  if (side == Side::Buy) {
    link_back(book.bids[ro.price], idx);
  } else {
    link_back(book.asks[ro.price], idx);
  }
  // rest_index_ entry stays valid (same arena index, same order_id).

  send_replace_accepted(e.order_id_, now_ns);
}

void ExchangeSimulator::on_pending_cancel(const PendingCancelEvent& e) {
  const int64_t now_ns = clock_->epoch_nanos();

  // Use the index to find the exact order in O(1).
  auto it = rest_index_.find(e.order_id_);
  if (it == rest_index_.end()) {
    send_cancel_rejected(e.order_id_, RejectReason::OrderUnknown, now_ns);
    logger_->debug("Cancel rejected: unknown order {}", e.order_id_);
    return;
  }

  const int32_t idx = it->second;
  const int32_t instrument_id = arena_[idx].instrument_id;
  const Side side = arena_[idx].side;
  const int64_t price = arena_[idx].price;

  auto book_it = resting_.find(instrument_id);
  if (book_it == resting_.end()) {
    // Inconsistent state: index exists, book missing
    rest_index_.erase(it);
    arena_.release(idx);
    send_cancel_rejected(e.order_id_, RejectReason::OrderUnknown, now_ns);
    logger_->debug("Cancel rejected: indexed but no book for order {}", e.order_id_);
    return;
  }

  Book& book = book_it->second;
  bool removed = false;

  if (side == Side::Buy) {
    auto lvl = book.bids.find(price);
    if (lvl != book.bids.end()) {
      unlink(lvl->second, idx);
      if (lvl->second.head < 0) book.bids.erase(lvl);
      removed = true;
    }
  } else if (side == Side::Sell) {
    auto lvl = book.asks.find(price);
    if (lvl != book.asks.end()) {
      unlink(lvl->second, idx);
      if (lvl->second.head < 0) book.asks.erase(lvl);
      removed = true;
    }
  }

  rest_index_.erase(it);
  arena_.release(idx);

  if (removed) {
    send_cancel_accepted(e.order_id_, now_ns);
  } else {
    // Index said it existed but we didn't find it — already cleaned up above.
    send_cancel_rejected(e.order_id_, RejectReason::OrderUnknown, now_ns);
    logger_->debug("Cancel rejected: indexed but not found in book, order {}", e.order_id_);
  }
}


void ExchangeSimulator::handle_market_order(const PendingEvent& e, int64_t now_ns) {
  send_accepted(e.order_id_, now_ns);

  // Market order: sweep with no price bound.
  const int64_t no_bound = (e.side_ == Side::Buy) ? INT64_MAX : INT64_MIN;
  const int64_t qty_left = sweep_displayed_book(e, no_bound, now_ns);

  if (qty_left > 0) {
    // Remainder unfilled (book too thin or not ready) — cancel it (IOC-style).
    send_cancel_accepted(e.order_id_, now_ns);
  }
}

void ExchangeSimulator::handle_limit_order(const PendingEvent& e, int64_t now_ns) {
  send_accepted(e.order_id_, now_ns);

  // Sweep only levels at-or-better than our limit — a GTC buy below the market
  // must never fill through its own limit price.
  const int64_t qty_left = sweep_displayed_book(e, e.price_, now_ns);

  if (qty_left > 0) {
    // GTC remainder rests in the book with normal queue position.
    rest_order(e, qty_left, /*post_only=*/false, now_ns);
  }
}

// Taker sweep of the displayed book, bounded by limit_price. Fill budget is the
// currently displayed depth; consumed depth is removed from the simulated book
// so back-to-back takers cannot reuse the same liquidity before the next
// market-data event refreshes it.
int64_t ExchangeSimulator::sweep_displayed_book(const PendingEvent& e, int64_t limit_price,
                                                int64_t now_ns) {
  int64_t qty_left = static_cast<int64_t>(e.quantity_);
  const int32_t instr = e.instrument_id_;

  auto* incr = order_book_manager_.get_incremental_book(instr);
  if (!incr || !incr->ready()) {
    return qty_left;  // nothing displayed to fill against
  }

  // Record fills first, then mutate the book: bids()/asks() return the book's
  // cached level vectors, which must not be invalidated mid-iteration.
  sweep_fills_.clear();
  if (e.side_ == Side::Buy) {
    // Sweep the asks (lowest to highest price), never above our limit.
    for (const auto& level : incr->asks()) {
      if (qty_left <= 0 || level.price > limit_price) break;
      const int64_t trade_qty = std::min(qty_left, level.quantity);
      if (trade_qty <= 0) continue;
      send_executed(e.order_id_, e.side_, instr, level.price, trade_qty, now_ns);
      sweep_fills_.push_back({level.price, trade_qty});
      qty_left -= trade_qty;
    }
  } else {
    // Sweep the bids (highest to lowest price), never below our limit.
    for (const auto& level : incr->bids()) {
      if (qty_left <= 0 || level.price < limit_price) break;
      const int64_t trade_qty = std::min(qty_left, level.quantity);
      if (trade_qty <= 0) continue;
      send_executed(e.order_id_, e.side_, instr, level.price, trade_qty, now_ns);
      sweep_fills_.push_back({level.price, trade_qty});
      qty_left -= trade_qty;
    }
  }

  // Our taker fills consumed displayed liquidity — decrement the simulated book
  // (apply_trade drains the level exactly like an external print would).
  for (const auto& fill : sweep_fills_) {
    TradeEvent t{};
    t.instrument_id_ = instr;
    t.timestamp_ns_ = now_ns;
    t.side_ = e.side_;
    t.price_ = fill.price;
    t.size_ = fill.quantity;
    incr->apply_trade(t);
  }

  return qty_left;
}


void ExchangeSimulator::link_back(LevelFifo& level, int32_t idx) {
  RestingOrder& o = arena_[idx];
  o.prev = level.tail;
  o.next = -1;
  if (level.tail >= 0) arena_[level.tail].next = idx;
  else                 level.head = idx;
  level.tail = idx;
}

void ExchangeSimulator::unlink(LevelFifo& level, int32_t idx) {
  RestingOrder& o = arena_[idx];
  if (o.prev >= 0) arena_[o.prev].next = o.next; else level.head = o.next;
  if (o.next >= 0) arena_[o.next].prev = o.prev; else level.tail = o.prev;
  o.next = o.prev = -1;
}

void ExchangeSimulator::rest_post_only_limit(const PendingEvent& e, int64_t now_ns) {
  send_accepted(e.order_id_, now_ns);
  rest_order(e, static_cast<int64_t>(e.quantity_), /*post_only=*/true, now_ns);
}

// Rest `leaves` of an order in our book. Does NOT send an accept — callers
// acknowledge the order themselves (a marketable limit is accepted before its
// sweep, so only the remainder reaches here).
void ExchangeSimulator::rest_order(const PendingEvent& e, int64_t leaves, bool post_only,
                                   int64_t now_ns) {
  const int32_t idx = arena_.alloc();          // no further arena_ growth below -> ro stays valid
  RestingOrder& ro = arena_[idx];
  ro.order_id     = e.order_id_;
  ro.instrument_id= e.instrument_id_;
  ro.side         = e.side_;
  ro.price        = e.price_;
  ro.leaves       = leaves;
  ro.cum_filled   = static_cast<int64_t>(e.quantity_) - leaves;  // taker fills before resting
  ro.post_only    = post_only;
  ro.ts_ns        = now_ns;
  // Aggregate-L2 queue position: start behind (queue_init_fraction of) the market volume
  // already displayed at our price. (0 if we improve the book / open a new level -> first in line.)
  ro.queue_ahead  = initial_queue_ahead(ro.instrument_id, ro.side, ro.price);
  ro.next = ro.prev = -1;

  Book& book = resting_[ro.instrument_id];
  if (ro.side == Side::Buy) {
    link_back(book.bids[ro.price], idx);
  } else {
    link_back(book.asks[ro.price], idx);
  }

  // index by id
  rest_index_[ro.order_id] = idx;
}


// Consume one price level's FIFO with a trade budget: trade volume first drains the market
// depth queued ahead of each order, then fills the order (possibly partially) at its limit.
void ExchangeSimulator::consume_level(LevelFifo& level, int64_t& remaining, int64_t now_ns) {
  while (level.head >= 0 && remaining > 0) {
    const int32_t idx = level.head;
    RestingOrder& o = arena_[idx];

    // (1) trade volume first eats the market depth queued ahead of us
    const int64_t eat = std::min(remaining, o.queue_ahead);
    if (eat > 0) {
      o.queue_ahead -= eat;
      remaining     -= eat;
      // That market depth was queued ahead of every successor at this level too
      // (each order's queue_ahead was measured against the same displayed
      // depth), so propagate the consumption — otherwise the shared budget is
      // double-charged and fills are underestimated.
      for (int32_t j = o.next; j >= 0; j = arena_[j].next) {
        arena_[j].queue_ahead = std::max<int64_t>(0, arena_[j].queue_ahead - eat);
      }
    }
    if (o.queue_ahead > 0 || remaining <= 0) return;  // still behind the queue, or budget spent

    // (2) we are now at the front of the queue -> fill (possibly partial) at OUR limit price
    const int64_t fill = std::min(remaining, o.leaves);
    send_executed(o.order_id, o.side, o.instrument_id, o.price, fill, now_ns);
    o.leaves     -= fill;
    o.cum_filled += fill;
    remaining    -= fill;

    if (o.leaves <= 0) {
      rest_index_.erase(o.order_id);
      const int32_t next = o.next;        // pop front of the intrusive FIFO
      level.head = next;
      if (next >= 0) arena_[next].prev = -1;
      else           level.tail = -1;
      arena_.release(idx);
    } else {
      return;                       // partial fill -> order keeps resting at the front
    }
  }
}

// Displayed (aggregate) market depth at an exact price on the given side; 0 if no such level.
// Levels are price-sorted (bids high->low, asks low->high) so we break as soon as we pass the
// target price -> O(distance-from-top), not O(depth). Hot path under the Optimistic queue model.
int64_t ExchangeSimulator::displayed_depth_at(int32_t instrument_id, Side side, int64_t price) {
  const auto* book = order_book_manager_.get_incremental_book(instrument_id);
  if (!book || !book->ready()) return 0;
  if (side == Side::Buy) {
    for (const auto& lvl : book->bids()) {         // high -> low
      if (lvl.price == price) return lvl.quantity;
      if (lvl.price < price) break;                // passed our price; not present
    }
  } else {
    for (const auto& lvl : book->asks()) {         // low -> high
      if (lvl.price == price) return lvl.quantity;
      if (lvl.price > price) break;
    }
  }
  return 0;
}

// Initial queue volume ahead of a freshly-placed/re-priced order = queue_init_fraction of
// displayed depth (1.0 = back of queue, the conservative default; lower to calibrate fill rate).
int64_t ExchangeSimulator::initial_queue_ahead(int32_t instrument_id, Side side, int64_t price) {
  const int64_t depth = displayed_depth_at(instrument_id, side, price);
  if (queue_init_fraction_ >= 1.0) return depth;
  if (queue_init_fraction_ <= 0.0) return 0;
  return static_cast<int64_t>(queue_init_fraction_ * static_cast<double>(depth));
}






// ---------------- Event plumbing ----------------

void ExchangeSimulator::send_accepted(int64_t order_id, int64_t now_ns) {

  // Create response slot and use placement new (like RingBufferWriter does)
  MessageSlot response_slot;

  // Use placement new to construct AcceptedEvent directly in the slot
  auto* const accepted = new (response_slot.raw_data()) AcceptedEvent();

  // Fill the response fields
  accepted->order_id_ = order_id;
  accepted->timestamp_ns_ = now_ns;

  response_handler_->on_exchange_response(response_slot);
}

void ExchangeSimulator::send_rejected(int64_t order_id, RejectReason reason, int64_t now_ns) {

  MessageSlot response_slot;

  auto* const rejected = new (response_slot.raw_data()) RejectedEvent();
  rejected->timestamp_ns_ = now_ns;
  rejected->order_id_ = order_id;
  rejected->reject_reason_ = reason;

  response_handler_->on_exchange_response(response_slot);
}

void ExchangeSimulator::send_replace_accepted(int64_t order_id, int64_t now_ns) {

  MessageSlot response_slot;

  auto* const replace_accepted = new (response_slot.raw_data()) ReplaceAcceptedEvent();
  replace_accepted->timestamp_ns_ = now_ns;
  replace_accepted->order_id_ = order_id;

  response_handler_->on_exchange_response(response_slot);
}

void ExchangeSimulator::send_replace_rejected(int64_t order_id, RejectReason reason, int64_t now_ns) {

  MessageSlot response_slot;

  auto* const replace_rejected = new (response_slot.raw_data()) ReplaceRejectedEvent();
  replace_rejected->timestamp_ns_ = now_ns;
  replace_rejected->order_id_ = order_id;
  replace_rejected->reject_reason_ = reason;

  response_handler_->on_exchange_response(response_slot);
}

void ExchangeSimulator::send_cancel_accepted(int64_t order_id, int64_t now_ns) {

  MessageSlot response_slot;

  auto* const cancel_accepted = new (response_slot.raw_data()) CancelAcceptedEvent();
  cancel_accepted->timestamp_ns_ = now_ns;
  cancel_accepted->order_id_ = order_id;

  response_handler_->on_exchange_response(response_slot);
}

void ExchangeSimulator::send_cancel_rejected(int64_t order_id, RejectReason reason, int64_t now_ns) {

  MessageSlot response_slot;

  auto* const cancel_rejected = new (response_slot.raw_data()) CancelRejectedEvent();
  cancel_rejected->timestamp_ns_ = now_ns;
  cancel_rejected->order_id_ = order_id;
  cancel_rejected->reject_reason_ = reason;

  response_handler_->on_exchange_response(response_slot);
}

void ExchangeSimulator::send_executed(int64_t order_id, Side side, int32_t instrument_id,
                                      int64_t px, int64_t qty, int64_t now_ns) {

  MessageSlot response_slot;

  auto* const executed = new (response_slot.raw_data()) ExecutedEvent();

  executed->timestamp_ns_ = now_ns;
  executed->order_id_ = order_id;
  executed->side_ = side;
  executed->instrument_id_ = instrument_id;
  executed->last_price_ = px;
  executed->last_quantity_ = qty;

  response_handler_->on_exchange_response(response_slot);
}

} // namespace reflex::backtest
