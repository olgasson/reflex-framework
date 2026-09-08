#pragma once
#include <string>

#include "agent.hpp"
#include "component_state.hpp"
#include "offset_epoch_nano_clock.hpp"
#include "../timer_manager.hpp"

namespace reflex {

class Component : public Agent {

public:

    virtual std::string get_component_name() = 0;

    virtual ComponentState get_component_state() = 0;

    virtual OffsetEpochNanoClock& get_nano_clock() = 0;

    virtual TimerManager& get_timer_manager() = 0;

};

}
