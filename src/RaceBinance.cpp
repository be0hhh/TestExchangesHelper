#include "exchange_probe/race/Normalizers.hpp"

#include "RaceBinanceCommon.hpp"

namespace exchange_probe::race {

bool normalize_binance_usdm(
    void* rawState, const FrameView& frame,
    NormalizeBatch& output) noexcept {
  output = NormalizeBatch{};
  auto* state = static_cast<VenueNormalizerState*>(rawState);
  if (!state || state->venue != Venue::BinanceUsdM || frame.binary ||
      !frame.data || frame.size == 0u) {
    output.malformed = true;
    return false;
  }
  const bool ok = binance_like_detail::normalize(*state, frame, output);
  output.malformed = !ok;
  return ok;
}

}  // namespace exchange_probe::race
