#include "exchange_probe/race/clock.hpp"
#include "exchange_probe/race/ingress.hpp"
#include "exchange_probe/race/session.hpp"

#include <cassert>
#include <cstdint>
#include <vector>

int main() {
  using namespace exchange_probe::race;

  const auto mapping = sample_time_mapping();
  assert(mapping.monoBeforeNs != 0u);
  assert(mapping.monoAfterNs >= mapping.monoBeforeNs);
  assert(mapped_realtime_ns(mapping.monoBeforeNs, mapping) != 0u);

  ConnectionIngress ingress;
  RaceRecord input{};
  input.eventOrdinal = 42u;
  input.timestamps.recvMonoNs = now_mono_raw_ns();
  assert(ingress.publish(input));
  RaceRecord output{};
  assert(ingress.consume(output));
  assert(output.eventOrdinal == 42u);
  assert(ingress.published() == 1u);
  assert(ingress.consumed() == 1u);
  assert(ingress.drops() == 0u);

  for (std::size_t i = 0u; i < kIngressRingSlots - 1u; ++i) {
    assert(ingress.publish(input));
  }
  assert(!ingress.publish(input));
  assert(ingress.drops() == 1u);
  assert(ingress.degraded());

  ReadyBarrier ready{3u};
  assert(ready.set_terminal(0u, FeedStatus::Ready));
  assert(ready.set_terminal(1u, FeedStatus::Unavailable));
  assert(ready.set_terminal(2u, FeedStatus::Rejected));
  assert(ready.ready());
  assert(!ready.set_terminal(2u, FeedStatus::Ready));

  const std::vector<CpuLocation> topology{
      {0u, 0u, 0u}, {1u, 0u, 0u}, {2u, 1u, 0u},
      {3u, 1u, 0u}, {4u, 2u, 0u}, {5u, 2u, 0u},
  };
  const auto affinity = plan_affinity(topology, {10u, 11u, 12u}, 1u);
  assert(affinity.size() == 3u);
  assert(affinity[0].physicalCore != 0u);
  assert(!affinity[0].oversubscribed);
  assert(!affinity[1].oversubscribed);
  assert(affinity[2].oversubscribed);
}
