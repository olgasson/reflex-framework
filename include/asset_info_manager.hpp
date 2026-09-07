#pragma once

#include <array>
#include <cstdint>
#include <string_view>
#include <unordered_map>

#include "messages.hpp"

namespace reflex {

enum class Currency : int8_t {
    BTC = 1, ETH = 2, LTC = 3, SOL = 4, ADA = 5, XRP = 6, AVAX = 7, DOGE = 8,
    USDT = 10, TUSD = 11, FDUSD = 12,
    USD = 20, EUR = 21
};

enum class InstrumentType : int8_t {
    SPOT = 1,
    FUTURE = 2
};

enum class SizeUnit : int8_t {
    ASSET = 1,
    CONTRACTS = 2
};

enum class Tenor : int8_t {
    PERPETUAL = 1
};

// Compact instrument representation - no polymorphism, just data
struct Instrument {
    InstrumentType type_;
    Currency base_currency_;
    Currency quote_currency_;
    Tenor tenor_;  // Only used for futures

    constexpr Instrument(InstrumentType t, Currency base, Currency quote, Tenor ten = Tenor::PERPETUAL)
        : type_(t), base_currency_(base), quote_currency_(quote), tenor_(ten) {}

    // Fast equality - just integer comparisons
    constexpr bool operator==(const Instrument& other) const noexcept {
        return type_ == other.type_ &&
               base_currency_ == other.base_currency_ &&
               quote_currency_ == other.quote_currency_ &&
               (type_ == InstrumentType::SPOT || tenor_ == other.tenor_);
    }

    // Hash for unordered_map - single computation
    constexpr int32_t hash() const noexcept {
        return (static_cast<int32_t>(type_) << 24) |
               (static_cast<int32_t>(base_currency_) << 16) |
               (static_cast<int32_t>(quote_currency_) << 8) |
               static_cast<int32_t>(tenor_);
    }
};

// Zero-allocation AssetInfo - all POD for cache efficiency
struct AssetInfo {
    int32_t instrument_id_;
    Exchange exchange_;
    Instrument instrument_;

    // Trading parameters - stored as integers for exact arithmetic
    int64_t tick_increment_;    // tick size in nano units
    int64_t size_increment_;    // size increment in nano units
    int64_t min_order_size_;    // minimum order size in nano units
    int64_t contract_size_;     // contract size in nano units

    int32_t taker_fee_ppb_;          // taker fee, parts-per-billion of notional (1 bps = 100,000 ppb)
    int32_t maker_fee_ppb_;          // maker fee in ppb; NEGATIVE = rebate
    SizeUnit size_unit_;

    // Pre-computed string views for zero-allocation lookups
    const char* exchange_symbol_;     // Points to static string literal
    const char* parquet_symbol_;      // Points to static string literal

    // Add default constructor
    constexpr AssetInfo() 
        : instrument_id_(0), exchange_(Exchange::Binance),
          instrument_(InstrumentType::SPOT, Currency::BTC, Currency::USD),
          tick_increment_(0), size_increment_(0), min_order_size_(0),
          contract_size_(0), taker_fee_ppb_(0), maker_fee_ppb_(0),
          size_unit_(SizeUnit::ASSET), exchange_symbol_(nullptr), parquet_symbol_(nullptr) {}

    constexpr AssetInfo(const int32_t id, Exchange exch, const Instrument inst,
                       const char* ex_symbol, const char* pq_symbol,
                       int64_t tick_inc, int64_t size_inc, int64_t min_size,
                       int64_t contract_size, int32_t taker_fee_ppb, int32_t maker_fee_ppb,
                       SizeUnit unit)
        : instrument_id_(id), exchange_(exch), instrument_(inst),
          tick_increment_(tick_inc), size_increment_(size_inc),
          min_order_size_(min_size), contract_size_(contract_size),
          taker_fee_ppb_(taker_fee_ppb), maker_fee_ppb_(maker_fee_ppb), size_unit_(unit),
          exchange_symbol_(ex_symbol), parquet_symbol_(pq_symbol) {}

    static constexpr double kFixed = 1e-8;

    constexpr double lot_size() const noexcept {
      return static_cast<double>(size_increment_) * kFixed;
    }
    constexpr double contract_size() const noexcept {
      return static_cast<double>(contract_size_) * kFixed;
    }
    constexpr double tick() const noexcept {
      return static_cast<double>(tick_increment_) * kFixed;
    }
    constexpr double price_factor()   const noexcept { return 1.0 / tick(); }    // convert price → “nano‐ticks”
    constexpr double asset_size(const double raw) const noexcept
    {
      // Exchange sends *contracts*? convert to underlying asset amount.
      return (size_unit_ == SizeUnit::CONTRACTS)
               ? raw * contract_size()
               : raw;
    }
};

// Hash function for Instrument
struct InstrumentHash {
    constexpr std::size_t operator()(const Instrument& inst) const noexcept {
        return inst.hash();
    }
};

// Ultra-fast AssetInfoManager - optimized for trading hotpath
class AssetInfoManager {
public:
    // Maximum instrument ID for direct array access
    static constexpr int32_t MAX_INSTRUMENT_ID = 20000;

    // Initialize all static data - call once at startup
    static void initialize() noexcept;

    // Ultra-fast lookups - O(1) with no allocations

    // Fastest: Direct array access by instrument ID
    static inline const AssetInfo* get_asset_info_fast(int32_t instrument_id) noexcept {
        // Single unsigned comparison covers both the < 0 and >= MAX bounds.
        return (static_cast<uint32_t>(instrument_id) < static_cast<uint32_t>(MAX_INSTRUMENT_ID))
                   ? instrument_store_[instrument_id]
                   : nullptr;
    }

    // Fast: Hash map lookup by exchange symbol
    static const AssetInfo* get_asset_info(std::string_view exchange_symbol, Exchange exchange) noexcept;

    // Fast: Hash map lookup by instrument
    static const AssetInfo* get_asset_info(const Instrument& instrument, Exchange exchange) noexcept;

    // Utility methods
    static bool is_initialized() noexcept { return initialized_; }

    static constexpr std::string_view currency_to_string(Currency c) noexcept {
        switch (c) {
            case Currency::BTC: return "BTC";
            case Currency::ETH: return "ETH";
            case Currency::LTC: return "LTC";
            case Currency::SOL: return "SOL";
            case Currency::ADA: return "ADA";
            case Currency::XRP: return "XRP";
            case Currency::AVAX: return "AVAX";
            case Currency::DOGE: return "DOGE";
            case Currency::USDT: return "USDT";
            case Currency::TUSD: return "TUSD";
            case Currency::FDUSD: return "FDUSD";
            case Currency::USD: return "USD";
            case Currency::EUR: return "EUR";
            default: return "UNKNOWN";
        }
    }

    static Currency currency_from_string(std::string_view name) noexcept;

    static const AssetInfo* get_by_instrument_id(int32_t instrument_id);

   private:
    // Direct array for O(1) access by instrument ID - fastest possible lookup
    static std::array<const AssetInfo*, MAX_INSTRUMENT_ID> instrument_store_;

    // Hash maps for other lookup patterns - still very fast
    static std::unordered_map<int64_t, const AssetInfo*> exchange_symbol_map_;  // (exchange << 32) | symbol_hash
    static std::unordered_map<int64_t, const AssetInfo*> exchange_instrument_map_;  // (exchange << 32) | instrument_hash

    // Static storage for all AssetInfo objects - no dynamic allocation
    static std::array<AssetInfo, 256> asset_storage_;
    static int32_t storage_index_;

    static bool initialized_;

    // Helper methods
    static const AssetInfo* create_asset_info(int32_t instrument_id, Exchange exchange,
                                             Instrument instrument, const char* exchange_symbol,
                                             const char* parquet_symbol, int64_t tick_increment,
                                             int64_t size_increment, int64_t min_order_size,
                                             int64_t contract_size, int32_t taker_fee_ppb,
                                             int32_t maker_fee_ppb, SizeUnit size_unit) noexcept;

    static constexpr int64_t make_exchange_symbol_key(Exchange exchange, uint32_t symbol_hash) noexcept;
    static constexpr int64_t make_exchange_instrument_key(Exchange exchange, uint32_t instrument_hash) noexcept;
    static uint32_t string_hash(std::string_view str) noexcept;

    static void init_spot_instruments() noexcept;
    static void init_future_instruments() noexcept;

    // Fee constants in PPB of notional (1 bps = 100,000 ppb). A NEGATIVE maker
    // fee is a rebate. Venue fee schedules are tiered and change over time;
    // these are base-tier defaults, override per backtest where needed.
    struct Fees {
        static constexpr int32_t BINANCE_TAKER = 1'000'000;             // 10 bps spot taker
        static constexpr int32_t BINANCE_MAKER = 1'000'000;             // 10 bps spot maker
        static constexpr int32_t BINANCE_DERIVATIVES_TAKER = 400'000;   // 4 bps
        static constexpr int32_t BINANCE_DERIVATIVES_MAKER = 200'000;   // 2 bps
        static constexpr int32_t OKX_TAKER = 500'000;   // OKX USDT-perp base taker = 0.05% (was 20 bps = wrong)
        static constexpr int32_t OKX_MAKER = 0;         // base is 2 bps; 0 ~= mid-VIP target tier
    };
};

// Inline implementations for hotpath functions
inline const AssetInfo* AssetInfoManager::get_asset_info(std::string_view exchange_symbol, Exchange exchange) noexcept {
    const uint32_t symbol_hash = string_hash(exchange_symbol);
    const int64_t key = make_exchange_symbol_key(exchange, symbol_hash);

    auto it = exchange_symbol_map_.find(key);
    return (it != exchange_symbol_map_.end()) ? it->second : nullptr;
}

inline const AssetInfo* AssetInfoManager::get_asset_info(const Instrument& instrument, Exchange exchange) noexcept {
    const int64_t key = make_exchange_instrument_key(exchange, static_cast<uint32_t>(instrument.hash()));

    auto it = exchange_instrument_map_.find(key);
    return (it != exchange_instrument_map_.end()) ? it->second : nullptr;
}

// Keys are built entirely in unsigned arithmetic: the hash occupies the low 32
// bits only, so a hash with the top bit set cannot sign-extend and erase the
// exchange bits.
constexpr int64_t AssetInfoManager::make_exchange_symbol_key(Exchange exchange, uint32_t symbol_hash) noexcept {
    return static_cast<int64_t>((static_cast<uint64_t>(exchange) << 32) | static_cast<uint64_t>(symbol_hash));
}

constexpr int64_t AssetInfoManager::make_exchange_instrument_key(Exchange exchange, uint32_t instrument_hash) noexcept {
    return static_cast<int64_t>((static_cast<uint64_t>(exchange) << 32) | static_cast<uint64_t>(instrument_hash));
}

} // namespace reflex