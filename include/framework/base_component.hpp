#pragma once
#include <string>

#include "component_state.hpp"
#include "../offset_epoch_nano_clock.hpp"
#include "component.hpp"
#include "component_config.hpp"
#include <spdlog/spdlog.h>
#include <spdlog/fmt/ostr.h>   // for operator<< support

#include "system_state.hpp"
#include "../timer_manager.hpp"

namespace reflex {

class BaseComponent : public Component {
public:
  explicit BaseComponent(const ComponentConfig& config);

  void on_start() override;
  void on_close() override;
  int do_work() override;

  // Commander& get_commander() override;
  OffsetEpochNanoClock& get_nano_clock() override;
  TimerManager& get_timer_manager() override;

  std::string get_component_name() override {
    return component_name_;
  };

  ComponentState get_component_state() override {
    return component_state_;
  };

protected:

  virtual int on_do_work() { return 0; }

  // State transition callbacks - can be overridden by derived classes
  virtual void on_initializing() {}
  virtual void on_connecting() {}
  virtual void on_running() {}
  virtual void on_failed() {}
  virtual void on_disconnected() {}
  virtual void on_stopped() {}

  std::string component_name_;

  ComponentState component_state_;
  SystemState system_state_;

  // Last state a lifecycle message was logged for - lets terminal states
  // (FAILED/DISCONNECTED/STOPPED) log once instead of on every do_work spin.
  ComponentState last_logged_state_ = ComponentState::INITIALIZING;

  ComponentConfig config_;
  std::unique_ptr<TimerManager> timer_manager_;
  OffsetEpochNanoClock nano_clock_;

  std::shared_ptr<spdlog::logger> logger_;

};

}