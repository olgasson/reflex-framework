#include "../../include/websocket/subscription_queue.hpp"

namespace reflex {

SubscriptionQueue::SubscriptionQueue(TimerManager& tm,
                                     const std::string& ctx,
                                     int64_t interval_ns)
    : timer_manager_(tm),
      interval_ns_(interval_ns),
      logger_(LoggerFactory::getLogger(ctx + ".SubscriptionQueue"))
{}

void SubscriptionQueue::enqueue(Task task)
{
  queue_.push(std::move(task));
  if (!timer_armed_)                 // no timer in flight, kick-start chain
    arm_next_timer();
}

/* ------------------------------------------------------------------ */

void SubscriptionQueue::arm_next_timer()
{
  timer_armed_ = true;

  int64_t fire_time =
      timer_manager_.get_latest_time() + interval_ns_;

  timer_manager_.add_timer(
      fire_time,
      -1,                     // one-shot
      [this]() { process_next(); });
}

void SubscriptionQueue::process_next()
{
  timer_armed_ = false;

  if (queue_.empty()) {
    logger_->debug("process_next invoked but queue empty");
    return;
  }

  auto job = std::move(queue_.front());
  queue_.pop();

  try {
    job();
  } catch (const std::exception& ex) {
    logger_->error("Subscription task threw: {}", ex.what());
  } catch (...) {
    logger_->error("Subscription task threw unknown exception");
  }

  if (!queue_.empty()) {
    arm_next_timer();
  }
}

} // namespace reflex
