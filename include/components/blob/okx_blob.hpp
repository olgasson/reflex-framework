#pragma once

#include <openssl/buffer.h>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <poll.h>
#include "../../websocket/websocket_client.hpp"
#include "disruptorplus/ring_buffer.hpp"
#include "disruptorplus/single_threaded_claim_strategy.hpp"
#include "disruptorplus/spin_wait_strategy.hpp"
#include "framework/base_component.hpp"
#include "messages.hpp"
#include "websocket/subscription_queue.hpp"

struct yyjson_val;

namespace reflex {

class OkxBlob : public BaseComponent {
 public:
  explicit OkxBlob(
      const BlobConfig& config,
      std::shared_ptr<disruptorplus::ring_buffer<MessageSlot>> log_buffer,
      std::shared_ptr<disruptorplus::single_threaded_claim_strategy<disruptorplus::spin_wait_strategy>> claim_strategy);
  ~OkxBlob() override;

  void on_start() override;
  int on_do_work() override;
  int drive_connection_state(std::unique_ptr<WebsocketClient>& client, const std::string& channel_name);

 protected:
  void on_connecting() override;

  void publish_trade_data(int32_t instrument_id, int64_t price_enc, int64_t size_enc, std::string_view side_str,
                          int64_t exch_ts_ns);

  void publish_l1_update(int32_t instrument_id, int64_t bid_px_enc, int64_t bid_sz_enc, int64_t ask_px_enc,
                         int64_t ask_sz_enc, int64_t exch_ts_ns);

  void publish_l2_book(int32_t instrument_id, const std::vector<std::pair<int64_t, int64_t>>& bids,
                       const std::vector<std::pair<int64_t, int64_t>>& asks, bool is_snapshot);

  void publish_mark_price(int32_t instrument_id, int64_t mark_price_enc, int64_t index_price_enc,
                          int64_t exch_ts_ns);

  void publish_funding_rate(int32_t instrument_id, int64_t funding_rate_enc, int64_t next_funding_rate_enc,
                            int64_t interest_rate_enc, int64_t funding_time_ns, int64_t next_funding_time_ns);

  void publish_open_interest(int32_t instrument_id, int64_t open_interest_enc, int64_t open_interest_ccy_enc,
                             int64_t open_interest_usd_enc, int64_t exch_ts_ns);

 private:
  bool requires_public_login() const noexcept;
  std::string book_channel_{"books"};
  bool public_login_required_{false};

  void disconnect_websockets();
  void setup_public_client_callbacks(std::unique_ptr<WebsocketClient>& client);

  void load_api_credentials(const std::string& api_file_path);
  std::string generate_login_message();
  void setup_business_client_callbacks(std::unique_ptr<WebsocketClient>& client);
  std::string generate_hmac_signature(const std::string& timestamp, const std::string& method,
                                      const std::string& request_path, const std::string& request_body);
  int64_t generate_unix_timestamp();

  void on_login_successful();
  void subscribe_public_channels();
  void subscribe_business_channels();
  void resubscribe_book_channel(const std::string& symbol);

  void handle_public_message(std::string_view message);
  void handle_business_message(std::string_view message);
  void process_subscription_response(yyjson_val* root);
  void process_l2_update(yyjson_val* root);
  void process_l1_update(yyjson_val* root);
  void process_trade_event(yyjson_val* root);
  void process_mark_price_update(yyjson_val* root);
  void process_funding_rate_update(yyjson_val* root);
  void process_open_interest_update(yyjson_val* root);

  int32_t resolve_instrument(const char* inst_id) noexcept;

  void warn_malformed(const char* channel, const char* what);

  std::string get_book_subscription(const std::string& symbol);
  std::string get_book_unsubscription(const std::string& symbol);
  static std::string get_ticker_subscription(const std::string& symbol);
  static std::string get_trade_subscription(const std::string& symbol);
  static std::string get_mark_price_subscription(const std::string& symbol);
  static std::string get_funding_rate_subscription(const std::string& symbol);
  static std::string get_open_interest_subscription(const std::string& symbol);

  static constexpr const char* PUBLIC_WS_URL = "wss://ws.okx.com:8443/ws/v5/public";
  static constexpr const char* BUSINESS_WS_URL = "wss://ws.okx.com:8443/ws/v5/business";

  BlobConfig blob_config_;

  std::unique_ptr<WebsocketClient> public_client_;
  std::unique_ptr<WebsocketClient> business_client_;

  std::string api_key_;
  std::string secret_key_;
  std::string passphrase_;

  std::vector<int64_t> highest_seen_seq_num_;

  int64_t last_malformed_warn_ns_{0};
  static constexpr int64_t kMalformedWarnIntervalNanos = 1'000'000'000;

  std::vector<std::pair<int64_t, int64_t>> l2_bids_scratch_;
  std::vector<std::pair<int64_t, int64_t>> l2_asks_scratch_;
  std::vector<struct pollfd> poll_fds_scratch_;

  struct InstrumentCacheEntry {
    std::string symbol;
    int32_t instrument_id;
  };
  std::vector<InstrumentCacheEntry> instrument_id_cache_;

  int frames_this_pass_ = 0;

  std::vector<std::string> currency_pairs_;
  std::unique_ptr<SubscriptionQueue> subscription_queue_;

  std::shared_ptr<disruptorplus::ring_buffer<MessageSlot>> log_buffer_;
  std::shared_ptr<disruptorplus::single_threaded_claim_strategy<disruptorplus::spin_wait_strategy>> claim_strategy_;
};

}
