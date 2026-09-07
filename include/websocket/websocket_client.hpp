#pragma once

#include <array>
#include <deque>
#include <functional>
#include <memory>
#include <poll.h>  // Add this for pollfd
#include <string>
#include <string_view>
#include <vector>

#include "offset_epoch_nano_clock.hpp"
#include <libwebsockets.h>
#include "spdlog/logger.h"

// Forward declarations
struct lws_context;
struct lws;

namespace reflex {

enum class ConnectionState {
  DISCONNECTED,
  CONNECTING,
  CONNECTED,
  AUTHENTICATING,    // Business channel: sent auth, waiting for response
  AUTHENTICATED,     // Business channel: auth successful, ready to subscribe
  WORKING,           // Final state: processing live data
  RECONNECTING,
  ERROR
};

inline std::string to_string(ConnectionState state) {
  switch (state) {
    case ConnectionState::DISCONNECTED:
      return "DISCONNECTED";
    case ConnectionState::CONNECTING:
      return "CONNECTING";
    case ConnectionState::CONNECTED:
      return "CONNECTED";
    case ConnectionState::RECONNECTING:
      return "RECONNECTING";
    case ConnectionState::ERROR:
      return "ERROR";
    case ConnectionState::AUTHENTICATING:
      return "AUTHENTICATING";
    case ConnectionState::AUTHENTICATED:
      return "AUTHENTICATED";
    case ConnectionState::WORKING:
      return "WORKING";
    default:
      return "INVALID";
  }
}

class WebsocketClient {
public:
  using MessageCallback = std::function<void(std::string_view)>;
  using StateCallback = std::function<void(ConnectionState)>;

  explicit WebsocketClient(const std::string& url, const std::string& context, OffsetEpochNanoClock& nano_clock);
  ~WebsocketClient();

  // Owns raw lws_context*/lws* handles - not copyable or movable.
  WebsocketClient(const WebsocketClient&) = delete;
  WebsocketClient& operator=(const WebsocketClient&) = delete;
  WebsocketClient(WebsocketClient&&) = delete;
  WebsocketClient& operator=(WebsocketClient&&) = delete;

  // Core interface for your BaseService integration
  void connect();
  void disconnect(bool hard_reset, std::string_view reason = {});
  void get_poll_fds(std::vector<struct pollfd>& fds);
  void service_ready_fds(const std::vector<struct pollfd>& fds);

  // Direct message sending (no queue needed since you control timing)
  /** Queue @p message for transmission. @p on_sent (optional) is invoked once
      the frame has been fully handed to the socket - use it to timestamp a
      request at the moment it actually left the process. */
  bool send_message_immediate(const std::string& message,
                              std::function<void()> on_sent = {});

  //------------------------ zero-copy TX API ------------------------------
  /** Claim a transmit buffer for @p len payload bytes (after LWS_PRE).
      Returns nullptr when the fixed pool is full or len > kMaxFrameSize. */
  unsigned char* begin_frame(size_t len);

  /** Commit the buffer obtained from begin_frame() so it will be sent.
      @p payload_ptr must be the pointer returned by begin_frame(). */
  void end_frame(unsigned char* payload_ptr, size_t len);

  void set_message_callback(MessageCallback callback);
  void set_state_callback(StateCallback callback);

  /** Disable the OKX-style application-level text "ping" heartbeat for venues
      that only accept websocket control frames (some venues close the
      connection on unexpected text messages). */
  void set_text_ping_enabled(bool enabled) { text_ping_enabled_ = enabled; }

  /** Add a custom HTTP header to the websocket upgrade handshake (e.g. an
      API-key header some venues require). Call before connect(); headers
      persist across reconnects. `name` without trailing colon. */
  void add_handshake_header(const std::string& name, const std::string& value);

  /** Remove all custom handshake headers. Venues whose headers embed a
      timestamped signature must clear + re-add fresh headers before every
      connect, or reconnects fail auth with a stale signature. */
  void clear_handshake_headers() { handshake_headers_.clear(); }

  /** Invoked from the lws APPEND_HANDSHAKE_HEADER callback; returns 0 on
      success, -1 if the header buffer is exhausted (aborts the connect). */
  int append_handshake_headers(struct lws* wsi, unsigned char** p, unsigned char* end);

  void set_state(ConnectionState new_state, std::string_view reason = {});
  void set_disconnect_reason(std::string_view reason);

  // State queries
  ConnectionState get_state() const { return state_; }
  bool is_connected() const {
    return state_ == ConnectionState::CONNECTED ||
           state_ == ConnectionState::AUTHENTICATING ||
           state_ == ConnectionState::AUTHENTICATED ||
           state_ == ConnectionState::WORKING;
  }

  /** Expose the underlying libwebsockets context so callers can invoke
      lws_service(ctx, 0) when they already hold the event‑loop lock. */
  struct lws_context* get_context() const noexcept { return context_; }

  void send_ping_if_needed(uint64_t now_nanos);

  // Public callback for libwebsockets - using int to avoid forward
  // declaration issues
  static int lws_callback(struct lws* wsi, int reason, void* user, void* in, size_t len);

  // Public callback handlers (needed for static callback access)
  void handle_client_established();
  void handle_client_receive(struct lws* wsi, const char* data, size_t len);
  void handle_client_writeable();
  void handle_client_closed();
  void handle_client_connection_error(const char* details = nullptr);
  void handle_client_pong();
  void update_pong_timestamp();
  bool is_pong_timed_out(uint64_t now_nanos) const;
  bool has_pending_work();
  void do_pending_work();
  int64_t get_last_state_change_nanos() const noexcept { return last_state_change_nanos_; }

  /* expose the cached pollfd to the caller */
  const struct pollfd* pollfd_ptr() const noexcept { return pfd_.fd >= 0 ? &pfd_ : nullptr; }
  bool needs_poll() const; // see .cpp

  bool is_connection_timed_out(uint64_t now_nanos) const {
    return state_ == ConnectionState::CONNECTING &&
           now_nanos - last_state_change_nanos_ > CONNECTION_TIMEOUT_NANOS;
  }

  // pool configuration
  static constexpr size_t kMaxFrameSize = 4096;   // tune per venue
  static constexpr size_t kTxPoolSize   = 32;     // in-flight frames

  // Cap for reassembling fragmented RX messages (large book snapshots arrive
  // split across several LWS_CALLBACK_CLIENT_RECEIVE chunks). Anything larger
  // is dropped with an error log.
  static constexpr size_t kMaxRxMessageSize = 4 * 1024 * 1024;

private:
  std::shared_ptr<spdlog::logger> logger_;
  OffsetEpochNanoClock& nano_clock_;

  // Configuration
  std::string url_;
  std::string host_;
  std::string path_;
  int port_;
  bool use_ssl_;

  // libwebsockets objects
  struct lws_context* context_ = nullptr;
  struct lws* wsi_ = nullptr;

  // State management
  ConnectionState state_ = ConnectionState::DISCONNECTED;

  // Callbacks
  MessageCallback message_callback_;
  StateCallback state_callback_;

  /* cached pollfd to avoid per-loop allocation */
  struct pollfd pfd_{};

  // Zero-copy transmit pool
  struct TxBlock {
    std::array<unsigned char, LWS_PRE + kMaxFrameSize> buf{};
    size_t len = 0;
    std::function<void()> on_sent;
  };

  std::array<TxBlock, kTxPoolSize> tx_pool_{};
  std::vector<size_t>              free_stack_;   // indices available
  std::deque<size_t>               pending_q_;    // ready to send

  // RX fragment reassembly. Complete single-chunk messages bypass this buffer
  // (zero-copy fast path); fragmented messages accumulate here until the final
  // fragment arrives. rx_dropping_ swallows the remainder of an oversized
  // message so we resynchronise on the next message boundary.
  std::string rx_buffer_;
  bool rx_dropping_ = false;

  // Internal methods
  void parse_url();
  void create_context();
  void cleanup();
  void reset_transfer_state();

  uint64_t last_ping_time_nanos_ = 0;
  static constexpr uint64_t kPingIntervalNanos = 15ULL * 1'000'000'000; // 15 seconds


  uint64_t last_pong_time_nanos_ = 0;
  static constexpr uint64_t kPongTimeoutNanos = 60ULL * 1'000'000'000ULL; // 60 seconds


  uint64_t last_state_change_nanos_ = 0;

  bool text_ping_enabled_ = true;  // OKX-style text heartbeat (see setter)

  // Extra HTTP headers for the upgrade handshake ("name:" -> value)
  std::vector<std::pair<std::string, std::string>> handshake_headers_;

  static constexpr uint64_t CONNECTION_TIMEOUT_NANOS = 60ULL * 1'000'000'000; // 60 seconds (was 30 seconds)

  std::string pending_disconnect_reason_;

};

}  // namespace reflex
