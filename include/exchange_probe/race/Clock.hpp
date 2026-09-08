#pragma once

#include "exchange_probe/race/Types.hpp"

#include <cstdint>

namespace exchange_probe::race {

[[nodiscard]] std::uint64_t now_mono_raw_ns() noexcept;
[[nodiscard]] std::uint64_t now_realtime_ns() noexcept;
[[nodiscard]] TimeMappingSample sample_time_mapping() noexcept;
[[nodiscard]] std::uint64_t mapped_realtime_ns(
    std::uint64_t monoNs, const TimeMappingSample& mapping) noexcept;

}  // namespace exchange_probe::race
