#pragma once
#include <chrono>
#include <cstdint>
#include <thread>

#include "idle_strategy.hpp"

namespace reflex {

class BackoffIdleStrategy final : public IdleStrategy {
public:
    explicit BackoffIdleStrategy(int64_t max_spins = 100, int64_t max_yields = 10,
                                 int64_t min_park_ns = 1'000, int64_t max_park_ns = 1'000'000)
        : max_spins_(max_spins),
          max_yields_(max_yields),
          min_park_ns_(min_park_ns),
          max_park_ns_(max_park_ns),
          park_ns_(min_park_ns) {}

    void idle(int work_count) override {
        if (work_count > 0) {
            reset();
            return;
        }

        if (spins_ < max_spins_) {
            ++spins_;
            cpu_relax();
            return;
        }

        if (yields_ < max_yields_) {
            ++yields_;
            std::this_thread::yield();
            return;
        }

        std::this_thread::sleep_for(std::chrono::nanoseconds(park_ns_));
        park_ns_ = park_ns_ * 2 <= max_park_ns_ ? park_ns_ * 2 : max_park_ns_;
    }

    void reset() override {
        spins_ = 0;
        yields_ = 0;
        park_ns_ = min_park_ns_;
    }

private:
    static void cpu_relax() {
#if defined(__x86_64__) || defined(__i386__)
        __builtin_ia32_pause();
#elif defined(__aarch64__)
        asm volatile("yield" ::: "memory");
#else
        std::this_thread::yield();
#endif
    }

    const int64_t max_spins_;
    const int64_t max_yields_;
    const int64_t min_park_ns_;
    const int64_t max_park_ns_;

    int64_t spins_ = 0;
    int64_t yields_ = 0;
    int64_t park_ns_;
};

}
