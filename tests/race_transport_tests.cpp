#include "exchange_probe/race/transport.hpp"

#include <cassert>
#include <cstdint>

int main() {
  using namespace exchange_probe::race;
  std::uint8_t payload[4u]{1u, 2u, 3u, 4u};
  FrameView frame{
      .data = payload,
      .size = sizeof(payload),
      .recvMonoNs = 55u,
      .binary = true,
  };
  RawSampleStore samples;
  assert(samples.capture(frame, false));
  assert(samples.count() == 1u);
  assert(samples.sample(0u).recvMonoNs == 55u);
  assert(samples.sample(0u).storedSize == 4u);
  assert(samples.sample(0u).bytes[3u] == 4u);
  for (unsigned i = 1u; i < 4u; ++i) assert(samples.capture(frame, false));
  assert(!samples.capture(frame, false));
  assert(samples.capture(frame, true));
  assert(samples.sample(4u).malformed);

  ConnectionObservation observation;
  auto* generation0 = begin_generation_observation(observation, 0u);
  auto* generation1 = begin_generation_observation(observation, 1u);
  assert(generation0 != nullptr && generation0->generation == 0u);
  assert(generation1 != nullptr && generation1->generation == 1u);
  assert(observation.generationCount == 2u);

  ConnectionSpec spec;
  spec.source.sourceId = 17u;
  spec.source.connectionId = 19u;
  RaceRecord record;
  record.source.origin = RecordOrigin::DerivedBook;
  stamp_record_arrival(record, spec, 3u, 71u);
  assert(record.source.sourceId == 17u);
  assert(record.source.connectionId == 19u);
  assert(record.source.connectionGeneration == 3u);
  assert(record.source.origin == RecordOrigin::DerivedBook);
  assert(record.timestamps.recvMonoNs == 71u);
}
