#include "../include/asset_info_manager.hpp"

#include <mutex>

namespace reflex {

// Helper: convert a decimal price/size into fixed‑point (1e‑8) integer units
constexpr int64_t fp(double v) { return static_cast<int64_t>(v * 100'000'000.0 + 0.5); }

// Static member definitions - REQUIRED
std::array<const AssetInfo*, AssetInfoManager::MAX_INSTRUMENT_ID> AssetInfoManager::instrument_store_{};
std::unordered_map<int64_t, const AssetInfo*> AssetInfoManager::exchange_symbol_map_;
std::unordered_map<int64_t, const AssetInfo*> AssetInfoManager::exchange_instrument_map_;
std::array<AssetInfo, 256> AssetInfoManager::asset_storage_;
int32_t AssetInfoManager::storage_index_ = 0;
bool AssetInfoManager::initialized_ = false;

// String hash implementation
uint32_t AssetInfoManager::string_hash(std::string_view str) noexcept {
    // Fast FNV-1a hash (unsigned throughout - a sign-extended hash would
    // clobber the exchange bits when building the 64-bit lookup keys)
    uint32_t hash = 2166136261u;
    for (char c : str) {
        hash ^= static_cast<uint8_t>(c);
        hash *= 16777619u;
    }
    return hash;
}

Currency AssetInfoManager::currency_from_string(std::string_view name) noexcept {
  // Fast lookup using constexpr if possible
  if (name == "BTC") return Currency::BTC;
  if (name == "ETH") return Currency::ETH;
  if (name == "LTC") return Currency::LTC;
  if (name == "SOL") return Currency::SOL;
  if (name == "ADA") return Currency::ADA;
  if (name == "XRP") return Currency::XRP;
  if (name == "AVAX") return Currency::AVAX;
  if (name == "DOGE") return Currency::DOGE;
  if (name == "USDT") return Currency::USDT;
  if (name == "TUSD") return Currency::TUSD;
  if (name == "FDUSD") return Currency::FDUSD;
  if (name == "USD") return Currency::USD;
  if (name == "EUR") return Currency::EUR;
  return Currency::BTC;  // Default fallback
}
const AssetInfo* AssetInfoManager::get_by_instrument_id(int32_t instrument_id) {
  if (instrument_id < 0 || instrument_id >= MAX_INSTRUMENT_ID) {
    return nullptr;
  }
  return instrument_store_[instrument_id];
}

// Create asset info helper
const AssetInfo* AssetInfoManager::create_asset_info(
    int32_t instrument_id, Exchange exchange, Instrument instrument,
    const char* exchange_symbol, const char* parquet_symbol,
    int64_t tick_increment, int64_t size_increment, int64_t min_order_size,
    int64_t contract_size, int32_t taker_fee_ppb, int32_t maker_fee_ppb,
    SizeUnit size_unit) noexcept {

    if (static_cast<std::size_t>(storage_index_) >= asset_storage_.size()) {
        return nullptr; // Storage full
    }

    // Construct in-place in static storage
    asset_storage_[storage_index_] = AssetInfo(
        instrument_id, exchange, instrument, exchange_symbol, parquet_symbol,
        tick_increment, size_increment, min_order_size, contract_size,
        taker_fee_ppb, maker_fee_ppb, size_unit
    );

    const AssetInfo* asset_info = &asset_storage_[storage_index_++];

    // Index by ID (fastest lookup)
    if (instrument_id < MAX_INSTRUMENT_ID) {
        instrument_store_[instrument_id] = asset_info;
    }

    // Index by exchange + symbol
    const uint32_t symbol_hash = string_hash(exchange_symbol);
    const int64_t symbol_key = make_exchange_symbol_key(exchange, symbol_hash);
    exchange_symbol_map_[symbol_key] = asset_info;

    // Index by exchange + instrument
    const int64_t instrument_key = make_exchange_instrument_key(exchange, static_cast<uint32_t>(instrument.hash()));
    exchange_instrument_map_[instrument_key] = asset_info;

    return asset_info;
}

// Main initialization function. Mutates static state, and every
// BaseComponent constructor calls it - guard with std::call_once so
// concurrent component construction cannot race the initialization.
void AssetInfoManager::initialize() noexcept {
    static std::once_flag init_flag;
    std::call_once(init_flag, []() {
        // Clear all storage
        instrument_store_.fill(nullptr);
        exchange_symbol_map_.clear();
        exchange_instrument_map_.clear();
        storage_index_ = 0;

        init_spot_instruments();
        init_future_instruments();

        initialized_ = true;
    });
}

void AssetInfoManager::init_spot_instruments() noexcept {
    // Helper lambda for creating spots
    auto create_spot = [](Currency base, Currency quote) {
        return Instrument(InstrumentType::SPOT, base, quote);
    };

    // Create instruments - no dynamic allocation
    const auto btc_usdt = create_spot(Currency::BTC, Currency::USDT);
    const auto eth_usdt = create_spot(Currency::ETH, Currency::USDT);
    const auto ltc_usdt = create_spot(Currency::LTC, Currency::USDT);
    const auto sol_usdt = create_spot(Currency::SOL, Currency::USDT);
    const auto ada_usdt = create_spot(Currency::ADA, Currency::USDT);
    const auto xrp_usdt = create_spot(Currency::XRP, Currency::USDT);
    const auto avax_usdt = create_spot(Currency::AVAX, Currency::USDT);
    const auto doge_usdt = create_spot(Currency::DOGE, Currency::USDT);

    // BINANCE – values expressed in native decimals, converted with fp()
    create_asset_info(201, Exchange::Binance, btc_usdt, "BTCUSDT", "BTC/USDT",
        fp(0.01), fp(0.00001), fp(0.00001), fp(1.0),
        Fees::BINANCE_TAKER, Fees::BINANCE_MAKER, SizeUnit::ASSET);

    create_asset_info(202, Exchange::Binance, eth_usdt, "ETHUSDT", "ETH/USDT",
        fp(0.01), fp(0.0001), fp(0.0001), fp(1.0),
        Fees::BINANCE_TAKER, Fees::BINANCE_MAKER, SizeUnit::ASSET);

    create_asset_info(203, Exchange::Binance, ltc_usdt, "LTCUSDT", "LTC/USDT",
        fp(0.01), fp(0.001), fp(0.001), fp(1.0),
        Fees::BINANCE_TAKER, Fees::BINANCE_MAKER, SizeUnit::ASSET);

    create_asset_info(204, Exchange::Binance, sol_usdt, "SOLUSDT", "SOL/USDT",
        fp(0.01), fp(0.001), fp(0.001), fp(1.0),
        Fees::BINANCE_TAKER, Fees::BINANCE_MAKER, SizeUnit::ASSET);

    create_asset_info(205, Exchange::Binance, ada_usdt, "ADAUSDT", "ADA/USDT",
        fp(0.0001), fp(0.1), fp(0.1), fp(1.0),
        Fees::BINANCE_TAKER, Fees::BINANCE_MAKER, SizeUnit::ASSET);

    create_asset_info(209, Exchange::Binance, xrp_usdt, "XRPUSDT", "XRP/USDT",
        fp(0.0001), fp(0.1), fp(0.1), fp(1.0),
        Fees::BINANCE_TAKER, Fees::BINANCE_MAKER, SizeUnit::ASSET);

    create_asset_info(210, Exchange::Binance, avax_usdt, "AVAXUSDT", "AVAX/USDT",
        fp(0.001), fp(0.01), fp(0.01), fp(1.0),
        Fees::BINANCE_TAKER, Fees::BINANCE_MAKER, SizeUnit::ASSET);

    create_asset_info(211, Exchange::Binance, doge_usdt, "DOGEUSDT", "DOGE/USDT",
        fp(0.00001), fp(1.0), fp(1.0), fp(1.0),
        Fees::BINANCE_TAKER, Fees::BINANCE_MAKER, SizeUnit::ASSET);

    // OKX – spot
    create_asset_info(301, Exchange::Okx, btc_usdt, "BTC-USDT", "BTC/USDT",
        fp(0.1),  fp(0.000001), fp(0.00001), 0,
        Fees::OKX_TAKER, Fees::OKX_MAKER, SizeUnit::ASSET);

    create_asset_info(302, Exchange::Okx, eth_usdt, "ETH-USDT", "ETH/USDT",
        fp(0.01), fp(0.001),    fp(0.001),   0,
        Fees::OKX_TAKER, Fees::OKX_MAKER, SizeUnit::ASSET);

    create_asset_info(303, Exchange::Okx, ltc_usdt, "LTC-USDT", "LTC/USDT",
        fp(0.01), fp(0.1),      fp(0.1),     0,
        Fees::OKX_TAKER, Fees::OKX_MAKER, SizeUnit::ASSET);

    create_asset_info(304, Exchange::Okx, sol_usdt, "SOL-USDT", "SOL/USDT",
        fp(0.01), fp(0.1),      fp(0.1),     0,
        Fees::OKX_TAKER, Fees::OKX_MAKER, SizeUnit::ASSET);

    create_asset_info(305, Exchange::Okx, ada_usdt, "ADA-USDT", "ADA/USDT",
        fp(0.0001), fp(1.0),    fp(1.0),     0,
        Fees::OKX_TAKER, Fees::OKX_MAKER, SizeUnit::ASSET);

    create_asset_info(306, Exchange::Okx, xrp_usdt, "XRP-USDT", "XRP/USDT",
        fp(0.0001), fp(1.0),    fp(1.0),     0,
        Fees::OKX_TAKER, Fees::OKX_MAKER, SizeUnit::ASSET);

    create_asset_info(307, Exchange::Okx, avax_usdt, "AVAX-USDT", "AVAX/USDT",
        fp(0.001), fp(0.1),     fp(0.1),     0,
        Fees::OKX_TAKER, Fees::OKX_MAKER, SizeUnit::ASSET);

    create_asset_info(308, Exchange::Okx, doge_usdt, "DOGE-USDT", "DOGE/USDT",
        fp(0.00001), fp(1.0),    fp(1.0),     0,
        Fees::OKX_TAKER, Fees::OKX_MAKER, SizeUnit::ASSET);
}

void AssetInfoManager::init_future_instruments() noexcept {
    // Helper lambda for creating futures
    auto create_future = [](Currency base, Currency quote) {
        return Instrument(InstrumentType::FUTURE, base, quote, Tenor::PERPETUAL);
    };

    const auto btc_usdt_perp = create_future(Currency::BTC, Currency::USDT);
    const auto eth_usdt_perp = create_future(Currency::ETH, Currency::USDT);
    const auto ltc_usdt_perp = create_future(Currency::LTC, Currency::USDT);
    const auto sol_usdt_perp = create_future(Currency::SOL, Currency::USDT);
    const auto ada_usdt_perp = create_future(Currency::ADA, Currency::USDT);
    const auto xrp_usdt_perp = create_future(Currency::XRP, Currency::USDT);
    const auto avax_usdt_perp = create_future(Currency::AVAX, Currency::USDT);
    const auto doge_usdt_perp = create_future(Currency::DOGE, Currency::USDT);

    // OKX USDT‑margined perpetual swaps (ctVal varies by asset; see entries)
    create_asset_info(10301, Exchange::Okx, btc_usdt_perp, "BTC-USDT-SWAP", "PERPETUAL/BTC/USDT",
        fp(0.1),   fp(0.01), fp(0.01), fp(0.01),   // tick, size‑inc, min‑size, contract‑size
        Fees::OKX_TAKER, Fees::OKX_MAKER, SizeUnit::CONTRACTS);

    create_asset_info(10302, Exchange::Okx, eth_usdt_perp, "ETH-USDT-SWAP", "PERPETUAL/ETH/USDT",
        fp(0.01),  fp(0.01), fp(0.01), fp(0.1),
        Fees::OKX_TAKER, Fees::OKX_MAKER, SizeUnit::CONTRACTS);

    create_asset_info(10303, Exchange::Okx, ltc_usdt_perp, "LTC-USDT-SWAP", "PERPETUAL/LTC/USDT",
        fp(0.01),  fp(1.0), fp(1.0), fp(1.0),
        Fees::OKX_TAKER, Fees::OKX_MAKER, SizeUnit::CONTRACTS);

    create_asset_info(10304, Exchange::Okx, sol_usdt_perp, "SOL-USDT-SWAP", "PERPETUAL/SOL/USDT",
        fp(0.01), fp(0.01), fp(0.01), fp(1.0),
        Fees::OKX_TAKER, Fees::OKX_MAKER, SizeUnit::CONTRACTS);

    create_asset_info(10305, Exchange::Okx, ada_usdt_perp, "ADA-USDT-SWAP", "PERPETUAL/ADA/USDT",
        fp(0.0001), fp(1.0), fp(1.0), fp(1.0),
        Fees::OKX_TAKER, Fees::OKX_MAKER, SizeUnit::CONTRACTS);

    create_asset_info(10306, Exchange::Okx, xrp_usdt_perp, "XRP-USDT-SWAP", "PERPETUAL/XRP/USDT",
        fp(0.0001), fp(0.01), fp(0.01), fp(100.0),
        Fees::OKX_TAKER, Fees::OKX_MAKER, SizeUnit::CONTRACTS);

    create_asset_info(10307, Exchange::Okx, avax_usdt_perp, "AVAX-USDT-SWAP", "PERPETUAL/AVAX/USDT",
        fp(0.001), fp(1.0), fp(1.0), fp(1.0),
        Fees::OKX_TAKER, Fees::OKX_MAKER, SizeUnit::CONTRACTS);

    create_asset_info(10308, Exchange::Okx, doge_usdt_perp, "DOGE-USDT-SWAP", "PERPETUAL/DOGE/USDT",
        fp(0.00001), fp(1.0), fp(1.0), fp(1.0),
        Fees::OKX_TAKER, Fees::OKX_MAKER, SizeUnit::CONTRACTS);
}

} // namespace reflex
