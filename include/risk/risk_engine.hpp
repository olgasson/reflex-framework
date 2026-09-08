#pragma once
#include <deque>
#include <cmath>
#include <string>
#include <sstream>
#include <algorithm>
#include <cstdint>
#include "messages.hpp"
#include "asset_info_manager.hpp"

namespace reflex {

class RiskEngine {
public:
  struct Fill {
    Side   side;
    double qty_contracts;
    double px;
    bool   taker{false};
    int64_t ts_ns{0};
    double fee_quote{0.0};
    bool fee_quote_valid{false};
  };

  struct Snapshot {
    double position_contracts;
    double position_base;
    double avg_entry_quote;
    double realized_pnl_quote;
    double unrealized_pnl_quote;
    double fees_quote;
    double funding_pnl_quote;
  };

  struct PnlAttribution {
    double execution_edge_quote{0.0};
    double inventory_carry_quote{0.0};
    double fees_quote{0.0};
    double funding_pnl_quote{0.0};
    double attributed_total_quote{0.0};
    double fifo_total_quote{0.0};
    double identity_residual_quote{0.0};
    double identity_tolerance_quote{0.0};
    uint64_t fills_before_first_mark{0};
    bool valid{false};

    [[nodiscard]] bool closes() const noexcept {
      return valid &&
             std::abs(identity_residual_quote) <= identity_tolerance_quote;
    }
  };


  explicit RiskEngine(int32_t instrument_id)
  {
    const auto* ai = AssetInfoManager::get_by_instrument_id(instrument_id);
    ct_val_btc_ = ai ? ai->asset_size(1.0) : 0.01;
    maker_fee_  = ai ? (ai->maker_fee_ppb_ / 1e9) : 0.0;
    taker_fee_  = ai ? (ai->taker_fee_ppb_ / 1e9) : 0.0002;
  }

  void on_fill(const Fill& f)
  {
    const double dir = (f.side == Side::Buy ? +1.0 : -1.0);
    const double d_contracts = dir * f.qty_contracts;
    const double d_base_btc  = d_contracts * ct_val_btc_;
    const double notional    = std::abs(d_base_btc) * f.px;

    const double fee_rate = f.taker ? taker_fee_ : maker_fee_;
    const double booked_fee = f.fee_quote_valid && std::isfinite(f.fee_quote)
                                  ? f.fee_quote
                                  : notional * fee_rate;
    fees_usd_ += booked_fee;
    if (f.taker) {
      taker_fees_usd_ += booked_fee;
    } else {
      maker_notional_usd_ += notional;
    }

    if (mark_px_ > 0.0) {
      execution_edge_quote_ += d_base_btc * (mark_px_ - f.px);
    } else {
      ++fills_before_first_mark_;
      attribution_valid_ = false;
    }

    if (pos_base_btc_ == 0.0 || same_sign(pos_base_btc_, d_base_btc)) {
      fifo_.push_back({f.px, d_base_btc});
      pos_base_btc_ += d_base_btc;
      recalc_avg_entry();
      recompute_unrealized();
      return;
    }

    double remaining = std::abs(d_base_btc);

    while (remaining > 1e-15 && !fifo_.empty()) {
      auto& leg = fifo_.front();
      const double leg_abs  = std::abs(leg.qty_base);
      const double close    = std::min(remaining, leg_abs);
      const double leg_sign = (leg.qty_base >= 0.0 ? +1.0 : -1.0);

      realized_pnl_usd_ += (f.px - leg.px) * (close * leg_sign);

      leg.qty_base    -= close * leg_sign;
      pos_base_btc_   -= close * leg_sign;
      remaining       -= close;

      if (std::abs(leg.qty_base) <= 1e-15) fifo_.pop_front();
    }

    if (remaining > 1e-15) {
      const double trade_sign = (d_base_btc > 0.0 ? +1.0 : -1.0);
      const double residual   = remaining * trade_sign;
      fifo_.push_back({f.px, residual});
      pos_base_btc_ += residual;
    }

    recalc_avg_entry();

    recompute_unrealized();
  }

  void set_fees(double maker_bps, double taker_bps) {
    maker_fee_ = maker_bps / 10000.0;
    taker_fee_ = taker_bps / 10000.0;
  }

  void on_mark_price(double mark_px_usd)
  {
    if (mark_px_usd <= 0.0) return;
    if (mark_px_ > 0.0) {
      inventory_carry_quote_ +=
          pos_base_btc_ * (mark_px_usd - mark_px_);
    }
    mark_px_ = mark_px_usd;
    recompute_unrealized();
  }

  void on_funding_payment(double payment_quote)
  {
    if (!std::isfinite(payment_quote)) return;
    funding_pnl_usd_ += payment_quote;
  }

  bool initialize_position(double position_contracts,
                           double average_entry_quote) noexcept {
    if (!std::isfinite(position_contracts) ||
        !std::isfinite(average_entry_quote) ||
        (position_contracts != 0.0 && average_entry_quote <= 0.0)) {
      return false;
    }
    reset();
    if (position_contracts == 0.0) return true;
    pos_base_btc_ = position_contracts * ct_val_btc_;
    fifo_.push_back({average_entry_quote, pos_base_btc_});
    avg_entry_px_ = average_entry_quote;
    attribution_valid_ = false;
    recompute_unrealized();
    return true;
  }

  double pos_contracts()   const { return (ct_val_btc_ > 0.0) ? pos_base_btc_ / ct_val_btc_ : 0.0; }
  double pos_base_btc()    const { return pos_base_btc_; }
  double avg_entry_px()    const { return avg_entry_px_; }
  double mark_px()         const { return mark_px_; }
  double realized_pnl_usd()const { return realized_pnl_usd_; }
  double unrealized_pnl_usd() const { return unreal_pnl_usd_; }
  double fees_usd()        const { return fees_usd_; }
  double funding_pnl_usd() const { return funding_pnl_usd_; }
  double contract_value_base() const { return ct_val_btc_; }

  double total_pnl_quote() const noexcept {
    return realized_pnl_usd_ + unreal_pnl_usd_ - fees_usd_ +
           funding_pnl_usd_;
  }

  double fee_normalized_pnl_quote(double assumed_maker_rate) const noexcept {
    return total_pnl_quote() + fees_usd_ -
           (taker_fees_usd_ + maker_notional_usd_ * assumed_maker_rate);
  }
  double maker_notional_usd() const noexcept { return maker_notional_usd_; }
  double taker_fees_usd() const noexcept { return taker_fees_usd_; }

  Snapshot snapshot() const noexcept {
    return Snapshot{
      .position_contracts   = pos_contracts(),
      .position_base        = pos_base_btc_,
      .avg_entry_quote      = avg_entry_px_,
      .realized_pnl_quote   = realized_pnl_usd_,
      .unrealized_pnl_quote = unreal_pnl_usd_,
      .fees_quote           = fees_usd_,
      .funding_pnl_quote    = funding_pnl_usd_
    };
  }

  [[nodiscard]] PnlAttribution pnl_attribution() const noexcept {
    const double attributed_total = execution_edge_quote_ +
                                    inventory_carry_quote_ - fees_usd_ +
                                    funding_pnl_usd_;
    const double fifo_total = total_pnl_quote();
    const double residual = fifo_total - attributed_total;
    const double scale = std::abs(execution_edge_quote_) +
                         std::abs(inventory_carry_quote_) +
                         std::abs(fees_usd_) +
                         std::abs(funding_pnl_usd_) +
                         std::abs(fifo_total);
    return PnlAttribution{
      .execution_edge_quote = execution_edge_quote_,
      .inventory_carry_quote = inventory_carry_quote_,
      .fees_quote = fees_usd_,
      .funding_pnl_quote = funding_pnl_usd_,
      .attributed_total_quote = attributed_total,
      .fifo_total_quote = fifo_total,
      .identity_residual_quote = residual,
      .identity_tolerance_quote = 1e-8 * std::max(1.0, scale),
      .fills_before_first_mark = fills_before_first_mark_,
      .valid = attribution_valid_ && mark_px_ > 0.0
    };
  }

  std::string diag_string() const {
    std::ostringstream os;
    int64_t L = 0, S = 0;
    for (const auto& leg : fifo_) (leg.qty_base >= 0 ? ++L : ++S);
    os.setf(std::ios::fixed); os.precision(8);
    os << "pos_contr=" << pos_contracts()
       << " pos_base=" << pos_base_btc_
       << " mark="     << mark_px_
       << " RPnL="     << realized_pnl_usd_
       << " UPnL="     << unreal_pnl_usd_
       << " fees="     << fees_usd_
       << " funding="  << funding_pnl_usd_
       << " avg_entry="<< avg_entry_px_
       << " lots(L="   << L << ",S=" << S << ")";
    return os.str();
  }

  void reset() {
    fifo_.clear();
    pos_base_btc_ = avg_entry_px_ = mark_px_ = 0.0;
    realized_pnl_usd_ = unreal_pnl_usd_ = fees_usd_ = 0.0;
    funding_pnl_usd_ = 0.0;
    execution_edge_quote_ = inventory_carry_quote_ = 0.0;
    fills_before_first_mark_ = 0;
    attribution_valid_ = true;
  }

private:
  struct Leg {
    double px;
    double qty_base;
  };

  static inline bool same_sign(double a, double b) {
    return (a == 0.0 || b == 0.0) ? (a == b) : (a > 0) == (b > 0);
  }

  static inline double fp_to_double(uint64_t fp) {
    return static_cast<double>(fp) / 100'000'000.0;
  }

  void recalc_avg_entry() {
    if (fifo_.empty()) { avg_entry_px_ = 0.0; return; }
    double wsum = 0.0, asum = 0.0;
    for (const auto& l : fifo_) { const double a = std::abs(l.qty_base); wsum += l.px * a; asum += a; }
    avg_entry_px_ = (asum > 0.0 ? wsum / asum : mark_px_);
  }

  void recompute_unrealized() {
    if (std::abs(pos_base_btc_) < 1e-12) {
      pos_base_btc_ = 0.0;
      fifo_.clear();
      unreal_pnl_usd_ = 0.0;
    } else if (mark_px_ > 0.0) {
      unreal_pnl_usd_ = (mark_px_ - avg_entry_px_) * pos_base_btc_;
    } else {
      unreal_pnl_usd_ = 0.0;
    }
  }

  double ct_val_btc_{0.01};
  double maker_fee_{0.0};
  double taker_fee_{0.0002};

  std::deque<Leg> fifo_;
  double pos_base_btc_{0.0};
  double avg_entry_px_{0.0};
  double mark_px_{0.0};

  double realized_pnl_usd_{0.0};
  double unreal_pnl_usd_{0.0};
  double fees_usd_{0.0};
  double maker_notional_usd_{0.0};
  double taker_fees_usd_{0.0};
  double funding_pnl_usd_{0.0};
  double execution_edge_quote_{0.0};
  double inventory_carry_quote_{0.0};
  uint64_t fills_before_first_mark_{0};
  bool attribution_valid_{true};
};

}
