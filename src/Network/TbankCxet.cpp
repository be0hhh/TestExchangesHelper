#include "exchange_probe/TbankGrpcDiagnostic.hpp"

#include "cxet/Primitives/Buf/SecureWipe.hpp"
#include "cxet/Network/Grpc/GrpcClient.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <limits>
#include <string>
#include <utility>

#include <arpa/inet.h>
#include <poll.h>
#include <sys/socket.h>

namespace exchange_probe::tbank_grpc {
namespace {

namespace grpc = cxet::network::grpc;
using Status = grpc::GrpcClientStatus;
constexpr std::string_view kHost = "invest-public-api.tbank.ru";
constexpr std::uint64_t kConnectTimeoutNs = 5'000'000'000ull;
constexpr std::uint32_t kMessageBytes = 65'536u;
constexpr std::size_t kAuthorizationBytes = 8192u;

std::uint64_t now_ns() noexcept {
  const auto value = std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count();
  return value > 0 ? static_cast<std::uint64_t>(value) : 0u;
}

std::uint64_t connect_deadline(std::uint64_t now, std::uint64_t requested) noexcept {
  if (now == 0u || now > std::numeric_limits<std::uint64_t>::max() - kConnectTimeoutNs)
    return 0u;
  const auto bound = now + kConnectTimeoutNs;
  return requested == 0u ? bound : std::min(bound, requested);
}

class CxetConnection final : public Connection {
 public:
  ~CxetConnection() override { close(); }

  bool connect(const ConnectOptions& options, EventSink sink,
               const std::function<void()>& service_other) override {
    sink_ = std::move(sink);
    if (attempted_) return failure("connect_input", Status::InvalidInput);
    attempted_ = true;
    if (options.host != kHost || !options.pinned_ip.empty() || options.ca_file.empty() ||
        options.ca_file.find('\0') != std::string_view::npos || options.port != 443u ||
        (options.ping_interval_ns != 0u &&
         (options.ping_interval_ns != 10'000'000'000ull ||
          options.ping_ack_timeout_ns != 2'000'000'000ull)))
      return failure("connect_input", Status::InvalidInput);

    host_.assign(options.host);
    ca_file_.assign(options.ca_file);
    const auto start = now_ns();
    const auto deadline = connect_deadline(start, options.deadline_ns);
    if (deadline == 0u || start >= deadline)
      return failure("connect_deadline", Status::DeadlineExpired);
    const grpc::GrpcClientConfig config{
        .host = host_.c_str(), .serverName = host_.c_str(), .caFile = ca_file_.c_str(),
        .port = options.port, .maximumStreams = 4u, .maximumFrameBytes = 16'384u,
        .maximumMessageBytes = kMessageBytes, .maximumRequestBytes = kMessageBytes,
        .receiveBufferBytes = 16'384u + 9u,
        .sendBufferBytes = grpc::GrpcClient::minimumStreamOpenBufferBytes(kMessageBytes),
        .connectTimeoutNs = kConnectTimeoutNs, .ioTimeoutNs = kConnectTimeoutNs,
        .peerProbeIntervalNs = options.ping_interval_ns,
        .peerProbeAckTimeoutNs = options.ping_interval_ns ? options.ping_ack_timeout_ns : 0u};
    if (!resolver_.initialize({})) return failure("resolver_init", Status::ResolveFailed);
    if (!client_.initialize(config)) return failure("initialize", client_.status());

    if (service_other) service_other();
    auto status = client_.prepareConnectCold(resolver_, start, deadline);
    if (service_other) service_other();
    if (status != Status::WouldBlock && status != Status::Ready)
      return failure("connect_prepare", status);
    for (;;) {
      const auto now = now_ns();
      if (now == 0u || now >= deadline) {
        client_.close();
        return failure("connect_deadline", Status::DeadlineExpired);
      }
      status = client_.progressConnectCold(now);
      if (status != Status::WouldBlock && status != Status::Ready) {
        client_.close();
        return failure("connect_tls", status);
      }
      status = client_.progressConnectOwner(now);
      if (status == Status::Ready) break;
      if (status != Status::WouldBlock) {
        client_.close();
        return failure("connect", status);
      }
      if (service_other) service_other();
      // The native progress operations are nonblocking; give the peer and TLS
      // socket a short opportunity to advance without starving another client.
      (void)::poll(nullptr, 0, 1);
    }
    if (!read_peer_ip()) {
      client_.close();
      return failure("peer_address", Status::IoFailed);
    }
    open_ = true;
    emit({.kind = EventKind::Ready, .monotonic_ns = now_ns()});
    return true;
  }

  std::string peer_ip() const override { return peer_ip_; }

  std::uint32_t begin_rpc(std::string_view path, std::string_view bearer,
                 std::span<const std::byte> request, bool streaming,
                 std::uint64_t deadline_ns) override {
    if (!open_ || goaway_ || !((path == kMarketStreamPath && streaming) ||
        (path == kTradingStatusPath && !streaming) ||
        (path == kUserTariffPath && !streaming && request.empty())) ||
        bearer.empty() || bearer.size() > kAuthorizationBytes - 7u ||
        request.size() > kMessageBytes || !client_.canOpenRequestStream())
      return 0u;
    std::array<char, kAuthorizationBytes> authorization{};
    ::cxet::bytescan::SecureWipe wipe(authorization);
    constexpr std::string_view prefix = "Bearer ";
    std::memcpy(authorization.data(), prefix.data(), prefix.size());
    std::memcpy(authorization.data() + prefix.size(), bearer.data(), bearer.size());
    const auto stream = client_.beginServerStream(path.data(), path.size(),
        authorization.data(), prefix.size() + bearer.size(), request.data(), request.size(),
        deadline_ns);
    if (stream == 0u) {
      const auto status = client_.status();
      if (status != Status::WouldBlock) failure("begin_rpc", status);
      return 0u;
    }
    return stream;
  }

  void poll(std::uint64_t now) override {
    if (!open_) return;
    grpc::GrpcClientEvent native{};
    const auto status = client_.poll(&native, true, now);
    if (status != Status::Ready && status != Status::WouldBlock) {
      open_ = false;
      if (status == Status::PeerClosed)
        emit({.kind = EventKind::Eof, .monotonic_ns = now_ns(),
              .stage = "poll", .local_status = static_cast<int>(status)});
      else
        failure("poll", status);
      return;
    }
    if (native.kind == grpc::GrpcClientEventKind::None) return;
    Event event{.monotonic_ns = native.receivedMonotonicNs ? native.receivedMonotonicNs : now_ns(),
                .stream_id = native.streamId,
                .last_stream_id = native.goawayLastStreamId,
                .http2_error = native.errorCode,
                .http_status = native.httpStatus,
                .grpc_status = native.grpcStatus,
                .http_status_present = native.httpStatusPresent,
                .grpc_status_present = native.grpcStatusPresent};
    std::array<std::byte, kMessageBytes> copied{};
    switch (native.kind) {
      case grpc::GrpcClientEventKind::PingAck: event.kind = EventKind::PingAck; break;
      case grpc::GrpcClientEventKind::Goaway:
        goaway_ = true;
        event.kind = EventKind::Goaway;
        event.debug_bytes = native.payloadSize;
        break;
      case grpc::GrpcClientEventKind::Headers: event.kind = EventKind::Headers; break;
      case grpc::GrpcClientEventKind::Message:
        if (native.payloadSize > copied.size() || (native.payloadSize && !native.payload)) {
          open_ = false;
          failure("payload", Status::CapacityExceeded);
          return;
        }
        if (native.payloadSize) std::memcpy(copied.data(), native.payload, native.payloadSize);
        event.kind = EventKind::Message;
        event.payload = {copied.data(), native.payloadSize};
        break;
      case grpc::GrpcClientEventKind::Trailers: event.kind = EventKind::RpcComplete; break;
      case grpc::GrpcClientEventKind::StreamReset: event.kind = EventKind::StreamReset; break;
      case grpc::GrpcClientEventKind::None: return;
    }
    if (native.terminal && !client_.completeStream(native.streamId)) {
      open_ = false;
      failure("complete_stream", Status::ProtocolFailed);
      return;
    }
    emit(event);
    // A prepared unary can encode a terminal Message. beginServerStream uses
    // streaming assembly today, but preserve both event and completion if Core
    // returns such a terminal message in a future compatible implementation.
    if (native.kind == grpc::GrpcClientEventKind::Message && native.terminal) {
      event.kind = EventKind::RpcComplete;
      event.payload = {};
      emit(event);
    }
  }

  bool open() const override { return open_; }
  void close() noexcept override {
    open_ = false;
    client_.close();
  }

 private:
  void emit(const Event& event) { if (sink_) sink_(event); }
  bool failure(std::string_view stage, Status status) {
    emit({.kind = EventKind::Failure, .monotonic_ns = now_ns(),
          .stage = stage, .local_status = static_cast<int>(status)});
    return false;
  }
  bool read_peer_ip() {
    sockaddr_storage address{};
    socklen_t length = sizeof(address);
    if (::getpeername(client_.nativeFd(), reinterpret_cast<sockaddr*>(&address), &length) != 0)
      return false;
    char text[INET6_ADDRSTRLEN]{};
    const void* bytes = nullptr;
    if (address.ss_family == AF_INET)
      bytes = &reinterpret_cast<const sockaddr_in*>(&address)->sin_addr;
    else if (address.ss_family == AF_INET6)
      bytes = &reinterpret_cast<const sockaddr_in6*>(&address)->sin6_addr;
    if (!bytes || !::inet_ntop(address.ss_family, bytes, text, sizeof(text))) return false;
    peer_ip_ = text;
    return true;
  }

  std::string host_;
  std::string ca_file_;
  std::string peer_ip_;
  EventSink sink_;
  grpc::BoundedDnsResolver resolver_;
  grpc::GrpcClient client_;
  bool attempted_{};
  bool open_{};
  bool goaway_{};
};

}  // namespace

std::unique_ptr<Connection> make_cxet_connection() {
  return std::make_unique<CxetConnection>();
}

}  // namespace exchange_probe::tbank_grpc
