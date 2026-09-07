// backtest/exchange_simulator.cpp

#include "backtest/exchange_simulator.hpp"
#include "asset_info_manager.hpp"
#include "logger_factory.hpp"
#include "utils/codec_utils.hpp"

#include <algorithm>
#include <climits>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <utility>

namespace reflex::backtest {

namespace {
const char* side_to_csv(Side side) {
  switch (side) {
    case Side::Buy:
      return "Buy";
    case Side::Sell:
      return "Sell";
    default:
      return "Undefined";
  }
}

double fixed_to_double(int64_t value) {
  return static_cast<double>(value) * 1e-8;
}

const char* queue_model_to_csv(ExchangeSimulator::QueueModel model) {
  return model == ExchangeSimulator::QueueModel::Optimistic ? "Optimistic" : "Pessimistic";
}
}  // namespace

ExchangeSimulator::ExchangeSimulator(std::shared_ptr<ClockInterface> clock,
                   ExchangeResponseHandler* response_handler
)
    : clock_(std::move(clock)),
    response_handler_(response_handler)
{

  logger_ = LoggerFactory::getLogger("ExchangeSimulator");
  sweep_fills_.reserve(64);
}


ExchangeSimulator::~ExchangeSimulator() { flush_quote_lifecycle(); }

void ExchangeSimulator::set_quote_lifecycle_output_path(std::filesystem::path output_path) {
  quote_lifecycle_path_ = std::move(output_path);
}

void ExchangeSimulator::flush_quote_lifecycle() {
  if (quote_lifecycle_stream_.is_open()) {
    quote_lifecycle_stream_.flush();
    quote_lifecycle_stream_.close();
  }
}

void ExchangeSimulator::ensure_quote_lifecycle_stream_open() {
  if (quote_lifecycle_path_.empty() || quote_lifecycle_stream_.is_open()) {
    return;
  }

  const auto parent = quote_lifecycle_path_.parent_path();
  if (!parent.empty()) {
    std::error_code ec;
    std::filesystem::create_directories(parent, ec);
    if (ec) {
      logger_->warn("Failed to create quote lifecycle output directory {}: {}",
                    parent.string(),
                    ec.message());
    }
  }

  quote_lifecycle_stream_.open(quote_lifecycle_path_, std::ios::out | std::ios::trunc);
  if (!quote_lifecycle_stream_.is_open()) {
    logger_->warn("Failed to open quote lifecycle output file: {}", quote_lifecycle_path_.string());
    return;
  }

  quote_lifecycle_stream_
      << "event_type,timestamp_ns,order_id,instrument_id,side,price,quantity,leaves,"
      << "quote_created_ns,initial_queue_ahead,queue_ahead,displayed_depth_at_placement,"
      << "queue_init_fraction,queue_model,terminal_reason,"
      << "price_decoded,quantity_decoded,leaves_decoded,"
      << "initial_queue_ahead_decoded,queue_ahead_decoded,displayed_depth_at_placement_decoded,"
      << "bbo_bid,bbo_ask,trade_price,trade_qty,trade_side,request_id,"
      << "touch_depth_at_placement,traded_ahead,credited_cancellation,"
      << "touch_depth_at_placement_decoded,traded_ahead_decoded,"
      << "credited_cancellation_decoded,"
      << "capped_advancement,capped_advancement_decoded\n";
}

void ExchangeSimulator::write_quote_lifecycle_row(const char* event_type,
                                                  int64_t timestamp_ns,
                                                  int64_t order_id,
                                                  int32_t instrument_id,
                                                  Side side,
                                                  int64_t price,
                                                  int64_t quantity,
                                                  int64_t leaves,
                                                  int64_t quote_created_ns,
                                                  int64_t initial_queue_ahead,
                                                  int64_t queue_ahead,
                                                  int64_t displayed_depth_at_placement,
                                                  const char* terminal_reason,
                                                  int64_t request_id,
                                                  int64_t bbo_bid,
                                                  int64_t bbo_ask,
                                                  int64_t trade_price,
                                                  int64_t trade_qty,
                                                  Side trade_side,
                                                  int64_t touch_depth_at_placement,
                                                  int64_t traded_ahead,
                                                  int64_t credited_cancellation,
                                                  int64_t capped_advancement) {
  ensure_quote_lifecycle_stream_open();
  if (!quote_lifecycle_stream_.is_open()) {
    return;
  }

  quote_lifecycle_stream_
      << event_type << ','
      << timestamp_ns << ','
      << order_id << ','
      << instrument_id << ','
      << side_to_csv(side) << ','
      << price << ','
      << quantity << ','
      << leaves << ','
      << quote_created_ns << ','
      << initial_queue_ahead << ','
      << queue_ahead << ','
      << displayed_depth_at_placement << ','
      << queue_init_fraction_ << ','
      << queue_model_to_csv(queue_model_) << ','
      << terminal_reason << ','
      << fixed_to_double(price) << ','
      << fixed_to_double(quantity) << ','
      << fixed_to_double(leaves) << ','
      << fixed_to_double(initial_queue_ahead) << ','
      << fixed_to_double(queue_ahead) << ','
      << fixed_to_double(displayed_depth_at_placement) << ','
      << fixed_to_double(bbo_bid) << ','
      << fixed_to_double(bbo_ask) << ','
      << fixed_to_double(trade_price) << ','
      << fixed_to_double(trade_qty) << ','
      << side_to_csv(trade_side) << ','
      << request_id << ','
      << touch_depth_at_placement << ','
      << traded_ahead << ','
      << credited_cancellation << ','
      << fixed_to_double(touch_depth_at_placement) << ','
      << fixed_to_double(traded_ahead) << ','
      << fixed_to_double(credited_cancellation) << ','
      << capped_advancement << ','
      << fixed_to_double(capped_advancement) << '\n';
  ++quote_lifecycle_count_;
}

void ExchangeSimulator::write_quote_lifecycle_row(const char* event_type,
                                                  int64_t timestamp_ns,
                                                  const RestingOrder& order,
                                                  int64_t quantity,
                                                  const char* terminal_reason,
                                                  int64_t request_id,
                                                  int64_t bbo_bid,
                                                  int64_t bbo_ask,
                                                  int64_t trade_price,
                                                  int64_t trade_qty,
                                                  Side trade_side) {
  write_quote_lifecycle_row(event_type,
                            timestamp_ns,
                            order.order_id,
                            order.instrument_id,
                            order.side,
                            order.price,
                            quantity,
                            order.leaves,
                            order.ts_ns,
                            order.initial_queue_ahead,
                            order.queue_ahead,
                            order.displayed_depth_at_placement,
                            terminal_reason,
                            request_id,
                            bbo_bid,
                            bbo_ask,
                            trade_price,
                            trade_qty,
                            trade_side,
                            order.touch_depth_at_placement,
                            order.traded_ahead,
                            order.credited_cancellation,
                            order.capped_advancement);
}

void ExchangeSimulator::process_l1_update(const L1UpdateEvent& event) {
  order_book_manager_.process_l1_update(event);
  // Opt-in: the top-of-book stream is real-time, so the touch level's
  // displayed size is observed at event granularity here, while a depth diff
  // stream samples it periodically. Running the same queue accounting on each
  // L1 observation exposes intra-interval touch churn, and is the only way the
  // Optimistic cap can act at all on an L1-only tape.
  if (queue_accounting_on_l1_) {
    const auto* book_state =
        order_book_manager_.get_incremental_book(event.instrument_id_);
    const bool settled =
        book_state != nullptr && book_state->ready() && !book_state->in_snapshot();
    if (settled) apply_queue_accounting(event.instrument_id_);
  }
}

void ExchangeSimulator::process_l2_update(const L2UpdateEvent& event) {
    // Maintain external book
  order_book_manager_.process_l2_update(event);

  auto it = resting_.find(event.instrument_id_);
  if (it == resting_.end()) return;
  Book& book = it->second;

  // A SNAPSHOT resync clears BOTH sides and rebuilds them across many
  // messages. While that is in flight the book is partially rebuilt, so a
  // level we rest on reads as empty and looks fully cancelled -- which would
  // hand resting orders many messages of phantom credit, and would let the
  // optimistic model cap queue position against an unbuilt book. Suppress
  // both, and invalidate the baselines so the first SETTLED observation
  // re-seeds from the rebuilt book instead of diffing across the gap.
  // (The feed marks each side's snapshot batch separately while the book
  // clears both, so trusting the batch flags here would be unsound; the
  // book's own settled state is the reliable signal.)
  const auto* book_state =
      order_book_manager_.get_incremental_book(event.instrument_id_);
  const bool settled =
      book_state != nullptr && book_state->ready() && !book_state->in_snapshot();
  const bool is_snapshot = event.snapshot_ == BooleanEnum::TRUE;
  if (!settled || is_snapshot) {
    // A completed snapshot is an authoritative re-anchor, not evidence that
    // the intervening depth disappeared ahead of us. On its final message,
    // seed from the completed book so the first subsequent delta is measured
    // normally. While it is incomplete, invalidate the baselines.
    for (auto& [px, lvl] : book.bids) {
      lvl.last_displayed = settled
                               ? displayed_depth_at(event.instrument_id_,
                                                    Side::Buy, px)
                               : -1;
      lvl.traded_since_obs = 0;
      lvl.unexplained_recent = 0;
    }
    for (auto& [px, lvl] : book.asks) {
      lvl.last_displayed = settled
                               ? displayed_depth_at(event.instrument_id_,
                                                    Side::Sell, px)
                               : -1;
      lvl.traded_since_obs = 0;
      lvl.unexplained_recent = 0;
    }
    return;
  }

  apply_queue_accounting(event.instrument_id_);
}

void ExchangeSimulator::apply_queue_accounting(int32_t instrument_id) {
  auto it = resting_.find(instrument_id);
  if (it == resting_.end()) return;
  Book& book = it->second;
  const int64_t now_ns = clock_->epoch_nanos();
  for (auto& [px, lvl] : book.bids) {
    observe_level(instrument_id, Side::Buy, px, lvl, now_ns);
  }
  for (auto& [px, lvl] : book.asks) {
    observe_level(instrument_id, Side::Sell, px, lvl, now_ns);
  }
}

// One settled observation of a level we rest on. Diffs the displayed depth
// against the previous observation, nets out volume that has actually
// traded there since, and then advances our queue position by the configured
// model:
//   Optimistic: queue_ahead <= displayed depth. This is a PHYSICAL bound
//     (volume ahead of us can never exceed the others' displayed volume at
//     our price), so it never manufactures advancement beyond what the book
//     proves; it is still called "optimistic" because every reduction that
//     forces the bound is attributed ahead of us.
//   Pessimistic + alpha: bounded cancellation attribution (x^alpha) on the
//     unexplained reduction.
// Every order records its pre-observation position so a print that arrives
// inside the netting window and was already reflected in this observation is
// applied to that position (see consume_level_anticipated).
void ExchangeSimulator::observe_level(int32_t instrument_id, Side side,
                                      int64_t price, LevelFifo& level,
                                      int64_t now_ns) {
  const int64_t displayed = displayed_depth_at(instrument_id, side, price);
  const int64_t previous = level.last_displayed;
  const int64_t traded = level.traded_since_obs;
  level.last_displayed = displayed;
  level.traded_since_obs = 0;
  int64_t cancelled = 0;
  if (previous > 0 && displayed < previous) {
    const int64_t reduction = previous - displayed;
    // Displayed depth already reflects executions; subtracting the traded
    // volume is what keeps a print from being counted as a cancellation too.
    cancelled = reduction - std::min(reduction, traded);
  }
  // A netting episode is open while an unexplained reduction observed inside
  // the window has not yet been matched by prints. The pre-observation
  // position is snapshotted ONCE at episode start; further observations in
  // the same episode accumulate their reductions but must not move the
  // snapshot, otherwise delayed prints are applied to an already-capped
  // position and the queue advances twice.
  const bool episode_open =
      trade_netting_window_ns_ > 0 && level.unexplained_recent > 0 &&
      now_ns - level.last_obs_ts_ns <= trade_netting_window_ns_;
  if (!episode_open) {
    level.unexplained_recent = 0;
    for (int32_t i = level.head; i >= 0; i = arena_[i].next) {
      arena_[i].queue_ahead_pre_obs = arena_[i].queue_ahead;
    }
  }
  if (cancelled > 0 && trade_netting_window_ns_ > 0) {
    level.unexplained_recent += cancelled;
    level.last_obs_ts_ns = now_ns;
  }

  if (queue_model_ == QueueModel::Optimistic) {
    int64_t others = displayed;
    if (tape_includes_own_orders_) {
      int64_t own = 0;
      for (int32_t i = level.head; i >= 0; i = arena_[i].next) own += arena_[i].leaves;
      others = std::max<int64_t>(0, displayed - own);
    }
    for (int32_t i = level.head; i >= 0; i = arena_[i].next) {
      RestingOrder& order = arena_[i];
      if (order.queue_ahead > others) {
        order.capped_advancement += order.queue_ahead - others;
        order.queue_ahead = others;
      }
    }
    return;
  }

  if (cancel_credit_alpha_ <= 0.0 || cancelled <= 0 || previous <= 0) return;
  // Market-by-price data cannot locate a cancellation in the FIFO, so
  // x^alpha maps our queue percentile to a bounded probability that the
  // removed volume was ahead of us.
  for (int32_t i = level.head; i >= 0; i = arena_[i].next) {
    RestingOrder& order = arena_[i];
    if (order.queue_ahead <= 0) continue;
    // queue_ahead can exceed the last displayed depth after a collapse, so
    // clamp the percentile into [0,1]. Alpha changes WHERE cancellations are
    // attributed, never their total quantity.
    const double percentile = std::clamp(
        static_cast<double>(order.queue_ahead) /
            static_cast<double>(previous),
        0.0, 1.0);
    const double ahead_probability =
        std::pow(percentile, cancel_credit_alpha_);
    const double credit =
        static_cast<double>(cancelled) * ahead_probability;
    const double queue_ahead = static_cast<double>(order.queue_ahead);
    const int64_t applied = credit >= queue_ahead
                                ? order.queue_ahead
                                : static_cast<int64_t>(credit);
    if (applied <= 0) continue;
    order.queue_ahead = std::max<int64_t>(0, order.queue_ahead - applied);
    order.credited_cancellation += applied;
  }
}


void ExchangeSimulator::process_trade_event(const TradeEvent& event) {
  // The book manager's trade heuristic subtracts the print from the best
  // level. With real-time L1 accounting that level is refreshed by the feed
  // itself, and a print that arrives AFTER the L1 update already reflecting
  // it would be subtracted twice, leaving displayed depth too low until the
  // next update and letting the cap over-advance every order at that price.
  // The feed is authoritative in that mode.
  if (!queue_accounting_on_l1_) {
    order_book_manager_.process_trade_event(event);
  }

  auto it = resting_.find(event.instrument_id_);
  if (it == resting_.end()) return;
  Book& book = it->second;  // reference, not copy

  const int64_t trade_price = event.price_;
  int64_t remaining = event.size_;   // contracts that actually traded in this print = our fill budget
  if (remaining <= 0) return;
  const int64_t now_ns = clock_->epoch_nanos();

  // event.side_ is the AGGRESSOR side. A counterfactual order was absent from
  // the historical book, so a print through its price proves price priority
  // but not unlimited demand. One finite print budget is shared across every
  // eligible simulated level in price-time order. At a strictly better level,
  // the through print proves the historical queue ahead has gone; the
  // remaining observed quantity can then fill us, partially if necessary.
  if (event.side_ == Side::Buy) {
    // Aggressive BUY lifts asks -> can fill our resting ASKS priced <= trade_price (best/lowest first).
    for (auto lvl = book.asks.begin(); lvl != book.asks.end(); ) {
      if (lvl->first > trade_price) break;          // asks low->high: remaining levels are worse-priced
      if (lvl->first < trade_price) {
        fill_level_through(lvl->second, remaining, now_ns, event);
      }
      else {
        consume_at_price(lvl->second, remaining, event.size_, now_ns, event);
      }
      if (lvl->second.head < 0) lvl = book.asks.erase(lvl);
      else ++lvl;
    }
  } else if (event.side_ == Side::Sell) {
    // Aggressive SELL hits bids -> can fill our resting BIDS priced >= trade_price (best/highest first).
    for (auto lvl = book.bids.begin(); lvl != book.bids.end(); ) {
      if (lvl->first < trade_price) break;          // bids high->low: remaining levels are worse-priced
      if (lvl->first > trade_price) {
        fill_level_through(lvl->second, remaining, now_ns, event);
      }
      else {
        consume_at_price(lvl->second, remaining, event.size_, now_ns, event);
      }
      if (lvl->second.head < 0) lvl = book.bids.erase(lvl);
      else ++lvl;
    }
  }
}

void ExchangeSimulator::on_rate_limited(const MessageSlot& event) {
  const int64_t now_ns = clock_->epoch_nanos();
  switch (event.get_type()) {
    case MessageType::Pending: {
      const auto& pending = event.as<PendingEvent>();
      send_rejected(pending.order_id_, RejectReason::RateLimit, now_ns);
      break;
    }
    case MessageType::PendingReplace: {
      const auto& replace = event.as<PendingReplaceEvent>();
      send_replace_rejected(replace.order_id_, replace.request_id_,
                            RejectReason::RateLimit, now_ns);
      break;
    }
    case MessageType::PendingCancel: {
      const auto& cancel = event.as<PendingCancelEvent>();
      send_cancel_rejected(cancel.order_id_, cancel.request_id_,
                           RejectReason::RateLimit, now_ns);
      break;
    }
    default:
      break;
  }
}

void ExchangeSimulator::on_pending(const PendingEvent& event) {
  const int64_t now_ns = clock_->epoch_nanos();

  if (event.quantity_ <= 0) {
    send_rejected(event.order_id_, RejectReason::InvalidQuantity, now_ns);
    return;
  }
  if (event.order_type_ == OrderType::Limit) {
    if (event.price_ <= 0) {
      send_rejected(event.order_id_, RejectReason::InvalidPrice, now_ns);
      return;
    }
    if (const auto* asset =
            reflex::AssetInfoManager::get_by_instrument_id(event.instrument_id_);
        asset && asset->tick_increment_ > 0 &&
        event.price_ % asset->tick_increment_ != 0) {
      throw std::runtime_error(
          "ExchangeSimulator: limit price is off the configured instrument tick grid");
    }
  }

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
      const int64_t displayed_depth = displayed_depth_at(event.instrument_id_, event.side_, event.price_);
      write_quote_lifecycle_row("rejected",
                                now_ns,
                                event.order_id_,
                                event.instrument_id_,
                                event.side_,
                                event.price_,
                                static_cast<int64_t>(event.quantity_),
                                0,
                                now_ns,
                                0,
                                0,
                                displayed_depth,
                                "post_only");
      send_rejected(event.order_id_, RejectReason::PostOnly, now_ns);
      return;
    }
    rest_post_only_limit(event, now_ns);
  } else {
    // Non post-only limit: fill the marketable part against the displayed
    // book — never through the limit price — then cancel (IOC/FOK) or rest
    // (GTC) the remainder.
    handle_limit_order(event, now_ns);
  }
}


void ExchangeSimulator::on_pending_replace(const PendingReplaceEvent& e) {
  const auto now_ns = clock_->epoch_nanos();

  auto it = rest_index_.find(e.order_id_);
  if (it == rest_index_.end()) {
    write_quote_lifecycle_row("replace_rejected",
                              now_ns,
                              e.order_id_,
                              e.instrument_id_,
                              Side::Undefined,
                              e.price_,
                              static_cast<int64_t>(e.quantity_),
                              0,
                              now_ns,
                              0,
                              0,
                              0,
                              "order_unknown",
                              e.request_id_);
    send_replace_rejected(e.order_id_, e.request_id_,
                          RejectReason::OrderUnknown, now_ns);
    logger_->debug("Replace rejected: unknown order {}", e.order_id_);
    return;
  }

  const int32_t idx = it->second;
  const int32_t instrument_id = arena_[idx].instrument_id;
  const Side side = arena_[idx].side;
  const int64_t old_price = arena_[idx].price;
  // Amend semantics: e.quantity_ is the new TOTAL order quantity, so
  // leaves = new_qty - cum_filled. Setting leaves = new_qty outright would let
  // an order fill its earlier executions all over again (overfill). A total
  // at or below what has already filled leaves nothing to work and is
  // rejected without touching the resting order.
  const int64_t cumulative_filled =
      arena_[idx].quantity - arena_[idx].leaves;
  if (e.quantity_ <= cumulative_filled) {
    write_quote_lifecycle_row("replace_rejected",
                              now_ns,
                              arena_[idx],
                              static_cast<int64_t>(e.quantity_),
                              "invalid_quantity",
                              e.request_id_);
    send_replace_rejected(e.order_id_, e.request_id_,
                          RejectReason::InvalidQuantity, now_ns);
    logger_->debug(
        "Replace rejected: total quantity {} does not exceed cumulative fill {} "
        "for order {}",
        e.quantity_, cumulative_filled, e.order_id_);
    return;
  }
  const int64_t replacement_leaves = e.quantity_ - cumulative_filled;
  if (const auto* asset = reflex::AssetInfoManager::get_by_instrument_id(instrument_id);
      asset && asset->tick_increment_ > 0 && e.price_ % asset->tick_increment_ != 0) {
    throw std::runtime_error(
        "ExchangeSimulator: replacement price is off the configured instrument tick grid");
  }
  auto& book = resting_[instrument_id];

  // Post-only crossing check against external TOB
  const int64_t best_bid = order_book_manager_.get_best_bid(instrument_id);
  const int64_t best_ask = order_book_manager_.get_best_ask(instrument_id);

  const bool crosses =
      (side == Side::Buy  && best_ask > 0 && e.price_ >= best_ask) ||
      (side == Side::Sell && best_bid > 0 && e.price_ <= best_bid);

  if (crosses) {
    write_quote_lifecycle_row("replace_rejected",
                              now_ns,
                              arena_[idx],
                              static_cast<int64_t>(e.quantity_),
                              "post_only",
                              e.request_id_);
    send_replace_rejected(e.order_id_, e.request_id_, RejectReason::PostOnly,
                          now_ns);
    return; // keep original resting order + index intact
  }

  // Same invariant as placement: re-pricing behind the touch on an L1-only book
  // would re-initialize queue_ahead from fabricated zero depth.
  require_l2_provenance_for_behind_touch(instrument_id, side, e.price_);

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
    write_quote_lifecycle_row("replace_rejected",
                              now_ns,
                              arena_[idx],
                              static_cast<int64_t>(e.quantity_),
                              "order_unknown",
                              e.request_id_);
    send_replace_rejected(e.order_id_, e.request_id_,
                          RejectReason::OrderUnknown, now_ns);
    logger_->debug("Replace rejected: indexed but not found in book, order {}", e.order_id_);
    rest_index_.erase(it);          // clean up bad index to avoid future confusion
    arena_.release(idx);
    return;
  }

  // Re-price and re-queue at the back of the new level (loses time priority).
  RestingOrder& ro = arena_[idx];
  ro.price = e.price_;
  ro.quantity = e.quantity_;
  ro.leaves = replacement_leaves;
  ro.ts_ns = now_ns;
  ro.displayed_depth_at_placement = displayed_depth_at(instrument_id, side, ro.price);
  const int64_t touch = side == Side::Buy
                            ? order_book_manager_.get_best_bid(instrument_id)
                            : order_book_manager_.get_best_ask(instrument_id);
  ro.touch_depth_at_placement =
      ro.price == touch ? ro.displayed_depth_at_placement : 0;
  ro.queue_ahead = initial_queue_ahead(instrument_id, side, ro.price);
  ro.initial_queue_ahead = ro.queue_ahead;
  ro.traded_ahead = 0;
  ro.credited_cancellation = 0;
  ro.capped_advancement = 0;
  ro.queue_ahead_pre_obs = ro.queue_ahead;
  ro.next = ro.prev = -1;
  if (side == Side::Buy) {
    link_back(book.bids[ro.price], idx);
  } else {
    link_back(book.asks[ro.price], idx);
  }
  // rest_index_ entry stays valid (same arena index, same order_id).

  send_replace_accepted(e.order_id_, e.request_id_, instrument_id, now_ns);
  write_quote_lifecycle_row("replace_accepted", now_ns, ro,
                            static_cast<int64_t>(e.quantity_), "",
                            e.request_id_);
}

void ExchangeSimulator::on_pending_cancel(const PendingCancelEvent& e) {
  const int64_t now_ns = clock_->epoch_nanos();

  // Use the index to find the exact order in O(1).
  auto it = rest_index_.find(e.order_id_);
  if (it == rest_index_.end()) {
    write_quote_lifecycle_row("cancel_rejected",
                              now_ns,
                              e.order_id_,
                              0,
                              Side::Undefined,
                              0,
                              0,
                              0,
                              now_ns,
                              0,
                              0,
                              0,
                              "order_unknown",
                              e.request_id_);
    send_cancel_rejected(e.order_id_, e.request_id_,
                         RejectReason::OrderUnknown, now_ns);
    logger_->debug("Cancel rejected: unknown order {}", e.order_id_);
    return;
  }

  const int32_t idx = it->second;
  const int32_t instrument_id = arena_[idx].instrument_id;
  const Side side = arena_[idx].side;
  const int64_t price = arena_[idx].price;

  auto book_it = resting_.find(instrument_id);
  if (book_it == resting_.end()) {
    const RestingOrder order_snapshot = arena_[idx];
    // Inconsistent state: index exists, book missing
    rest_index_.erase(it);
    arena_.release(idx);
    write_quote_lifecycle_row("cancel_rejected", now_ns, order_snapshot,
                              order_snapshot.leaves, "order_unknown",
                              e.request_id_);
    send_cancel_rejected(e.order_id_, e.request_id_,
                         RejectReason::OrderUnknown, now_ns);
    logger_->debug("Cancel rejected: indexed but no book for order {}", e.order_id_);
    return;
  }

  Book& book = book_it->second;
  bool removed = false;
  const RestingOrder order_snapshot = arena_[idx];

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
    write_quote_lifecycle_row("canceled", now_ns, order_snapshot,
                              order_snapshot.leaves, "canceled",
                              e.request_id_);
    send_cancel_accepted(e.order_id_, e.request_id_,
                         CancelReason::UserRequest, now_ns);
  } else {
    // Index said it existed but we didn't find it — already cleaned up above.
    write_quote_lifecycle_row("cancel_rejected", now_ns, order_snapshot,
                              order_snapshot.leaves, "order_unknown",
                              e.request_id_);
    send_cancel_rejected(e.order_id_, e.request_id_,
                         RejectReason::OrderUnknown, now_ns);
    logger_->debug("Cancel rejected: indexed but not found in book, order {}", e.order_id_);
  }
}


void ExchangeSimulator::send_ioc_remainder_cancel(const PendingEvent& e,
                                                  int64_t qty_left,
                                                  int64_t now_ns) {
  write_quote_lifecycle_row("canceled",
                            now_ns,
                            e.order_id_,
                            e.instrument_id_,
                            e.side_,
                            e.price_,
                            static_cast<int64_t>(e.quantity_),
                            qty_left,
                            now_ns,
                            0,
                            0,
                            0,
                            "system_ioc_remainder",
                            0);
  send_cancel_accepted(e.order_id_, 0, CancelReason::System, now_ns);
}

void ExchangeSimulator::handle_market_order(const PendingEvent& e, int64_t now_ns) {
  send_accepted(e.order_id_, e.instrument_id_, now_ns);

  // Market order: sweep with no price bound.
  const int64_t no_bound = (e.side_ == Side::Buy) ? INT64_MAX : INT64_MIN;
  const int64_t qty_left = sweep_displayed_book(e, no_bound, now_ns);

  if (qty_left > 0) {
    // Remainder unfilled (book too thin or not ready) — cancel it (IOC-style).
    send_ioc_remainder_cancel(e, qty_left, now_ns);
  }
}

void ExchangeSimulator::handle_limit_order(const PendingEvent& e, int64_t now_ns) {
  send_accepted(e.order_id_, e.instrument_id_, now_ns);

  // Sweep only levels at-or-better than our limit — a buy below the market
  // must never fill through its own limit price.
  const int64_t qty_left = sweep_displayed_book(e, e.price_, now_ns);
  if (qty_left <= 0) return;

  // IOC (and FOK, which is not modelled beyond its immediacy) cancels the
  // remainder with a system cancel; a GTC remainder rests in the book with
  // normal queue position.
  const bool immediate = e.time_in_force_ == TimeInForce::Ioc ||
                         e.time_in_force_ == TimeInForce::Fok;
  if (immediate) {
    send_ioc_remainder_cancel(e, qty_left, now_ns);
  } else {
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
      send_executed(e.order_id_, e.side_, instr, level.price, trade_qty, now_ns, /*taker=*/true);
      sweep_fills_.push_back({level.price, trade_qty});
      qty_left -= trade_qty;
    }
  } else {
    // Sweep the bids (highest to lowest price), never below our limit.
    for (const auto& level : incr->bids()) {
      if (qty_left <= 0 || level.price < limit_price) break;
      const int64_t trade_qty = std::min(qty_left, level.quantity);
      if (trade_qty <= 0) continue;
      send_executed(e.order_id_, e.side_, instr, level.price, trade_qty, now_ns, /*taker=*/true);
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
  // Seed the cancel-credit baseline the moment this level first hosts one of
  // our orders. Without it the FIRST observation after placement is spent
  // establishing the baseline, so the earliest cancellations -- the window
  // where queue position matters most -- are silently uncredited.
  if (level.last_displayed < 0) {
    level.last_displayed = o.displayed_depth_at_placement;
    level.traded_since_obs = 0;
  }
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

// Behind-touch queue simulation needs real L2 depth: on an L1-only book,
// displayed_depth_at() returns 0 away from the touch, silently fabricating a
// front-of-queue position. Enforced on BOTH the placement and replace paths.
void ExchangeSimulator::require_l2_provenance_for_behind_touch(
    int32_t instrument_id, Side side, int64_t price) const {
  const auto* external_book = order_book_manager_.get_incremental_book(instrument_id);
  if (!external_book || !external_book->ready()) return;
  const int64_t best_bid = external_book->best_bid_price();
  const int64_t best_ask = external_book->best_ask_price();
  const bool behind_touch =
      (side == Side::Buy && best_bid > 0 && price < best_bid) ||
      (side == Side::Sell && best_ask > 0 && price > best_ask);
  if (behind_touch && !external_book->has_l2_provenance()) {
    throw std::runtime_error(
        "ExchangeSimulator: behind-touch queue simulation requires L2 depth; "
        "L1-only replay would fabricate zero queue ahead");
  }
}

void ExchangeSimulator::rest_post_only_limit(const PendingEvent& e, int64_t now_ns) {
  require_l2_provenance_for_behind_touch(e.instrument_id_, e.side_, e.price_);
  send_accepted(e.order_id_, e.instrument_id_, now_ns);
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
  ro.quantity     = static_cast<int64_t>(e.quantity_);   // taker fills before resting = quantity - leaves
  ro.leaves       = leaves;
  ro.post_only    = post_only;
  ro.ts_ns        = now_ns;
  // Aggregate-L2 queue position: start behind (queue_init_fraction of) the market volume
  // already displayed at our price. (0 if we improve the book / open a new level -> first in line.)
  ro.displayed_depth_at_placement = displayed_depth_at(ro.instrument_id, ro.side, ro.price);
  const int64_t touch = ro.side == Side::Buy
                            ? order_book_manager_.get_best_bid(ro.instrument_id)
                            : order_book_manager_.get_best_ask(ro.instrument_id);
  ro.touch_depth_at_placement =
      ro.price == touch ? ro.displayed_depth_at_placement : 0;
  ro.queue_ahead  = initial_queue_ahead(ro.instrument_id, ro.side, ro.price);
  ro.initial_queue_ahead = ro.queue_ahead;
  ro.traded_ahead = 0;
  ro.credited_cancellation = 0;
  ro.capped_advancement = 0;
  ro.queue_ahead_pre_obs = ro.queue_ahead;
  ro.next = ro.prev = -1;

  Book& book = resting_[ro.instrument_id];
  if (ro.side == Side::Buy) {
    link_back(book.bids[ro.price], idx);
  } else {
    link_back(book.asks[ro.price], idx);
  }
  write_quote_lifecycle_row("accepted", now_ns, ro, static_cast<int64_t>(e.quantity_), "");

  // index by id
  rest_index_[ro.order_id] = idx;
}


// A print at our exact price. If a book observation inside the netting window
// already showed an unexplained reduction at this level, the leading part of
// this print (up to that reduction) is the execution the observation already
// reflected: it consumed the FRONT of the level, so it is applied to each
// order's pre-observation position rather than draining the post-observation
// position a second time. The remainder is fresh volume and is netted against
// the next observation as before.
void ExchangeSimulator::consume_at_price(LevelFifo& level, int64_t& remaining,
                                         int64_t print_size, int64_t now_ns,
                                         const TradeEvent& trigger) {
  int64_t anticipated = 0;
  if (level.unexplained_recent > 0 && trade_netting_window_ns_ > 0 &&
      now_ns - level.last_obs_ts_ns <= trade_netting_window_ns_) {
    anticipated = std::min(remaining, level.unexplained_recent);
    level.unexplained_recent -= anticipated;
  }
  if (anticipated > 0) {
    int64_t budget = anticipated;
    consume_level_anticipated(level, budget, now_ns, trigger);
    remaining -= anticipated;  // these units traded here, ahead of or into us
  }
  // Volume that genuinely traded at this price AFTER the last observation,
  // netted against the next observed depth reduction so cancel credit never
  // double-counts it.
  level.traded_since_obs += std::max<int64_t>(0, print_size - anticipated);
  consume_level(level, remaining, now_ns, trigger);
}

void ExchangeSimulator::pop_front_filled(LevelFifo& level, int32_t idx) {
  RestingOrder& o = arena_[idx];
  rest_index_.erase(o.order_id);
  const int32_t next = o.next;        // pop front of the intrusive FIFO
  level.head = next;
  if (next >= 0) arena_[next].prev = -1;
  else           level.tail = -1;
  arena_.release(idx);
}

void ExchangeSimulator::consume_level_anticipated(LevelFifo& level,
                                                  int64_t& anticipated,
                                                  int64_t now_ns,
                                                  const TradeEvent& trigger) {
  while (level.head >= 0 && anticipated > 0) {
    const int32_t idx = level.head;
    RestingOrder& o = arena_[idx];
    const int64_t eat = std::min(anticipated, o.queue_ahead_pre_obs);
    o.queue_ahead_pre_obs -= eat;
    o.traded_ahead += eat;
    anticipated -= eat;
    // The print proves the true position is at most the pre-observation
    // position less what it consumed; the observation's own bound stands.
    if (o.queue_ahead > o.queue_ahead_pre_obs) o.queue_ahead = o.queue_ahead_pre_obs;
    // Keep provenance closed: initial = traded + credited + capped + queue.
    o.capped_advancement = std::max<int64_t>(
        0, o.initial_queue_ahead - o.queue_ahead - o.traded_ahead -
               o.credited_cancellation);
    // That market depth was queued ahead of every successor at this level
    // too (each order's position was measured against the same displayed
    // depth), so propagate the consumption instead of charging it again.
    for (int32_t j = o.next; eat > 0 && j >= 0; j = arena_[j].next) {
      RestingOrder& s = arena_[j];
      const int64_t s_eat = std::min(eat, s.queue_ahead_pre_obs);
      s.queue_ahead_pre_obs -= s_eat;
      s.traded_ahead += s_eat;
      if (s.queue_ahead > s.queue_ahead_pre_obs) s.queue_ahead = s.queue_ahead_pre_obs;
      s.capped_advancement = std::max<int64_t>(
          0, s.initial_queue_ahead - s.queue_ahead - s.traded_ahead -
                 s.credited_cancellation);
    }
    if (o.queue_ahead_pre_obs > 0 || anticipated <= 0) return;
    const int64_t fill = std::min(anticipated, o.leaves);
    send_executed(o.order_id, o.side, o.instrument_id, o.price, fill, now_ns);
    o.leaves -= fill;
    anticipated -= fill;
    const bool fully_filled = (o.leaves <= 0);
    const int64_t bbo_bid = order_book_manager_.get_best_bid(o.instrument_id);
    const int64_t bbo_ask = order_book_manager_.get_best_ask(o.instrument_id);
    write_quote_lifecycle_row(fully_filled ? "fully_filled" : "filled",
                              now_ns, o, fill, fully_filled ? "filled" : "",
                              0, bbo_bid, bbo_ask, trigger.price_,
                              trigger.size_, trigger.side_);
    if (fully_filled) pop_front_filled(level, idx);
    else return;
  }
}

// Consume one price level's FIFO with a trade budget: trade volume first drains the market
// depth queued ahead of each order, then fills the order (possibly partially) at its limit.
void ExchangeSimulator::consume_level(LevelFifo& level, int64_t& remaining, int64_t now_ns, const TradeEvent& trigger) {
  while (level.head >= 0 && remaining > 0) {
    const int32_t idx = level.head;
    RestingOrder& o = arena_[idx];

    // (1) trade volume first eats the market depth queued ahead of us
    const int64_t eat = std::min(remaining, o.queue_ahead);
    if (eat > 0) {
      o.queue_ahead -= eat;
      o.queue_ahead_pre_obs = std::max<int64_t>(0, o.queue_ahead_pre_obs - eat);
      o.traded_ahead += eat;
      remaining     -= eat;
      // That market depth was queued ahead of every successor at this level
      // too (each order's queue_ahead was measured against the same displayed
      // depth), so propagate the consumption — otherwise the shared budget is
      // double-charged and fills are underestimated.
      for (int32_t j = o.next; j >= 0; j = arena_[j].next) {
        RestingOrder& s = arena_[j];
        const int64_t s_eat = std::min(eat, s.queue_ahead);
        s.queue_ahead -= s_eat;
        s.queue_ahead_pre_obs = std::max<int64_t>(0, s.queue_ahead_pre_obs - s_eat);
        s.traded_ahead += s_eat;
      }
    }
    if (o.queue_ahead > 0 || remaining <= 0) return;  // still behind the queue, or budget spent

    // (2) we are now at the front of the queue -> fill (possibly partial) at OUR limit price
    const int64_t fill = std::min(remaining, o.leaves);
    send_executed(o.order_id, o.side, o.instrument_id, o.price, fill, now_ns);
    o.leaves  -= fill;
    remaining -= fill;
    const bool fully_filled = (o.leaves <= 0);
    const int64_t bbo_bid = order_book_manager_.get_best_bid(o.instrument_id);
    const int64_t bbo_ask = order_book_manager_.get_best_ask(o.instrument_id);
    write_quote_lifecycle_row(fully_filled ? "fully_filled" : "filled",
                              now_ns,
                              o,
                              fill,
                              fully_filled ? "filled" : "",
                              0,
                              bbo_bid,
                              bbo_ask,
                              trigger.price_,
                              trigger.size_,
                              trigger.side_);

    if (fully_filled) {
      pop_front_filled(level, idx);
    } else {
      return;                       // partial fill -> order keeps resting at the front
    }
  }
}

// A print strictly through this level proves the historical queue ahead has
// gone. Because our order is counterfactual, only the print's remaining
// observed quantity is a defensible fill budget; it may fill us partially.
void ExchangeSimulator::fill_level_through(LevelFifo& level,
                                           int64_t& remaining,
                                           int64_t now_ns,
                                           const TradeEvent& trigger) {
  while (level.head >= 0 && remaining > 0) {
    const int32_t idx = level.head;
    RestingOrder& o = arena_[idx];
    o.queue_ahead = 0;
    const int64_t fill = std::min(remaining, o.leaves);
    send_executed(o.order_id, o.side, o.instrument_id, o.price, fill, now_ns);
    o.leaves -= fill;
    remaining -= fill;
    const bool fully_filled = o.leaves <= 0;
    const int64_t bbo_bid = order_book_manager_.get_best_bid(o.instrument_id);
    const int64_t bbo_ask = order_book_manager_.get_best_ask(o.instrument_id);
    write_quote_lifecycle_row(fully_filled ? "fully_filled" : "filled",
                              now_ns,
                              o,
                              fill,
                              "trade_through",
                              0,
                              bbo_bid,
                              bbo_ask,
                              trigger.price_,
                              trigger.size_,
                              trigger.side_);
    if (!fully_filled) return;
    pop_front_filled(level, idx);
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

void ExchangeSimulator::send_accepted(int64_t order_id,
                                      int32_t instrument_id,
                                      int64_t now_ns) {

  // Create response slot and use placement new (like RingBufferWriter does)
  MessageSlot response_slot;

  // Use placement new to construct AcceptedEvent directly in the slot
  auto* const accepted = new (response_slot.raw_data()) AcceptedEvent();

  // Fill the response fields
  accepted->order_id_ = order_id;
  accepted->timestamp_ns_ = now_ns;
  const int64_t best_bid = order_book_manager_.get_best_bid(instrument_id);
  const int64_t best_ask = order_book_manager_.get_best_ask(instrument_id);
  if (best_bid > 0 && best_ask > 0 &&
      best_bid <= std::numeric_limits<int64_t>::max() - best_ask) {
    accepted->backtest_arrival_mid_x2_ = best_bid + best_ask;
  }

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

void ExchangeSimulator::send_replace_accepted(int64_t order_id,
                                              int64_t request_id,
                                              int32_t instrument_id,
                                              int64_t now_ns) {

  MessageSlot response_slot;

  auto* const replace_accepted = new (response_slot.raw_data()) ReplaceAcceptedEvent();
  replace_accepted->timestamp_ns_ = now_ns;
  replace_accepted->order_id_ = order_id;
  replace_accepted->request_id_ = request_id;
  const int64_t best_bid = order_book_manager_.get_best_bid(instrument_id);
  const int64_t best_ask = order_book_manager_.get_best_ask(instrument_id);
  if (best_bid > 0 && best_ask > 0 &&
      best_bid <= std::numeric_limits<int64_t>::max() - best_ask) {
    replace_accepted->backtest_arrival_mid_x2_ = best_bid + best_ask;
  }

  response_handler_->on_exchange_response(response_slot);
}

void ExchangeSimulator::send_replace_rejected(int64_t order_id,
                                              int64_t request_id,
                                              RejectReason reason,
                                              int64_t now_ns) {

  MessageSlot response_slot;

  auto* const replace_rejected = new (response_slot.raw_data()) ReplaceRejectedEvent();
  replace_rejected->timestamp_ns_ = now_ns;
  replace_rejected->order_id_ = order_id;
  replace_rejected->request_id_ = request_id;
  replace_rejected->reject_reason_ = reason;

  response_handler_->on_exchange_response(response_slot);
}

void ExchangeSimulator::send_cancel_accepted(int64_t order_id,
                                             int64_t request_id,
                                             CancelReason reason,
                                             int64_t now_ns) {

  MessageSlot response_slot;

  auto* const cancel_accepted = new (response_slot.raw_data()) CancelAcceptedEvent();
  cancel_accepted->timestamp_ns_ = now_ns;
  cancel_accepted->order_id_ = order_id;
  cancel_accepted->request_id_ = request_id;
  cancel_accepted->cancel_reason_ = reason;

  response_handler_->on_exchange_response(response_slot);
}

void ExchangeSimulator::send_cancel_rejected(int64_t order_id,
                                             int64_t request_id,
                                             RejectReason reason,
                                             int64_t now_ns) {

  MessageSlot response_slot;

  auto* const cancel_rejected = new (response_slot.raw_data()) CancelRejectedEvent();
  cancel_rejected->timestamp_ns_ = now_ns;
  cancel_rejected->order_id_ = order_id;
  cancel_rejected->request_id_ = request_id;
  cancel_rejected->reject_reason_ = reason;

  response_handler_->on_exchange_response(response_slot);
}

void ExchangeSimulator::send_executed(int64_t order_id, Side side, int32_t instrument_id,
                                      int64_t px, int64_t qty, int64_t now_ns,
                                      bool taker) {

  MessageSlot response_slot;

  auto* const executed = new (response_slot.raw_data()) ExecutedEvent();

  executed->timestamp_ns_ = now_ns;
  executed->order_id_ = order_id;
  executed->side_ = side;
  executed->instrument_id_ = instrument_id;
  executed->last_price_ = px;
  executed->last_quantity_ = qty;
  // Taker fills carry the instrument's taker fee (ppb of notional) as a
  // commission in quote currency, mirroring what a live fill would report.
  if (const auto* asset =
          reflex::AssetInfoManager::get_by_instrument_id(instrument_id);
      taker && asset != nullptr) {
    const double notional_quote =
        std::abs(fixed_to_double(qty) * asset->asset_size(1.0)) * fixed_to_double(px);
    const double commission_quote =
        notional_quote * static_cast<double>(asset->taker_fee_ppb_) / 1e9;
    if (std::isfinite(commission_quote) && commission_quote >= 0.0) {
      executed->commission_ = CodecUtils::encode_price(commission_quote);
      executed->commission_valid_ = 1;
    }
  }
  const int64_t best_bid = order_book_manager_.get_best_bid(instrument_id);
  const int64_t best_ask = order_book_manager_.get_best_ask(instrument_id);
  if (executed->commission_valid_ == 0 && best_bid > 0 && best_ask > 0 &&
      best_bid <= std::numeric_limits<int64_t>::max() - best_ask) {
    executed->backtest_execution_mid_x2_ = best_bid + best_ask;
  }

  response_handler_->on_exchange_response(response_slot);
}

} // namespace reflex::backtest
