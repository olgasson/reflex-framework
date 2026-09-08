#pragma once
#include "messages.hpp"

namespace reflex::backtest {

class ExchangeResponseHandler {
public:
  virtual ~ExchangeResponseHandler() = default;
  
  virtual void on_exchange_response(const MessageSlot& response_event) = 0;
};

}
