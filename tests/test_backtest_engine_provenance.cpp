
#include "backtest/backtest_engine.hpp"

#include <gtest/gtest.h>
#include <stdexcept>

namespace {

reflex::MessageSlot make_l1(reflex::Exchange exchange, int32_t instrument_id) {
  reflex::MessageSlot slot;
  auto* event = new (slot.raw_data()) reflex::L1UpdateEvent();
  event->exchange_ = exchange;
  event->instrument_id_ = instrument_id;
  event->timestamp_ns_ = 1;
  event->bid_price_ = 100;
  event->offer_price_ = 101;
  return slot;
}

TEST(BackTestEngineProvenanceTest, AcceptsOnlyConfiguredExecutionVenue) {
  reflex::backtest::BackTestEngineConfig config;
  config.trading_exchange_ = reflex::Exchange::Okx;
  config.trading_instrument_id_ = 10301;
  config.fail_on_provenance_mismatch_ = true;
  reflex::backtest::BackTestEngine engine(config);

  auto okx = make_l1(reflex::Exchange::Okx, 10301);
  EXPECT_NO_THROW(engine.process_file_data_event(&okx));
  EXPECT_EQ(engine.get_market_data_provenance_stats().accepted_execution_events, 1u);
  EXPECT_EQ(engine.get_market_data_provenance_stats().seen(reflex::Exchange::Okx), 1u);

  auto other = make_l1(reflex::Exchange::BinanceDerivatives, 10201);
  EXPECT_THROW(engine.process_file_data_event(&other), std::runtime_error);
  EXPECT_EQ(engine.get_market_data_provenance_stats().rejected_exchange_events, 1u);
  EXPECT_EQ(engine.get_market_data_provenance_stats().rejected_instrument_events, 1u);
}

TEST(BackTestEngineProvenanceTest, ExactReferenceDoesNotRelaxOtherMismatch) {
  reflex::backtest::BackTestEngineConfig config;
  config.trading_exchange_ = reflex::Exchange::Okx;
  config.trading_instrument_id_ = 10301;
  config.reference_exchange_ = reflex::Exchange::BinanceDerivatives;
  config.reference_instrument_id_ = 10201;
  config.deliver_reference_market_data_to_strategy_ = true;
  config.fail_on_provenance_mismatch_ = true;
  reflex::backtest::BackTestEngine engine(config);

  auto reference = make_l1(reflex::Exchange::BinanceDerivatives, 10201);
  EXPECT_NO_THROW(engine.process_file_data_event(&reference));
  EXPECT_EQ(engine.get_market_data_provenance_stats().delivered_reference_events, 1u);
  EXPECT_EQ(engine.get_market_data_provenance_stats().rejected_exchange_events, 0u);

  auto wrong_reference = make_l1(reflex::Exchange::BinanceDerivatives, 10202);
  EXPECT_THROW(engine.process_file_data_event(&wrong_reference), std::runtime_error);
  EXPECT_EQ(engine.get_market_data_provenance_stats().rejected_exchange_events, 1u);
  EXPECT_EQ(engine.get_market_data_provenance_stats().rejected_instrument_events, 1u);
}

TEST(BackTestEngineProvenanceTest, NullSlotIsDroppedNotDereferenced) {
  reflex::backtest::BackTestEngine engine(reflex::backtest::BackTestEngineConfig{});
  EXPECT_NO_THROW(engine.process_file_data_event(nullptr));
  EXPECT_EQ(engine.get_market_data_provenance_stats().accepted_execution_events, 0u);
}

TEST(BackTestEngineProvenanceTest, SeparatesActionAndResponseLatencies) {
  reflex::backtest::BackTestEngineConfig config;
  config.strategy_to_exchange_latency_ns_ = 9;
  config.exchange_to_strategy_latency_ns_ = 8;
  config.order_entry_latency_ns_ = 3;
  config.cancel_latency_ns_ = 2;
  config.replace_latency_ns_ = 4;
  config.response_latency_ns_ = 5;
  reflex::backtest::BackTestEngine engine(config);

  EXPECT_EQ(engine.configured_outbound_latency_ns(reflex::MessageType::Pending), 3);
  EXPECT_EQ(engine.configured_outbound_latency_ns(reflex::MessageType::PendingCancel), 2);
  EXPECT_EQ(engine.configured_outbound_latency_ns(reflex::MessageType::PendingReplace), 4);
  EXPECT_EQ(engine.configured_outbound_latency_ns(reflex::MessageType::Heartbeat), 9);
  EXPECT_EQ(engine.configured_response_latency_ns(), 5);
}

TEST(BackTestEngineProvenanceTest, NegativeOverridesUseLegLatency) {
  reflex::backtest::BackTestEngineConfig config;
  config.strategy_to_exchange_latency_ns_ = 7;
  config.exchange_to_strategy_latency_ns_ = 6;
  reflex::backtest::BackTestEngine engine(config);

  EXPECT_EQ(engine.configured_outbound_latency_ns(reflex::MessageType::Pending), 7);
  EXPECT_EQ(engine.configured_outbound_latency_ns(reflex::MessageType::PendingCancel), 7);
  EXPECT_EQ(engine.configured_response_latency_ns(), 6);
}

}
