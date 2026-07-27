#pragma once
#include "idle_strategy.hpp"

namespace reflex {

class NoopIdleStrategy final : public IdleStrategy {
public:

    void idle(int /*work_count*/) override {
        //do nothing
    }

    void reset() override {
        //do nothing
    }

};
}
