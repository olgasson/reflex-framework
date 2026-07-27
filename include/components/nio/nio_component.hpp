// include/framework/nio_component.hpp
#pragma once
#include <vector>
#include <memory>

#include "framework/base_component.hpp"

namespace reflex {

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

  // get_component_state(): BaseComponent's implementation returns the real
  // component_state_ (a hardcoded override previously pinned it to INITIALIZING).

protected:
  std::vector<std::unique_ptr<Component>> child_components_;
};

} // namespace reflex::core