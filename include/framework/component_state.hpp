#pragma once

#include <ostream>
#include <string>
#include <spdlog/spdlog.h>
#include <spdlog/fmt/ostr.h>   // for operator<< support


// Define your enum in the reflex namespace
namespace reflex {

enum class ComponentState {
  INITIALIZING,
  CONNECTING,
  RUNNING,
  FAILED,
  DISCONNECTED,
  STOPPED
};

inline std::string to_string(ComponentState state) {
  switch (state) {
    case ComponentState::INITIALIZING: return "INITIALIZING";
    case ComponentState::CONNECTING: return "CONNECTING";
    case ComponentState::RUNNING: return "RUNNING";
    case ComponentState::FAILED: return "FAILED";
    case ComponentState::DISCONNECTED: return "DISCONNECTED";
    case ComponentState::STOPPED: return "STOPPED";
    default: return "UNKNOWN";
  }
}

// std::ostream support
inline std::ostream& operator<<(std::ostream& os, ComponentState state) {
  return os << to_string(state);
}

} // namespace reflex

template <>
struct fmt::formatter<reflex::ComponentState> : formatter<std::string> {
  auto format(reflex::ComponentState state, format_context& ctx) const {
    return fmt::formatter<std::string>::format(reflex::to_string(state), ctx);
  }
};
