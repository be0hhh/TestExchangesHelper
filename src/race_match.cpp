#include "exchange_probe/race/match.hpp"

namespace exchange_probe::race {

TradeMatchKind match_trade_identity(
    const TradeIdentity& lhs, const TradeIdentity& rhs) noexcept {
  if (lhs.nativeShape != 0u && rhs.nativeShape != 0u) {
    return lhs.nativeShape == rhs.nativeShape &&
                   lhs.nativeFirst == rhs.nativeFirst &&
                   lhs.nativeSecond == rhs.nativeSecond
               ? TradeMatchKind::ExactSingle
               : TradeMatchKind::None;
  }
  if (lhs.tradeId != 0u && rhs.tradeId != 0u) {
    return lhs.tradeId == rhs.tradeId ? TradeMatchKind::ExactSingle
                                      : TradeMatchKind::None;
  }
  if (lhs.aggregateId != 0u && rhs.aggregateId != 0u) {
    if (lhs.aggregateId == rhs.aggregateId &&
        lhs.firstTradeId == rhs.firstTradeId &&
        lhs.lastTradeId == rhs.lastTradeId && lhs.count == rhs.count) {
      return TradeMatchKind::ExactAggregate;
    }
    return TradeMatchKind::None;
  }
  const TradeIdentity* aggregate = nullptr;
  const TradeIdentity* single = nullptr;
  if (lhs.firstTradeId != 0u && lhs.lastTradeId >= lhs.firstTradeId &&
      rhs.tradeId != 0u) {
    aggregate = &lhs;
    single = &rhs;
  } else if (
      rhs.firstTradeId != 0u && rhs.lastTradeId >= rhs.firstTradeId &&
      lhs.tradeId != 0u) {
    aggregate = &rhs;
    single = &lhs;
  }
  if (aggregate != nullptr && single->tradeId >= aggregate->firstTradeId &&
      single->tradeId <= aggregate->lastTradeId) {
    return TradeMatchKind::AggregateContainsSingle;
  }
  return TradeMatchKind::None;
}

std::uint8_t bbo_change_mask(
    const BboState& before, const BboState& after) noexcept {
  std::uint8_t output = 0u;
  if (before.bidPrice != after.bidPrice) output |= kBidPriceChanged;
  if (before.bidQuantity != after.bidQuantity) output |= kBidQuantityChanged;
  if (before.askPrice != after.askPrice) output |= kAskPriceChanged;
  if (before.askQuantity != after.askQuantity) output |= kAskQuantityChanged;
  return output;
}

bool same_bbo_transition(
    const BboTransition& lhs, const BboTransition& rhs) noexcept {
  if (!(lhs.before == rhs.before) || !(lhs.after == rhs.after) ||
      lhs.changeMask == 0u || rhs.changeMask == 0u ||
      lhs.changeMask != rhs.changeMask) {
    return false;
  }
  return lhs.sequence == 0u || rhs.sequence == 0u ||
         lhs.sequence == rhs.sequence;
}

std::uint64_t fingerprint_seed() noexcept {
  return 1'469'598'103'934'665'603ull;
}

std::uint64_t fingerprint_value(
    std::uint64_t current, std::int64_t value) noexcept {
  constexpr std::uint64_t kPrime = 1'099'511'628'211ull;
  auto bits = static_cast<std::uint64_t>(value);
  for (unsigned byte = 0u; byte < 8u; ++byte) {
    current ^= bits & 0xffu;
    current *= kPrime;
    bits >>= 8u;
  }
  return current;
}

}  // namespace exchange_probe::race
