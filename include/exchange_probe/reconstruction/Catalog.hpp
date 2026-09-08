#pragma once

#include "exchange_probe/reconstruction/Types.hpp"

#include <cstddef>

namespace exchange_probe::reconstruction {

struct ProductCatalog final {
  ProductEligibility rows[kMaximumProducts]{};
  std::size_t count{0u};
  ProductEligibilityReason failure{
      ProductEligibilityReason::Eligible};
};

[[nodiscard]] ProductCatalog buildProductCatalog() noexcept;

}  // namespace exchange_probe::reconstruction
