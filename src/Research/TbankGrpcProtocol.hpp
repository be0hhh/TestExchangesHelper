#pragma once

#include "exchange_probe/TbankGrpcDiagnostic.hpp"

#include <cstddef>
#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>
#include <boost/json/object.hpp>

namespace exchange_probe::tbank_grpc {
[[nodiscard]] bool valid_uid(std::string_view uid) noexcept;
// Invalid UIDs produce an empty body. These bodies exclude the gRPC frame prefix.
[[nodiscard]] std::vector<std::byte> trading_status_request(std::string_view uid);
[[nodiscard]] std::vector<std::byte> book_stream_request(std::string_view uid);
// Diagnostic source observations never admit production market data.
enum class MarketKind { Book, Trades, Other };
enum class MarketDecode { Other, Ack, Data, MalformedSyntax, SchemaAnomaly };
struct MarketEnvelope {
  MarketKind kind{MarketKind::Other};
  MarketDecode result{MarketDecode::Other};
  std::span<const std::byte> payload{};
};
struct MarketObservation {
  MarketKind kind{MarketKind::Other};
  MarketDecode result{MarketDecode::Other};
  std::string_view uid; // borrowed, valid only with input body
  std::uint64_t status{}, source{}, depth{};
  bool source_present{}, status_present{}, depth_present{};
  boost::json::object details;
};
[[nodiscard]] std::vector<std::byte> combined_market_request(std::string_view uid, unsigned source);
[[nodiscard]] MarketEnvelope market_envelope(std::span<const std::byte>) noexcept;
[[nodiscard]] MarketObservation inspect_market_source(const MarketEnvelope&, bool include_details = true);
struct InstrumentObservation {
  std::string uid, ticker, class_code, exchange;
};
[[nodiscard]] std::vector<std::byte> instrument_request(std::string_view uid);
[[nodiscard]] std::vector<std::byte> schedule_request(std::string_view section,
    std::uint64_t from_realtime_ns, std::uint64_t to_realtime_ns);
[[nodiscard]] bool inspect_instrument(std::span<const std::byte>, std::string_view expected_uid,
    InstrumentObservation&, boost::json::object& details);
[[nodiscard]] bool inspect_trading_schedule(std::span<const std::byte>,
    std::string_view expected_section, boost::json::object& details);
enum class AckResult { NotAck, Accepted, Rejected, Malformed };
[[nodiscard]] AckResult inspect_book_ack(std::span<const std::byte> body,
                                         std::string_view expected_uid) noexcept;

struct TariffGroup {
  std::uint32_t minute{};
  std::uint32_t second{};
  bool trading_status{};
  bool user_tariff{};
};
struct TariffSummary {
  std::array<TariffGroup, 128> groups{};
  std::size_t count{};
};
// Limits are ceilings, not observed usage or available headroom. Only groups
// covering one of these exact methods are retained, without method strings.
[[nodiscard]] bool inspect_user_tariff(std::span<const std::byte> body,
                                       TariffSummary& summary) noexcept;
[[nodiscard]] bool tariff_covers_experiment(const TariffSummary& summary) noexcept;
[[nodiscard]] bool inspect_trading_status(std::span<const std::byte> body,
    std::string_view expected_uid = kSberUid, boost::json::object* details = nullptr);
// Once sent, wait a full period from that send; missed slots are never replayed.
[[nodiscard]] std::uint64_t next_lifetime_slot(std::uint64_t first, std::uint64_t now) noexcept;
// Never service a second queued connection after the first reports a stop.
void poll_lifetime_connections(const std::array<Connection*, 2>& connections,
                               const std::function<bool()>& stopped, std::uint64_t now);
[[nodiscard]] bool lifetime_horizon_complete(std::uint64_t now, std::uint64_t end,
                                            const std::array<std::uint64_t, 2>& next,
                                            const std::array<std::uint64_t, 2>& completed) noexcept;
[[nodiscard]] bool lifetime_slot_expired(std::uint64_t now, std::uint64_t end,
                                         const std::array<std::uint64_t, 2>& next) noexcept;
} // namespace exchange_probe::tbank_grpc
