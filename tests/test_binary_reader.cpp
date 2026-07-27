// tests/test_binary_reader.cpp
//
// BinaryReader / MultiFileBinaryReader EOF-sentinel behavior (header-only and
// unreadable files) and a binary_splitter -> binary_reader round trip.

#include "backtest/binary_reader.hpp"
#include "backtest/binary_splitter.hpp"
#include "asset_info_manager.hpp"
#include "file_header.hpp"
#include "messages.hpp"

#include <gtest/gtest.h>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

using namespace reflex;
using namespace reflex::backtest;

namespace {

MessageSlot make_l1(int64_t ts, int32_t instrument_id, int64_t bid, int64_t ask) {
  MessageSlot slot;
  auto* e = new (slot.raw_data()) L1UpdateEvent();
  e->timestamp_ns_ = ts;
  e->instrument_id_ = instrument_id;
  e->bid_price_ = bid;
  e->bid_size_ = 1;
  e->offer_price_ = ask;
  e->offer_size_ = 1;
  return slot;
}

MessageSlot make_trade(int64_t ts, int32_t instrument_id, int64_t price) {
  MessageSlot slot;
  auto* e = new (slot.raw_data()) TradeEvent();
  e->timestamp_ns_ = ts;
  e->instrument_id_ = instrument_id;
  e->side_ = Side::Sell;
  e->price_ = price;
  e->size_ = 1;
  return slot;
}

std::string write_capture(const std::string& name, const std::vector<MessageSlot>& slots) {
  const std::string path = (std::filesystem::temp_directory_path() / name).string();
  std::FILE* f = std::fopen(path.c_str(), "wb");
  EXPECT_NE(f, nullptr);
  FileHeader header{};  // default ctor sets magic + version
  header.message_slot_size_ = sizeof(MessageSlot);
  EXPECT_EQ(std::fwrite(&header, sizeof(header), 1, f), 1u);
  for (const auto& slot : slots) {
    EXPECT_EQ(std::fwrite(slot.raw_data(), sizeof(MessageSlot), 1, f), 1u);
  }
  std::fclose(f);
  return path;
}

}  // namespace

TEST(BinaryReaderTest, GetHeaderThrowsBeforeOpen) {
  BinaryReader reader("/nonexistent/reflex_no_such_file.bin");
  EXPECT_THROW(reader.get_header(), std::logic_error);
}

TEST(BinaryReaderTest, HeaderOnlyFileHasNoMessages) {
  const auto path = write_capture("reflex_reader_header_only.bin", {});
  BinaryReader reader(path);
  ASSERT_TRUE(reader.open());
  EXPECT_EQ(reader.total_messages(), 0u);
  EXPECT_TRUE(reader.is_end_of_file());
  EXPECT_EQ(reader.peek_next_timestamp(), INT64_MAX);
  EXPECT_EQ(reader.read_next_message(), nullptr);
}

TEST(MultiFileBinaryReaderTest, HeaderOnlyFileListReportsNoData) {
  const auto path = write_capture("reflex_multi_header_only.bin", {});
  MultiFileBinaryReader reader({path});
  EXPECT_FALSE(reader.has_more_data());
  EXPECT_TRUE(reader.is_end_of_files());
  EXPECT_EQ(reader.peek_next_timestamp(), INT64_MAX);
  EXPECT_EQ(reader.read_next_message(), nullptr);
}

TEST(MultiFileBinaryReaderTest, SkipsUnreadableAndHeaderOnlyTrailingFiles) {
  const int64_t base = 1'700'000'000'000'000'000LL;
  const auto valid = write_capture("reflex_multi_valid.bin",
                                   {make_l1(base, 1, 100, 101),
                                    make_trade(base + 1'000, 1, 100),
                                    make_l1(base + 2'000, 1, 100, 101)});
  const std::string missing =
      (std::filesystem::temp_directory_path() / "reflex_multi_missing.bin").string();
  const auto empty = write_capture("reflex_multi_empty_tail.bin", {});

  MultiFileBinaryReader reader({valid, missing, empty});

  // Peek/read the three real messages; timestamps must agree with the peek.
  for (int i = 0; i < 3; ++i) {
    ASSERT_TRUE(reader.has_more_data());
    const int64_t peeked = reader.peek_next_timestamp();
    EXPECT_EQ(peeked, base + i * 1'000);
    ASSERT_NE(reader.read_next_message(), nullptr);
  }

  // Remaining paths are unreadable/header-only: the reader must say so, not
  // promise phantom data (which used to null-deref in the engine).
  EXPECT_FALSE(reader.has_more_data());
  EXPECT_TRUE(reader.is_end_of_files());
  EXPECT_EQ(reader.peek_next_timestamp(), INT64_MAX);
  EXPECT_EQ(reader.read_next_message(), nullptr);
}

TEST(BinarySplitterTest, SplitAndReadBackRoundTrip) {
  reflex::AssetInfoManager::initialize();
  const int64_t base = 1'700'000'000'000'000'000LL;

  // Interleaved two-instrument capture.
  std::vector<MessageSlot> slots;
  for (int i = 0; i < 10; ++i) {
    const int32_t instr = (i % 2 == 0) ? 1 : 2;
    if (i % 3 == 0) {
      slots.push_back(make_trade(base + i * 1'000, instr, 100 + i));
    } else {
      slots.push_back(make_l1(base + i * 1'000, instr, 100 + i, 101 + i));
    }
  }
  const auto input = write_capture("reflex_split_input.bin", slots);

  const auto out_dir =
      (std::filesystem::temp_directory_path() / "reflex_split_out").string();
  std::filesystem::remove_all(out_dir);

  BinarySplitter splitter(std::vector<std::string>{input});
  splitter.set_use_asset_names(false);
  ASSERT_TRUE(splitter.split_by_instrument(out_dir));

  const auto& stats = splitter.get_stats();
  EXPECT_EQ(stats.total_events, slots.size());
  EXPECT_EQ(stats.total_instruments, 2u);

  // Read each instrument's chunks back and compare against the source slots,
  // byte for byte and in original order.
  for (const uint32_t instr : {1u, 2u}) {
    ASSERT_TRUE(stats.instrument_files.count(instr));
    MultiFileBinaryReader reader(stats.instrument_files.at(instr));

    size_t matched = 0;
    for (const auto& original : slots) {
      // Select this instrument's slots from the interleaved input.
      const int32_t original_instr = (original.get_type() == MessageType::TradeEvent)
                                         ? original.as<TradeEvent>().instrument_id_
                                         : original.as<L1UpdateEvent>().instrument_id_;
      if (static_cast<uint32_t>(original_instr) != instr) continue;

      const MessageSlot* read_back = reader.read_next_message();
      ASSERT_NE(read_back, nullptr);
      EXPECT_EQ(std::memcmp(read_back->raw_data(), original.raw_data(), sizeof(MessageSlot)), 0);
      ++matched;
    }
    EXPECT_EQ(matched, 5u);
    EXPECT_FALSE(reader.has_more_data());
  }

  std::filesystem::remove_all(out_dir);
}
