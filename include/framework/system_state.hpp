#pragma once
#include <string>

#include "spdlog/fmt/bundled/core.h"

namespace reflex {

enum class SystemState {
  INIT,
   GATEWAY_STARTING,
   GATEWAY_READY,
   MARKETDATA_STARTING,
   MARKETDATA_READY,
   DATAOFFLOAD_STARTING,
   DATAOFFLOAD_READY,
   ALGO_STARTING,
   ALGO_READY,
   RUNNING,
   STOPPED,
   FAILED
};

inline std::string to_string(SystemState state) {
  switch (state) {
    case SystemState::INIT:
      return "INIT";
    case SystemState::GATEWAY_STARTING:
      return "GATEWAY_STARTING";
    case SystemState::GATEWAY_READY:
      return "GATEWAY_READY";
    case SystemState::MARKETDATA_STARTING:
      return "MARKETDATA_STARTING";
    case SystemState::MARKETDATA_READY:
      return "MARKETDATA_READY";
    case SystemState::DATAOFFLOAD_STARTING:
      return "DATAOFFLOAD_STARTING";
    case SystemState::DATAOFFLOAD_READY:
      return "DATAOFFLOAD_READY";
    case SystemState::ALGO_STARTING:
      return "ALGO_STARTING";
    case SystemState::ALGO_READY:
      return "ALGO_READY";
    case SystemState::RUNNING:
      return "RUNNING";
    case SystemState::STOPPED:
      return "STOPPED";
    case SystemState::FAILED:
      return "FAILED";
  }
  return "UNKNOWN";
}

inline std::ostream& operator<<(std::ostream& os, SystemState state) {
  return os << to_string(state);
}

}


template <>
struct fmt::formatter<reflex::SystemState> : formatter<std::string> {
  auto format(reflex::SystemState state, format_context& ctx) const {
    return fmt::formatter<std::string>::format(reflex::to_string(state), ctx);
  }
};
