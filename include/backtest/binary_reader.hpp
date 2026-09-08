#pragma once

#include <cstdint>
#include <fstream>
#include <memory>
#include <vector>
#include <string>
#include "messages.hpp"
#include "file_header.hpp"

namespace reflex::backtest {

class BinaryReader {
public:
  explicit BinaryReader(const std::string& file_path);
  ~BinaryReader();

  bool open();
  void close();

  const MessageSlot* read_next_message();

  inline const MessageSlot* peek_next_message() const {
    return (is_end_of_file() || !cursor_) ? nullptr : 
           reinterpret_cast<const MessageSlot*>(cursor_);
  }
  
  inline int64_t peek_next_timestamp() const {
    const MessageSlot* slot = peek_next_message();
    if (!slot) return INT64_MAX;

    switch (slot->get_type()) {
      case reflex::MessageType::L1UpdateEvent:
        return slot->as<reflex::L1UpdateEvent>().timestamp_ns_;
      case reflex::MessageType::L2UpdateEvent:
        return slot->as<reflex::L2UpdateEvent>().timestamp_ns_;
      case reflex::MessageType::TradeEvent:
        return slot->as<reflex::TradeEvent>().timestamp_ns_;
      case reflex::MessageType::MarkPriceEvent:
        return slot->as<reflex::MarkPriceEvent>().timestamp_ns_;
      case reflex::MessageType::FundingRateEvent:
        return slot->as<reflex::FundingRateEvent>().timestamp_ns_;
      case reflex::MessageType::OpenInterestEvent:
        return slot->as<reflex::OpenInterestEvent>().timestamp_ns_;
      case reflex::MessageType::LiquidationEvent:
        return slot->as<reflex::LiquidationEvent>().timestamp_ns_;
      default:
        return slot->as<reflex::HeartbeatEvent>().timestamp_ns_;
    }
  }

  uint64_t total_messages() const { return total_messages_; }
  uint64_t current_position() const { return current_position_; }
  bool is_end_of_file() const { return cursor_ >= data_end_; }

  const reflex::FileHeader& get_header() const;

private:
  std::string file_path_;
  int fd_ = -1;
  void* mapped_memory_ = nullptr;
  size_t file_size_ = 0;

  const char* data_start_ = nullptr;
  const char* cursor_   = nullptr;
  const char* data_end_ = nullptr;
  uint64_t total_messages_ = 0;
  uint64_t current_position_ = 0;

  bool memory_map_file();
  void unmap_file();
};

class MultiFileBinaryReader {
public:
  explicit MultiFileBinaryReader(std::vector<std::string> file_paths,
                                 bool merge_by_timestamp = false);

  const MessageSlot* read_next_message();

  uint64_t total_messages() const;
  bool is_end_of_files();
  bool has_more_data();
  int64_t peek_next_timestamp();

  const std::string& current_file() const;

private:
  bool open_current_reader();
  void close_current_reader();
  void advance_to_next_file();

  std::vector<std::string> file_paths_;
  bool merge_by_timestamp_{false};
  std::vector<std::unique_ptr<BinaryReader>> merged_readers_;
  size_t current_reader_index_ = 0;
  std::unique_ptr<BinaryReader> current_reader_;
  mutable bool total_messages_cached_{false};
  mutable uint64_t cached_total_messages_{0};
};

}
