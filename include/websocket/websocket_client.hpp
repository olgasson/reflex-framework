#pragma once

#include <array>
#include <deque>
#include <functional>
#include <memory>
#include <poll.h>
#include <string>
#include <string_view>
#include <vector>

#include "offset_epoch_nano_clock.hpp"
#include <libwebsockets.h>
#include "spdlog/logger.h"

struct lws_context;
struct lws;

namespace reflex {

enum class ConnectionState {
  DISCONNECTED,
  CONNECTING,
  CONNECTED,
  AUTHENTICATING,
  AUTHENTICATED,
  WORKING,
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

  WebsocketClient(const WebsocketClient&) = delete;
  WebsocketClient& operator=(const WebsocketClient&) = delete;
  WebsocketClient(WebsocketClient&&) = delete;
  WebsocketClient& operator=(WebsocketClient&&) = delete;

  void connect();
  void disconnect(bool hard_reset, std::string_view reason = {});
  void get_poll_fds(std::vector<struct pollfd>& fds);
  void service_ready_fds(const std::vector<struct pollfd>& fds);

  bool send_message_immediate(const std::string& message,
                              std::function<void()> on_sent = {});

  unsigned char* begin_frame(size_t len);

  void end_frame(unsigned char* payload_ptr, size_t len);

  void set_message_callback(MessageCallback callback);
  void set_state_callback(StateCallback callback);

  void set_text_ping_enabled(bool enabled) { text_ping_enabled_ = enabled; }

  void add_handshake_header(const std::string& name, const std::string& value);

  void clear_handshake_headers() { handshake_headers_.clear(); }

  int append_handshake_headers(struct lws* wsi, unsigned char** p, unsigned char* end);

  void set_state(ConnectionState new_state, std::string_view reason = {});
  void set_disconnect_reason(std::string_view reason);

  ConnectionState get_state() const { return state_; }
  bool is_connected() const {
    return state_ == ConnectionState::CONNECTED ||
           state_ == ConnectionState::AUTHENTICATING ||
           state_ == ConnectionState::AUTHENTICATED ||
           state_ == ConnectionState::WORKING;
  }

  struct lws_context* get_context() const noexcept { return context_; }

  void send_ping_if_needed(uint64_t now_nanos);

  static int lws_callback(struct lws* wsi, int reason, void* user, void* in, size_t len);

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

  const struct pollfd* pollfd_ptr() const noexcept { return pfd_.fd >= 0 ? &pfd_ : nullptr; }
  bool needs_poll() const;

  bool is_connection_timed_out(uint64_t now_nanos) const {
    return state_ == ConnectionState::CONNECTING &&
           now_nanos - last_state_change_nanos_ > CONNECTION_TIMEOUT_NANOS;
  }

  static constexpr size_t kMaxFrameSize = 4096;
  static constexpr size_t kTxPoolSize   = 32;

  static constexpr size_t kMaxRxMessageSize = 4 * 1024 * 1024;

private:
  std::shared_ptr<spdlog::logger> logger_;
  OffsetEpochNanoClock& nano_clock_;

  std::string url_;
  std::string host_;
  std::string path_;
  int port_;
  bool use_ssl_;

  struct lws_context* context_ = nullptr;
  struct lws* wsi_ = nullptr;

  ConnectionState state_ = ConnectionState::DISCONNECTED;

  MessageCallback message_callback_;
  StateCallback state_callback_;

  struct pollfd pfd_{};

  struct TxBlock {
    std::array<unsigned char, LWS_PRE + kMaxFrameSize> buf{};
    size_t len = 0;
    std::function<void()> on_sent;
  };

  std::array<TxBlock, kTxPoolSize> tx_pool_{};
  std::vector<size_t>              free_stack_;
  std::deque<size_t>               pending_q_;

  std::string rx_buffer_;
  bool rx_dropping_ = false;

  void parse_url();
  void create_context();
  void cleanup();
  void reset_transfer_state();

  uint64_t last_ping_time_nanos_ = 0;
  static constexpr uint64_t kPingIntervalNanos = 15ULL * 1'000'000'000;


  uint64_t last_pong_time_nanos_ = 0;
  static constexpr uint64_t kPongTimeoutNanos = 60ULL * 1'000'000'000ULL;


  uint64_t last_state_change_nanos_ = 0;

  bool text_ping_enabled_ = true;

  std::vector<std::pair<std::string, std::string>> handshake_headers_;

  static constexpr uint64_t CONNECTION_TIMEOUT_NANOS = 60ULL * 1'000'000'000;

  std::string pending_disconnect_reason_;

};

}
