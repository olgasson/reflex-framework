// include/framework/nio_component.hpp
#pragma once
#include <array>
#include <cstdint>
#include <memory>
#include <vector>

#include "framework/base_component.hpp"

namespace reflex {

// Sampled fan-out cycle timing for the single nio thread. Every child
// component shares this thread, so cycle tail latency is the head-of-line
// risk for every other component driven by it.
struct NioCycleStats {
  // Bucket upper bounds in ns: 1us,5us,10us,50us,100us,500us,1ms,inf
  static constexpr std::array<int64_t, 7> kBucketBoundsNs{
      1'000, 5'000, 10'000, 50'000, 100'000, 500'000, 1'000'000};
  std::array<uint64_t, 8> buckets{};
  uint64_t samples{0};
  int64_t max_cycle_ns{0};
  int64_t max_cycle_child{-1};  // index of slowest child in the worst cycle
};

class NIOComponent : public BaseComponent {
public:
  explicit NIOComponent(const ComponentConfig& config);
  ~NIOComponent() override = default;

  // Component management
  void add_component(std::unique_ptr<Component> component);

  // Override BaseComponent methods
  void on_start() override;
  void on_close() override;
  int on_do_work() override;

  // Override Component pure virtual method
  // get_component_state(): BaseComponent's implementation returns the real
  // component_state_ (a hardcoded override previously pinned it to INITIALIZING).


  [[nodiscard]] const NioCycleStats& cycle_stats() const noexcept {
    return cycle_stats_;
  }

protected:
  std::vector<std::unique_ptr<Component>> child_components_;
  NioCycleStats cycle_stats_{};
  uint64_t cycle_counter_{0};
  int64_t last_stats_log_ns_{0};
};

} // namespace reflex
