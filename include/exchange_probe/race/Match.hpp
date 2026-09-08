#pragma once

#include "exchange_probe/race/Types.hpp"

#include <cstdint>

namespace exchange_probe::race {

enum class TradeMatchKind : std::uint8_t {
  None = 0u,
  ExactSingle = 1u,
  ExactAggregate = 2u,
  AggregateContainsSingle = 3u,
};

struct BboTransition {
  BboState before{};
  BboState after{};
  std::uint64_t sequence{0u};
  std::uint8_t changeMask{0u};
};

[[nodiscard]] TradeMatchKind match_trade_identity(
    const TradeIdentity& lhs, const TradeIdentity& rhs) noexcept;
[[nodiscard]] std::uint8_t bbo_change_mask(
    const BboState& before, const BboState& after) noexcept;
[[nodiscard]] bool same_bbo_transition(
    const BboTransition& lhs, const BboTransition& rhs) noexcept;
[[nodiscard]] std::uint64_t fingerprint_seed() noexcept;
[[nodiscard]] std::uint64_t fingerprint_value(
    std::uint64_t current, std::int64_t value) noexcept;

}  // namespace exchange_probe::race
