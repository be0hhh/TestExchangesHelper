#include "exchange_probe/race/clock.hpp"
#include "exchange_probe/race/session.hpp"

#include <algorithm>
#include <ctime>
#include <limits>
#include <set>
#include <tuple>

namespace exchange_probe::race {
namespace {

[[nodiscard]] std::uint64_t clock_ns(clockid_t id) noexcept {
  timespec value{};
  if (::clock_gettime(id, &value) != 0 || value.tv_sec < 0 ||
      value.tv_nsec < 0) {
    return 0u;
  }
  constexpr std::uint64_t kNsPerSecond = 1'000'000'000u;
  const auto seconds = static_cast<std::uint64_t>(value.tv_sec);
  const auto nanos = static_cast<std::uint64_t>(value.tv_nsec);
  if (seconds >
      (std::numeric_limits<std::uint64_t>::max() - nanos) / kNsPerSecond) {
    return 0u;
  }
  return seconds * kNsPerSecond + nanos;
}

}  // namespace

std::uint64_t now_mono_raw_ns() noexcept {
#if defined(CLOCK_MONOTONIC_RAW)
  return clock_ns(CLOCK_MONOTONIC_RAW);
#else
  return clock_ns(CLOCK_MONOTONIC);
#endif
}

std::uint64_t now_realtime_ns() noexcept {
  return clock_ns(CLOCK_REALTIME);
}

TimeMappingSample sample_time_mapping() noexcept {
  TimeMappingSample output{};
  output.monoBeforeNs = now_mono_raw_ns();
  output.realtimeNs = now_realtime_ns();
  output.monoAfterNs = now_mono_raw_ns();
  return output;
}

std::uint64_t mapped_realtime_ns(
    std::uint64_t monoNs, const TimeMappingSample& mapping) noexcept {
  if (mapping.monoBeforeNs == 0u || mapping.monoAfterNs == 0u ||
      mapping.realtimeNs == 0u ||
      mapping.monoAfterNs < mapping.monoBeforeNs) {
    return 0u;
  }
  const std::uint64_t midpoint =
      mapping.monoBeforeNs +
      (mapping.monoAfterNs - mapping.monoBeforeNs) / 2u;
  if (monoNs >= midpoint) {
    const auto delta = monoNs - midpoint;
    if (mapping.realtimeNs >
        std::numeric_limits<std::uint64_t>::max() - delta) {
      return 0u;
    }
    return mapping.realtimeNs + delta;
  }
  const auto delta = midpoint - monoNs;
  return delta <= mapping.realtimeNs ? mapping.realtimeNs - delta : 0u;
}

bool terminal_feed_status(FeedStatus status) noexcept {
  switch (status) {
    case FeedStatus::Ready:
    case FeedStatus::Unavailable:
    case FeedStatus::Rejected:
    case FeedStatus::RequiresLogin:
    case FeedStatus::VipRequired:
    case FeedStatus::UnsupportedSchema:
    case FeedStatus::Disconnected:
    case FeedStatus::Failed:
    case FeedStatus::Degraded:
      return true;
    default:
      return false;
  }
}

ReadyBarrier::ReadyBarrier(std::size_t feedCount) : states_(feedCount) {
  for (auto& state : states_) {
    state.store(
        static_cast<std::uint8_t>(FeedStatus::Planned),
        std::memory_order_relaxed);
  }
}

bool ReadyBarrier::set_terminal(
    std::size_t feedIndex, FeedStatus statusValue) noexcept {
  if (feedIndex >= states_.size() || !terminal_feed_status(statusValue)) {
    return false;
  }
  auto& state = states_[feedIndex];
  auto previous = state.load(std::memory_order_acquire);
  while (!terminal_feed_status(static_cast<FeedStatus>(previous))) {
    if (state.compare_exchange_weak(
            previous, static_cast<std::uint8_t>(statusValue),
            std::memory_order_release, std::memory_order_acquire)) {
      terminalCount_.fetch_add(1u, std::memory_order_release);
      return true;
    }
  }
  return false;
}

bool ReadyBarrier::ready() const noexcept {
  return terminalCount_.load(std::memory_order_acquire) == states_.size();
}

std::size_t ReadyBarrier::terminal_count() const noexcept {
  return terminalCount_.load(std::memory_order_acquire);
}

FeedStatus ReadyBarrier::status(std::size_t feedIndex) const noexcept {
  if (feedIndex >= states_.size()) return FeedStatus::Failed;
  return static_cast<FeedStatus>(
      states_[feedIndex].load(std::memory_order_acquire));
}

std::vector<AffinityAssignment> plan_affinity(
    const std::vector<CpuLocation>& topology,
    const std::vector<std::uint32_t>& connectionIds,
    std::size_t reservedPhysicalCores) {
  std::vector<CpuLocation> physical;
  std::set<std::tuple<unsigned, unsigned>> seen;
  for (const auto& cpu : topology) {
    if (seen.emplace(cpu.package, cpu.physicalCore).second) {
      physical.push_back(cpu);
    }
  }
  std::sort(
      physical.begin(), physical.end(),
      [](const CpuLocation& lhs, const CpuLocation& rhs) {
        return std::tie(lhs.package, lhs.physicalCore, lhs.logicalCpu) <
               std::tie(rhs.package, rhs.physicalCore, rhs.logicalCpu);
      });
  if (physical.size() <= reservedPhysicalCores || connectionIds.empty()) {
    return {};
  }
  const std::size_t first = reservedPhysicalCores;
  const std::size_t available = physical.size() - first;
  std::vector<AffinityAssignment> result;
  result.reserve(connectionIds.size());
  for (std::size_t index = 0u; index < connectionIds.size(); ++index) {
    const auto& cpu = physical[first + index % available];
    result.push_back(AffinityAssignment{
        .connectionId = connectionIds[index],
        .logicalCpu = cpu.logicalCpu,
        .physicalCore = cpu.physicalCore,
        .package = cpu.package,
        .oversubscribed = index >= available,
    });
  }
  return result;
}

}  // namespace exchange_probe::race
