
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

    logger = spdlog::stdout_color_mt(name);
    logger->set_level(spdlog::level::info);

    logger->set_pattern(
      "\033[33m[%Y-%m-%d %H:%M:%S.%e]\033[0m "
      "\033[36m[%t]\033[0m "
      "\033[34m[%n]\033[0m "
      "%^[%l]:%$"
      " %v"
    );

    return logger;
  }

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
