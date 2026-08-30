#pragma once

#include "exchange_probe/race/types.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace exchange_probe::race {

class ReadyBarrier {
 public:
  explicit ReadyBarrier(std::size_t feedCount);

  [[nodiscard]] bool set_terminal(
      std::size_t feedIndex, FeedStatus status) noexcept;
  [[nodiscard]] bool ready() const noexcept;
  [[nodiscard]] std::size_t terminal_count() const noexcept;
  [[nodiscard]] FeedStatus status(std::size_t feedIndex) const noexcept;

 private:
  std::vector<std::atomic<std::uint8_t>> states_;
  std::atomic<std::size_t> terminalCount_{0u};
};

[[nodiscard]] bool terminal_feed_status(FeedStatus status) noexcept;

[[nodiscard]] std::vector<AffinityAssignment> plan_affinity(
    const std::vector<CpuLocation>& topology,
    const std::vector<std::uint32_t>& connectionIds,
    std::size_t reservedPhysicalCores = 1u);

}  // namespace exchange_probe::race
