#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>

namespace exchange_probe::tbank_grpc {

inline constexpr std::string_view kTradingStatusPath =
    "/tinkoff.public.invest.api.contract.v1.MarketDataService/GetTradingStatuses";
inline constexpr std::string_view kUserTariffPath =
    "/tinkoff.public.invest.api.contract.v1.UsersService/GetUserTariff";
inline constexpr std::string_view kMarketStreamPath =
    "/tinkoff.public.invest.api.contract.v1.MarketDataStreamService/MarketDataServerSideStream";
inline constexpr std::string_view kSberUid = "e6123145-9665-43e0-8413-cd61b8aa9b13";

// Diagnostic-only contract. Payload is borrowed and is valid only during the
// sink call; neither credentials nor raw protocol payloads belong in reports.
enum class EventKind {
  Ready,
  PingSent,
  PingAck,
  Goaway,
  Headers,
  Message,
  RpcComplete,
  StreamReset,
  Eof,
  Failure,
};

struct Event {
  EventKind kind{EventKind::Failure};
  std::uint64_t monotonic_ns{};
  std::uint32_t stream_id{};
  std::uint32_t last_stream_id{};
  std::uint32_t http2_error{};
  std::uint32_t http_status{};
  std::uint32_t grpc_status{};
  bool http_status_present{};
  bool grpc_status_present{};
  // CXET exposes received ACK events, but not whether its private probe nonce
  // matched. Never report that inference as a confirmed match.
  bool ping_ack_match_known{};
  bool ping_ack_matched{};
  bool extra_ping{};
  std::size_t debug_bytes{};
  std::span<const std::byte> payload{};
  // Fixed local stage name, never a remote message or a credential.
  std::string_view stage{};
  int local_status{};
};

using EventSink = std::function<void(const Event&)>;

struct ConnectOptions {
  std::string_view host;
  std::string_view pinned_ip;  // Empty for CXET; nghttp2 requires a numeric IP.
  std::string_view ca_file;
  std::uint16_t port{443};
  std::uint64_t deadline_ns{};
  std::uint64_t ping_interval_ns{};
  std::uint64_t ping_ack_timeout_ns{};
  // Diagnostic owner may fence further queued writes when a peer fails.
  std::function<bool()> stop_requested;
  bool report_ping_sends{};
  // Handoff experiment distinguishes complete frames from fragmented reads.
  bool frame_based_ping_idle{};
};

// All operations for one instance use one owner thread. During bounded socket
// waits connect() invokes service_other so an already connected peer progresses.
// EventSink must not re-enter a Connection: callbacks can originate in nghttp2.
class Connection {
 public:
  virtual ~Connection() = default;
  virtual bool connect(const ConnectOptions&, EventSink,
                       const std::function<void()>& service_other) = 0;
  [[nodiscard]] virtual std::string peer_ip() const = 0;
  // bearer is the raw token; implementations create the Bearer metadata.
  // Returns the exact HTTP/2 stream ID, or zero on refusal.
  virtual std::uint32_t begin_rpc(std::string_view path, std::string_view bearer,
                                  std::span<const std::byte> request, bool streaming,
                                  std::uint64_t deadline_ns) = 0;
  // Diagnostic-only, at most one extra PING on this physical connection.
  // True means queued, not handed to TLS or delivered to the peer.
  virtual bool begin_extra_ping(std::uint64_t) { return false; }
  virtual void poll(std::uint64_t now_ns) = 0;
  [[nodiscard]] virtual bool open() const = 0;
  virtual void close() noexcept = 0;
};

using Factory = std::unique_ptr<Connection> (*)();
struct Factories {
  Factory nghttp2{};
  Factory cxet{};
};

[[nodiscard]] std::unique_ptr<Connection> make_nghttp2_connection();
// Defined only in the root-graph executable and injected through Factories.
[[nodiscard]] std::unique_ptr<Connection> make_cxet_connection();

}  // namespace exchange_probe::tbank_grpc
