// core/nio_component.cpp
#include "../../include/components/nio/nio_component.hpp"

namespace reflex {

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

  // Start all child components
  for (auto& component : child_components_) {
    component->on_start();
  }
}

void NIOComponent::on_close() {
  logger_->info("Closing NIO component");

  // Close all child components
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

  // Fan out work to all child components
  for (auto& component : child_components_) {
    try {
      total_work += component->do_work();
    } catch (const std::exception& e) {
      logger_->error("Error in child component work: {}", e.what());
    }
  }

  return total_work;
}

} // namespace reflex::core