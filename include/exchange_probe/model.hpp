#pragma once

#include <boost/json/value.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace exchange_probe {

inline constexpr std::size_t kMaxHttpBodyBytes = 32U * 1024U * 1024U;
inline constexpr std::size_t kMaxWsMessageBytes = 8U * 1024U * 1024U;
inline constexpr std::size_t kMaxHttpHeadBytes = 64U * 1024U;
inline constexpr std::size_t kMaxRawPublicBytes = 4U * 1024U;
inline constexpr std::size_t kMaxSandboxBytes = 1U * 1024U * 1024U;
inline constexpr std::uint64_t kDefaultArtifactBytes =
    256ULL * 1024ULL * 1024ULL;

enum class Surface {
  Public,
  Private,
};

enum class Transport {
  None,
  Rest,
  WebSocket,
  Fix,
};

enum class Wire {
  None,
  Json,
  BinaryJson,
  Sbe,
  Protobuf,
  FixSbe,
};

enum class Selection {
  CoreSelected,
  DiagnosticVariant,
  ExternalAdapterRequired,
};

enum class ProfileStatus {
  Profiled,
  NotProfiled,
  NotApplicable,
  ExternalAdapterRequired,
};

enum class CoreStatus {
  NotChecked,
  Confirmed,
  Mismatch,
  NotObserved,
};

enum class Outcome {
  NotRun,
  Success,
  ExpectedRejection,
  TransportError,
  TlsError,
  ProxyError,
  HttpError,
  LogicalError,
  SchemaError,
  Timeout,
  CapacityExceeded,
  Unsupported,
  ConfigurationError,
  InternalError,
};

enum class Expectation {
  LogicalSuccess,
  SymbolRejected,
};

enum class RestContract {
  None,
  BinanceTicker24h,
  BinanceFunding,
  BybitTicker24h,
  BybitFunding,
  OkxTicker24h,
  OkxFunding,
  GateTicker24h,
  GateFunding,
  KucoinTicker24h,
  KucoinFunding,
  BitgetTicker24h,
  BitgetFunding,
  BinancePrivate,
  BybitPrivate,
  OkxPrivate,
  GatePrivate,
  KucoinPrivate,
  BitgetPrivate,
};

enum class AuthKind {
  None,
  BinanceHmac,
  BybitHmac,
  OkxHmac,
  GateHmac,
  KucoinHmac,
  BitgetHmac,
};

enum class WsAckKind {
  None,
  KeysOnly,
  BinanceSubscription,
  BybitSubscription,
  OkxSubscription,
  GateSubscription,
  BitgetSubscription,
  KucoinSubscription,
  PhemexSubscription,
  HyperliquidSubscription,
  BingxSubscription,
};

enum class WsDataKind {
  AnyJson,
  TopicJson,
  KucoinBinaryJson,
  SbeHeader,
  ProtobufEnvelope,
  BingxTrades,
};

enum class Compression {
  None,
  Gzip,
};

struct SourceAnchor {
  std::string path;
  std::string symbol;
  std::vector<std::string> literals;
};

struct RestCase {
  std::string name;
  std::string host;
  std::string path;
  std::vector<std::string> capabilities;
  Selection selection{Selection::DiagnosticVariant};
  RestContract contract{RestContract::None};
  AuthKind auth{AuthKind::None};
  Expectation expectation{Expectation::LogicalSuccess};
  std::string expected_symbol;
  std::vector<unsigned> expected_rejection_statuses;
  std::vector<std::string> expected_rejection_codes;
  bool private_case{false};
  bool native{true};
  SourceAnchor core_anchor;
  SourceAnchor parser_anchor;
};

struct WsCase {
  std::string name;
  std::string host;
  std::string path{"/"};
  std::string subscribe;
  std::vector<std::string> capabilities;
  Selection selection{Selection::DiagnosticVariant};
  Wire wire{Wire::Json};
  WsAckKind ack_kind{WsAckKind::None};
  WsDataKind data_kind{WsDataKind::AnyJson};
  Compression compression{Compression::None};
  std::map<std::string, std::string> handshake_headers;
  std::string expected_symbol;
  std::string expected_topic;
  std::string expected_request_id;
  std::uint16_t expected_sbe_schema{0};
  std::vector<std::uint16_t> expected_sbe_templates;
  bool subscribe_binary{false};
  bool inbound_binary{false};
  bool read_welcome{false};
  bool require_ack{false};
  bool require_data{true};
  bool data_implies_ack{false};
  bool ack_implies_data{false};
  std::string application_heartbeat;
  SourceAnchor core_anchor;
  SourceAnchor parser_anchor;
};

struct FixCase {
  std::string name;
  std::string host;
  std::uint16_t port{0};
  std::string session_kind;
  std::string capability;
  SourceAnchor core_anchor;
  SourceAnchor payload_anchor;
};

struct CapabilityRow {
  std::string name;
  Surface surface{Surface::Public};
  Transport transport{Transport::None};
  Wire wire{Wire::None};
  Selection selection{Selection::DiagnosticVariant};
  ProfileStatus profile_status{ProfileStatus::NotProfiled};
  CoreStatus core_status{CoreStatus::NotChecked};
  bool requires_confirmation{false};
  bool native{false};
};

struct ProductSpec {
  std::string venue;
  std::string product;
  std::string credential_prefix;
  std::vector<RestCase> public_rest;
  std::vector<RestCase> private_rest;
  std::vector<WsCase> public_ws;
  std::vector<FixCase> fix_sessions;
  std::vector<CapabilityRow> capabilities;
  std::vector<std::string> notes;
  SourceAnchor credential_anchor;
};

struct ContractEvidence {
  bool logical_success{false};
  bool schema_success{false};
  bool symbol_success{false};
  std::size_t row_count{0};
  std::string contract;
  std::string native_symbol;
  std::string api_code;
  std::string units;
  std::vector<std::string> missing_fields;
  std::string error;
};

struct StageTimings {
  std::uint64_t dns_us{0};
  std::uint64_t tcp_connect_us{0};
  std::uint64_t proxy_connect_us{0};
  std::uint64_t tls_handshake_us{0};
  std::uint64_t request_write_us{0};
  std::uint64_t ttfb_us{0};
  std::uint64_t body_read_us{0};
  std::uint64_t json_parse_us{0};
  std::uint64_t ws_handshake_us{0};
  std::uint64_t welcome_us{0};
  std::uint64_t subscribe_write_us{0};
  std::uint64_t ack_us{0};
  std::uint64_t first_data_us{0};
  std::uint64_t total_us{0};
};

struct TransportMetadata {
  std::string remote_ip;
  std::string ip_family;
  std::string tls_version;
  std::string tls_cipher;
  std::string alpn;
  std::string certificate_not_after;
  std::uint64_t tcp_rtt_us{0};
  std::uint64_t tcp_rtt_variance_us{0};
  std::uint64_t tcp_retransmits{0};
  std::uint64_t tcp_congestion_window{0};
  std::uint64_t tcp_mss{0};
  bool tcp_info_available{false};
  bool tls_session_reused{false};
};

struct HttpResult {
  unsigned status{0};
  std::uint64_t elapsed_ms{0};
  std::size_t body_bytes{0};
  boost::json::value json;
  bool json_present{false};
  bool tls_verified{false};
  StageTimings timings;
  TransportMetadata transport;
  std::string stage;
  std::string error;
};

struct WsResult {
  std::uint64_t elapsed_ms{0};
  std::size_t payload_bytes{0};
  bool connected{false};
  bool tls_verified{false};
  bool ack_complete{false};
  bool data_complete{false};
  bool binary{false};
  unsigned control_pings{0};
  StageTimings timings;
  TransportMetadata transport;
  std::string protocol_stage;
  std::string payload;
  boost::json::value json;
  bool json_present{false};
  std::string error;
};

struct Observation {
  std::string kind;
  std::string venue;
  std::string product;
  std::string case_name;
  std::string capability;
  Surface surface{Surface::Public};
  Transport transport{Transport::None};
  Wire wire{Wire::None};
  Selection selection{Selection::DiagnosticVariant};
  Expectation expectation{Expectation::LogicalSuccess};
  Outcome outcome{Outcome::NotRun};
  bool expectation_met{false};
  bool transport_ok{false};
  bool tls_ok{false};
  bool http_ok{false};
  bool logical_ok{false};
  bool schema_ok{false};
  unsigned http_status{0};
  std::uint64_t elapsed_ms{0};
  std::size_t payload_bytes{0};
  unsigned attempts_allowed{1};
  unsigned attempts_used{0};
  StageTimings timings;
  TransportMetadata transport_metadata;
  std::string stage;
  std::string error;
  boost::json::value evidence;
  std::optional<std::string> raw_public;
};

struct ProxyConfig {
  bool enabled{false};
  bool valid{true};
  std::string host;
  std::string port;
  std::string authorization;
  std::string error;
};

struct RunLimits {
  std::chrono::milliseconds timeout{10'000};
  unsigned attempts{1};
  bool raw_public{false};
};

[[nodiscard]] std::string_view to_string(Surface value) noexcept;
[[nodiscard]] std::string_view to_string(Transport value) noexcept;
[[nodiscard]] std::string_view to_string(Wire value) noexcept;
[[nodiscard]] std::string_view to_string(Selection value) noexcept;
[[nodiscard]] std::string_view to_string(ProfileStatus value) noexcept;
[[nodiscard]] std::string_view to_string(CoreStatus value) noexcept;
[[nodiscard]] std::string_view to_string(Outcome value) noexcept;
[[nodiscard]] std::string_view to_string(Expectation value) noexcept;
[[nodiscard]] std::string_view to_string(RestContract value) noexcept;

[[nodiscard]] std::vector<ProductSpec> make_profiles();
void finalize_capabilities(ProductSpec& product);

}  // namespace exchange_probe
