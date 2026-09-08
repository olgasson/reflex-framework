#pragma once

#include <cstddef>
#include <deque>
#include <vector>
#include "order.hpp"

namespace reflex {

class OrderPool {
public:
  explicit OrderPool(std::size_t initial_capacity = 1024) {
    if (initial_capacity == 0) {
      initial_capacity = 1;
    }
    free_list_.reserve(initial_capacity);
    grow(initial_capacity);
  }

  Order* acquire() {
    if (free_list_.empty()) {
      grow(storage_.size());
    }
    Order* order = free_list_.back();
    free_list_.pop_back();
    order->reset();
    return order;
  }

  void release(Order* order) {
    if (order != nullptr) {
      free_list_.push_back(order);
    }
  }

  int available() const {
    return static_cast<int>(free_list_.size());
  }

  std::size_t capacity() const {
    return storage_.size();
  }

private:
  void grow(std::size_t count) {
    free_list_.reserve(free_list_.size() + count);
    for (std::size_t i = 0; i < count; ++i) {
      storage_.emplace_back();
      free_list_.push_back(&storage_.back());
    }
  }

  std::deque<Order> storage_;
  std::vector<Order*> free_list_;
};

}
