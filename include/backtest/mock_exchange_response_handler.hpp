
#pragma once
#include "exchange_response_handler.hpp"
#include <vector>
#include <memory>

namespace reflex::backtest {

class MockExchangeResponseHandler : public ExchangeResponseHandler {
public:
  std::vector<std::unique_ptr<reflex::MessageSlot>> events;

  void on_exchange_response(const reflex::MessageSlot& response_event) override {
    events.push_back(std::make_unique<reflex::MessageSlot>(response_event));
  }

  template<typename T>
  bool has() const {
    for (const auto& ev : events) {
      if (ev->get_type() == T::MESSAGE_TYPE) {
        return true;
      }
    }
    return false;
  }

  template<typename T>
  const T* last() const {
    for (auto it = events.rbegin(); it != events.rend(); ++it) {
      if ((*it)->get_type() == T::MESSAGE_TYPE) {
        return &(*it)->as<T>();
      }
    }
    return nullptr;
  }

  void clear() { events.clear(); }
};

}
