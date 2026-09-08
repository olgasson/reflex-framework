#include <gtest/gtest.h>
#include <vector>
#include "timer_manager.hpp"

using namespace reflex;

TEST(TimerManagerTest, SingleShotTimerFires) {
  TimerManager manager;
  bool called = false;

  manager.add_timer(1000, -1, [&]() {
      called = true;
  });

  manager.check_scheduled_timers(999);
  EXPECT_FALSE(called);

  manager.check_scheduled_timers(1000);
  EXPECT_TRUE(called);
}

TEST(TimerManagerTest, RepeatingTimerFiresMultipleTimes) {
  TimerManager manager;
  int call_count = 0;

  manager.add_timer(1000, 500, [&]() {
      call_count++;
  });

  for (uint64_t time = 0; time <= 3000; time += 500) {
    manager.check_scheduled_timers(time);
  }

  EXPECT_EQ(call_count, 5);
}

TEST(TimerManagerTest, RemoveTimerPreventsFiring) {
  TimerManager manager;
  bool called = false;

  auto callback = [&]() {
    called = true;
  };

  const auto id = manager.add_timer(1000, -1, callback);
  manager.remove_timer(id);
  manager.check_scheduled_timers(2000);

  EXPECT_FALSE(called);
}

TEST(TimerManagerTest, LatestTimeIsUpdatedCorrectly) {
  TimerManager manager;
  manager.check_scheduled_timers(123456);
  EXPECT_EQ(manager.get_latest_time(), 123456);
}

TEST(TimerManagerTest, MultipleTimersFireInOrder) {
  TimerManager manager;
  std::vector<int> order;

  manager.add_timer(1000, -1, [&]() { order.push_back(1); });
  manager.add_timer(900, -1,  [&]() { order.push_back(2); });
  manager.add_timer(1100, -1, [&]() { order.push_back(3); });

  manager.check_scheduled_timers(1200);

  ASSERT_EQ(order.size(), 3);
  EXPECT_EQ(order[0], 2);
  EXPECT_EQ(order[1], 1);
  EXPECT_EQ(order[2], 3);
}
