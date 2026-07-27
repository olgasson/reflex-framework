#pragma once
#include <deque>
#include <cmath>
#include <string>
#include <sstream>
#include <algorithm>
#include "messages.hpp"              // for Side
#include "asset_info_manager.hpp"    // your AssetInfoManager

namespace reflex {

class RiskEngine {
public:
  struct Fill {
    Side   side;            // Buy/Sell
    double qty_contracts;   // contracts, human units (e.g. 0.10)
    double px;              // price in USD (e.g. 118572.70)
    bool   taker{false};    // fee tier to apply
    int64_t ts_ns{0};
  };

  struct Snapshot {
    double position_contracts;
    double position_base;
    double avg_entry_quote;
    double realized_pnl_quote;
    double unrealized_pnl_quote;
    double fees_quote;
  };


  explicit RiskEngine(int32_t instrument_id)
  {
    const auto* ai = AssetInfoManager::get_by_instrument_id(instrument_id);
    // Defaults if missing
    ct_val_btc_ = ai ? fp_to_double(ai->contract_size_) : 0.01; // 0.01 BTC/contract
    maker_fee_  = ai ? (ai->maker_fee_bps_ / 10000.0) : 0.0;
    taker_fee_  = ai ? (ai->taker_fee_bps_ / 10000.0) : 0.0002;
  }

  // Convert your engine's fixed-point ints to doubles before calling these:
  //   qty_contracts = qty_fp8 / 1e8
  //   px            = px_fp8  / 1e8
  //   mark_px       = mid_fp8 / 1e8
  void on_fill(const Fill& f)
  {
    const double dir = (f.side == Side::Buy ? +1.0 : -1.0);
    const double d_contracts = dir * f.qty_contracts;
    const double d_base_btc  = d_contracts * ct_val_btc_;      // BTC
    const double notional    = std::abs(d_base_btc) * f.px;    // USD

    const double fee_rate = f.taker ? taker_fee_ : maker_fee_;
    fees_usd_ += notional * fee_rate;

    // Same direction or flat → append a leg (priced at fill px, in base units)
    if (pos_base_btc_ == 0.0 || same_sign(pos_base_btc_, d_base_btc)) {
      fifo_.push_back({f.px, d_base_btc});  // signed base
      pos_base_btc_ += d_base_btc;
      recalc_avg_entry();                   // optional; used in diag
      return;
    }

    // Closing (opposite sign): consume FIFO legs in base units
    double remaining = std::abs(d_base_btc);

    while (remaining > 1e-15 && !fifo_.empty()) {
      auto& leg = fifo_.front();                  // leg.qty_base is signed
      const double leg_abs  = std::abs(leg.qty_base);
      const double close    = std::min(remaining, leg_abs);
      const double leg_sign = (leg.qty_base >= 0.0 ? +1.0 : -1.0);

      // Realized: (exit - entry) * closed_base_with_leg_sign
      realized_pnl_usd_ += (f.px - leg.px) * (close * leg_sign);

      // Reduce leg & position toward zero
      leg.qty_base    -= close * leg_sign;
      pos_base_btc_   -= close * leg_sign;
      remaining       -= close;

      if (std::abs(leg.qty_base) <= 1e-15) fifo_.pop_front();
    }

    // If we crossed through zero and still have residual, it becomes a new leg
    if (remaining > 1e-15) {
      const double trade_sign = (d_base_btc > 0.0 ? +1.0 : -1.0);  // sign of the incoming trade
      const double residual   = remaining * trade_sign;
      fifo_.push_back({f.px, residual});
      pos_base_btc_ += residual;
    }

    recalc_avg_entry();

    // Recompute unrealized after fill if we have a mark
    if (std::abs(pos_base_btc_) < 1e-12) {
      // Flat — clamp to exactly flat and zero UPnL
      pos_base_btc_   = 0.0;
      fifo_.clear();
      unreal_pnl_usd_ = 0.0;
    } else if (mark_px_ > 0.0) {
      unreal_pnl_usd_ = (mark_px_ - avg_entry_px_) * pos_base_btc_;
    }
  }

  // Override the instrument's default fees (bps). Used to backtest different VIP tiers.
  void set_fees(double maker_bps, double taker_bps) {
    maker_fee_ = maker_bps / 10000.0;
    taker_fee_ = taker_bps / 10000.0;
  }

  void on_mark_price(double mark_px_usd)
  {
    mark_px_ = mark_px_usd;
    // Unrealized PnL: (mark - avg_entry) * position (in base)
    unreal_pnl_usd_ = (mark_px_ - avg_entry_px_) * pos_base_btc_;
  }

  // ------------ Accessors / Diag ------------
  double pos_contracts()   const { return (ct_val_btc_ > 0.0) ? pos_base_btc_ / ct_val_btc_ : 0.0; }
  double pos_base_btc()    const { return pos_base_btc_; }
  double avg_entry_px()    const { return avg_entry_px_; }
  double mark_px()         const { return mark_px_; }
  double realized_pnl_usd()const { return realized_pnl_usd_; }
  double unrealized_pnl_usd() const { return unreal_pnl_usd_; }
  double fees_usd()        const { return fees_usd_; }
  double contract_value_base() const { return ct_val_btc_; }

  double total_pnl_quote() const noexcept {
    // total = realized + unrealized - fees
    return realized_pnl_usd_ + unreal_pnl_usd_ - fees_usd_;
  }

  Snapshot snapshot() const noexcept {
    return Snapshot{
      .position_contracts   = pos_contracts(),        // uses accessor
      .position_base        = pos_base_btc_,          // signed base (BTC)
      .avg_entry_quote      = avg_entry_px_,          // USD
      .realized_pnl_quote   = realized_pnl_usd_,      // USD
      .unrealized_pnl_quote = unreal_pnl_usd_,        // USD
      .fees_quote           = fees_usd_               // USD
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
       << " avg_entry="<< avg_entry_px_
       << " lots(L="   << L << ",S=" << S << ")";
    return os.str();
  }

  void reset() {
    fifo_.clear();
    pos_base_btc_ = avg_entry_px_ = mark_px_ = 0.0;
    realized_pnl_usd_ = unreal_pnl_usd_ = fees_usd_ = 0.0;
  }

private:
  struct Leg {
    double px;         // entry price USD
    double qty_base;   // signed base (BTC)
  };

  static inline bool same_sign(double a, double b) {
    return (a == 0.0 || b == 0.0) ? (a == b) : (a > 0) == (b > 0);
  }

  static inline double fp_to_double(uint64_t fp) {
    // your fp() helper encodes 1e-8 fixed point in an unsigned 64;
    // if you’re already storing doubles in AssetInfo, just return it.
    return static_cast<double>(fp) / 100'000'000.0;
  }

  void recalc_avg_entry() {
    if (fifo_.empty()) { avg_entry_px_ = 0.0; return; }
    // Weighted by absolute base (so long/short sides give a meaningful center)
    double wsum = 0.0, asum = 0.0;
    for (const auto& l : fifo_) { const double a = std::abs(l.qty_base); wsum += l.px * a; asum += a; }
    avg_entry_px_ = (asum > 0.0 ? wsum / asum : mark_px_);
  }

  // ---- Params from instrument ----
  double ct_val_btc_{0.01}; // BTC per contract (OKX BTC-USDT-SWAP)
  double maker_fee_{0.0};    // fraction, e.g. 0.0000
  double taker_fee_{0.0002};

  // ---- State (doubles, human units) ----
  std::deque<Leg> fifo_;     // FIFO in base BTC
  double pos_base_btc_{0.0};
  double avg_entry_px_{0.0};
  double mark_px_{0.0};

  double realized_pnl_usd_{0.0};
  double unreal_pnl_usd_{0.0};
  double fees_usd_{0.0};
};

} // namespace reflex
