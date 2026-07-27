#pragma once
#include <chrono>

namespace reflex {

class ClockInterface {
public:
  virtual ~ClockInterface() = default;
  virtual int64_t epoch_nanos() const = 0;
  virtual int64_t epoch_millis() const = 0;
  virtual int64_t epoch_micros() const = 0;
};


class OffsetEpochNanoClock : public ClockInterface {
public:
  explicit OffsetEpochNanoClock() {
    auto now_system = std::chrono::system_clock::now();
    auto now_steady = std::chrono::steady_clock::now();
    offset = std::chrono::duration_cast<std::chrono::nanoseconds>(
                 now_system.time_since_epoch() - now_steady.time_since_epoch())
                 .count();
  }

  int64_t epoch_nanos() const override {
    auto now = std::chrono::steady_clock::now();
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               now.time_since_epoch())
               .count() +
           offset;
  }

  int64_t epoch_millis() const override {
    return epoch_nanos() / 1'000'000;
  }

  int64_t epoch_micros() const override {
    return epoch_nanos() / 1'000;
  }

private:
  int64_t offset;
};


class WallClock : public ClockInterface {
public:
  int64_t epoch_nanos() const override {
    return nano_clock_.epoch_nanos();
  }

  int64_t epoch_millis() const override {
    return nano_clock_.epoch_millis();
  }

  int64_t epoch_micros() const override {
    return nano_clock_.epoch_micros();
  }

private:
  OffsetEpochNanoClock nano_clock_;
};


class SimulationClock : public ClockInterface {
public:
  SimulationClock() : current_time_ns_(0) {}

  int64_t epoch_nanos() const override {
    return static_cast<int64_t>(current_time_ns_);
  }

  int64_t epoch_millis() const override {
    return epoch_nanos() / 1'000'000;
  }

  int64_t epoch_micros() const override {
    return epoch_nanos() / 1'000;
  }


  void set_time(uint64_t time_ns) {
    current_time_ns_ = time_ns;
  }


private:
  uint64_t current_time_ns_;
};



}