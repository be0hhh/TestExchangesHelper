#pragma once

#include "exchange_probe/reconstruction/Types.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>

namespace exchange_probe::reconstruction {

struct CaptureOptions final {
  ProductEligibility product{};
  Symbol symbol{};
  std::int64_t tickSizeRaw{0};
  std::filesystem::path outputPath{};
  std::chrono::seconds duration{600};
  bool includeDepth{false};
};

struct CaptureResult final {
  std::uint64_t observations{0u};
  std::uint64_t bookTickerSides{0u};
  std::uint64_t trades{0u};
  std::uint64_t depthBbos{0u};
  std::uint64_t terminalEvents{0u};
  std::string error{};
};

[[nodiscard]] bool capture(const CaptureOptions& options,
                           CaptureResult& result) noexcept;

}  // namespace exchange_probe::reconstruction
