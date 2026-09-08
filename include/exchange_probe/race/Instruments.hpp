#pragma once

#include "exchange_probe/race/Catalog.hpp"

#include <string>
#include <vector>

namespace exchange_probe::race {

enum class InstrumentAvailability : std::uint8_t {
  Available = 1u,
  Unavailable = 2u,
  Ambiguous = 3u,
  InvalidResponse = 4u,
};

struct InstrumentRow {
  std::string nativeSymbol;
  std::string base;
  std::string quote;
  std::string contractKind;
  std::string status;
};

struct InstrumentResolution {
  InstrumentAvailability availability{InstrumentAvailability::Unavailable};
  std::string requestedBase;
  std::string nativeSymbol;
  std::string reason;
};

[[nodiscard]] InstrumentResolution resolve_usdt_perpetual(
    const std::vector<InstrumentRow>& rows, const std::string& requestedBase);
[[nodiscard]] bool parse_instrument_response(
    Venue venue, const char* data, std::size_t size,
    std::vector<InstrumentRow>& rows, std::string& error);

}  // namespace exchange_probe::race
