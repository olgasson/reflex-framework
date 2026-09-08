#pragma once
#include "idle_strategy.hpp"

namespace reflex {

class NoopIdleStrategy final : public IdleStrategy {
public:

    void idle(int ) override {
    }

    void reset() override {
    }

};
}
