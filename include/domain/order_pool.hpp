#pragma once

#include <cstddef>
#include <deque>
#include <vector>
#include "order.hpp"

namespace reflex {

// Fixed-identity object pool for Order.
//
// Storage is a std::deque slab: elements keep stable addresses as the pool
// grows, so an Order* handed out by acquire() stays valid even if a later
// acquire() triggers growth (callers in AlgoOrderManagement hold pointers
// across acquisitions). Free slots are tracked by a vector of pointers used as
// a LIFO freelist — O(1) acquire/release with no per-object heap allocation on
// the hot path and no allocation at all once warmed up.
class OrderPool {
public:
  explicit OrderPool(std::size_t initial_capacity = 1024) {
    if (initial_capacity == 0) {
      initial_capacity = 1;
    }
    free_list_.reserve(initial_capacity);
    grow(initial_capacity);
  }

  // Get a reset Order from the pool. Grows the pool (doubling) on underflow
  // instead of leaking a raw `new Order` as the old implementation did.
  Order* acquire() {
    if (free_list_.empty()) {
      grow(storage_.size());  // double capacity; storage_ is never empty here
    }
    Order* order = free_list_.back();
    free_list_.pop_back();
    order->reset();
    return order;
  }

  // Return an Order previously obtained from acquire().
  void release(Order* order) {
    if (order != nullptr) {
      free_list_.push_back(order);
    }
  }

  // Number of orders currently available without growing.
  int available() const {
    return static_cast<int>(free_list_.size());
  }

  // Total number of orders the pool has allocated (in use + available).
  std::size_t capacity() const {
    return storage_.size();
  }

private:
  void grow(std::size_t count) {
    free_list_.reserve(free_list_.size() + count);
    for (std::size_t i = 0; i < count; ++i) {
      storage_.emplace_back();              // stable address (deque)
      free_list_.push_back(&storage_.back());
    }
  }

  std::deque<Order> storage_;     // owns every Order; addresses stable on growth
  std::vector<Order*> free_list_; // LIFO freelist of available slots
};

} // namespace reflex
