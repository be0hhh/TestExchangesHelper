#pragma once

#include "exchange_probe/TbankGrpcDiagnostic.hpp"

#include <cstddef>
#include <array>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace exchange_probe::tbank_grpc {
[[nodiscard]] bool valid_uid(std::string_view uid) noexcept;
// Invalid UIDs produce an empty body. These bodies exclude the gRPC frame prefix.
[[nodiscard]] std::vector<std::byte> trading_status_request(std::string_view uid);
[[nodiscard]] std::vector<std::byte> book_stream_request(std::string_view uid);
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
[[nodiscard]] bool inspect_trading_status(std::span<const std::byte> body) noexcept;
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
