#pragma once
/**
 *  launcher_common.hpp
 *
 *  Shared scaffolding for the launcher executables: signal-driven shutdown,
 *  the common component/blob configuration and the default OKX symbol
 *  universe. Each launcher translation unit keeps only its distinct
 *  component wiring.
 */

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "../../include/framework/component_config.hpp"

namespace launcher {

// Ring buffer capacity shared by all launchers. Must be a power of 2.
inline constexpr size_t kRingBufferSize = 8192;

// ---------------------------------------------------------------------------
// Shutdown signalling.
//
// The signal handler only stores into a namespace-scope lock-free atomic
// flag (async-signal-safe); main() polls the flag. No statics are first-
// touched inside the handler and no separate barrier instances exist.
// ---------------------------------------------------------------------------
inline std::atomic<bool> g_shutdown{false};
static_assert(std::atomic<bool>::is_always_lock_free,
              "signal handler needs a lock-free flag to be async-signal-safe");

inline void install_signal_handlers() {
  std::signal(SIGINT, [](int) { g_shutdown.store(true); });
  std::signal(SIGTERM, [](int) { g_shutdown.store(true); });
}

inline void await_shutdown() {
  while (!g_shutdown.load()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  std::cout << "Shutdown signal received" << std::endl;
}

// ---------------------------------------------------------------------------
// Common configuration
// ---------------------------------------------------------------------------
inline const std::vector<std::string>& default_okx_symbols() {
  static const std::vector<std::string> symbols{
      "BTC-USDT",      "ETH-USDT",      "LTC-USDT",      "SOL-USDT",
      "ADA-USDT",      "XRP-USDT",      "AVAX-USDT",     "DOGE-USDT",
      "BTC-USDT-SWAP", "ETH-USDT-SWAP", "LTC-USDT-SWAP", "SOL-USDT-SWAP",
      "ADA-USDT-SWAP", "XRP-USDT-SWAP", "AVAX-USDT-SWAP", "DOGE-USDT-SWAP"};
  return symbols;
}

inline reflex::ComponentConfig make_nio_config() {
  return reflex::ComponentConfig{
      .component_name_ = "NIO",
      .environment_ = "test",
  };
}

inline reflex::BlobConfig make_blob_config() {
  reflex::ComponentConfig base{
      .component_name_ = "okx_blob",
      .environment_ = "test",
  };

  const char* api_path_env = std::getenv("REFLEX_API_PATH");
  const std::string api_path = api_path_env ? api_path_env : "okx_credentials.txt";

  return reflex::BlobConfig(base, "wss://ws.okx.com:8443/ws/v5/public", api_path, default_okx_symbols());
}

}  // namespace launcher
