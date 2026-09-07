// include/backtest/binary_reader.hpp
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

  // Zero-copy read - returns pointer directly into mmap'd memory
  const MessageSlot* read_next_message();

  // Zero-copy peek - just look at next slot without advancing cursor
  inline const MessageSlot* peek_next_message() const {
    return (is_end_of_file() || !cursor_) ? nullptr : 
           reinterpret_cast<const MessageSlot*>(cursor_);
  }
  
  // Optimized timestamp extraction. INT64_MAX = no more messages — the same
  // "nothing here" sentinel the engine's k-way time merge uses, so an exhausted
  // file can never win the merge (a UINT64_MAX sentinel used to wrap to -1 when
  // cast to int64_t and drag the simulation clock backwards).
  inline int64_t peek_next_timestamp() const {
    const MessageSlot* slot = peek_next_message();
    if (!slot) return INT64_MAX;

    // Fast path - most common message types first
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
        // Every message struct shares the common header (type_ + 7 reserved bytes,
        // then timestamp_ns_ at offset 8) — read it generically. Returning 0 here
        // used to drag the simulation clock backwards to the epoch on any
        // unhandled type.
        return slot->as<reflex::HeartbeatEvent>().timestamp_ns_;
    }
  }

  // Stats
  uint64_t total_messages() const { return total_messages_; }
  uint64_t current_position() const { return current_position_; }
  bool is_end_of_file() const { return cursor_ >= data_end_; }

  // Throws std::logic_error if the file is not open.
  const reflex::FileHeader& get_header() const;

private:
  std::string file_path_;
  int fd_ = -1;
  void* mapped_memory_ = nullptr;
  size_t file_size_ = 0;

  const char* data_start_ = nullptr;
  // Pointer‐walking state (avoids per‑slot multiply)
  const char* cursor_   = nullptr;   // next unread slot
  const char* data_end_ = nullptr;   // one‑past‑last byte
  uint64_t total_messages_ = 0;
  uint64_t current_position_ = 0;

  bool memory_map_file();
  void unmap_file();
};

// Multi-file reader that lazily mmaps one chunk at a time
class MultiFileBinaryReader {
public:
  // merge_by_timestamp=false replays the files back to back in the given
  // order. merge_by_timestamp=true keeps every file open and always hands out
  // the earliest pending event across them (a k-way merge of independently
  // sorted captures, e.g. paired venues).
  explicit MultiFileBinaryReader(std::vector<std::string> file_paths,
                                 bool merge_by_timestamp = false);

  const MessageSlot* read_next_message();

  uint64_t total_messages() const;
  // These three may advance past unreadable or header-only files to give a
  // truthful answer, so they mutate reader state and are deliberately non-const.
  bool is_end_of_files();
  bool has_more_data();
  int64_t peek_next_timestamp();  // INT64_MAX = all files exhausted

  // Path of the file currently being read (for error reporting); empty when exhausted.
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

} // namespace reflex::backtest
