#include "gtest/gtest.h"

#define private public
#define protected public
#include "../include/components/blob/okx_blob.hpp"
#undef private
#undef protected

#include "../include/framework/component_config.hpp"
#include "../include/messages.hpp"
#include "../include/utils/codec_utils.hpp"

#include <disruptorplus/ring_buffer.hpp>
#include <disruptorplus/sequence_barrier.hpp>
#include <disruptorplus/single_threaded_claim_strategy.hpp>
#include <disruptorplus/spin_wait_strategy.hpp>

#include <memory>
#include <string>
#include <vector>

using namespace reflex;
namespace {

class OkxBlobTest : public ::testing::Test {
 protected:
  void SetUp() override {
    constexpr size_t kBufferSize = 1024;
    ring_buffer_ = std::make_shared<disruptorplus::ring_buffer<MessageSlot>>(kBufferSize);
    wait_strategy_ = std::make_shared<disruptorplus::spin_wait_strategy>();
    claim_strategy_ = std::make_shared<disruptorplus::single_threaded_claim_strategy<disruptorplus::spin_wait_strategy>>(
        kBufferSize, *wait_strategy_);
    consumer_barrier_ =
        std::make_shared<disruptorplus::sequence_barrier<disruptorplus::spin_wait_strategy>>(*wait_strategy_);
    claim_strategy_->add_claim_barrier(*consumer_barrier_);

    ComponentConfig base_config{
        .component_name_ = "okx_blob_test",
        .environment_ = "test",
    };
    BlobConfig blob_config(base_config, "wss://ws.okx.com:8443/ws/v5/public", "/tmp/okx_creds.txt",
                           std::vector<std::string>{instrument_});
    blob_ = std::make_unique<OkxBlob>(blob_config, ring_buffer_, claim_strategy_);
  }

  void TearDown() override {
    consumer_barrier_.reset();
    blob_.reset();
    claim_strategy_.reset();
    wait_strategy_.reset();
    ring_buffer_.reset();
  }

  std::string make_trade_message(const std::string& side) const {
    return std::string("{\"arg\":{\"channel\":\"trades-all\",\"instId\":\"") + instrument_
        + "\"},\"data\":[{\"instId\":\"" + instrument_ + "\",\"side\":\"" + side
        + "\",\"px\":\"100.0\",\"sz\":\"0.01\",\"ts\":\"1700000000000\"}]}";
  }

  std::string make_bbo_message() const {
    return std::string("{\"arg\":{\"channel\":\"bbo-tbt\",\"instId\":\"") + instrument_
        + "\"},\"data\":[{\"asks\":[[\"100.6\",\"3\",\"0\",\"1\"]],\"bids\":[[\"100.5\",\"2\",\"0\",\"1\"]],"
        + "\"ts\":\"1700000000000\"}]}";
  }

  std::string make_l2_snapshot_message() const {
    return std::string("{\"arg\":{\"channel\":\"books\",\"instId\":\"") + instrument_
        + "\"},\"action\":\"snapshot\",\"data\":[{"
        + "\"asks\":[[\"100.1\",\"1\",\"0\",\"1\"]],"
        + "\"bids\":[[\"100.0\",\"1\",\"0\",\"1\"],[\"99.9\",\"2\",\"0\",\"1\"]],"
        + "\"ts\":\"1700000000000\"}]}";
  }

  std::string make_l2_seq_message(const std::string& action, int64_t prev_seq_id, int64_t seq_id) const {
    return std::string("{\"arg\":{\"channel\":\"books\",\"instId\":\"") + instrument_ + "\"},\"action\":\"" + action
        + "\",\"data\":[{"
        + "\"asks\":[[\"100.1\",\"1\",\"0\",\"1\"]],"
        + "\"bids\":[[\"100.0\",\"1\",\"0\",\"1\"]],"
        + "\"ts\":\"1700000000000\",\"prevSeqId\":" + std::to_string(prev_seq_id) + ",\"seqId\":"
        + std::to_string(seq_id) + "}]}";
  }

  template <typename Event>
  const Event& event_at(disruptorplus::sequence_t seq) const {
    return (*ring_buffer_)[seq].template as<Event>();
  }

  static constexpr disruptorplus::sequence_t kNone = static_cast<disruptorplus::sequence_t>(-1);
  disruptorplus::sequence_t last_seq() const { return claim_strategy_->last_published(); }
  bool published() const { return last_seq() != kNone; }

  void drain() {
    if (published()) {
      consumer_barrier_->publish(last_seq());
    }
  }

  const std::string instrument_{"BTC-USDT-SWAP"};
  std::unique_ptr<OkxBlob> blob_;
  std::shared_ptr<disruptorplus::ring_buffer<MessageSlot>> ring_buffer_;
  std::shared_ptr<disruptorplus::spin_wait_strategy> wait_strategy_;
  std::shared_ptr<disruptorplus::single_threaded_claim_strategy<disruptorplus::spin_wait_strategy>> claim_strategy_;
  std::shared_ptr<disruptorplus::sequence_barrier<disruptorplus::spin_wait_strategy>> consumer_barrier_;
};

TEST_F(OkxBlobTest, ParsesBuyTradeSide) {
  blob_->handle_business_message(make_trade_message("buy"));
  ASSERT_TRUE(published()) << "No trade event was published";
  const TradeEvent& event = event_at<TradeEvent>(last_seq());
  EXPECT_EQ(event.type_, MessageType::TradeEvent);
  EXPECT_EQ(event.side_, Side::Buy);
  EXPECT_EQ(event.price_, CodecUtils::encode_price("100.0"));
  drain();
}

TEST_F(OkxBlobTest, ParsesSellTradeSide) {
  blob_->handle_business_message(make_trade_message("sell"));
  ASSERT_TRUE(published()) << "No trade event was published";
  const TradeEvent& event = event_at<TradeEvent>(last_seq());
  EXPECT_EQ(event.side_, Side::Sell);
  drain();
}

TEST_F(OkxBlobTest, ParsesBboIntoL1Update) {
  blob_->handle_public_message(make_bbo_message());
  ASSERT_TRUE(published()) << "No L1 event was published";
  const L1UpdateEvent& event = event_at<L1UpdateEvent>(last_seq());
  EXPECT_EQ(event.type_, MessageType::L1UpdateEvent);
  EXPECT_EQ(event.bid_price_, CodecUtils::encode_price("100.5"));
  EXPECT_EQ(event.offer_price_, CodecUtils::encode_price("100.6"));
  EXPECT_EQ(event.exchange_, Exchange::Okx);
  drain();
}

TEST_F(OkxBlobTest, ParsesL2SnapshotWithActionField) {
  blob_->handle_public_message(make_l2_snapshot_message());
  ASSERT_TRUE(published()) << "Expected L2 messages to be published";
  ASSERT_EQ(last_seq(), 1u) << "Expected exactly two L2 messages (bid+ask)";
  const L2UpdateEvent& bids = event_at<L2UpdateEvent>(0);
  EXPECT_EQ(bids.type_, MessageType::L2UpdateEvent);
  EXPECT_EQ(bids.side_, Side::Buy);
  EXPECT_EQ(bids.snapshot_, BooleanEnum::TRUE);
  EXPECT_EQ(bids.num_levels_, 2);
  EXPECT_EQ(bids.price_1_, CodecUtils::encode_price("100.0"));
  EXPECT_EQ(bids.price_2_, CodecUtils::encode_price("99.9"));

  const L2UpdateEvent& asks = event_at<L2UpdateEvent>(1);
  EXPECT_EQ(asks.side_, Side::Sell);
  EXPECT_EQ(asks.snapshot_, BooleanEnum::TRUE);
  EXPECT_EQ(asks.price_1_, CodecUtils::encode_price("100.1"));
  drain();
}

TEST_F(OkxBlobTest, PongIsIgnored) {
  EXPECT_NO_THROW(blob_->handle_public_message("pong"));
  EXPECT_NO_THROW(blob_->handle_business_message("pong"));
  EXPECT_FALSE(published()) << "pong must not publish anything";
}

TEST_F(OkxBlobTest, MalformedJsonIsIgnored) {
  EXPECT_NO_THROW(blob_->handle_public_message("{not valid json"));
  EXPECT_NO_THROW(blob_->handle_public_message(""));
  EXPECT_FALSE(published()) << "malformed input must not publish anything";
}


TEST_F(OkxBlobTest, TradeWithMissingPriceIsDropped) {
  const std::string msg = std::string("{\"arg\":{\"channel\":\"trades-all\",\"instId\":\"") + instrument_
      + "\"},\"data\":[{\"instId\":\"" + instrument_ + "\",\"side\":\"buy\",\"sz\":\"0.01\",\"ts\":\"1700000000000\"}]}";
  EXPECT_NO_THROW(blob_->handle_business_message(msg));
  EXPECT_FALSE(published()) << "trade without px must be dropped";
}

TEST_F(OkxBlobTest, TradeWithMissingTimestampIsDropped) {
  const std::string msg = std::string("{\"arg\":{\"channel\":\"trades-all\",\"instId\":\"") + instrument_
      + "\"},\"data\":[{\"instId\":\"" + instrument_ + "\",\"side\":\"sell\",\"px\":\"100.0\",\"sz\":\"0.01\"}]}";
  EXPECT_NO_THROW(blob_->handle_business_message(msg));
  EXPECT_FALSE(published()) << "trade without ts must be dropped";
}

TEST_F(OkxBlobTest, TradeWithNumericPriceTypeIsDropped) {
  const std::string msg = std::string("{\"arg\":{\"channel\":\"trades-all\",\"instId\":\"") + instrument_
      + "\"},\"data\":[{\"instId\":\"" + instrument_ + "\",\"side\":\"buy\",\"px\":100.0,\"sz\":0.01,\"ts\":\"1700000000000\"}]}";
  EXPECT_NO_THROW(blob_->handle_business_message(msg));
  EXPECT_FALSE(published()) << "trade with non-string px/sz must be dropped";
}

TEST_F(OkxBlobTest, TradeWithEmptyDataArrayIsDropped) {
  const std::string msg = std::string("{\"arg\":{\"channel\":\"trades-all\",\"instId\":\"") + instrument_
      + "\"},\"data\":[]}";
  EXPECT_NO_THROW(blob_->handle_business_message(msg));
  EXPECT_FALSE(published());
}

TEST_F(OkxBlobTest, BboWithMissingTimestampIsDropped) {
  const std::string msg = std::string("{\"arg\":{\"channel\":\"bbo-tbt\",\"instId\":\"") + instrument_
      + "\"},\"data\":[{\"asks\":[[\"100.6\",\"3\",\"0\",\"1\"]],\"bids\":[[\"100.5\",\"2\",\"0\",\"1\"]]}]}";
  EXPECT_NO_THROW(blob_->handle_public_message(msg));
  EXPECT_FALSE(published()) << "bbo without ts must be dropped";
}

TEST_F(OkxBlobTest, BboWithNumericLevelTypesIsDropped) {
  const std::string msg = std::string("{\"arg\":{\"channel\":\"bbo-tbt\",\"instId\":\"") + instrument_
      + "\"},\"data\":[{\"asks\":[[100.6,3,0,1]],\"bids\":[[100.5,2,0,1]],\"ts\":\"1700000000000\"}]}";
  EXPECT_NO_THROW(blob_->handle_public_message(msg));
  EXPECT_FALSE(published()) << "bbo with non-string levels must be dropped";
}

TEST_F(OkxBlobTest, BboWithDataAsObjectIsDropped) {
  const std::string msg = std::string("{\"arg\":{\"channel\":\"bbo-tbt\",\"instId\":\"") + instrument_
      + "\"},\"data\":{\"ts\":\"1700000000000\"}}";
  EXPECT_NO_THROW(blob_->handle_public_message(msg));
  EXPECT_FALSE(published()) << "data as object (not array) must be dropped";
}

TEST_F(OkxBlobTest, L2WithMissingInstIdIsDropped) {
  const std::string msg = "{\"arg\":{\"channel\":\"books\"},\"action\":\"snapshot\",\"data\":[{"
      "\"asks\":[[\"100.1\",\"1\",\"0\",\"1\"]],\"bids\":[[\"100.0\",\"1\",\"0\",\"1\"]],\"ts\":\"1700000000000\"}]}";
  EXPECT_NO_THROW(blob_->handle_public_message(msg));
  EXPECT_FALSE(published()) << "L2 without instId must be dropped";
}

TEST_F(OkxBlobTest, L2MalformedLevelsAreSkipped) {
  const std::string msg = std::string("{\"arg\":{\"channel\":\"books\",\"instId\":\"") + instrument_
      + "\"},\"action\":\"snapshot\",\"data\":[{"
      + "\"asks\":[[\"100.1\",\"1\",\"0\",\"1\"]],"
      + "\"bids\":[[100.0,1],[\"99.9\",\"2\",\"0\",\"1\"]],"
      + "\"ts\":\"1700000000000\"}]}";
  EXPECT_NO_THROW(blob_->handle_public_message(msg));
  ASSERT_TRUE(published());
  ASSERT_EQ(last_seq(), 1u) << "Expected one bid message (valid level only) plus one ask message";
  const L2UpdateEvent& bids = event_at<L2UpdateEvent>(0);
  EXPECT_EQ(bids.num_levels_, 1);
  EXPECT_EQ(bids.price_1_, CodecUtils::encode_price("99.9"));
  drain();
}

TEST_F(OkxBlobTest, L2WithNoDataIsDropped) {
  const std::string msg = std::string("{\"arg\":{\"channel\":\"books\",\"instId\":\"") + instrument_
      + "\"},\"action\":\"update\"}";
  EXPECT_NO_THROW(blob_->handle_public_message(msg));
  EXPECT_FALSE(published());
}


TEST_F(OkxBlobTest, L2InSequenceUpdatesArePublished) {
  blob_->handle_public_message(make_l2_seq_message("snapshot", -1, 10));
  ASSERT_TRUE(published());
  const auto after_snapshot = last_seq();

  blob_->handle_public_message(make_l2_seq_message("update", 10, 11));
  EXPECT_GT(last_seq(), after_snapshot) << "in-sequence update must be published";
  drain();
}

TEST_F(OkxBlobTest, L2SequenceGapDropsUpdate) {
  blob_->handle_public_message(make_l2_seq_message("snapshot", -1, 10));
  ASSERT_TRUE(published());
  const auto after_snapshot = last_seq();

  blob_->handle_public_message(make_l2_seq_message("update", 15, 16));
  EXPECT_EQ(last_seq(), after_snapshot) << "gapped update must not be published";

  EXPECT_EQ(blob_->subscription_queue_->size(), 2u);
  drain();
}

TEST_F(OkxBlobTest, L2SnapshotResetsSequenceTracking) {
  blob_->handle_public_message(make_l2_seq_message("snapshot", -1, 10));
  blob_->handle_public_message(make_l2_seq_message("update", 10, 11));
  blob_->handle_public_message(make_l2_seq_message("snapshot", -1, 50));
  const auto after_snapshot = last_seq();
  blob_->handle_public_message(make_l2_seq_message("update", 50, 51));
  EXPECT_GT(last_seq(), after_snapshot) << "update chained off the new snapshot must be published";
  EXPECT_EQ(blob_->subscription_queue_->size(), 0u) << "no gap recovery expected";
  drain();
}

}
