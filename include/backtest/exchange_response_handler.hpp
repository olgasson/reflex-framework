#pragma once
#include "messages.hpp"

namespace reflex::backtest {

class ExchangeResponseHandler {
public:
  virtual ~ExchangeResponseHandler() = default;
  
  // Called by ExchangeSimulator to send responses back to strategy
  virtual void on_exchange_response(const MessageSlot& response_event) = 0;
};

} // namespace reflex::backtest