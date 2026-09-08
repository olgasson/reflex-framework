#include "../include/websocket/websocket_client.hpp"
#include <libwebsockets.h>
#include <cstring>
#include <vector>
#include <iostream>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>

#include "logger_factory.hpp"

namespace reflex {

static int lws_callback_impl(struct lws* wsi, enum lws_callback_reasons reason,
                             void* user, void* in, size_t len);

static struct lws_protocols protocols[] = {
    {
        "reflex-client",
        lws_callback_impl,
        sizeof(WebsocketClient*),
        4096 * 20,
        0, nullptr, 0
    },
    LWS_PROTOCOL_LIST_TERM
};

WebsocketClient::WebsocketClient(const std::string& url, const std::string& context, OffsetEpochNanoClock& nano_clock)
  : nano_clock_(nano_clock)
  , url_(url) {
  logger_ = LoggerFactory::getLogger(context + ".WebsocketClient");
  parse_url();
  pfd_.fd = -1;
  pfd_.events = POLLIN | POLLOUT;
  pfd_.revents = 0;

  free_stack_.reserve(kTxPoolSize);
  for (size_t i = 0; i < kTxPoolSize; ++i) {
    free_stack_.push_back(kTxPoolSize - 1 - i);
  }

}

WebsocketClient::~WebsocketClient() {
  cleanup();
}

void WebsocketClient::parse_url() {
  size_t protocol_end = url_.find("://");
  if (protocol_end == std::string::npos) {
    throw std::invalid_argument("Invalid URL format");
  }

  std::string protocol = url_.substr(0, protocol_end);
  use_ssl_ = (protocol == "wss");

  size_t host_start = protocol_end + 3;
  size_t port_start = url_.find(":", host_start);
  size_t path_start = url_.find("/", host_start);

  if (port_start != std::string::npos && port_start < path_start) {
    host_ = url_.substr(host_start, port_start - host_start);
    size_t port_end = (path_start != std::string::npos) ? path_start : url_.length();
    try {
      port_ = std::stoi(url_.substr(port_start + 1, port_end - port_start - 1));
    } catch (const std::exception&) {
      throw std::invalid_argument("Invalid port in URL: " + url_);
    }
    if (port_ <= 0 || port_ > 65535) {
      throw std::invalid_argument("Port out of range in URL: " + url_);
    }
  } else {
    port_ = use_ssl_ ? 443 : 80;
    size_t host_end = (path_start != std::string::npos) ? path_start : url_.length();
    host_ = url_.substr(host_start, host_end - host_start);
  }

  path_ = (path_start != std::string::npos) ? url_.substr(path_start) : "/";
}

void WebsocketClient::create_context() {
  if (context_) {
    return;
  }

  struct lws_context_creation_info info;
  memset(&info, 0, sizeof(info));

  info.port = CONTEXT_PORT_NO_LISTEN;
  info.protocols = protocols;
  info.user = this;
  info.gid = -1;
  info.uid = -1;

  if (use_ssl_) {
    info.options |= LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT;

    static constexpr const char* kCaBundlePaths[] = {
        "/etc/ssl/cert.pem",
        "/etc/ssl/certs/ca-certificates.crt",
        "/etc/pki/tls/certs/ca-bundle.crt",
        "/opt/homebrew/etc/openssl@3/cert.pem",
        "/usr/local/etc/openssl@3/cert.pem",
    };
    for (const char* ca_path : kCaBundlePaths) {
      if (::access(ca_path, R_OK) == 0) {
        info.client_ssl_ca_filepath = ca_path;
        break;
      }
    }
    if (info.client_ssl_ca_filepath == nullptr) {
      logger_->warn("No system CA bundle found - relying on OpenSSL default verify paths");
    }
  }

  info.ka_time = 5;
  info.ka_probes = 2;
  info.ka_interval = 2;
  info.timeout_secs = 8;

  lws_set_log_level(LLL_ERR | LLL_WARN, nullptr);

  context_ = lws_create_context(&info);
  if (!context_) {
    logger_->error("Failed to create libwebsockets context");
    return;
  }
}

void WebsocketClient::connect() {
  if (state_ != ConnectionState::DISCONNECTED) {
    disconnect(true, "connect() resetting existing session");
  }

  create_context();
  if (!context_) {
    set_state(ConnectionState::ERROR, "failed to create libwebsockets context");
    return;
  }

  struct lws_client_connect_info connect_info;
  memset(&connect_info, 0, sizeof(connect_info));

  connect_info.context = context_;
  connect_info.address = host_.c_str();
  connect_info.port = port_;
  connect_info.path = path_.c_str();
  connect_info.host = host_.c_str();
  connect_info.origin = host_.c_str();
  connect_info.protocol = protocols[0].name;
  connect_info.userdata = this;

  if (use_ssl_) {
    connect_info.ssl_connection = LCCSCF_USE_SSL;
  }

  set_state(ConnectionState::CONNECTING, "initiating client connection");

  wsi_ = lws_client_connect_via_info(&connect_info);
  if (!wsi_) {
    logger_->error("Failed to initiate WebSocket connection");
    set_state(ConnectionState::ERROR, "lws_client_connect_via_info returned nullptr");
    return;
  }

  pfd_.fd = lws_get_socket_fd(wsi_);
  lws_set_wsi_user(wsi_, this);
}


void WebsocketClient::disconnect(bool hard_reset, std::string_view reason) {
  if (!reason.empty()) {
    set_disconnect_reason(reason);
  }
  if (hard_reset) {
    if (wsi_) {
      lws_set_wsi_user(wsi_, nullptr);
      wsi_ = nullptr;
    }
    if (context_) {
      lws_context_destroy(context_);
      context_ = nullptr;
    }
  } else {
    if (wsi_) {
      lws_set_wsi_user(wsi_, nullptr);
      lws_set_timeout(wsi_, PENDING_TIMEOUT_USER_OK, LWS_TO_KILL_ASYNC);
      wsi_ = nullptr;
    }
  }
  pfd_.fd = -1;
  reset_transfer_state();
  set_state(ConnectionState::DISCONNECTED);
}

void WebsocketClient::reset_transfer_state() {
  while (!pending_q_.empty()) {
    tx_pool_[pending_q_.front()].on_sent = {};
    free_stack_.push_back(pending_q_.front());
    pending_q_.pop_front();
  }
  rx_buffer_.clear();
  rx_dropping_ = false;
}

void WebsocketClient::set_disconnect_reason(std::string_view reason) {
  pending_disconnect_reason_.assign(reason.begin(), reason.end());
}

unsigned char* WebsocketClient::begin_frame(size_t len)
{
  if (!is_connected() || len > kMaxFrameSize || free_stack_.empty()) {
    return nullptr;
  }

  size_t idx = free_stack_.back();
  free_stack_.pop_back();
  tx_pool_[idx].len = len;
  return tx_pool_[idx].buf.data() + LWS_PRE;
}

void WebsocketClient::end_frame(unsigned char* payload_ptr, size_t len)
{
  size_t idx = kTxPoolSize;
  for (size_t i = 0; i < kTxPoolSize; ++i) {
    if (tx_pool_[i].buf.data() + LWS_PRE == payload_ptr) {
      idx = i;
      break;
    }
  }

  if (idx == kTxPoolSize) {
    logger_->error("end_frame with unknown pointer");
    return;
  }

  tx_pool_[idx].len = len;
  pending_q_.push_back(idx);
  if (wsi_) {
    lws_callback_on_writable(wsi_);
  }
}


void WebsocketClient::get_poll_fds(std::vector<struct pollfd>& fds) {
  if (pfd_.fd >= 0) {
    pfd_.revents = 0;
    fds.push_back(pfd_);
  }
}

void WebsocketClient::service_ready_fds(const std::vector<struct pollfd>& fds) {
  if (!context_ || !wsi_) return;

  int socket_fd = lws_get_socket_fd(wsi_);
  if (socket_fd < 0) return;

  for (const auto& fd : fds) {
    if (fd.fd == socket_fd && fd.revents != 0) {
      struct lws_pollfd pfd = {fd.fd, fd.events, fd.revents};
      lws_service_fd(context_, &pfd);
#if defined(__linux__)
      const int quickack = 1;
      setsockopt(socket_fd, IPPROTO_TCP, TCP_QUICKACK, &quickack,
                 sizeof(quickack));
#endif
      return;
    }
  }
}


bool WebsocketClient::has_pending_work() {
  return !pending_q_.empty();
}

void WebsocketClient::do_pending_work() {
  if (!has_pending_work() || !is_connected() || !wsi_) return;

  lws_callback_on_writable(wsi_);
}

bool WebsocketClient::send_message_immediate(const std::string& msg,
                                             std::function<void()> on_sent)
{
  if (!is_connected()) {
    logger_->warn("Cannot send – not connected");
    return false;
  }

  unsigned char* p = begin_frame(msg.size());
  if (!p) {
    logger_->error("TX pool exhausted – dropping message");
    return false;
  }

  std::memcpy(p, msg.data(), msg.size());
  if (on_sent) {
    for (auto& block : tx_pool_) {
      if (block.buf.data() + LWS_PRE == p) {
        block.on_sent = std::move(on_sent);
        break;
      }
    }
  }
  end_frame(p, msg.size());
  return true;
}


void WebsocketClient::set_message_callback(MessageCallback callback) {
  message_callback_ = std::move(callback);
}

void WebsocketClient::set_state_callback(StateCallback callback) {
  state_callback_ = std::move(callback);
}

void WebsocketClient::set_state(ConnectionState new_state, std::string_view reason) {
  std::string reason_buffer;
  if (!reason.empty()) {
    reason_buffer.assign(reason.begin(), reason.end());
  } else if ((new_state == ConnectionState::DISCONNECTED || new_state == ConnectionState::ERROR) &&
             !pending_disconnect_reason_.empty()) {
    reason_buffer = std::move(pending_disconnect_reason_);
    pending_disconnect_reason_.clear();
  }

  if (!reason_buffer.empty()) {
    logger_->info("Websocket state changed: {} -> {} ({})", to_string(state_), to_string(new_state), reason_buffer);
  } else {
    logger_->info("Websocket state changed: {} -> {}", to_string(state_), to_string(new_state));
  }
  if (state_ != new_state) {
    state_ = new_state;
    last_state_change_nanos_ = nano_clock_.epoch_nanos();
    if (state_callback_) {
      state_callback_(new_state);
    }
  }
}

void WebsocketClient::cleanup() {
  wsi_ = nullptr;
  pfd_.fd = -1;

  if (context_) {
    lws_context_destroy(context_);
    context_ = nullptr;
  }
}


int WebsocketClient::lws_callback(struct lws* wsi, int reason, void* user, void* in, size_t len) {
  return lws_callback_impl(wsi, static_cast<enum lws_callback_reasons>(reason), user, in, len);
}


static int lws_callback_impl(struct lws* wsi, enum lws_callback_reasons reason,
                             void* user, void* in, size_t len) {
    WebsocketClient* client = nullptr;
    static auto ws_callback_logger = LoggerFactory::getLogger("WebsocketClient.Callback");

    if (wsi) {
        client = static_cast<WebsocketClient*>(lws_wsi_user(wsi));
    }
    if (!client && user) {
        client = static_cast<WebsocketClient*>(user);
    }

    switch (reason) {
        case LWS_CALLBACK_CLIENT_ESTABLISHED:
            if (client) client->handle_client_established();
            break;

        case LWS_CALLBACK_CLIENT_CONNECTION_ERROR: {
            const char* err = in ? static_cast<const char*>(in) : "";
            if (in) {
                ws_callback_logger->error("Connection error: {}", err);
            }
            if (client) client->handle_client_connection_error(err);
            break;
        }

        case LWS_CALLBACK_CLIENT_APPEND_HANDSHAKE_HEADER:
            if (client) {
                return client->append_handshake_headers(
                    wsi, reinterpret_cast<unsigned char**>(in),
                    *reinterpret_cast<unsigned char**>(in) + len);
            }
            break;

        case LWS_CALLBACK_CLIENT_RECEIVE:
            if (client) {
                client->handle_client_receive(wsi, static_cast<const char*>(in), len);
            }
            break;

        case LWS_CALLBACK_CLIENT_WRITEABLE:
            if (client) client->handle_client_writeable();
            break;

        case LWS_CALLBACK_WS_PEER_INITIATED_CLOSE:
            if (in && len >= 2) {
                uint16_t code = ((uint8_t*)in)[0] << 8 | ((uint8_t*)in)[1];
                std::string msg((char*)in + 2, len - 2);
                ws_callback_logger->warn("Peer closed connection: {} \"{}\"", code, msg);
                if (client) {
                    client->set_disconnect_reason("peer initiated close code=" + std::to_string(code) + " msg=\"" + msg + "\"");
                }
            } else if (client) {
                client->set_disconnect_reason("peer initiated close without payload");
            }
            break;

        case LWS_CALLBACK_CLIENT_CLOSED:
            if (client) client->handle_client_closed();
            break;

        case LWS_CALLBACK_CLIENT_RECEIVE_PONG:
            if (client) client->handle_client_pong();
            break;

        default:
            break;
    }

    return 0;
}


void WebsocketClient::handle_client_established() {
  const int socket_fd = lws_get_socket_fd(wsi_);
  if (socket_fd >= 0) {
    const int enable = 1;
    if (setsockopt(socket_fd, IPPROTO_TCP, TCP_NODELAY, &enable,
                   sizeof(enable)) != 0) {
      logger_->warn("failed to set TCP_NODELAY on websocket fd {}",
                    socket_fd);
    }
  }
  char peer_address[128]{};
  if (lws_get_peer_simple(wsi_, peer_address, sizeof(peer_address)) != nullptr &&
      peer_address[0] != '\0') {
    logger_->info("WebSocket peer address: {}", peer_address);
  }
  set_state(ConnectionState::CONNECTED);
  last_pong_time_nanos_ = nano_clock_.epoch_nanos();
  last_ping_time_nanos_ = nano_clock_.epoch_nanos();
  rx_buffer_.clear();
  rx_dropping_ = false;
}

void WebsocketClient::add_handshake_header(const std::string& name, const std::string& value) {
  handshake_headers_.emplace_back(name + ":", value);
}

int WebsocketClient::append_handshake_headers(struct lws* wsi, unsigned char** p, unsigned char* end) {
  for (const auto& [name, value] : handshake_headers_) {
    if (lws_add_http_header_by_name(wsi, reinterpret_cast<const unsigned char*>(name.c_str()),
                                    reinterpret_cast<const unsigned char*>(value.c_str()),
                                    static_cast<int>(value.size()), p, end) != 0) {
      logger_->error("Handshake header buffer exhausted adding {}", name);
      return -1;
    }
  }
  return 0;
}

void WebsocketClient::handle_client_receive(struct lws* wsi, const char* data, size_t len) {
  if (wsi != wsi_ || !message_callback_) {
    return;
  }

  const bool is_final = lws_is_final_fragment(wsi) && lws_remaining_packet_payload(wsi) == 0;

  if (rx_dropping_) {
    rx_dropping_ = !is_final;
    return;
  }

  if (rx_buffer_.empty() && is_final) {
    message_callback_(std::string_view(data, len));
    return;
  }

  if (rx_buffer_.size() + len > kMaxRxMessageSize) {
    logger_->error("RX message exceeded {} byte reassembly cap - dropping message", kMaxRxMessageSize);
    rx_buffer_.clear();
    rx_dropping_ = !is_final;
    return;
  }

  rx_buffer_.append(data, len);
  if (is_final) {
    message_callback_(std::string_view(rx_buffer_));
    rx_buffer_.clear();
  }
}

void WebsocketClient::handle_client_writeable()
{
  if (!wsi_ || !is_connected()) {
    return;
  }

  if (lws_partial_buffered(wsi_)) {
    lws_callback_on_writable(wsi_);
    return;
  }

  if (pending_q_.empty()) {
    return;
  }

  const size_t idx = pending_q_.front();
  pending_q_.pop_front();
  TxBlock& blk = tx_pool_[idx];

  const int n = lws_write(wsi_, blk.buf.data() + LWS_PRE, blk.len, LWS_WRITE_TEXT);
  auto on_sent = std::move(blk.on_sent);
  blk.on_sent = {};
  free_stack_.push_back(idx);

  if (n < static_cast<int>(blk.len)) {
    logger_->error("lws_write failed (ret={}, len={})", n, blk.len);
    set_state(ConnectionState::ERROR, "lws_write short return");
    return;
  }

  if (on_sent) on_sent();

  if (!pending_q_.empty() || lws_partial_buffered(wsi_)) {
    lws_callback_on_writable(wsi_);
  }
}


void WebsocketClient::handle_client_closed() {
  if (pending_disconnect_reason_.empty()) {
    set_disconnect_reason("libwebsockets reported CLIENT_CLOSED");
  }
  wsi_ = nullptr;
  pfd_.fd = -1;
  reset_transfer_state();
  set_state(ConnectionState::DISCONNECTED);
}

void WebsocketClient::handle_client_connection_error(const char* details) {
  const char* msg = (details && std::strlen(details) > 0) ? details : "connection error (no details)";
  wsi_ = nullptr;
  pfd_.fd = -1;
  reset_transfer_state();
  set_state(ConnectionState::ERROR, msg);
}

void WebsocketClient::send_ping_if_needed(uint64_t now_nanos) {
  if (!is_connected() || !wsi_) return;
  if (now_nanos - last_ping_time_nanos_ < kPingIntervalNanos) return;

  last_ping_time_nanos_ = now_nanos;

  if (!text_ping_enabled_) return;

  if (send_message_immediate("ping")) {
    logger_->info("WS text ping sent to {}", host_);
  } else {
    logger_->warn("WS text ping failed to send on {}", host_);
  }
}

bool WebsocketClient::needs_poll() const {
  return context_ && lws_service_adjust_timeout(context_, 0, 0) > 0;
}

void WebsocketClient::handle_client_pong() {
  update_pong_timestamp();
  logger_->info("WS-PONG (control frame) from {}", host_);
}

bool WebsocketClient::is_pong_timed_out(uint64_t now) const {
  return is_connected() &&
           last_pong_time_nanos_ != 0 &&
           now - last_pong_time_nanos_ > kPongTimeoutNanos;
}

void WebsocketClient::update_pong_timestamp() {
  last_pong_time_nanos_ = nano_clock_.epoch_nanos();
}

}
