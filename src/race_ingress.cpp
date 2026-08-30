#include "exchange_probe/race/ingress.hpp"

namespace exchange_probe::race {

bool ConnectionIngress::publish(const RaceRecord& record) noexcept {
  if (!ring_.tryPush(record)) {
    drops_.fetch_add(1u, std::memory_order_relaxed);
    degraded_.store(true, std::memory_order_release);
    return false;
  }
  published_.fetch_add(1u, std::memory_order_relaxed);
  return true;
}

bool ConnectionIngress::consume(RaceRecord& output) noexcept {
  if (!ring_.tryPop(output)) return false;
  consumed_.fetch_add(1u, std::memory_order_relaxed);
  return true;
}

}  // namespace exchange_probe::race
