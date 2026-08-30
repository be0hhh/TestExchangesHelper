#pragma once

#include "exchange_probe/reconstruction/types.hpp"

#include "api/market/PublicMarketDataSubscriptionManager.hpp"

namespace exchange_probe::reconstruction {

// Cold fail-closed proof that the manager routes used for capture are bound
// to the same canonical descriptor lanes and concrete RuntimeV1 callbacks
// recorded in ProductEligibility.
[[nodiscard]] bool validateCaptureProvenance(
    const ProductEligibility& product,
    const Symbol& symbol,
    bool includeDepth,
    const cxet::api::market::PublicMarketDataSubscriptionManager& manager)
    noexcept;

[[nodiscard]] bool validateCaptureHeaderProvenance(
    const CaptureHeader& header) noexcept;

}  // namespace exchange_probe::reconstruction
