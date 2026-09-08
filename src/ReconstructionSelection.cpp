#include "exchange_probe/reconstruction/Selection.hpp"

#include <cstring>

namespace exchange_probe::reconstruction {
namespace {

[[nodiscard]] bool textEqual(const char* lhs, const char* rhs,
                             std::size_t capacity) noexcept {
  return lhs && rhs && std::strncmp(lhs, rhs, capacity) == 0;
}

[[nodiscard]] bool symbolLess(const Symbol& lhs, const Symbol& rhs) noexcept {
  return std::strncmp(lhs.data, rhs.data, Symbol::capacity) < 0;
}

[[nodiscard]] bool eligibleUsdt(const InstrumentCandidate& candidate) noexcept {
  return candidate.active && candidate.tickSizeRaw > 0 &&
         textEqual(candidate.quoteAsset, "USDT",
                   sizeof(candidate.quoteAsset));
}

[[nodiscard]] bool betterVolume(const InstrumentCandidate& lhs,
                                const InstrumentCandidate& rhs) noexcept {
  if (lhs.quoteVolumeRaw != rhs.quoteVolumeRaw) {
    return lhs.quoteVolumeRaw > rhs.quoteVolumeRaw;
  }
  return symbolLess(lhs.symbol, rhs.symbol);
}

}  // namespace

InstrumentSelection selectCampaignInstruments(
    const InstrumentCandidate* candidates, std::size_t count,
    bool exactTicker24hCapability) noexcept {
  InstrumentSelection output{};
  if (!candidates && count != 0u) return output;

  const InstrumentCandidate* eth = nullptr;
  for (std::size_t index = 0u; index < count; ++index) {
    const auto& candidate = candidates[index];
    if (eligibleUsdt(candidate) &&
        textEqual(candidate.baseAsset, "ETH", sizeof(candidate.baseAsset))) {
      if (!eth || symbolLess(candidate.symbol, eth->symbol)) eth = &candidate;
    }
  }
  if (!eth) return output;

  output.ethAvailable = true;
  output.rows[output.count++] = *eth;
  if (!exactTicker24hCapability) {
    output.volumeFallbackToEthOnly = true;
    return output;
  }

  const InstrumentCandidate* top[2]{};
  for (std::size_t index = 0u; index < count; ++index) {
    const auto& candidate = candidates[index];
    if (!eligibleUsdt(candidate) || !candidate.exactQuoteVolume ||
        candidate.quoteVolumeRaw <= 0 ||
        textEqual(candidate.baseAsset, "BTC", sizeof(candidate.baseAsset)) ||
        textEqual(candidate.baseAsset, "ETH", sizeof(candidate.baseAsset))) {
      continue;
    }
    if (!top[0] || betterVolume(candidate, *top[0])) {
      top[1] = top[0];
      top[0] = &candidate;
    } else if (!top[1] || betterVolume(candidate, *top[1])) {
      top[1] = &candidate;
    }
  }
  for (const InstrumentCandidate* candidate : top) {
    if (candidate && output.count < kMaximumSelectedSymbols) {
      output.rows[output.count++] = *candidate;
    }
  }
  return output;
}

}  // namespace exchange_probe::reconstruction
