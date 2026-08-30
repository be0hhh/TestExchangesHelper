#pragma once

#include "exchange_probe/race/types.hpp"
#include "runtime/SpscRing.hpp"

#include <atomic>
#include <cstdint>

namespace exchange_probe::race {

inline constexpr std::size_t kIngressRingSlots = 4096u;

class ConnectionIngress {
 public:
  ConnectionIngress() noexcept = default;
  ConnectionIngress(const ConnectionIngress&) = delete;
  ConnectionIngress& operator=(const ConnectionIngress&) = delete;

  [[nodiscard]] bool publish(const RaceRecord& record) noexcept;
  [[nodiscard]] bool consume(RaceRecord& output) noexcept;

  [[nodiscard]] std::uint64_t published() const noexcept {
    return published_.load(std::memory_order_relaxed);
  }
  [[nodiscard]] std::uint64_t consumed() const noexcept {
    return consumed_.load(std::memory_order_relaxed);
  }
  [[nodiscard]] std::uint64_t drops() const noexcept {
    return drops_.load(std::memory_order_relaxed);
  }
  [[nodiscard]] bool degraded() const noexcept {
    return degraded_.load(std::memory_order_acquire);
  }

 private:
  cxet::runtime::SpscRing<RaceRecord, kIngressRingSlots> ring_{};
  alignas(64) std::atomic<std::uint64_t> published_{0u};
  alignas(64) std::atomic<std::uint64_t> consumed_{0u};
  alignas(64) std::atomic<std::uint64_t> drops_{0u};
  alignas(64) std::atomic<bool> degraded_{false};
};

}  // namespace exchange_probe::race
