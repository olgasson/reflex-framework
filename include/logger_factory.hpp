
#pragma once

#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <memory>
#include <mutex>
#include <string>

class LoggerFactory {
public:
  static std::shared_ptr<spdlog::logger> getLogger(const std::string& name) {
    if (auto logger = spdlog::get(name)) {
      return logger;
    }

    static std::mutex creation_mutex;
    std::lock_guard<std::mutex> lock(creation_mutex);

    auto logger = spdlog::get(name);
    if (logger) {
      return logger;
    }

    // Create new colored console logger
    logger = spdlog::stdout_color_mt(name);
    logger->set_level(spdlog::level::info);

    // Custom colored pattern:
    // \033[33m = Yellow timestamp
    // \033[36m = Cyan thread ID
    // \033[34m = Blue logger name
    // %^[%l]%$ = Auto-colored log level (green=info, red=error, etc.)
    // \033[0m = Reset color
    logger->set_pattern(
      "\033[33m[%Y-%m-%d %H:%M:%S.%e]\033[0m "  // Yellow timestamp
      "\033[36m[%t]\033[0m "                     // Cyan thread ID
      "\033[34m[%n]\033[0m "                     // Blue logger name
      "%^[%l]:%$"                                // Auto-colored log level
      " %v"                                      // Message (no color change)
    );

    return logger;
  }

  // Same format as getLogger() but on stderr, for processes whose stdout is
  // a data stream (e.g. tools that pipe records to another process).
  static std::shared_ptr<spdlog::logger> getStderrLogger(const std::string& name) {
    if (auto logger = spdlog::get(name)) return logger;

    static std::mutex creation_mutex;
    std::lock_guard<std::mutex> lock(creation_mutex);
    auto logger = spdlog::get(name);
    if (logger) return logger;
    logger = spdlog::stderr_color_mt(name);
    logger->set_level(spdlog::level::info);
    logger->set_pattern(
        "\033[33m[%Y-%m-%d %H:%M:%S.%e]\033[0m "
        "\033[36m[%t]\033[0m "
        "\033[34m[%n]\033[0m "
        "%^[%l]:%$ %v");
    return logger;
  }
};
