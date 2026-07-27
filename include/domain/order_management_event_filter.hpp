#pragma once

#include <string>
#include "../messages.hpp"

namespace reflex {

// ------------------------------
// Interface
// ------------------------------
class OrderManagementEventFilter {
public:
  virtual ~OrderManagementEventFilter() = default;

  virtual bool matches(const PendingEvent&) const { return false; }
  virtual bool matches(const AcceptedEvent&) const { return false; }
  virtual bool matches(const RejectedEvent&) const { return false; }
  virtual bool matches(const ExecutedEvent&) const { return false; }
  virtual bool matches(const PendingReplaceEvent&) const { return false; }
  virtual bool matches(const ReplaceAcceptedEvent&) const { return false; }
  virtual bool matches(const ReplaceRejectedEvent&) const { return false; }
  virtual bool matches(const PendingCancelEvent&) const { return false; }
  virtual bool matches(const CancelAcceptedEvent&) const { return false; }
  virtual bool matches(const CancelRejectedEvent&) const { return false; }
};

// ------------------------------
// PassAllEventFilter
// ------------------------------
class PassAllEventFilter : public OrderManagementEventFilter {
public:
  bool matches(const PendingEvent&) const override { return true; }
  bool matches(const AcceptedEvent&) const override { return true; }
  bool matches(const RejectedEvent&) const override { return true; }
  bool matches(const ExecutedEvent&) const override { return true; }
  bool matches(const PendingReplaceEvent&) const override { return true; }
  bool matches(const ReplaceAcceptedEvent&) const override { return true; }
  bool matches(const ReplaceRejectedEvent&) const override { return true; }
  bool matches(const PendingCancelEvent&) const override { return true; }
  bool matches(const CancelAcceptedEvent&) const override { return true; }
  bool matches(const CancelRejectedEvent&) const override { return true; }
};


} // namespace reflex
