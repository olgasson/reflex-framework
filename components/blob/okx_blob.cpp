#include "../../include/components/blob/okx_blob.hpp"

#include <fstream>
#include <sstream>
#include <thread>

#include "../../include/utils/codec_utils.hpp"
#include "asset_info_manager.hpp"
#include "yyjson.h"
#include <cstdint>
#include <utility>
#include <openssl/evp.h>
#include <openssl/hmac.h>

#include <libwebsockets.h>



// route enum
enum class Route : int8_t { LOGIN, SUB, BOOK_L2, BBO, MARK_PRICE, FUNDING_RATE, OPEN_INTEREST, TRADES, UNKNOWN };


static Route route_from_root(yyjson_val* root) {
  if (auto* ev = yyjson_obj_get(root, "event"); ev) {
    const char* event_str = yyjson_get_str(ev);
    if (event_str) {
      // Direct string comparison instead of hashing - simpler and safer
      if (strcmp(event_str, "login") == 0) return Route::LOGIN;
      if (strcmp(event_str, "subscribe") == 0) return Route::SUB;
    }
  }
  if (auto* arg = yyjson_obj_get(root, "arg")) {
    auto* ch = yyjson_obj_get(arg, "channel");
    if (ch) {
      const char* channel_str = yyjson_get_str(ch);
      if (channel_str) {
        if (strcmp(channel_str, "books-l2-tbt") == 0 || strcmp(channel_str, "books") == 0) return Route::BOOK_L2;
        if (strcmp(channel_str, "bbo-tbt") == 0) return Route::BBO;
        if (strcmp(channel_str, "mark-price") == 0) return Route::MARK_PRICE;
        if (strcmp(channel_str, "funding-rate") == 0) return Route::FUNDING_RATE;
        if (strcmp(channel_str, "open-interest") == 0) return Route::OPEN_INTEREST;
        if (strcmp(channel_str, "trades-all") == 0) return Route::TRADES;
      }
    }
  }
  return Route::UNKNOWN;
}

namespace reflex {

bool OkxBlob::requires_public_login() const noexcept {
  return public_login_required_;
}

OkxBlob::OkxBlob(
    const BlobConfig& config, std::shared_ptr<disruptorplus::ring_buffer<MessageSlot>> log_buffer,
    std::shared_ptr<disruptorplus::single_threaded_claim_strategy<disruptorplus::spin_wait_strategy>> claim_strategy)
    : BaseComponent(config),
      blob_config_(config),
      highest_seen_seq_num_(100000, 0),
      currency_pairs_(config.currency_pairs_),
      subscription_queue_(std::make_unique<SubscriptionQueue>(BaseComponent::get_timer_manager(), component_name_)),
      log_buffer_(std::move(log_buffer)),
      claim_strategy_(std::move(claim_strategy)) {
  public_login_required_ = false;
  // Constructor stays mostly the same, but don't start connections here
}

OkxBlob::~OkxBlob() { disconnect_websockets(); }

void OkxBlob::on_start() { logger_->info("OkxBlob starting up"); }

// Implement the state callbacks
void OkxBlob::on_connecting() {
  logger_->info("OkxBlob entering CONNECTING state - loading credentials and initiating connection");

  try {
    // load API credentials first
    load_api_credentials(blob_config_.api_file_path_);

  } catch (const std::exception& e) {
    logger_->error("Failed to connect OkxBlob: {}", e.what());
    component_state_ = ComponentState::FAILED;
  }
}

int OkxBlob::on_do_work() {
  int work_count = 0;

  // Full websocket servicing.
  // Reuse a member buffer instead of allocating a vector every work-loop pass.
  std::vector<struct pollfd>& all_fds = poll_fds_scratch_;
  all_fds.clear();

  if (public_client_) {
    public_client_->get_poll_fds(all_fds);
  }
  if (business_client_) {
    business_client_->get_poll_fds(all_fds);
  }

  if (!all_fds.empty()) {
    poll(all_fds.data(), all_fds.size(), 0);
  }

  if (public_client_) {
    public_client_->service_ready_fds(all_fds);
    ++work_count;
  }
  if (business_client_) {
    business_client_->service_ready_fds(all_fds);
    ++work_count;
  }

  if (public_client_ && public_client_->needs_poll()) {
    lws_service(public_client_->get_context(), 0);
    ++work_count;
  }
  if (business_client_ && business_client_->needs_poll()) {
    lws_service(business_client_->get_context(), 0);
    ++work_count;
  }

  // Drive state machines (this is where your state logic goes)
  work_count += drive_connection_state(public_client_, "public");
  work_count += drive_connection_state(business_client_, "business");

  // Handle ping/pong and pending work
  int64_t now = get_nano_clock().epoch_nanos();
  if (public_client_) {
    public_client_->send_ping_if_needed(now);
    if (public_client_->is_pong_timed_out(now)) {
      logger_->warn("Public channel pong timed out - disconnecting");
      public_client_->disconnect(true, "public pong timeout");
      work_count++;
    }
    if (public_client_->is_connection_timed_out(now)) {
      logger_->warn("Public channel connection timeout - retrying");
      public_client_->disconnect(true, "public connection timeout"); // Hard reset for timeouts
      work_count++;
    }
    if (public_client_->has_pending_work()) {
      public_client_->do_pending_work();
      work_count++;
    }
  }
  if (business_client_) {
    business_client_->send_ping_if_needed(now);
    if (business_client_->is_pong_timed_out(now)) {
      logger_->warn("Business channel pong timed out - disconnecting");
      business_client_->disconnect(true, "business pong timeout");
      work_count++;
    }
    if (business_client_->is_connection_timed_out(now)) {
      logger_->warn("Business channel connection timeout - retrying");
      business_client_->disconnect(true, "business connection timeout"); // Hard reset for timeouts
      work_count++;
    }

    if (business_client_->has_pending_work()) {
      business_client_->do_pending_work();
      work_count++;
    }
  }

  return work_count;
}

int OkxBlob::drive_connection_state(std::unique_ptr<WebsocketClient>& client, const std::string& channel_name)

{
  ConnectionState state = client ? client->get_state() : ConnectionState::DISCONNECTED;
  int work_done = 0;

  switch (state) {
    case ConnectionState::DISCONNECTED:
      // Create the client if it doesn't exist
      if (!client) {
        if (channel_name == "public") {
          client = std::make_unique<WebsocketClient>(PUBLIC_WS_URL, "okx_public", get_nano_clock());
          setup_public_client_callbacks(client);
        } else if (channel_name == "business") {
          client = std::make_unique<WebsocketClient>(BUSINESS_WS_URL, "okx_business", get_nano_clock());
          setup_business_client_callbacks(client);
        }
        work_done++;
      }

      // Initiate connection (non-blocking)
      if (client) {
        client->connect();
        work_done++;

        // Log the connection attempt
        logger_->info("Initiating connection to {} channel", channel_name);
      }

      break;

    case ConnectionState::CONNECTING:
      // logger_->info("WebSocket {} channel connecting", channel_name);
      break;

    case ConnectionState::CONNECTED: {
      if (channel_name == "business") {
        // Public channel can subscribe immediately
        subscribe_business_channels();
        client->set_state(ConnectionState::WORKING);
        work_done++;
        logger_->info("Subscribed to business channels");
        // Public channel stays in CONNECTED state

      } else if (channel_name == "public") {
        if (requires_public_login()) {
          std::string login_msg = generate_login_message();
          if (client->send_message_immediate(login_msg)) {
            client->set_state(ConnectionState::AUTHENTICATING);
            logger_->info("Sent authentication message to public channel");
            work_done++;
          } else {
            logger_->error("Failed to send authentication message on public channel");
          }
        } else {
          subscribe_public_channels();
          client->set_state(ConnectionState::WORKING);
          work_done++;
          logger_->info("Subscribed to public channels");
        }
      }

      break;
    }

    case ConnectionState::AUTHENTICATING: {
      // Waiting for auth response - message handler will transition to AUTHENTICATED
      // logger_->info("Waiting for Public channel authentication response");
      break;
    }

    case ConnectionState::AUTHENTICATED: {
      if (requires_public_login()) {
        logger_->info("Public channel authenticated - subscribing");
        subscribe_public_channels();
        client->set_state(ConnectionState::WORKING);
        work_done++;
      }
      break;
    }

    case ConnectionState::WORKING: {
      // Final state - just process messages
      break;
    }

    case ConnectionState::RECONNECTING:
      // Handle reconnection logic
      break;

    case ConnectionState::ERROR:
      logger_->error("WebSocket {} channel error calling disconnect", channel_name);
      client->disconnect(true, channel_name + " channel entered ERROR state");
      break;

    default:
      logger_->error("Unknown WebSocket {} channel state: {}", channel_name, to_string(state));
      break;
  }

  // Ping/pong, timeout and pending-work handling live in on_do_work() only;
  // duplicating them here made timeout disconnects fire twice per loop.

  return work_done;
}

void OkxBlob::load_api_credentials(const std::string& api_file_path) {
  std::ifstream file(api_file_path);
  if (!file.is_open()) {
    logger_->error("Cannot proceed without valid credentials {}", api_file_path);
    throw std::runtime_error("Cannot open API credentials file: " + api_file_path);
  }

  std::string line;
  while (std::getline(file, line)) {
    // Skip empty lines and comments
    if (line.empty() || line[0] == '#') continue;

    // Handle both formats: "key=value" and "key : value" or "key = value"
    size_t delimiter = line.find('=');
    if (delimiter == std::string::npos) {
      delimiter = line.find(':');
    }

    if (delimiter != std::string::npos) {
      std::string key = line.substr(0, delimiter);
      std::string value = line.substr(delimiter + 1);

      // Trim whitespace, quotes, and carriage returns
      key.erase(0, key.find_first_not_of(" \t"));
      key.erase(key.find_last_not_of(" \t") + 1);

      value.erase(0, value.find_first_not_of(" \t\""));
      value.erase(value.find_last_not_of(" \t\"\r\n") + 1);

      if (key == "key") {
        api_key_ = value;
      } else if (key == "secret") {
        secret_key_ = value;
      } else if (key == "passphrase") {
        passphrase_ = value;
      }
    }
  }

  if (api_key_.empty() || secret_key_.empty() || passphrase_.empty()) {
    logger_->error("Missing API credentials - found key: '{}', secret: '{}', passphrase: '{}'",
                   api_key_.empty() ? "MISSING" : "FOUND", secret_key_.empty() ? "MISSING" : "FOUND",
                   passphrase_.empty() ? "MISSING" : "FOUND");
    throw std::runtime_error("Missing required API credentials in file: " + api_file_path);
  }

  logger_->info("Loaded OKX API credentials successfully");
}

void OkxBlob::disconnect_websockets() {
  if (public_client_) {
    public_client_->disconnect(false, "component shutdown");
    public_client_.reset();
  }

  if (business_client_) {
    business_client_->disconnect(false, "component shutdown");
    business_client_.reset();
  }
}

// Add these helper methods to avoid code duplication
void OkxBlob::setup_public_client_callbacks(std::unique_ptr<WebsocketClient>& client) {
  client->set_message_callback([this](std::string_view message) { handle_public_message(message); });
}

void OkxBlob::setup_business_client_callbacks(std::unique_ptr<WebsocketClient>& client) {
  client->set_message_callback([this](std::string_view message) { handle_business_message(message); });
}

std::string OkxBlob::generate_hmac_signature(const std::string& timestamp, const std::string& method,
                                             const std::string& request_path, const std::string& request_body) {
  std::string prehash = timestamp + method + request_path + request_body;

  unsigned char digest[EVP_MAX_MD_SIZE];
  unsigned int digest_len;

  HMAC(EVP_sha256(), secret_key_.c_str(), secret_key_.length(), reinterpret_cast<const unsigned char*>(prehash.c_str()),
       prehash.length(), digest, &digest_len);

  // Base64 encode
  BIO* bio = BIO_new(BIO_s_mem());
  BIO* b64 = BIO_new(BIO_f_base64());
  BIO_set_flags(b64, BIO_FLAGS_BASE64_NO_NL);
  bio = BIO_push(b64, bio);

  BIO_write(bio, digest, digest_len);
  BIO_flush(bio);

  BUF_MEM* buf_mem;
  BIO_get_mem_ptr(bio, &buf_mem);
  std::string signature(buf_mem->data, buf_mem->length);

  BIO_free_all(bio);
  return signature;
}

int64_t OkxBlob::generate_unix_timestamp() {
  auto now = std::chrono::system_clock::now();
  auto seconds = std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch());
  return static_cast<int64_t>(seconds.count());
}

std::string OkxBlob::generate_login_message() {
  std::string timestamp = std::to_string(generate_unix_timestamp());
  std::string signature = generate_hmac_signature(timestamp, "GET", "/users/self/verify", "");


  std::ostringstream oss;
  oss << R"({"op": "login", "args": [{"apiKey": ")" << api_key_ << R"(", "passphrase": ")" << passphrase_
      << R"(", "timestamp": ")" << timestamp << R"(", "sign": ")" << signature << R"("}]})";

  return oss.str();
}

void OkxBlob::on_login_successful() {
  logger_->info("OKX authentication successful");
  if (public_client_) {
    public_client_->set_state(ConnectionState::AUTHENTICATED);
  }
}

void OkxBlob::subscribe_public_channels() {
  if (requires_public_login() && public_client_->get_state() != ConnectionState::AUTHENTICATED) {
    return;
  }

  logger_->info("Subscribing to public channels via queue (book channel: {})", book_channel_);

  for (const auto& symbol : currency_pairs_) {
    const bool is_derivative =
        (symbol.find("-SWAP") != std::string::npos) || (symbol.find("-FUT") != std::string::npos);

    // Queue L2 book subscription
    subscription_queue_->enqueue([this, symbol]() {
      std::string book_sub = get_book_subscription(symbol);
      public_client_->send_message_immediate(book_sub);
      logger_->info("Subscribed to {} for {}", book_channel_, symbol);
    });

    // Queue L1 ticker subscription
    subscription_queue_->enqueue([this, symbol]() {
      std::string ticker_sub = get_ticker_subscription(symbol);
      public_client_->send_message_immediate(ticker_sub);
      logger_->info("Subscribed to L1 ticker for {}", symbol);
    });

    // Queue mark price subscription
    subscription_queue_->enqueue([this, symbol]() {
      std::string mark_price_sub = get_mark_price_subscription(symbol);
      public_client_->send_message_immediate(mark_price_sub);
      logger_->info("Subscribed to mark price for {}", symbol);
    });

    // Queue funding rate subscription (derivatives only)
    if (is_derivative) {
      subscription_queue_->enqueue([this, symbol]() {
        std::string funding_sub = get_funding_rate_subscription(symbol);
        public_client_->send_message_immediate(funding_sub);
        logger_->info("Subscribed to funding rate for {}", symbol);
      });
    }

    // Queue open interest subscription (derivatives only)
    if (is_derivative) {
      subscription_queue_->enqueue([this, symbol]() {
        std::string oi_sub = get_open_interest_subscription(symbol);
        public_client_->send_message_immediate(oi_sub);
        logger_->info("Subscribed to open interest for {}", symbol);
      });
    }
  }
}

void OkxBlob::subscribe_business_channels() {
  logger_->info("Subscribing to business channels via queue");

  for (const auto& symbol : currency_pairs_) {
    // Queue trade subscription
    subscription_queue_->enqueue([this, symbol]() {
      std::string trade_sub = get_trade_subscription(symbol);
      business_client_->send_message_immediate(trade_sub);
      logger_->info("Subscribed to trades for {}", symbol);
    });
  }
}

std::string OkxBlob::get_book_subscription(const std::string& symbol) {
  return std::string("{\"op\": \"subscribe\", \"args\": [{\"channel\": \"") + book_channel_ +
         "\", \"instId\": \"" + symbol + "\"}]}";
}

std::string OkxBlob::get_book_unsubscription(const std::string& symbol) {
  return std::string("{\"op\": \"unsubscribe\", \"args\": [{\"channel\": \"") + book_channel_ +
         "\", \"instId\": \"" + symbol + "\"}]}";
}

void OkxBlob::resubscribe_book_channel(const std::string& symbol) {
  // Gap recovery: unsubscribe + subscribe through the paced subscription queue
  // so the venue's per-connection rate limits are respected. The fresh
  // subscription re-seeds the book with a snapshot.
  subscription_queue_->enqueue([this, symbol]() {
    if (public_client_ && public_client_->is_connected()) {
      public_client_->send_message_immediate(get_book_unsubscription(symbol));
      logger_->info("Unsubscribed {} from {} (gap recovery)", symbol, book_channel_);
    }
  });
  subscription_queue_->enqueue([this, symbol]() {
    if (public_client_ && public_client_->is_connected()) {
      public_client_->send_message_immediate(get_book_subscription(symbol));
      logger_->info("Re-subscribed {} to {} (gap recovery)", symbol, book_channel_);
    }
  });
}

std::string OkxBlob::get_ticker_subscription(const std::string& symbol) {
  return R"({"op": "subscribe", "args": [{"channel": "bbo-tbt", "instId": ")" + symbol + R"("}]})";
}

std::string OkxBlob::get_trade_subscription(const std::string& symbol) {
  return R"({"op": "subscribe", "args": [{"channel": "trades-all", "instId": ")" + symbol + R"("}]})";
}

std::string OkxBlob::get_mark_price_subscription(const std::string& symbol) {
  return R"({"op": "subscribe", "args": [{"channel": "mark-price", "instId": ")" + symbol + R"("}]})";
}

std::string OkxBlob::get_funding_rate_subscription(const std::string& symbol) {
  return R"({"op": "subscribe", "args": [{"channel": "funding-rate", "instId": ")" + symbol + R"("}]})";
}

std::string OkxBlob::get_open_interest_subscription(const std::string& symbol) {
  return R"({"op": "subscribe", "args": [{"channel": "open-interest", "instId": ")" + symbol + R"("}]})";
}

void OkxBlob::handle_public_message(std::string_view msg) {
  if (msg == "pong") {
    if (public_client_) {
      public_client_->update_pong_timestamp();
    }
    return;
  }
  // Parse exactly once; route_from_root and every process_* below operate on
  // this same document root.
  yyjson_doc* doc = yyjson_read(msg.data(), msg.size(), 0);
  if (!doc) {
    warn_malformed("public", "JSON parse failure");
    return;
  }
  yyjson_val* root = yyjson_doc_get_root(doc);

  switch (route_from_root(root)) {
    case Route::LOGIN:
      on_login_successful();
      break;
    case Route::SUB:
      process_subscription_response(root);
      break;
    case Route::BOOK_L2:
      process_l2_update(root);
      break;
    case Route::BBO:
      process_l1_update(root);
      break;
    case Route::MARK_PRICE:
      process_mark_price_update(root);
      break;
    case Route::FUNDING_RATE:
      process_funding_rate_update(root);
      break;
    case Route::OPEN_INTEREST:
      process_open_interest_update(root);
      break;
    default:
      logger_->warn("Public message not routed: {}", msg);
      break;
  }
  yyjson_doc_free(doc);
}

void OkxBlob::handle_business_message(std::string_view msg) {
  if (msg == "pong") {
    if (business_client_) {
      business_client_->update_pong_timestamp();
    }
    return;
  }
  yyjson_doc* doc = yyjson_read(msg.data(), msg.size(), 0);
  if (doc == nullptr) {
    warn_malformed("business", "JSON parse failure");
    return;
  }
  yyjson_val* root = yyjson_doc_get_root(doc);

  if (route_from_root(root) == Route::TRADES) {
    process_trade_event(root);
  }

  yyjson_doc_free(doc);
}

void OkxBlob::warn_malformed(const char* channel, const char* what) {
  // Rate-limited: a bad upstream burst must not turn into a log storm on the
  // market-data hot path.
  const int64_t now = get_nano_clock().epoch_nanos();
  if (now - last_malformed_warn_ns_ < kMalformedWarnIntervalNanos) {
    return;
  }
  last_malformed_warn_ns_ = now;
  logger_->warn("Dropping malformed {} frame ({})", channel, what);
}

int32_t OkxBlob::resolve_instrument(const char* inst_id) noexcept {
  if (inst_id == nullptr) {
    return -1;
  }
  for (const auto& entry : instrument_id_cache_) {
    if (entry.symbol == inst_id) {  // length + memcmp, short-circuits fast
      return entry.instrument_id;
    }
  }
  const AssetInfo* info = AssetInfoManager::get_asset_info(inst_id, Exchange::Okx);
  if (info == nullptr) {
    return -1;
  }
  instrument_id_cache_.push_back({std::string(inst_id), info->instrument_id_});
  return info->instrument_id_;
}

void OkxBlob::process_l2_update(yyjson_val* root) {
  yyjson_val* data_arr = yyjson_obj_get(root, "data");
  if (!yyjson_is_arr(data_arr) || yyjson_arr_size(data_arr) == 0) {
    return;
  }

  yyjson_val* data = yyjson_arr_get_first(data_arr);
  yyjson_val* arg = yyjson_obj_get(root, "arg");

  const char* inst_id = yyjson_get_str(yyjson_obj_get(arg, "instId"));
  if (inst_id == nullptr) {
    warn_malformed(book_channel_.c_str(), "missing instId");
    return;
  }

  const int32_t instrument_id = resolve_instrument(inst_id);
  if (instrument_id < 0) {
    return;
  }

  // Snapshot detection from the parsed "action" field ("snapshot"/"update")
  // instead of scanning the entire raw payload for the substring "snapshot".
  const char* action = yyjson_get_str(yyjson_obj_get(root, "action"));
  const bool is_snapshot = (action != nullptr) && std::strcmp(action, "snapshot") == 0;

  // ---- L2 sequence-gap detection -----------------------------------------
  // Every OKX books frame carries seqId/prevSeqId; prevSeqId must equal the
  // seqId of the previous frame for this instrument. On a mismatch the local
  // book is no longer trustworthy: drop the frame and re-subscribe so the
  // book re-seeds from a fresh snapshot. A snapshot resets the tracked seq.
  const int64_t seq_id = yyjson_get_sint(yyjson_obj_get(data, "seqId"));
  if (static_cast<size_t>(instrument_id) < highest_seen_seq_num_.size()) {
    int64_t& tracked_seq = highest_seen_seq_num_[static_cast<size_t>(instrument_id)];
    if (is_snapshot) {
      tracked_seq = seq_id;
    } else {
      yyjson_val* prev_seq_val = yyjson_obj_get(data, "prevSeqId");
      const int64_t prev_seq_id = yyjson_get_sint(prev_seq_val);
      if (prev_seq_val != nullptr && tracked_seq != 0 && prev_seq_id != tracked_seq) {
        logger_->error("L2 sequence gap on {}: prevSeqId={} but last seen seqId={} - resubscribing {}", inst_id,
                       prev_seq_id, tracked_seq, book_channel_);
        tracked_seq = 0;  // nothing is in-sequence until the fresh snapshot re-seeds
        resubscribe_book_channel(inst_id);
        return;  // drop the gapped update
      }
      tracked_seq = seq_id;
    }
  }

  // ---- collect bids / asks as already-encoded mantissas into reused buffers
  auto fill_side = [this](yyjson_val* arr, std::vector<std::pair<int64_t, int64_t>>& out) {
    out.clear();
    if (!yyjson_is_arr(arr)) {
      return;
    }
    out.reserve(yyjson_arr_size(arr));
    yyjson_arr_iter it;
    yyjson_arr_iter_init(arr, &it);
    yyjson_val* lvl;
    while ((lvl = yyjson_arr_iter_next(&it))) {
      if (!yyjson_is_arr(lvl) || yyjson_arr_size(lvl) < 2) {
        continue;
      }
      const char* px = yyjson_get_str(yyjson_arr_get(lvl, 0));
      const char* sz = yyjson_get_str(yyjson_arr_get(lvl, 1));
      if (px == nullptr || sz == nullptr) {
        warn_malformed(book_channel_.c_str(), "non-string price/size level");
        continue;  // skip the malformed level
      }
      out.emplace_back(CodecUtils::encode_price(px), CodecUtils::encode_price(sz));
    }
  };

  fill_side(yyjson_obj_get(data, "bids"), l2_bids_scratch_);
  fill_side(yyjson_obj_get(data, "asks"), l2_asks_scratch_);

  publish_l2_book(instrument_id, l2_bids_scratch_, l2_asks_scratch_, is_snapshot);
}

void OkxBlob::process_l1_update(yyjson_val* root) {
  yyjson_val* data_array = yyjson_obj_get(root, "data");
  if (!yyjson_is_arr(data_array) || yyjson_arr_size(data_array) == 0) {
    return;
  }

  yyjson_val* data = yyjson_arr_get_first(data_array);
  yyjson_val* arg = yyjson_obj_get(root, "arg");

  const char* inst_id = yyjson_get_str(yyjson_obj_get(arg, "instId"));
  const char* ts_str = yyjson_get_str(yyjson_obj_get(data, "ts"));
  if (inst_id == nullptr || ts_str == nullptr) {
    warn_malformed("bbo-tbt", "missing instId/ts");
    return;
  }
  // OKX sends milliseconds; the recorded stream is normalized to nanoseconds.
  const int64_t exch_ts_ns = static_cast<int64_t>(std::strtoull(ts_str, nullptr, 10)) * 1000000LL;

  const int32_t instrument_id = resolve_instrument(inst_id);
  if (instrument_id < 0) {
    return;
  }

  // Extract bid/ask - yyjson's fast array access
  int64_t bid_px_enc = 0;
  int64_t bid_sz_enc = 0;
  int64_t ask_px_enc = 0;
  int64_t ask_sz_enc = 0;

  yyjson_val* bids_array = yyjson_obj_get(data, "bids");
  if (yyjson_is_arr(bids_array) && yyjson_arr_size(bids_array) > 0) {
    yyjson_val* bid = yyjson_arr_get_first(bids_array);
    if (yyjson_is_arr(bid) && yyjson_arr_size(bid) >= 2) {
      const char* bid_px_s = yyjson_get_str(yyjson_arr_get(bid, 0));
      const char* bid_sz_s = yyjson_get_str(yyjson_arr_get(bid, 1));
      if (bid_px_s == nullptr || bid_sz_s == nullptr) {
        warn_malformed("bbo-tbt", "non-string bid price/size");
        return;
      }
      bid_px_enc = CodecUtils::encode_price(bid_px_s);
      bid_sz_enc = CodecUtils::encode_price(bid_sz_s);
    }
  }

  yyjson_val* asks_array = yyjson_obj_get(data, "asks");
  if (yyjson_is_arr(asks_array) && yyjson_arr_size(asks_array) > 0) {
    yyjson_val* ask = yyjson_arr_get_first(asks_array);
    if (yyjson_is_arr(ask) && yyjson_arr_size(ask) >= 2) {
      const char* ask_px_s = yyjson_get_str(yyjson_arr_get(ask, 0));
      const char* ask_sz_s = yyjson_get_str(yyjson_arr_get(ask, 1));
      if (ask_px_s == nullptr || ask_sz_s == nullptr) {
        warn_malformed("bbo-tbt", "non-string ask price/size");
        return;
      }
      ask_px_enc = CodecUtils::encode_price(ask_px_s);
      ask_sz_enc = CodecUtils::encode_price(ask_sz_s);
    }
  }

  publish_l1_update(instrument_id, bid_px_enc, bid_sz_enc, ask_px_enc, ask_sz_enc, exch_ts_ns);
}

void OkxBlob::process_trade_event(yyjson_val* root) {
  yyjson_val* data_array = yyjson_obj_get(root, "data");
  if (!yyjson_is_arr(data_array) || yyjson_arr_size(data_array) == 0) {
    return;
  }

  yyjson_val* data = yyjson_arr_get_first(data_array);

  const char* inst_id = yyjson_get_str(yyjson_obj_get(data, "instId"));

  const int32_t instrument_id = resolve_instrument(inst_id);
  if (instrument_id < 0) {
    return;
  }

  const char* px_str = yyjson_get_str(yyjson_obj_get(data, "px"));
  const char* sz_str = yyjson_get_str(yyjson_obj_get(data, "sz"));
  const char* ts_str = yyjson_get_str(yyjson_obj_get(data, "ts"));
  if (px_str == nullptr || sz_str == nullptr || ts_str == nullptr) {
    warn_malformed("trades-all", "missing px/sz/ts");
    return;
  }

  double price = std::strtod(px_str, nullptr);
  double size = std::strtod(sz_str, nullptr);
  // OKX sends milliseconds; the recorded stream is normalized to nanoseconds.
  const int64_t exch_ts_ns = static_cast<int64_t>(std::strtoull(ts_str, nullptr, 10)) * 1000000LL;

  // side_raw points into the still-live yyjson document; publish_trade_data only
  // reads it (compares against "buy"), so a view is safe and avoids an alloc.
  const char* side_raw = yyjson_get_str(yyjson_obj_get(data, "side"));
  const std::string_view side = side_raw ? std::string_view(side_raw) : std::string_view{};

  publish_trade_data(instrument_id, CodecUtils::encode_price(price), CodecUtils::encode_quantity(size), side,
                     exch_ts_ns);
}

void OkxBlob::process_mark_price_update(yyjson_val* root) {
  yyjson_val* data_array = yyjson_obj_get(root, "data");
  if (!yyjson_is_arr(data_array) || yyjson_arr_size(data_array) == 0) {
    return;
  }

  yyjson_val* data = yyjson_arr_get_first(data_array);
  const char* inst_id = yyjson_get_str(yyjson_obj_get(data, "instId"));
  if (inst_id == nullptr) {
    return;
  }

  const char* mark_px_str = yyjson_get_str(yyjson_obj_get(data, "markPx"));
  const char* index_px_str = yyjson_get_str(yyjson_obj_get(data, "indexPx"));
  const char* ts_str = yyjson_get_str(yyjson_obj_get(data, "ts"));

  const int32_t instrument_id = resolve_instrument(inst_id);
  if (instrument_id < 0) {
    return;
  }
  int64_t mark_price_enc = mark_px_str ? CodecUtils::encode_price(mark_px_str) : 0;
  int64_t index_price_enc = index_px_str ? CodecUtils::encode_price(index_px_str) : mark_price_enc;
  int64_t exch_ts_ns = 0;
  if (ts_str && *ts_str) {
    exch_ts_ns = static_cast<int64_t>(std::strtoull(ts_str, nullptr, 10)) * 1000000LL;
  }

  publish_mark_price(instrument_id, mark_price_enc, index_price_enc, exch_ts_ns);
}

void OkxBlob::process_funding_rate_update(yyjson_val* root) {
  yyjson_val* data_array = yyjson_obj_get(root, "data");
  if (!yyjson_is_arr(data_array) || yyjson_arr_size(data_array) == 0) {
    return;
  }

  yyjson_val* data = yyjson_arr_get_first(data_array);
  const char* inst_id = yyjson_get_str(yyjson_obj_get(data, "instId"));
  if (inst_id == nullptr) {
    return;
  }

  const char* funding_rate_str = yyjson_get_str(yyjson_obj_get(data, "fundingRate"));
  const char* next_funding_rate_str = yyjson_get_str(yyjson_obj_get(data, "nextFundingRate"));
  const char* interest_rate_str = yyjson_get_str(yyjson_obj_get(data, "interestRate"));
  const char* funding_time_str = yyjson_get_str(yyjson_obj_get(data, "fundingTime"));
  const char* next_funding_time_str = yyjson_get_str(yyjson_obj_get(data, "nextFundingTime"));

  const int32_t instrument_id = resolve_instrument(inst_id);
  if (instrument_id < 0) {
    return;
  }
  int64_t funding_rate_enc = funding_rate_str ? CodecUtils::encode_price(funding_rate_str) : 0;
  int64_t next_funding_rate_enc = next_funding_rate_str ? CodecUtils::encode_price(next_funding_rate_str) : 0;
  int64_t interest_rate_enc = interest_rate_str ? CodecUtils::encode_price(interest_rate_str) : 0;

  auto to_ns = [](const char* s) -> int64_t {
    if (s == nullptr || *s == '\0') {
      return 0;
    }
    return static_cast<int64_t>(std::strtoull(s, nullptr, 10)) * 1000000LL;
  };

  int64_t funding_time_ns = to_ns(funding_time_str);
  int64_t next_funding_time_ns = to_ns(next_funding_time_str);

  publish_funding_rate(instrument_id, funding_rate_enc, next_funding_rate_enc, interest_rate_enc, funding_time_ns,
                       next_funding_time_ns);
}

void OkxBlob::process_open_interest_update(yyjson_val* root) {
  yyjson_val* data_array = yyjson_obj_get(root, "data");
  if (!yyjson_is_arr(data_array) || yyjson_arr_size(data_array) == 0) {
    return;
  }

  yyjson_val* data = yyjson_arr_get_first(data_array);
  const char* inst_id = yyjson_get_str(yyjson_obj_get(data, "instId"));
  if (inst_id == nullptr) {
    return;
  }

  const char* oi_str = yyjson_get_str(yyjson_obj_get(data, "oi"));
  const char* oi_ccy_str = yyjson_get_str(yyjson_obj_get(data, "oiCcy"));
  const char* oi_usd_str = yyjson_get_str(yyjson_obj_get(data, "oiUsd"));
  const char* ts_str = yyjson_get_str(yyjson_obj_get(data, "ts"));

  const int32_t instrument_id = resolve_instrument(inst_id);
  if (instrument_id < 0) {
    return;
  }
  int64_t open_interest_enc = oi_str ? CodecUtils::encode_quantity(std::strtod(oi_str, nullptr)) : 0;
  int64_t open_interest_ccy_enc = oi_ccy_str ? CodecUtils::encode_quantity(std::strtod(oi_ccy_str, nullptr)) : 0;
  int64_t open_interest_usd_enc = oi_usd_str ? CodecUtils::encode_quantity(std::strtod(oi_usd_str, nullptr)) : 0;
  int64_t exch_ts_ns = 0;
  if (ts_str && *ts_str) {
    exch_ts_ns = static_cast<int64_t>(std::strtoull(ts_str, nullptr, 10)) * 1000000LL;
  }

  publish_open_interest(instrument_id, open_interest_enc, open_interest_ccy_enc, open_interest_usd_enc, exch_ts_ns);
}

void OkxBlob::process_subscription_response(yyjson_val* root) {
  yyjson_val* arg = yyjson_obj_get(root, "arg");

  if (arg != nullptr) {
    const char* channel = yyjson_get_str(yyjson_obj_get(arg, "channel"));
    const char* inst_id = yyjson_get_str(yyjson_obj_get(arg, "instId"));

    if (channel != nullptr && inst_id != nullptr) {
      if (strcmp(channel, "books-l2-tbt") == 0) {
        logger_->info("Subscription confirmed: books-l2-tbt for {}", inst_id);
      } else if (strcmp(channel, "bbo-tbt") == 0) {
        logger_->info("Subscription confirmed: bbo-tbt for {}", inst_id);
      } else if (strcmp(channel, "mark-price") == 0) {
        logger_->info("Subscription confirmed: mark-price for {}", inst_id);
      } else if (strcmp(channel, "funding-rate") == 0) {
        logger_->info("Subscription confirmed: funding-rate for {}", inst_id);
      } else if (strcmp(channel, "open-interest") == 0) {
        logger_->info("Subscription confirmed: open-interest for {}", inst_id);
      } else if (strcmp(channel, "trades-all") == 0) {
        logger_->info("Subscription confirmed: trades-all for {}", inst_id);
      } else {
        logger_->info("Subscription confirmed: {} for {}", channel, inst_id);
      }
    }
  }
}

void OkxBlob::publish_trade_data(int32_t instrument_id, int64_t price_enc, int64_t size_enc, std::string_view side_str,
                                 int64_t exch_ts_ns) {
  const int64_t now = get_nano_clock().epoch_nanos();
  const Side side = (side_str == "buy" ? Side::Buy : Side::Sell);

  disruptorplus::sequence_range range;
  if (claim_strategy_->try_claim(1, range)) {
    const auto seq = range.first();  // exactly one slot
    auto* const trade = new ((*log_buffer_)[seq].raw_data()) TradeEvent();

    trade->instrument_id_ = instrument_id;
    trade->price_ = price_enc;
    trade->size_ = size_enc;
    trade->exchange_timestamp_ = exch_ts_ns;
    trade->timestamp_ns_ = now;
    trade->side_ = side;
    trade->exchange_ = Exchange::Okx;

    // logger_->info(
    //     "Publishing Trade Event at seq{}: timestamp={}, exchange_timestamp={}, exchange=OKX, instrument_id={},
    //     price={}, size={}, side={}", seq, now, exch_ts_ns, instrument_id, price_enc, size_enc, side == Side::Buy ?
    //     "Buy" : "Sell");

    claim_strategy_->publish(seq);
  } else {
    logger_->error("Failed to claim a slot in the ring buffer");
  }
}

void OkxBlob::publish_l1_update(int32_t instrument_id, int64_t bid_px_enc, int64_t bid_sz_enc, int64_t ask_px_enc,
                                int64_t ask_sz_enc, int64_t exch_ts_ns) {
  const int64_t now = get_nano_clock().epoch_nanos();

  disruptorplus::sequence_range range;
  if (claim_strategy_->try_claim(1, range)) {
    const auto seq = range.first();
    auto* const l1_update = new ((*log_buffer_)[seq].raw_data()) L1UpdateEvent();

    l1_update->instrument_id_ = instrument_id;
    l1_update->bid_price_ = bid_px_enc;
    l1_update->bid_size_ = bid_sz_enc;
    l1_update->offer_price_ = ask_px_enc;
    l1_update->offer_size_ = ask_sz_enc;
    l1_update->exchange_timestamp_ = exch_ts_ns;
    l1_update->timestamp_ns_ = now;
    l1_update->exchange_ = Exchange::Okx;

    // logger_->info(
    //   "Publishing L1 Update at seq{}: timestamp={}, exchange_timestamp={}, exchange=OKX, instrument_id={},
    //   bid_price={}, bid_size={}, ask_price={}, ask_size={}", seq, now, exch_ts_ns, instrument_id, bid_px_enc,
    //   bid_sz_enc, ask_px_enc, ask_sz_enc
    //   );

    claim_strategy_->publish(seq);
  } else {
    logger_->error("Failed to claim a slot in the ring buffer");
  }
}

// L2UpdateEvent has no exchange-timestamp field: L2 events carry local receive
// time only (timestamp_ns_).
void OkxBlob::publish_l2_book(int32_t instrument_id, const std::vector<std::pair<int64_t, int64_t>>& bids,
                              const std::vector<std::pair<int64_t, int64_t>>& asks, bool is_snapshot) {
  const int64_t now = get_nano_clock().epoch_nanos();

  if (bids.empty() && asks.empty()) {
    return;  // Nothing to publish
  }

  // Helper lambda to publish a side (bids or asks) with dual-level packing
  auto publish_side = [&](const std::vector<std::pair<int64_t, int64_t>>& levels, Side side) {
    if (levels.empty()) {
      return;
    }

    // Calculate number of messages needed (2 levels per message, round up)
    const size_t num_messages = (levels.size() + 1) / 2;

    // Claim all slots for this side at once
    disruptorplus::sequence_range range;
    if (!claim_strategy_->try_claim(num_messages, range)) {
      logger_->error("Failed to claim {} slots in the ring buffer for L2 {} side", num_messages, to_string(side));
      return;
    }

    auto seq = range.first();
    for (size_t i = 0; i < levels.size(); i += 2) {
      auto* msg = new ((*log_buffer_)[seq].raw_data()) L2UpdateEvent();

      msg->type_ = MessageType::L2UpdateEvent;
      msg->exchange_ = Exchange::Okx;
      msg->side_ = side;  // SHARED for both levels
      msg->instrument_id_ = instrument_id;
      msg->timestamp_ns_ = now;
      msg->snapshot_ = is_snapshot ? BooleanEnum::TRUE : BooleanEnum::FALSE;

      // Level 1 (always present)
      msg->price_1_ = levels[i].first;
      msg->size_1_ = levels[i].second;

      // Level 2 (if exists)
      if (i + 1 < levels.size()) {
        msg->num_levels_ = 2;
        msg->price_2_ = levels[i + 1].first;
        msg->size_2_ = levels[i + 1].second;
      } else {
        msg->num_levels_ = 1;
        msg->price_2_ = 0;
        msg->size_2_ = 0;
      }

      // Batch flags
      msg->is_batch_message_ = (num_messages > 1) ? BooleanEnum::TRUE : BooleanEnum::FALSE;
      msg->is_last_batch_ = (i + 2 >= levels.size()) ? BooleanEnum::TRUE : BooleanEnum::FALSE;

      claim_strategy_->publish(seq);
      ++seq;
    }
  };

  // Publish bids first, then asks
  publish_side(bids, Side::Buy);
  publish_side(asks, Side::Sell);
}

void OkxBlob::publish_mark_price(int32_t instrument_id, int64_t mark_price_enc, int64_t index_price_enc,
                                 int64_t exch_ts_ns) {
  const int64_t now = get_nano_clock().epoch_nanos();

  disruptorplus::sequence_range range;
  if (claim_strategy_->try_claim(1, range)) {
    const auto seq = range.first();
    auto* const event = new ((*log_buffer_)[seq].raw_data()) MarkPriceEvent();

    event->instrument_id_ = instrument_id;
    event->mark_price_ = mark_price_enc;
    event->index_price_ = index_price_enc;
    event->exchange_timestamp_ns_ = exch_ts_ns;
    event->timestamp_ns_ = now;
    event->exchange_ = Exchange::Okx;

    claim_strategy_->publish(seq);
  } else {
    logger_->error("Failed to claim a slot in the ring buffer for mark price event");
  }
}

void OkxBlob::publish_funding_rate(int32_t instrument_id, int64_t funding_rate_enc, int64_t next_funding_rate_enc,
                                   int64_t interest_rate_enc, int64_t funding_time_ns, int64_t next_funding_time_ns) {
  const int64_t now = get_nano_clock().epoch_nanos();

  disruptorplus::sequence_range range;
  if (claim_strategy_->try_claim(1, range)) {
    const auto seq = range.first();
    auto* const event = new ((*log_buffer_)[seq].raw_data()) FundingRateEvent();

    event->instrument_id_ = instrument_id;
    event->funding_rate_ = funding_rate_enc;
    event->next_funding_rate_ = next_funding_rate_enc;
    event->interest_rate_ = interest_rate_enc;
    event->funding_time_ns_ = funding_time_ns;
    event->next_funding_time_ns_ = next_funding_time_ns;
    event->timestamp_ns_ = now;
    event->exchange_ = Exchange::Okx;

    claim_strategy_->publish(seq);
  } else {
    logger_->error("Failed to claim a slot in the ring buffer for funding rate event");
  }
}

void OkxBlob::publish_open_interest(int32_t instrument_id, int64_t open_interest_enc, int64_t open_interest_ccy_enc,
                                    int64_t open_interest_usd_enc, int64_t exch_ts_ns) {
  const int64_t now = get_nano_clock().epoch_nanos();

  disruptorplus::sequence_range range;
  if (claim_strategy_->try_claim(1, range)) {
    const auto seq = range.first();
    auto* const event = new ((*log_buffer_)[seq].raw_data()) OpenInterestEvent();

    event->instrument_id_ = instrument_id;
    event->open_interest_ = open_interest_enc;
    event->open_interest_currency_ = open_interest_ccy_enc;
    event->open_interest_usd_ = open_interest_usd_enc;
    event->exchange_timestamp_ns_ = exch_ts_ns;
    event->timestamp_ns_ = now;
    event->exchange_ = Exchange::Okx;

    claim_strategy_->publish(seq);
  } else {
    logger_->error("Failed to claim a slot in the ring buffer for open interest event");
  }
}
}  // namespace reflex
