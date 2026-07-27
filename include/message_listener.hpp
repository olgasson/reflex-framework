#pragma once
#include "messages.hpp"

namespace reflex {

class MessageListener {
public:

  virtual void on_message(const MessageType& message_type, const void* payload, size_t size) = 0;

  virtual ~MessageListener() = default;

};

}
