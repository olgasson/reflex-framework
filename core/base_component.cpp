#include "../include/framework/base_component.hpp"

#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include "../include/framework/component_state.hpp"
#include "asset_info_manager.hpp"
#include "logger_factory.hpp"
#include "../include/framework/component_config.hpp"
#include "../include/utils/utils.hpp"

namespace reflex {

BaseComponent::BaseComponent(const ComponentConfig& config)
      : component_name_(config.component_name_),
        component_state_(ComponentState::INITIALIZING),
        system_state_(SystemState::INIT),
        config_(config),
        timer_manager_(std::make_unique<TimerManager>(component_name_)),
        nano_clock_(OffsetEpochNanoClock()) {


  logger_ = LoggerFactory::getLogger(component_name_);

  AssetInfoManager::initialize();

}

int BaseComponent::do_work() {
  int work_count = 0;
  switch (component_state_) {
    case ComponentState::INITIALIZING: {
      logger_->info("Transitioning to state: {}", ComponentState::CONNECTING);
      
      on_initializing();
      
      component_state_ = ComponentState::CONNECTING;
      break;
    }

    case ComponentState::CONNECTING: {
      on_connecting();

      if (component_state_ == ComponentState::FAILED) {
        break;
      }

      logger_->info("Transitioning to state: {}", ComponentState::RUNNING);
      component_state_ = ComponentState::RUNNING;
      break;
    }

    case ComponentState::RUNNING: {
      timer_manager_->check_scheduled_timers(nano_clock_.epoch_nanos());

      on_running();

      work_count += on_do_work();
      break;
    }

    case ComponentState::FAILED: {
      if (last_logged_state_ != ComponentState::FAILED) {
        logger_->error("Service lifecycle FAILED");
        last_logged_state_ = ComponentState::FAILED;
      }

      on_failed();
      break;
    }

    case ComponentState::DISCONNECTED: {
      if (last_logged_state_ != ComponentState::DISCONNECTED) {
        logger_->warn("Service disconnected");
        last_logged_state_ = ComponentState::DISCONNECTED;
      }

      on_disconnected();
      break;
    }

    case ComponentState::STOPPED: {
      if (last_logged_state_ != ComponentState::STOPPED) {
        logger_->info("Service stopped");
        last_logged_state_ = ComponentState::STOPPED;
      }

      on_stopped();
      break;
    }

    default: {
      logger_->error("Service lifecycle unknown {}", static_cast<int>(component_state_));
      break;
    }
  }
  return work_count;
}

void BaseComponent::on_start() {

}

void BaseComponent::on_close() {
  logger_->info("Service shutting down");
  system_state_ = SystemState::STOPPED;
}

OffsetEpochNanoClock& BaseComponent::get_nano_clock() {
  return nano_clock_;
}

TimerManager& BaseComponent::get_timer_manager() {
  return *timer_manager_;
}

}
