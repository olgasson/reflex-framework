#pragma once
#include <chrono>
#include <cstdint>
#include <thread>

#include "idle_strategy.hpp"

namespace reflex {

/**
 * Agrona-style backoff idle strategy: on consecutive zero-work passes,
 * escalate spin -> yield -> park, with the park duration doubling from
 * min_park_ns up to max_park_ns. Any productive pass resets to spinning.
 *
 * Purpose: collector deployments. The busy-spin NoopIdleStrategy burns a full
 * core per agent thread for latency that pure data capture does not need —
 * with the default 1ms max park, a quiet loop costs ~0.1% of a core while
 * adding at most ~1ms of wakeup latency to a feed that is itself batched at
 * 10-100ms. Keep NoopIdleStrategy for trading deployments.
 *
 * NOTE: this only helps if agents report work HONESTLY — an agent returning
 * nonzero work on every pass (e.g. counting "serviced the socket" as work)
 * pins the strategy in the spin state forever.
 */
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

}  // namespace reflex
