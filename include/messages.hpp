#pragma once
#include <cstdint>
#include <cstring>
#include <new>
#include <ostream>
#include <variant>

namespace reflex {
// ================= ENUMS =================

enum class OrderState : std::int8_t {
  Undefined = 0,
  New = 1,
  Working = 2,
  Rejected = 3,
  Filled = 4,
  Cancelled = 5
};

inline const char* to_string(OrderState state) {
  switch (state) {
    case OrderState::New:
      return "New";
    case OrderState::Working:
      return "Working";
    case OrderState::Rejected:
      return "Rejected";
    case OrderState::Filled:
      return "Filled";
    case OrderState::Cancelled:
      return "Cancelled";
    default:
      return "Invalid";
  }
}

inline std::ostream& operator<<(std::ostream& os, OrderState order_state) {
  return os << to_string(order_state);
}


enum class OrderWaitState : int8_t {
  Undefined = 0,
  None = 1,
  Pending = 2,
  PendingCancel = 3,
  PendingReplace = 4
};

inline const char* to_string(OrderWaitState state) {
  switch (state) {
    case OrderWaitState::None:
      return "None";
    case OrderWaitState::Pending:
      return "Pending";
    case OrderWaitState::PendingCancel:
      return "PendingCancel";
    case OrderWaitState::PendingReplace:
      return "PendingReplace";
    default:
      return "Invalid";
  }
}

inline std::ostream& operator<<(std::ostream& os, OrderWaitState order_wait_state) {
  return os << to_string(order_wait_state);
}


enum class ServiceState : int8_t {
  Starting = 0,
  Active = 1,
  Passive = 2,
  Stopping = 3,
  Stopped = 4
};

inline const char* to_string(ServiceState state) {
  switch (state) {
    case ServiceState::Starting:
      return "Starting";
    case ServiceState::Active:
      return "Active";
    case ServiceState::Passive:
      return "Passive";
    case ServiceState::Stopping:
      return "Stopping";
    case ServiceState::Stopped:
      return "Stopped";
    default:
      return "Unknown";
  }
}

inline std::ostream& operator<<(std::ostream& os, ServiceState state) {
  return os << to_string(state);
}

enum class MessageType : int8_t {
  Heartbeat = 1,
  Pending = 2,
  Accepted = 3,
  Rejected = 4,
  PendingReplace = 5,
  ReplaceAccepted = 6,
  ReplaceRejected = 7,
  PendingCancel = 8,
  CancelAccepted = 9,
  CancelRejected = 10,
  Executed = 11,
  Dummy = 12,
  TradeEvent = 14,
  L1UpdateEvent = 15,
  L2UpdateEvent = 16,
  MarkPriceEvent = 17,
  FundingRateEvent = 18,
  OpenInterestEvent = 19
};

inline const char* to_string(MessageType type) {
  switch (type) {
    case MessageType::Heartbeat:
      return "Heartbeat";
    case MessageType::Pending:
      return "Pending";
    case MessageType::Accepted:
      return "Accepted";
    case MessageType::Rejected:
      return "Rejected";
    case MessageType::PendingReplace:
      return "PendingReplace";
    case MessageType::ReplaceAccepted:
      return "ReplaceAccepted";
    case MessageType::ReplaceRejected:
      return "ReplaceRejected";
    case MessageType::PendingCancel:
      return "PendingCancel";
    case MessageType::CancelAccepted:
      return "CancelAccepted";
    case MessageType::CancelRejected:
      return "CancelRejected";
    case MessageType::Executed:
      return "Executed";
    case MessageType::Dummy:
      return "Dummy";
    case MessageType::TradeEvent:
      return "TradeEvent";
    case MessageType::L1UpdateEvent:
      return "L1UpdateEvent";
    case MessageType::L2UpdateEvent:
      return "L2UpdateEvent";
    case MessageType::MarkPriceEvent:
      return "MarkPriceEvent";
    case MessageType::FundingRateEvent:
      return "FundingRateEvent";
    case MessageType::OpenInterestEvent:
      return "OpenInterestEvent";
    default:
      return "Unknown";
  }
}

inline std::ostream& operator<<(std::ostream& os, MessageType type) {
  return os << to_string(type);
}

enum class Side : int8_t {
  Undefined = 0,
  Buy = 1,
  Sell = 2
};

inline const char* to_string(Side side) {
  switch (side) {
    case Side::Buy:
      return "Buy";
    case Side::Sell:
      return "Sell";
    case Side::Undefined:
      return "Undefined";
    default:
      return "Invalid";
  }
}

inline std::ostream& operator<<(std::ostream& os, Side s) {
  return os << to_string(s);
}

enum class OrderType : int8_t {
  Undefined = 0,
  Market = 1,
  Limit = 2,
  Stop = 3
};

inline const char* to_string(OrderType t) {
  switch (t) {
    case OrderType::Market:
      return "Market";
    case OrderType::Limit:
      return "Limit";
    case OrderType::Stop:
      return "Stop";
    case OrderType::Undefined:
      return "Undefined";
    default:
      return "Invalid";
  }
}

inline std::ostream& operator<<(std::ostream& os, OrderType t) {
  return os << to_string(t);
}

enum class TimeInForce : int8_t {
  Undefined = 0,
  Gtc = 1,
  Ioc = 2,
  Fok = 3
};

inline const char* to_string(TimeInForce tif) {
  switch (tif) {
    case TimeInForce::Gtc:
      return "Gtc";
    case TimeInForce::Ioc:
      return "Ioc";
    case TimeInForce::Fok:
      return "Fok";
    case TimeInForce::Undefined:
      return "Undefined";
    default:
      return "Invalid";
  }
}

inline std::ostream& operator<<(std::ostream& os, TimeInForce tif) {
  return os << to_string(tif);
}

enum class ExecInst : int8_t {
  Undefined = 0,
  Default = 1,
  ParticipateDontInitiate = 2
};

inline const char* to_string(ExecInst inst) {
  switch (inst) {
    case ExecInst::Default:
      return "Default";
    case ExecInst::ParticipateDontInitiate:
      return "ParticipateDontInitiate";
    case ExecInst::Undefined:
      return "Undefined";
    default:
      return "Invalid";
  }
}

inline std::ostream& operator<<(std::ostream& os, ExecInst inst) {
  return os << to_string(inst);
}

enum class RejectReason : int8_t {
  Unknown = 0,
  InvalidQuantity = 1,
  InvalidPrice = 2,
  RiskReject = 3,
  PostOnly = 4,
  RateLimit = 5,
  OrderUnknown = 6
};

inline const char* to_string(RejectReason reason) {
  switch (reason) {
    case RejectReason::Unknown:
      return "Unknown";
    case RejectReason::InvalidQuantity:
      return "InvalidQuantity";
    case RejectReason::InvalidPrice:
      return "InvalidPrice";
    case RejectReason::RiskReject:
      return "RiskReject";
    case RejectReason::PostOnly:
      return "PostOnly";
    case RejectReason::RateLimit:
      return "RateLimit";
    case RejectReason::OrderUnknown:
      return "OrderUnknown";
    default:
      return "Unknown";
  }
}

inline std::ostream& operator<<(std::ostream& os, RejectReason r) {
  return os << to_string(r);
}

enum class CancelReason : int8_t {
  UserRequest = 0,
  System = 1,
  Unknown = 2
};

inline const char* to_string(CancelReason reason) {
  switch (reason) {
    case CancelReason::UserRequest:
      return "UserRequest";
    case CancelReason::System:
      return "System";
    case CancelReason::Unknown:
      return "Unknown";
    default:
      return "UNKNOWN";
  }
}

inline std::ostream& operator<<(std::ostream& os, CancelReason r) {
  return os << to_string(r);
}

// Add these new enums for market data
enum class Exchange : int8_t {
  Binance = 1,
  BinanceDerivatives = 2,
  Okx = 3,
  Unknown = 4
};

enum class BooleanEnum : int8_t {
  FALSE = 0,
  TRUE = 1
};

constexpr size_t SERVICE_NAME_LENGTH = 16;

// ================= STRUCTS =================

struct alignas(64) HeartbeatEvent {
  static constexpr MessageType MESSAGE_TYPE = MessageType::Heartbeat;

  MessageType type_;
  int8_t header_reserved_[7];
  int64_t timestamp_ns_;
  char service_name_[SERVICE_NAME_LENGTH]{};
  char instance_name_[SERVICE_NAME_LENGTH]{};
  ServiceState service_state_;
  int8_t reserved_[15];

  HeartbeatEvent()
    : type_(MESSAGE_TYPE), header_reserved_{0}, timestamp_ns_(0),
      service_name_{0}, instance_name_{0}, service_state_(ServiceState::Starting),
      reserved_{0} {}
};

static_assert(sizeof(HeartbeatEvent) == 64, "HeartbeatEvent must be 64 bytes");

inline std::ostream& operator<<(std::ostream& os, const HeartbeatEvent& e) {
  os << "HeartbeatEvent{"
      << "timestamp_ns=" << e.timestamp_ns_
      << ", service_name=" << e.service_name_
      << ", instance_name=" << e.instance_name_
      << ", service_state=" << e.service_state_
      << "}";
  return os;
}


// ORDER EVENTS

struct alignas(64) PendingEvent {
  static constexpr MessageType MESSAGE_TYPE = MessageType::Pending;

  MessageType type_;
  Side side_;
  OrderType order_type_;
  TimeInForce time_in_force_;
  ExecInst exec_inst_;
  int8_t header_reserved_[3];
  int64_t timestamp_ns_;
  int64_t order_id_;
  int64_t parent_id_;
  int64_t request_id_;
  int64_t price_;
  int64_t quantity_;
  int32_t instrument_id_;
  int32_t account_;

  PendingEvent()
    : type_(MESSAGE_TYPE), side_(Side::Undefined), order_type_(OrderType::Undefined),
      time_in_force_(TimeInForce::Undefined), exec_inst_(ExecInst::Undefined),
      header_reserved_{0}, timestamp_ns_(0), order_id_(0), parent_id_(0),
      request_id_(0), price_(0), quantity_(0), instrument_id_(0), account_(0) {
  }
};

static_assert(sizeof(PendingEvent) == 64, "PendingEvent must be 64 bytes");

inline std::ostream& operator<<(std::ostream& os, const PendingEvent& e) {
  os << "PendingEvent{timestamp_ns=" << e.timestamp_ns_
      << ", order_id=" << e.order_id_
      << ", parent_id=" << e.parent_id_
      << ", request_id=" << e.request_id_
      << ", instrument_id=" << e.instrument_id_
      << ", account=" << e.account_
      << ", price=" << e.price_
      << ", quantity=" << e.quantity_
      << ", side=" << e.side_
      << ", order_type=" << e.order_type_
      << ", time_in_force=" << e.time_in_force_
      << ", exec_inst=" << e.exec_inst_
      << "}";
  return os;
}

struct alignas(64) AcceptedEvent {
  static constexpr MessageType MESSAGE_TYPE = MessageType::Accepted;

  MessageType type_;
  int8_t header_reserved_[7];
  int64_t timestamp_ns_;
  int64_t order_id_;
  int64_t exchange_order_id_;
  int8_t body_reserved_[32];

  AcceptedEvent()
    : type_(MESSAGE_TYPE), header_reserved_{0}, timestamp_ns_(0), order_id_(0),
      exchange_order_id_(0), body_reserved_{0} {
  }
};

static_assert(sizeof(AcceptedEvent) == 64, "AcceptedEvent must be 64 bytes");

inline std::ostream& operator<<(std::ostream& os, const AcceptedEvent& e) {
  os << "AcceptedEvent{timestamp_ns=" << e.timestamp_ns_
      << ", order_id=" << e.order_id_
      << ", exchange_order_id=" << e.exchange_order_id_
      << "}";
  return os;
}

struct alignas(64) RejectedEvent {
  static constexpr MessageType MESSAGE_TYPE = MessageType::Rejected;

  MessageType type_;
  int8_t header_reserved_[7];
  int64_t timestamp_ns_;
  int64_t order_id_;
  RejectReason reject_reason_;
  int8_t body_reserved_[39];

  RejectedEvent()
    : type_(MESSAGE_TYPE), header_reserved_{0}, timestamp_ns_(0), order_id_(0),
      reject_reason_(RejectReason::Unknown), body_reserved_{0} {
  }
};

static_assert(sizeof(RejectedEvent) == 64, "RejectedEvent must be 64 bytes");

inline std::ostream& operator<<(std::ostream& os, const RejectedEvent& e) {
  os << "RejectedEvent{timestamp=" << e.timestamp_ns_
      << ", order_id=" << e.order_id_
      << ", reject_reason=" << e.reject_reason_
      << "}";
  return os;
}

struct alignas(64) ExecutedEvent {
  static constexpr MessageType MESSAGE_TYPE = MessageType::Executed;

  MessageType type_;
  int8_t header_reserved_[7];
  int64_t timestamp_ns_;
  int64_t order_id_;
  int64_t exec_id_;
  int64_t last_price_;
  int64_t last_quantity_;
  int64_t commission_;
  int32_t instrument_id_;
  Side side_;
  int8_t body_reserved_[3];

  ExecutedEvent()
    : type_(MESSAGE_TYPE), header_reserved_{0}, timestamp_ns_(0), order_id_(0),
      exec_id_(0), last_price_(0), last_quantity_(0), commission_(0),
      instrument_id_(0), side_(Side::Undefined), body_reserved_{0} {
  }
};

static_assert(sizeof(ExecutedEvent) == 64, "ExecutedEvent must be 64 bytes");

inline std::ostream& operator<<(std::ostream& os, const ExecutedEvent& e) {
  os << "ExecutedEvent{timestamp=" << e.timestamp_ns_
      << ", order_id=" << e.order_id_
      << ", exec_id=" << e.exec_id_
      << ", instrument_id=" << e.instrument_id_
      << ", last_price=" << e.last_price_
      << ", last_quantity=" << e.last_quantity_
      << ", commission=" << e.commission_
      << ", side=" << e.side_
      << "}";
  return os;
}

struct alignas(64) CancelAcceptedEvent {
  static constexpr MessageType MESSAGE_TYPE = MessageType::CancelAccepted;

  MessageType type_;
  int8_t header_reserved_[7];
  int64_t timestamp_ns_;
  int64_t order_id_;
  CancelReason cancel_reason_;
  int8_t body_reserved_[39];

  CancelAcceptedEvent()
    : type_(MESSAGE_TYPE), header_reserved_{0}, timestamp_ns_(0), order_id_(0),
      cancel_reason_(CancelReason::Unknown), body_reserved_{0} {
  }
};

static_assert(sizeof(CancelAcceptedEvent) == 64, "CancelAcceptedEvent must be 64 bytes");

inline std::ostream& operator<<(std::ostream& os, const CancelAcceptedEvent& e) {
  os << "CancelAcceptedEvent{timestamp_ns=" << e.timestamp_ns_
      << ", order_id=" << e.order_id_
      << ", cancel_reason=" << e.cancel_reason_
      << "}";
  return os;
}

struct alignas(64) CancelRejectedEvent {
  static constexpr MessageType MESSAGE_TYPE = MessageType::CancelRejected;

  MessageType type_;
  int8_t header_reserved_[7];
  int64_t timestamp_ns_;
  int64_t order_id_;
  RejectReason reject_reason_;
  int8_t body_reserved_[39];

  CancelRejectedEvent()
    : type_(MESSAGE_TYPE), header_reserved_{0}, timestamp_ns_(0), order_id_(0),
      reject_reason_(RejectReason::Unknown), body_reserved_{0} {
  }
};

static_assert(sizeof(CancelRejectedEvent) == 64, "CancelRejectedEvent must be 64 bytes");


inline std::ostream& operator<<(std::ostream& os, const CancelRejectedEvent& e) {
  os << "CancelRejectedEvent{timestamp_ns=" << e.timestamp_ns_
      << ", order_id=" << e.order_id_
      << ", reject_reason=" << e.reject_reason_
      << "}";
  return os;
}

struct alignas(64) PendingCancelEvent {
  static constexpr MessageType MESSAGE_TYPE = MessageType::PendingCancel;

  MessageType type_;
  int8_t header_reserved_[7];
  int64_t timestamp_ns_;
  int64_t order_id_;
  int64_t request_id_;
  int8_t body_reserved_[32];

  PendingCancelEvent()
    : type_(MESSAGE_TYPE), header_reserved_{0}, timestamp_ns_(0), order_id_(0),
      request_id_(0), body_reserved_{0} {
  }
};

static_assert(sizeof(PendingCancelEvent) == 64, "PendingCancelEvent must be 64 bytes");

inline std::ostream& operator<<(std::ostream& os, const PendingCancelEvent& e) {
  os << "PendingCancelEvent{timestamp_ns=" << e.timestamp_ns_
      << ", order_id=" << e.order_id_
      << ", request_id=" << e.request_id_
      << "}";
  return os;
}

struct alignas(64) PendingReplaceEvent {
  static constexpr MessageType MESSAGE_TYPE = MessageType::PendingReplace;

  MessageType type_;
  int8_t header_reserved_[7];
  int64_t timestamp_ns_;
  int64_t order_id_;
  int64_t request_id_;
  int64_t quantity_;
  int64_t price_;
  int32_t instrument_id_;
  int8_t order_level_ = 0;
  int8_t body_reserved_[11];

  PendingReplaceEvent()
    : type_(MESSAGE_TYPE), header_reserved_{0}, timestamp_ns_(0), order_id_(0),
      request_id_(0), quantity_(0), price_(0), instrument_id_(0),
      order_level_(0), body_reserved_{0} {
  }
};

static_assert(sizeof(PendingReplaceEvent) == 64, "PendingReplaceEvent must be 64 bytes");

inline std::ostream& operator<<(std::ostream& os, const PendingReplaceEvent& e) {
  os << "PendingReplaceEvent{timestamp_ns=" << e.timestamp_ns_
      << ", order_id=" << e.order_id_
      << ", request_id=" << e.request_id_
      << ", quantity=" << e.quantity_
      << ", price=" << e.price_
      << ", order_level=" << e.order_level_
      << "}";
  return os;
}

struct alignas(64) ReplaceAcceptedEvent {
  static constexpr MessageType MESSAGE_TYPE = MessageType::ReplaceAccepted;

  MessageType type_;
  int8_t header_reserved_[7];
  int64_t timestamp_ns_;
  int64_t order_id_;
  int8_t body_reserved_[40];

  ReplaceAcceptedEvent()
    : type_(MESSAGE_TYPE), header_reserved_{0}, timestamp_ns_(0), order_id_(0),
      body_reserved_{0} {
  }
};

static_assert(sizeof(ReplaceAcceptedEvent) == 64, "ReplaceAcceptedEvent must be 64 bytes");

inline std::ostream& operator<<(std::ostream& os, const ReplaceAcceptedEvent& e) {
  os << "ReplaceAcceptedEvent{timestamp_ns=" << e.timestamp_ns_
      << ", order_id=" << e.order_id_
      << "}";
  return os;
}

struct alignas(64) ReplaceRejectedEvent {
  static constexpr MessageType MESSAGE_TYPE = MessageType::ReplaceRejected;

  MessageType type_;
  int8_t header_reserved_[7];
  int64_t timestamp_ns_;
  int64_t order_id_;
  RejectReason reject_reason_;
  int8_t body_reserved_[39];

  ReplaceRejectedEvent()
    : type_(MessageType::ReplaceRejected), header_reserved_{0}, timestamp_ns_(0), order_id_(0),
      reject_reason_(RejectReason::Unknown), body_reserved_{0} {
  }
};

static_assert(sizeof(ReplaceRejectedEvent) == 64, "ReplaceRejectedEvent must be 64 bytes");

inline std::ostream& operator<<(std::ostream& os, const ReplaceRejectedEvent& e) {
  os << "ReplaceRejectedEvent{timestamp_ns=" << e.timestamp_ns_
      << ", order_id=" << e.order_id_
      << ", reject_reason=" << e.reject_reason_
      << "}";
  return os;
}

// Trade event structure - simplified with timestamp_ns
struct alignas(64) TradeEvent {
  static constexpr MessageType MESSAGE_TYPE = MessageType::TradeEvent;

  MessageType type_;
  int8_t header_reserved_[7];
  int64_t timestamp_ns_;
  int64_t exchange_timestamp_;
  int64_t price_;
  int64_t size_;
  int32_t instrument_id_;
  Exchange exchange_;
  Side side_;
  int8_t body_reserved_[18];

  TradeEvent()
    : type_(MESSAGE_TYPE), header_reserved_{0}, timestamp_ns_(0), exchange_timestamp_(0),
      price_(0), size_(0), instrument_id_(0), exchange_(Exchange::Unknown),
      side_(Side::Undefined), body_reserved_{0} {}

};

static_assert(sizeof(TradeEvent) == 64, "TradeEvent must be 64 bytes");

inline std::ostream& operator<<(std::ostream& os, const TradeEvent& e) {
  os << "TradeEvent{timestamp_ns=" << e.timestamp_ns_
      << ", exchange_timestamp=" << e.exchange_timestamp_
      << ", exchange=" << static_cast<int>(e.exchange_)
      << ", instrument_id=" << e.instrument_id_
      << ", price=" << e.price_
      << ", size=" << e.size_
      << ", side=" << e.side_
      << "}";
  return os;
}

// L1 update event structure - simplified with timestamp_ns
struct alignas(64) L1UpdateEvent {
  static constexpr MessageType MESSAGE_TYPE = MessageType::L1UpdateEvent;
  MessageType type_;
  int8_t header_reserved_[7];
  int64_t timestamp_ns_;
  int64_t exchange_timestamp_;
  int64_t bid_price_;
  int64_t bid_size_;
  int64_t offer_price_;
  int64_t offer_size_;
  int32_t instrument_id_;
  Exchange exchange_;
  int8_t body_reserved_[3];

  L1UpdateEvent()
    : type_(MESSAGE_TYPE), header_reserved_{0}, timestamp_ns_(0), exchange_timestamp_(0),
      bid_price_(0), bid_size_(0), offer_price_(0), offer_size_(0), instrument_id_(0),
      exchange_(Exchange::Unknown), body_reserved_{0} {}
};

static_assert(sizeof(L1UpdateEvent) == 64, "L1UpdateLevelEvent must be 64 bytes");


inline std::ostream& operator<<(std::ostream& os, const L1UpdateEvent& e) {
  os << "L1UpdateEvent{timestamp_ns=" << e.timestamp_ns_
      << ", exchange_timestamp=" << e.exchange_timestamp_
      << ", exchange=" << static_cast<int>(e.exchange_)
      << ", instrument_id=" << e.instrument_id_
      << ", bid_price=" << e.bid_price_
      << ", bid_size=" << e.bid_size_
      << ", offer_price=" << e.offer_price_
      << ", offer_size=" << e.offer_size_
      << "}";
  return os;
}

struct alignas(64) L2UpdateEvent {
  static constexpr MessageType MESSAGE_TYPE = MessageType::L2UpdateEvent;

  // ===== Header: 16 bytes =====
  MessageType type_;                  // 1 byte
  Exchange exchange_;                  // 1 byte
  Side side_;                          // 1 byte - SHARED for both levels
  uint8_t num_levels_;                 // 1 byte - 1 or 2
  int32_t instrument_id_;              // 4 bytes
  int64_t timestamp_ns_;               // 8 bytes - LOCAL capture time

  // ===== Batch Flags: 8 bytes =====
  BooleanEnum snapshot_;               // 1 byte
  BooleanEnum is_batch_message_;       // 1 byte - TRUE if part of multi-message batch
  BooleanEnum is_last_batch_;          // 1 byte - TRUE if last message in batch
  int8_t header_reserved_[5];          // 5 bytes padding

  // ===== Level Data: 32 bytes (2×16) =====
  int64_t price_1_;                    // 8 bytes
  int64_t size_1_;                     // 8 bytes
  int64_t price_2_;                    // 8 bytes (0 if num_levels==1)
  int64_t size_2_;                     // 8 bytes (0 if num_levels==1)

  // ===== Reserved: 7 bytes =====
  int8_t reserved_[7];                 // Future expansion; alignas(64) supplies the final padding byte

  L2UpdateEvent()
    : type_(MESSAGE_TYPE), exchange_(Exchange::Unknown),
      side_(Side::Undefined), num_levels_(0),
      instrument_id_(0), timestamp_ns_(0),
      snapshot_(BooleanEnum::FALSE),
      is_batch_message_(BooleanEnum::FALSE), is_last_batch_(BooleanEnum::FALSE),
      header_reserved_{0},
      price_1_(0), size_1_(0), price_2_(0), size_2_(0),
      reserved_{0} {}

};

static_assert(sizeof(L2UpdateEvent) == 64, "L2UpdateLevelEvent must be 64 bytes");

inline std::ostream& operator<<(std::ostream& os, const L2UpdateEvent& e) {
  os << "L2UpdateEvent{"
      << "timestamp_ns=" << e.timestamp_ns_
      << ", instrument_id=" << e.instrument_id_
      << ", exchange=" << static_cast<int>(e.exchange_)
      << ", side=" << to_string(e.side_)
      << ", num_levels=" << static_cast<int>(e.num_levels_)
      << ", snapshot=" << (e.snapshot_ == BooleanEnum::TRUE ? "true" : "false")
      << ", is_last_batch=" << (e.is_last_batch_ == BooleanEnum::TRUE ? "true" : "false")
      << ", price_1=" << e.price_1_
      << ", size_1=" << e.size_1_;
  if (e.num_levels_ == 2) {
    os << ", price_2=" << e.price_2_
       << ", size_2=" << e.size_2_;
  }
  os << "}";
  return os;
}

struct alignas(64) MarkPriceEvent {
  static constexpr MessageType MESSAGE_TYPE = MessageType::MarkPriceEvent;

  MessageType type_;
  int8_t header_reserved_[7];
  int64_t timestamp_ns_;
  int64_t exchange_timestamp_ns_;
  int64_t mark_price_;
  int64_t index_price_;
  int32_t instrument_id_;
  Exchange exchange_;
  int8_t body_reserved_[19];

  MarkPriceEvent()
      : type_(MESSAGE_TYPE),
        header_reserved_{0},
        timestamp_ns_(0),
        exchange_timestamp_ns_(0),
        mark_price_(0),
        index_price_(0),
        instrument_id_(0),
        exchange_(Exchange::Unknown),
        body_reserved_{0} {}
};

static_assert(sizeof(MarkPriceEvent) == 64, "MarkPriceEvent must be 64 bytes");

inline std::ostream& operator<<(std::ostream& os, const MarkPriceEvent& e) {
  os << "MarkPriceEvent{timestamp_ns=" << e.timestamp_ns_
     << ", exchange_timestamp_ns=" << e.exchange_timestamp_ns_
     << ", mark_price=" << e.mark_price_
     << ", index_price=" << e.index_price_
     << ", instrument_id=" << e.instrument_id_
     << ", exchange=" << static_cast<int>(e.exchange_)
     << "}";
  return os;
}

struct alignas(64) FundingRateEvent {
  static constexpr MessageType MESSAGE_TYPE = MessageType::FundingRateEvent;

  MessageType type_;
  int8_t header_reserved_[7];
  int64_t timestamp_ns_;
  int64_t funding_time_ns_;
  int64_t next_funding_time_ns_;
  int64_t funding_rate_;
  int64_t next_funding_rate_;
  int64_t interest_rate_;
  int32_t instrument_id_;
  Exchange exchange_;
  int8_t body_reserved_[3];

  FundingRateEvent()
      : type_(MESSAGE_TYPE),
        header_reserved_{0},
        timestamp_ns_(0),
        funding_time_ns_(0),
        next_funding_time_ns_(0),
        funding_rate_(0),
        next_funding_rate_(0),
        interest_rate_(0),
        instrument_id_(0),
        exchange_(Exchange::Unknown),
        body_reserved_{0} {}
};

static_assert(sizeof(FundingRateEvent) == 64, "FundingRateEvent must be 64 bytes");

inline std::ostream& operator<<(std::ostream& os, const FundingRateEvent& e) {
  os << "FundingRateEvent{timestamp_ns=" << e.timestamp_ns_
     << ", funding_time_ns=" << e.funding_time_ns_
     << ", next_funding_time_ns=" << e.next_funding_time_ns_
     << ", funding_rate=" << e.funding_rate_
     << ", next_funding_rate=" << e.next_funding_rate_
     << ", interest_rate=" << e.interest_rate_
     << ", instrument_id=" << e.instrument_id_
     << ", exchange=" << static_cast<int>(e.exchange_)
     << "}";
  return os;
}

struct alignas(64) OpenInterestEvent {
  static constexpr MessageType MESSAGE_TYPE = MessageType::OpenInterestEvent;

  MessageType type_;
  int8_t header_reserved_[7];
  int64_t timestamp_ns_;
  int64_t exchange_timestamp_ns_;
  int64_t open_interest_;
  int64_t open_interest_currency_;
  int64_t open_interest_usd_;
  int32_t instrument_id_;
  Exchange exchange_;
  int8_t body_reserved_[11];

  OpenInterestEvent()
      : type_(MESSAGE_TYPE),
        header_reserved_{0},
        timestamp_ns_(0),
        exchange_timestamp_ns_(0),
        open_interest_(0),
        open_interest_currency_(0),
        open_interest_usd_(0),
        instrument_id_(0),
        exchange_(Exchange::Unknown),
        body_reserved_{0} {}
};

static_assert(sizeof(OpenInterestEvent) == 64, "OpenInterestEvent must be 64 bytes");

inline std::ostream& operator<<(std::ostream& os, const OpenInterestEvent& e) {
  os << "OpenInterestEvent{timestamp_ns=" << e.timestamp_ns_
     << ", exchange_timestamp_ns=" << e.exchange_timestamp_ns_
     << ", open_interest=" << e.open_interest_
     << ", open_interest_currency=" << e.open_interest_currency_
     << ", open_interest_usd=" << e.open_interest_usd_
     << ", instrument_id=" << e.instrument_id_
     << ", exchange=" << static_cast<int>(e.exchange_)
     << "}";
  return os;
}


// Raw 64-byte buffer that can hold any message type
struct alignas(64) MessageSlot {
    alignas(64) std::byte data_[64];

    MessageSlot() : data_{} {}

    // Get the message type (always at offset 0). Read via memcpy so no
    // object of type MessageType has to live at that address.
    MessageType get_type() const noexcept {
        MessageType type;
        std::memcpy(&type, data_, sizeof(type));
        return type;
    }

    // Raw access
    std::byte* raw_data() noexcept { return data_; }
    const std::byte* raw_data() const noexcept { return data_; }

    // Direct cast access for reading. Events are placement-new'd into data_
    // by the writers; std::launder makes the pointer to that object valid.
    template<typename T>
    const T& as() const noexcept {
        static_assert(sizeof(T) <= 64, "Message too large");
        return *std::launder(reinterpret_cast<const T*>(data_));
    }
};

static_assert(sizeof(MessageSlot) == 64, "MessageSlot must be exactly 64 bytes");
static_assert(alignof(MessageSlot) == 64, "MessageSlot must be 64-byte aligned");

using BaseEvent = std::variant<
  HeartbeatEvent,
  PendingEvent,
  AcceptedEvent,
  RejectedEvent,
  PendingReplaceEvent,
  ReplaceAcceptedEvent,
  ReplaceRejectedEvent,
  PendingCancelEvent,
  CancelAcceptedEvent,
  CancelRejectedEvent,
  ExecutedEvent
>;
} // namespace reflex
