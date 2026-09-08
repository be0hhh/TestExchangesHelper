#pragma once

#include "exchange_probe/race/Capture.hpp"
#include "exchange_probe/race/Instruments.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace exchange_probe::race::capture_detail {

[[nodiscard]] bool write_availability(
    const std::filesystem::path& directory, const char* venue,
    const std::vector<InstrumentResolution>& resolutions,
    std::string& error);

[[nodiscard]] unsigned logical_cpu(std::size_t connectionOrdinal) noexcept;

[[nodiscard]] PublicCaptureResult capture_bitget(
    const PublicCaptureOptions& options);
[[nodiscard]] PublicCaptureResult capture_bybit(
    const PublicCaptureOptions& options);

}  // namespace exchange_probe::race::capture_detail
