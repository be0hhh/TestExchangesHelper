#pragma once

#include "exchange_probe/race/types.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace exchange_probe::race {

enum class AnchorRole : std::uint8_t {
  None = 0u,
  DirectBbo = 1u,
  Trade = 2u,
  FastDepth = 3u,
};

struct FeedPlan {
  std::string id;
  EventClass eventClass{EventClass::None};
  Wire wire{Wire::Json};
  TransportKind transport{TransportKind::RuntimeSubscribe};
  AnchorRole anchor{AnchorRole::None};
  unsigned depth{0u};
  unsigned nominalIntervalMs{0u};
  bool reconstructBook{false};
  bool experimental{false};
  bool credentialsExpected{false};
};

struct InstrumentEndpoint {
  Venue venue{Venue::Unknown};
  std::string host;
  std::string path;
};

struct VenueFeedPlan {
  Venue venue{Venue::Unknown};
  std::string name;
  InstrumentEndpoint instruments;
  std::vector<FeedPlan> feeds;
};

struct RaceGroup {
  std::uint16_t id{0u};
  std::vector<std::size_t> feedIndices;
};

[[nodiscard]] std::vector<VenueFeedPlan> requested_venue_plans();
[[nodiscard]] std::vector<RaceGroup> build_race_groups(
    const std::vector<FeedPlan>& feeds, std::size_t receiverBudget);

}  // namespace exchange_probe::race
