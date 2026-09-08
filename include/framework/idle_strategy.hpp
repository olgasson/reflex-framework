#pragma once

namespace reflex {

class IdleStrategy {

public:
    virtual ~IdleStrategy() = default;

    virtual void idle(int work_count) = 0;

    virtual void reset() {}
};

}
