#include "exchange_probe/race/normalizers.hpp"

#include "race_binance_common.hpp"

namespace exchange_probe::race {

bool normalize_aster(
    void* rawState, const FrameView& frame,
    NormalizeBatch& output) noexcept {
  output = NormalizeBatch{};
  auto* state = static_cast<VenueNormalizerState*>(rawState);
  if (!state || state->venue != Venue::Aster || frame.binary || !frame.data ||
      frame.size == 0u) {
    output.malformed = true;
    return false;
  }
  const bool ok = binance_like_detail::normalize(*state, frame, output);
  output.malformed = !ok;
  return ok;
}

}  // namespace exchange_probe::race
