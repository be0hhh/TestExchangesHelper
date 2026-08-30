#pragma once

#include "exchange_probe/reconstruction/types.hpp"

#include <cstddef>

namespace exchange_probe::reconstruction {

[[nodiscard]] InstrumentSelection selectCampaignInstruments(
    const InstrumentCandidate* candidates,
    std::size_t count,
    bool exactTicker24hCapability) noexcept;

}  // namespace exchange_probe::reconstruction
