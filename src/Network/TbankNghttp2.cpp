#include "exchange_probe/TbankGrpcDiagnostic.hpp"
#include "TbankConnectionControl.hpp"

#include <nghttp2/nghttp2.h>
#include <openssl/crypto.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>

#include <arpa/inet.h>
#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cerrno>
#include <climits>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace exchange_probe::tbank_grpc {
namespace {

constexpr std::size_t kMaxMessage = 65536;
constexpr std::size_t kMaxFrame = 16384;
constexpr std::uint64_t kWaitSliceNs = 10000000;

std::uint64_t now_ns() {
  return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count());
}

std::uint64_t add_ns(std::uint64_t a, std::uint64_t b) {
  return b > std::numeric_limits<std::uint64_t>::max() - a
             ? std::numeric_limits<std::uint64_t>::max() : a + b;
}

class Nghttp2Connection final : public Connection {
 public:
  ~Nghttp2Connection() override { close(); }

  bool connect(const ConnectOptions& options, EventSink sink,
               const std::function<void()>& service_other) override {
    if (used_) return false;
    used_ = true;
    sink_ = std::move(sink);
    if (options.host.empty() || options.pinned_ip.empty() || options.ca_file.empty() || options.host.find('\0') != std::string_view::npos ||
        options.pinned_ip.find('\0') != std::string_view::npos ||
        options.ca_file.find('\0') != std::string_view::npos || options.port == 0 ||
        options.deadline_ns <= now_ns() ||
        (options.ping_interval_ns != 0 && options.ping_ack_timeout_ns == 0)) {
      return fail("configuration", 0);
    }
    host_ = options.host;
    stop_requested_ = options.stop_requested;
    report_ping_sends_ = options.report_ping_sends;
    frame_based_ping_idle_ = options.frame_based_ping_idle;
    if (stop_requested_ && stop_requested_()) return fail("connect_stopped", 0);
    ping_interval_ = options.ping_interval_ns;
    ping_timeout_ = options.ping_ack_timeout_ns;
    sockaddr_storage address{};
    socklen_t address_size = 0;
    const std::string pinned(options.pinned_ip);
    auto* ipv4 = reinterpret_cast<sockaddr_in*>(&address);
    auto* ipv6 = reinterpret_cast<sockaddr_in6*>(&address);
    if (inet_pton(AF_INET, pinned.c_str(), &ipv4->sin_addr) == 1) {
      ipv4->sin_family = AF_INET;
      ipv4->sin_port = htons(options.port);
      address_size = sizeof(*ipv4);
    } else if (inet_pton(AF_INET6, pinned.c_str(), &ipv6->sin6_addr) == 1) {
      ipv6->sin6_family = AF_INET6;
      ipv6->sin6_port = htons(options.port);
      address_size = sizeof(*ipv6);
    } else {
      return fail("pinned_ip", 0);
    }
    fd_ = ::socket(address.ss_family, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd_ < 0) return fail("tcp_socket", errno);
    if (stop_requested_ && stop_requested_()) return fail("connect_stopped", 0);
    const int connect_rc = ::connect(fd_, reinterpret_cast<const sockaddr*>(&address), address_size);
    if (connect_rc != 0 && (errno != EINPROGRESS || !wait_fd(POLLOUT, options.deadline_ns, service_other)))
      return fail(stop_requested_ && stop_requested_() ? "connect_stopped" : "tcp_connect", errno);
    if (stop_requested_ && stop_requested_()) return fail("connect_stopped", 0);
    int connect_error = 0;
    socklen_t connect_error_size = sizeof(connect_error);
    if (getsockopt(fd_, SOL_SOCKET, SO_ERROR, &connect_error, &connect_error_size) != 0 || connect_error != 0)
      return fail("tcp_connect", connect_error);
    sockaddr_storage peer{};
    socklen_t peer_size = sizeof(peer);
    std::array<char, NI_MAXHOST> numeric_peer{};
    if (getpeername(fd_, reinterpret_cast<sockaddr*>(&peer), &peer_size) != 0 ||
        getnameinfo(reinterpret_cast<const sockaddr*>(&peer), peer_size, numeric_peer.data(),
                    numeric_peer.size(), nullptr, 0, NI_NUMERICHOST) != 0) {
      return fail("peer_address", errno);
    }
    peer_ = numeric_peer.data();
    ctx_ = SSL_CTX_new(TLS_client_method());
    if (ctx_ == nullptr) return fail("tls_context", 0);
    SSL_CTX_set_verify(ctx_, SSL_VERIFY_PEER, nullptr);
    if (SSL_CTX_load_verify_locations(ctx_, std::string(options.ca_file).c_str(), nullptr) != 1)
      return fail("tls_ca", 0);
    ssl_ = SSL_new(ctx_);
    static constexpr unsigned char alpn[] = {2, 'h', '2'};
    const BIO_METHOD* method = socket_bio_method();
    BIO* bio = ssl_ == nullptr || method == nullptr ? nullptr : BIO_new(method);
    if (bio != nullptr) { BIO_set_data(bio, this); BIO_set_init(bio, 1); SSL_set_bio(ssl_, bio, bio); }
    if (ssl_ == nullptr || bio == nullptr ||
        SSL_set_tlsext_host_name(ssl_, host_.c_str()) != 1 ||
        SSL_set1_host(ssl_, host_.c_str()) != 1 ||
        SSL_set_alpn_protos(ssl_, alpn, sizeof(alpn)) != 0)
      return fail("tls_configuration", 0);
    SSL_set_mode(ssl_, SSL_MODE_ENABLE_PARTIAL_WRITE | SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);
    for (;;) {
      if (!detail::service_other_unless_stopped(service_other, stop_requested_))
        return fail("connect_stopped", 0);
      if (now_ns() >= options.deadline_ns) return fail("tls_deadline", 0);
      const int rc = SSL_connect(ssl_);
      if (rc == 1) break;
      const int error = SSL_get_error(ssl_, rc);
      if ((error != SSL_ERROR_WANT_READ && error != SSL_ERROR_WANT_WRITE) ||
          !wait_fd(error == SSL_ERROR_WANT_READ ? POLLIN : POLLOUT, options.deadline_ns, service_other))
        return fail(stop_requested_ && stop_requested_() ? "connect_stopped" : "tls_handshake", error);
    }
    const unsigned char* selected = nullptr;
    unsigned int selected_length = 0;
    SSL_get0_alpn_selected(ssl_, &selected, &selected_length);
    if (SSL_get_verify_result(ssl_) != X509_V_OK || selected_length != 2 ||
        selected == nullptr || selected[0] != 'h' || selected[1] != '2')
      return fail("tls_verification_or_alpn", 0);
    nghttp2_session_callbacks* callbacks = nullptr;
    if (nghttp2_session_callbacks_new(&callbacks) != 0) return fail("http2_callbacks", 0);
    nghttp2_session_callbacks_set_send_callback(callbacks, send_callback);
    nghttp2_session_callbacks_set_recv_callback(callbacks, recv_callback);
    nghttp2_session_callbacks_set_on_frame_recv_callback(callbacks, frame_callback);
    nghttp2_session_callbacks_set_on_frame_send_callback(callbacks, frame_send_callback);
    nghttp2_session_callbacks_set_on_header_callback(callbacks, header_callback);
    nghttp2_session_callbacks_set_on_data_chunk_recv_callback(callbacks, data_callback);
    nghttp2_session_callbacks_set_on_stream_close_callback(callbacks, stream_close_callback);
    const int session_rc = nghttp2_session_client_new(&session_, callbacks, this);
    nghttp2_session_callbacks_del(callbacks);
    if (session_rc != 0) return fail("http2_session", session_rc);
    const nghttp2_settings_entry settings[] = {{NGHTTP2_SETTINGS_MAX_FRAME_SIZE, kMaxFrame},
                                                {NGHTTP2_SETTINGS_ENABLE_PUSH, 0}};
    const int rc = nghttp2_submit_settings(session_, NGHTTP2_FLAG_NONE, settings, 2);
    if (rc != 0) return fail("http2_settings", rc);
    connected_ = true;
    last_receive_ = now_ns();
    emit(Event{.kind = EventKind::Ready, .monotonic_ns = last_receive_});
    return true;
  }

  std::string peer_ip() const override { return peer_; }

  std::uint32_t begin_rpc(std::string_view path, std::string_view bearer,
                 std::span<const std::byte> request, bool streaming,
                 std::uint64_t deadline_ns) override {
    if (!connected_ || goaway_ || stream_ != 0 ||
        !((path == kTradingStatusPath && !streaming) || (path == kMarketStreamPath && streaming) ||
          (path == kUserTariffPath && !streaming && request.empty())) ||
        bearer.empty() || bearer.size() > kMaxFrame || request.size() > kMaxMessage || deadline_ns <= now_ns() ||
        bearer.find_first_of("\0\r\n", 0, 3) != std::string_view::npos)
      return 0;
    request_.resize(request.size() + 5);
    request_[0] = 0;
    const auto size = static_cast<std::uint32_t>(request.size());
    request_[1] = static_cast<std::uint8_t>(size >> 24);
    request_[2] = static_cast<std::uint8_t>(size >> 16);
    request_[3] = static_cast<std::uint8_t>(size >> 8);
    request_[4] = static_cast<std::uint8_t>(size);
    if (!request.empty()) std::memcpy(request_.data() + 5, request.data(), request.size());
    request_offset_ = 0;
    std::string authorization = "Bearer ";
    authorization.append(bearer);
    auto nv = [](std::string_view name, std::string_view value, std::uint8_t flags = NGHTTP2_NV_FLAG_NONE) {
      return nghttp2_nv{reinterpret_cast<std::uint8_t*>(const_cast<char*>(name.data())),
                        reinterpret_cast<std::uint8_t*>(const_cast<char*>(value.data())),
                        name.size(), value.size(), flags};
    };
    std::array<nghttp2_nv, 7> headers = {
        nv(":method", "POST"), nv(":scheme", "https"), nv(":authority", host_),
        nv(":path", path), nv("content-type", "application/grpc"), nv("te", "trailers"),
        nv("authorization", authorization, NGHTTP2_NV_FLAG_NO_INDEX)};
    nghttp2_data_provider provider{};
    provider.source.ptr = this;
    provider.read_callback = request_callback;
    const int32_t id = nghttp2_submit_request(session_, nullptr, headers.data(), headers.size(), &provider, nullptr);
    OPENSSL_cleanse(authorization.data(), authorization.size());
    if (id < 0) {
      clear_request();
      fail("http2_submit_rpc", id);
      return 0;
    }
    stream_ = id;
    rpc_deadline_ = deadline_ns;
    http_present_ = grpc_present_ = false;
    http_status_ = grpc_status_ = 0;
    message_header_size_ = message_size_ = message_used_ = 0;
    return static_cast<std::uint32_t>(id);
  }

  bool begin_extra_ping(std::uint64_t deadline) override {
    if (!connected_ || goaway_ || ping_outstanding_ || extra_ever_ ||
        (stop_requested_ && stop_requested_()) || deadline <= now_ns()) return false;
    extra_ever_ = true;
    ++ping_nonce_;
    for (std::size_t i = 0; i < ping_data_.size(); ++i)
      ping_data_[i] = static_cast<std::uint8_t>(ping_nonce_ >> ((7 - i) * 8));
    ping_sent_ = false;
    ping_extra_ = true;
    if (nghttp2_submit_ping(session_, NGHTTP2_FLAG_NONE, ping_data_.data()) != 0) return false;
    ping_outstanding_ = true;
    ping_deadline_ = deadline;
    return true;
  }

  void poll(std::uint64_t current) override {
    if (!connected_ || (stop_requested_ && stop_requested_())) return;
    read_budget_ = kMaxMessage;
    if (stream_ != 0 && current >= rpc_deadline_) { fail("rpc_deadline", 0); return; }
    if (ping_outstanding_ && current >= ping_deadline_) { fail("ping_deadline", 0); return; }
    if (!ping_outstanding_ && ping_interval_ != 0 && current >= add_ns(last_receive_, ping_interval_)) {
      ++ping_nonce_;
      ping_extra_ = false;
      for (std::size_t i = 0; i < ping_data_.size(); ++i)
        ping_data_[i] = static_cast<std::uint8_t>(ping_nonce_ >> ((7 - i) * 8));
      ping_sent_ = false;
      const int rc = nghttp2_submit_ping(session_, NGHTTP2_FLAG_NONE, ping_data_.data());
      if (rc != 0) { fail("ping_submit", rc); return; }
      ping_outstanding_ = true;
      ping_deadline_ = add_ns(add_ns(last_receive_, ping_interval_), ping_timeout_);
      if (current >= ping_deadline_) { fail("ping_deadline", 0); return; }
    }
    if (read_needs_write_) {
      if (!receive()) return;
      if (read_needs_write_ || (stop_requested_ && stop_requested_())) return;
    }
    if (!send()) return;
    if (write_blocked_) return;
    receive();
  }

  bool open() const override { return connected_; }

  void close() noexcept override {
    connected_ = false;
    ping_outstanding_ = false;
    ping_sent_ = false;
    clear_request();
    if (session_ != nullptr) { nghttp2_session_del(session_); session_ = nullptr; }
    if (ssl_ != nullptr) { SSL_free(ssl_); ssl_ = nullptr; }
    if (ctx_ != nullptr) { SSL_CTX_free(ctx_); ctx_ = nullptr; }
    if (fd_ >= 0) { ::close(fd_); fd_ = -1; }
  }

 private:
  // OpenSSL's default socket BIO uses write(2), which can raise SIGPIPE on a
  // closed peer. This diagnostic must not change the process signal policy.
  static const BIO_METHOD* socket_bio_method() {
    static BIO_METHOD* method = [] {
      BIO_METHOD* m = BIO_meth_new(BIO_TYPE_SOURCE_SINK, "probe socket");
      if (m == nullptr) return m;
      if (BIO_meth_set_write(m, bio_write) != 1 || BIO_meth_set_read(m, bio_read) != 1 ||
          BIO_meth_set_ctrl(m, bio_control) != 1 || BIO_meth_set_create(m, bio_create) != 1 ||
          BIO_meth_set_destroy(m, bio_destroy) != 1) {
        BIO_meth_free(m);
        return static_cast<BIO_METHOD*>(nullptr);
      }
      return m;
    }();
    return method;
  }
  static int bio_create(BIO* bio) { BIO_set_init(bio, 0); BIO_set_data(bio, nullptr); return 1; }
  static int bio_destroy(BIO*) { return 1; }
  static long bio_control(BIO*, int command, long, void*) { return command == BIO_CTRL_FLUSH ? 1 : 0; }
  static int bio_write(BIO* bio, const char* data, int size) {
    BIO_clear_retry_flags(bio);
    auto* self = static_cast<Nghttp2Connection*>(BIO_get_data(bio));
    const auto result = ::send(self->fd_, data, static_cast<std::size_t>(size), MSG_NOSIGNAL);
    if (result < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) BIO_set_retry_write(bio);
    return static_cast<int>(result);
  }
  static int bio_read(BIO* bio, char* data, int size) {
    BIO_clear_retry_flags(bio);
    auto* self = static_cast<Nghttp2Connection*>(BIO_get_data(bio));
    const auto result = ::recv(self->fd_, data, static_cast<std::size_t>(size), 0);
    if (result < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) BIO_set_retry_read(bio);
    return static_cast<int>(result);
  }

  bool wait_fd(short events, std::uint64_t deadline, const std::function<void()>& other) {
    for (;;) {
      if (!detail::service_other_unless_stopped(other, stop_requested_)) return false;
      const auto current = now_ns();
      if (current >= deadline) return false;
      const auto remaining = std::min(deadline - current, kWaitSliceNs);
      const int millis = static_cast<int>((remaining + 999999) / 1000000);
      pollfd pfd{fd_, events, 0};
      const int rc = ::poll(&pfd, 1, millis);
      if (rc > 0) return true;
      if (rc < 0 && errno != EINTR) return false;
    }
  }

  void emit(const Event& event) { if (sink_) sink_(event); }
  bool fail(std::string_view stage, int status) {
    emit(Event{.kind = EventKind::Failure, .monotonic_ns = now_ns(),
               .stream_id = static_cast<std::uint32_t>(stream_), .stage = stage, .local_status = status});
    close();
    return false;
  }
  void clear_request() noexcept {
    if (!request_.empty()) OPENSSL_cleanse(request_.data(), request_.size());
    request_.clear();
    request_offset_ = 0;
  }
  bool send() {
    write_blocked_ = false;
    const int rc = nghttp2_session_send(session_);
    if (callback_failure_) return fail(callback_stage_, callback_status_);
    if (rc != 0) return fail("http2_send", rc);
    return finish_stream();
  }
  bool receive() {
    read_needs_write_ = false;
    const int rc = nghttp2_session_recv(session_);
    if (callback_failure_) return fail(callback_stage_, callback_status_);
    if (rc == NGHTTP2_ERR_EOF) {
      if (!finish_stream()) return false;
      emit(Event{.kind = EventKind::Eof, .monotonic_ns = now_ns(),
                 .stream_id = static_cast<std::uint32_t>(stream_)});
      close();
      return false;
    }
    if (rc != 0) return fail("http2_receive", rc);
    return finish_stream();
  }
  int callback_error(std::string_view stage, int status = 0) {
    callback_failure_ = true;
    callback_stage_ = stage;
    callback_status_ = status;
    return NGHTTP2_ERR_CALLBACK_FAILURE;
  }

  static ssize_t send_callback(nghttp2_session*, const std::uint8_t* data, std::size_t length,
                               int, void* user) {
    auto& self = *static_cast<Nghttp2Connection*>(user);
    const int rc = SSL_write(self.ssl_, data, static_cast<int>(std::min<std::size_t>(length, INT_MAX)));
    if (rc > 0) return rc;
    const int error = SSL_get_error(self.ssl_, rc);
    if (error == SSL_ERROR_WANT_READ || error == SSL_ERROR_WANT_WRITE) {
      self.write_blocked_ = true;
      return NGHTTP2_ERR_WOULDBLOCK;
    }
    return self.callback_error("tls_write", error);
  }
  static ssize_t recv_callback(nghttp2_session*, std::uint8_t* data, std::size_t length,
                               int, void* user) {
    auto& self = *static_cast<Nghttp2Connection*>(user);
    if (self.read_budget_ == 0) return NGHTTP2_ERR_WOULDBLOCK;
    const int rc = SSL_read(self.ssl_, data, static_cast<int>(std::min(length, self.read_budget_)));
    if (rc > 0) {
      self.read_budget_ -= static_cast<std::size_t>(rc);
      detail::record_tls_read(self.frame_based_ping_idle_, now_ns(), self.last_receive_);
      return rc;
    }
    const int error = SSL_get_error(self.ssl_, rc);
    if (error == SSL_ERROR_WANT_READ || error == SSL_ERROR_WANT_WRITE) {
      self.read_needs_write_ = error == SSL_ERROR_WANT_WRITE;
      return NGHTTP2_ERR_WOULDBLOCK;
    }
    if (error == SSL_ERROR_ZERO_RETURN || (error == SSL_ERROR_SYSCALL && rc == 0))
      return NGHTTP2_ERR_EOF;
    return self.callback_error("tls_read", error);
  }
  static ssize_t request_callback(nghttp2_session*, int32_t, std::uint8_t* destination,
                                  std::size_t length, std::uint32_t* flags,
                                  nghttp2_data_source* source, void*) {
    auto& self = *static_cast<Nghttp2Connection*>(source->ptr);
    const auto count = std::min(length, self.request_.size() - self.request_offset_);
    if (count != 0) std::memcpy(destination, self.request_.data() + self.request_offset_, count);
    self.request_offset_ += count;
    if (self.request_offset_ == self.request_.size()) {
      *flags |= NGHTTP2_DATA_FLAG_EOF;
      self.clear_request();
    }
    return static_cast<ssize_t>(count);
  }
  static bool number(const std::uint8_t* data, std::size_t length, std::uint32_t& value) {
    if (length == 0 || length > 10) return false;
    std::uint32_t result = 0;
    for (std::size_t i = 0; i < length; ++i) {
      if (data[i] < '0' || data[i] > '9') return false;
      const unsigned digit = data[i] - '0';
      if (result > (std::numeric_limits<std::uint32_t>::max() - digit) / 10) return false;
      result = result * 10 + digit;
    }
    value = result;
    return true;
  }
  static int header_callback(nghttp2_session*, const nghttp2_frame* frame,
                             const std::uint8_t* name, std::size_t name_length,
                             const std::uint8_t* value, std::size_t value_length,
                             std::uint8_t, void* user) {
    auto& self = *static_cast<Nghttp2Connection*>(user);
    if (frame->hd.stream_id != self.stream_) return 0;
    const std::string_view key(reinterpret_cast<const char*>(name), name_length);
    if (key == ":status") {
      if (self.http_present_ || value_length != 3 || !number(value, value_length, self.http_status_))
        return self.callback_error("http_status");
      self.http_present_ = true;
    } else if (key == "grpc-status") {
      if (self.grpc_present_ || !number(value, value_length, self.grpc_status_))
        return self.callback_error("grpc_status");
      self.grpc_present_ = true;
    }
    return 0;
  }
  static int data_callback(nghttp2_session*, std::uint8_t, int32_t stream,
                           const std::uint8_t* data, std::size_t length, void* user) {
    auto& self = *static_cast<Nghttp2Connection*>(user);
    if (stream != self.stream_) return 0;
    while (length != 0) {
      if (self.message_header_size_ < 5) {
        const auto count = std::min(length, 5 - self.message_header_size_);
        std::memcpy(self.message_header_.data() + self.message_header_size_, data, count);
        self.message_header_size_ += count;
        data += count;
        length -= count;
        if (self.message_header_size_ != 5) continue;
        const auto& h = self.message_header_;
        self.message_size_ = (static_cast<std::uint32_t>(h[1]) << 24) |
                             (static_cast<std::uint32_t>(h[2]) << 16) |
                             (static_cast<std::uint32_t>(h[3]) << 8) | h[4];
        if (h[0] != 0 || self.message_size_ > kMaxMessage)
          return self.callback_error("grpc_message_size_or_encoding");
      }
      const auto count = std::min(length, self.message_size_ - self.message_used_);
      if (count != 0) std::memcpy(self.message_.data() + self.message_used_, data, count);
      self.message_used_ += count;
      data += count;
      length -= count;
      if (self.message_used_ == self.message_size_) {
        self.emit(Event{.kind = EventKind::Message, .monotonic_ns = now_ns(),
                        .stream_id = static_cast<std::uint32_t>(stream),
                        .payload = std::span<const std::byte>(self.message_.data(), self.message_size_)});
        self.message_header_size_ = self.message_size_ = self.message_used_ = 0;
      }
    }
    return 0;
  }
  static int frame_send_callback(nghttp2_session*, const nghttp2_frame* frame, void* user) {
    auto& self = *static_cast<Nghttp2Connection*>(user);
    // nghttp2 finished handing the frame to SSL_write; this does not prove
    // that the peer received it.
    if (frame->hd.type == NGHTTP2_PING && (frame->hd.flags & NGHTTP2_FLAG_ACK) == 0 &&
        self.ping_outstanding_ &&
        std::equal(self.ping_data_.begin(), self.ping_data_.end(), frame->ping.opaque_data))
    {
      self.ping_sent_ = true;
      if (self.report_ping_sends_)
        self.emit(Event{.kind = EventKind::PingSent, .monotonic_ns = now_ns(),
                        .extra_ping = self.ping_extra_});
    }
    return 0;
  }
  static int frame_callback(nghttp2_session*, const nghttp2_frame* frame, void* user) {
    auto& self = *static_cast<Nghttp2Connection*>(user);
    const auto received = now_ns();
    const bool ping_ack = frame->hd.type == NGHTTP2_PING && (frame->hd.flags & NGHTTP2_FLAG_ACK) != 0;
    if (!ping_ack) detail::record_http2_frame(self.frame_based_ping_idle_, false, false, received, self.last_receive_);
    if (ping_ack) {
      const bool nonce_matches = self.ping_outstanding_ && self.ping_sent_ &&
          std::equal(self.ping_data_.begin(), self.ping_data_.end(), frame->ping.opaque_data);
      const bool matched = nonce_matches && received < self.ping_deadline_;
      detail::record_http2_frame(self.frame_based_ping_idle_, true, matched, received, self.last_receive_);
      self.emit(Event{.kind = EventKind::PingAck, .monotonic_ns = received,
                      .ping_ack_match_known = true, .ping_ack_matched = matched,
                      .extra_ping = self.ping_extra_});
      if (nonce_matches && !matched) return self.callback_error("ping_deadline");
      if (matched) {
        self.ping_outstanding_ = false;
      }
    } else if (frame->hd.type == NGHTTP2_GOAWAY) {
      self.goaway_ = true;
      self.emit(Event{.kind = EventKind::Goaway, .monotonic_ns = now_ns(),
                      .last_stream_id = static_cast<std::uint32_t>(frame->goaway.last_stream_id),
                      .http2_error = frame->goaway.error_code,
                      .debug_bytes = frame->goaway.opaque_data_len});
    } else if (frame->hd.type == NGHTTP2_HEADERS && frame->hd.stream_id == self.stream_) {
      self.emit(Event{.kind = EventKind::Headers, .monotonic_ns = now_ns(),
                      .stream_id = static_cast<std::uint32_t>(self.stream_),
                      .http_status = self.http_status_, .grpc_status = self.grpc_status_,
                      .http_status_present = self.http_present_, .grpc_status_present = self.grpc_present_});
      if ((frame->hd.flags & NGHTTP2_FLAG_END_STREAM) != 0) self.end_stream_ = true;
    } else if (frame->hd.type == NGHTTP2_DATA && frame->hd.stream_id == self.stream_ &&
               (frame->hd.flags & NGHTTP2_FLAG_END_STREAM) != 0) {
      self.end_stream_ = true;
    } else if (frame->hd.type == NGHTTP2_RST_STREAM && frame->hd.stream_id == self.stream_) {
      self.reset_seen_ = true;
      self.emit(Event{.kind = EventKind::StreamReset, .monotonic_ns = now_ns(),
                      .stream_id = static_cast<std::uint32_t>(self.stream_),
                      .http2_error = frame->rst_stream.error_code});
    }
    return 0;
  }
  static int stream_close_callback(nghttp2_session*, int32_t stream, std::uint32_t error, void* user) {
    auto& self = *static_cast<Nghttp2Connection*>(user);
    if (stream == self.stream_) {
      self.stream_closed_ = true;
      self.stream_error_ = error;
    }
    return 0;
  }
  bool finish_stream() {
    if (!stream_closed_) return true;
    stream_closed_ = false;
    if (!reset_seen_ && stream_error_ != NGHTTP2_NO_ERROR) {
      emit(Event{.kind = EventKind::StreamReset, .monotonic_ns = now_ns(),
                 .stream_id = static_cast<std::uint32_t>(stream_), .http2_error = stream_error_});
    } else if (!reset_seen_) {
      if (!end_stream_ || message_header_size_ != 0) return fail("grpc_truncated", 0);
      emit(Event{.kind = EventKind::RpcComplete, .monotonic_ns = now_ns(),
                 .stream_id = static_cast<std::uint32_t>(stream_),
                 .http_status = http_status_, .grpc_status = grpc_status_,
                 .http_status_present = http_present_, .grpc_status_present = grpc_present_});
    }
    stream_ = 0;
    end_stream_ = reset_seen_ = false;
    clear_request();
    return true;
  }

  EventSink sink_;
  std::function<bool()> stop_requested_;
  SSL_CTX* ctx_{};
  SSL* ssl_{};
  nghttp2_session* session_{};
  int fd_{-1};
  std::string host_;
  std::string peer_;
  bool used_{};
  bool connected_{};
  bool goaway_{};
  bool write_blocked_{};
  bool read_needs_write_{};
  std::size_t read_budget_{};
  bool callback_failure_{};
  std::string_view callback_stage_;
  int callback_status_{};
  int32_t stream_{};
  std::uint64_t rpc_deadline_{};
  bool end_stream_{};
  bool stream_closed_{};
  std::uint32_t stream_error_{};
  bool reset_seen_{};
  bool http_present_{};
  bool grpc_present_{};
  std::uint32_t http_status_{};
  std::uint32_t grpc_status_{};
  std::vector<std::uint8_t> request_;
  std::size_t request_offset_{};
  std::array<std::uint8_t, 5> message_header_{};
  std::array<std::byte, kMaxMessage> message_{};
  std::size_t message_header_size_{};
  std::size_t message_size_{};
  std::size_t message_used_{};
  std::uint64_t last_receive_{};
  std::uint64_t ping_interval_{};
  std::uint64_t ping_timeout_{};
  std::uint64_t ping_deadline_{};
  std::uint64_t ping_nonce_{};
  std::array<std::uint8_t, 8> ping_data_{};
  bool ping_outstanding_{};
  bool ping_sent_{};
  bool ping_extra_{};
  bool extra_ever_{};
  bool report_ping_sends_{};
  bool frame_based_ping_idle_{};
};
}  // namespace

std::unique_ptr<Connection> make_nghttp2_connection() {
  return std::make_unique<Nghttp2Connection>();
}
}  // namespace exchange_probe::tbank_grpc
