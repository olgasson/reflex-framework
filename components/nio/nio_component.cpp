#include "../../include/components/nio/nio_component.hpp"

#include <chrono>

namespace reflex {

namespace {
int64_t steady_now_ns() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}
constexpr uint64_t kSampleEvery = 64;
constexpr int64_t kStatsLogIntervalNs = 10'000'000'000;
}

NIOComponent::NIOComponent(const ComponentConfig& config)
    : BaseComponent(config) {
}

void NIOComponent::add_component(std::unique_ptr<Component> component) {
  logger_->info("Adding component to NIO: {}", component->get_component_name());
  child_components_.push_back(std::move(component));
}

void NIOComponent::on_start() {
  BaseComponent::on_start();
  logger_->info("Starting NIO component with {} child components", child_components_.size());

  for (auto& component : child_components_) {
    component->on_start();
  }
}

void NIOComponent::on_close() {
  logger_->info("Closing NIO component");

  for (auto& component : child_components_) {
    try {
      component->on_close();
    } catch (const std::exception& e) {
      logger_->error("Error closing child component: {}", e.what());
    }
  }

  BaseComponent::on_close();
}

int NIOComponent::on_do_work() {
  int total_work = 0;
  const bool sampled = (++cycle_counter_ % kSampleEvery) == 0;
  const int64_t cycle_start_ns = sampled ? steady_now_ns() : 0;
  int64_t slowest_child_ns = 0;
  int64_t slowest_child_index = -1;

  for (std::size_t index = 0; index < child_components_.size(); ++index) {
    const int64_t child_start_ns = sampled ? steady_now_ns() : 0;
    try {
      total_work += child_components_[index]->do_work();
    } catch (const std::exception& e) {
      logger_->error("Error in child component work: {}", e.what());
    }
    if (sampled) {
      const int64_t child_ns = steady_now_ns() - child_start_ns;
      if (child_ns > slowest_child_ns) {
        slowest_child_ns = child_ns;
        slowest_child_index = static_cast<int64_t>(index);
      }
    }
  }

  if (sampled) {
    const int64_t cycle_ns = steady_now_ns() - cycle_start_ns;
    ++cycle_stats_.samples;
    std::size_t bucket = NioCycleStats::kBucketBoundsNs.size();
    for (std::size_t b = 0; b < NioCycleStats::kBucketBoundsNs.size(); ++b) {
      if (cycle_ns <= NioCycleStats::kBucketBoundsNs[b]) {
        bucket = b;
        break;
      }
    }
    ++cycle_stats_.buckets[bucket];
    if (cycle_ns > cycle_stats_.max_cycle_ns) {
      cycle_stats_.max_cycle_ns = cycle_ns;
      cycle_stats_.max_cycle_child = slowest_child_index;
    }
    const int64_t now_ns = steady_now_ns();
    if (last_stats_log_ns_ == 0) last_stats_log_ns_ = now_ns;
    if (now_ns - last_stats_log_ns_ >= kStatsLogIntervalNs) {
      last_stats_log_ns_ = now_ns;
      const auto& b = cycle_stats_.buckets;
      logger_->info(
          "nio cycle sample (1/{}): n={} <=1us:{} <=5us:{} <=10us:{} "
          "<=50us:{} <=100us:{} <=500us:{} <=1ms:{} >1ms:{} max={}ns "
          "(child {})",
          kSampleEvery, cycle_stats_.samples, b[0], b[1], b[2], b[3], b[4],
          b[5], b[6], b[7], cycle_stats_.max_cycle_ns,
          cycle_stats_.max_cycle_child >= 0 &&
                  cycle_stats_.max_cycle_child <
                      static_cast<int64_t>(child_components_.size())
              ? child_components_[static_cast<std::size_t>(
                                      cycle_stats_.max_cycle_child)]
                    ->get_component_name()
              : "none");
    }
  }

  return total_work;
}


}
