
#pragma once
#include <string_view>

class ExchangeWebSocketHandler {
public:
  virtual void on_open() = 0;
  virtual void on_message(std::string_view message) = 0;
  virtual void on_error(std::string_view error) = 0;
  virtual void on_close() = 0;
  virtual std::string get_subscription_message() = 0;

  virtual ~ExchangeWebSocketHandler() = default;
};
