#pragma once

#include "exchange_probe/race/types.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace exchange_probe::race {

struct DistributionSummary {
  std::uint64_t samples{0u};
  std::int64_t minimumNs{0};
  std::int64_t p10Ns{0};
  std::int64_t p25Ns{0};
  std::int64_t medianNs{0};
  std::int64_t p75Ns{0};
  std::int64_t p90Ns{0};
  std::int64_t p95Ns{0};
  std::int64_t p99Ns{0};
  std::int64_t p999Ns{0};
  std::int64_t maximumNs{0};
  long double meanNs{0.0L};
};

struct SourceStatistics {
  SourceIdentity source{};
  EventClass eventClass{EventClass::None};
  std::uint64_t records{0u};
  std::uint64_t uniqueEvents{0u};
  std::uint64_t duplicates{0u};
  std::uint64_t staleOrInvalid{0u};
  std::uint64_t sequenceGaps{0u};
  std::uint64_t resyncs{0u};
  std::uint64_t firstArrivals{0u};
  DistributionSummary interArrival{};
  DistributionSummary frameBatchSize{};
};

struct PairwiseStatistics {
  std::uint32_t sourceA{0u};
  std::uint32_t sourceB{0u};
  std::uint16_t sessionId{0u};
  std::uint16_t raceGroupId{0u};
  Venue venue{Venue::Unknown};
  EventClass eventClass{EventClass::None};
  std::uint64_t matched{0u};
  std::uint64_t unmatchedA{0u};
  std::uint64_t unmatchedB{0u};
  std::uint64_t aFirst{0u};
  std::uint64_t bFirst{0u};
  std::uint64_t ties{0u};
  DistributionSummary deltaRecvANsMinusB{};
  std::vector<std::int64_t> signedDeltasNs;
};

inline constexpr std::size_t kBybitBboRaceLaneCount = 4u;
inline constexpr std::uint8_t kNoBboRaceLane = 0xffu;
inline constexpr std::size_t kMaximumDashboardEventSamples = 1024u;

struct AnalysisSourceDescriptor {
  SourceIdentity source{};
  std::string symbol;
  std::string feedId;
  std::uint8_t bboRaceLane{kNoBboRaceLane};
  bool healthClean{true};
};

struct AnalysisSessionWindow {
  std::uint16_t sessionId{0u};
  std::uint16_t raceGroupId{0u};
  std::uint64_t measuredStartMonoNs{0u};
  std::uint64_t measuredEndMonoNs{0u};
};

struct AnalysisContext {
  std::vector<AnalysisSourceDescriptor> sources;
  std::vector<AnalysisSessionWindow> sessionWindows;
  std::uint64_t rollingWindowNs{10'000'000'000u};
};

struct MultiwayLaneStatistics {
  std::uint32_t sourceId{0u};
  std::string feedId;
  std::uint64_t eligibleEvents{0u};
  std::uint64_t wins{0u};
  std::uint64_t tiedFirst{0u};
  DistributionSummary arrivalAfterWinner{};
  DistributionSummary winningLead{};
};

struct MultiwayEventSample {
  std::uint16_t sessionId{0u};
  std::uint16_t raceGroupId{0u};
  std::uint64_t sequence{0u};
  std::uint64_t offsetFromMeasuredStartNs{0u};
  BboState bbo{};
  std::array<std::int64_t, kBybitBboRaceLaneCount> arrivalAfterWinnerNs{};
  std::uint8_t firstLaneMask{0u};
};

struct RollingLaneResult {
  std::uint32_t sourceId{0u};
  std::uint64_t wins{0u};
  std::uint64_t tiedFirst{0u};
};

struct RollingRaceWindow {
  std::uint16_t sessionId{0u};
  std::uint16_t raceGroupId{0u};
  std::uint64_t startOffsetNs{0u};
  std::uint64_t endOffsetNs{0u};
  std::uint64_t matchedAll{0u};
  std::uint8_t leaderLane{kNoBboRaceLane};
  std::array<RollingLaneResult, kBybitBboRaceLaneCount> lanes{};
};

struct MultiwayRaceStatistics {
  std::uint16_t sessionId{0u};
  std::uint16_t raceGroupId{0u};
  Venue venue{Venue::Unknown};
  std::string symbol;
  std::uint64_t unionEvents{0u};
  std::uint64_t matchedAll{0u};
  std::uint64_t excludedPartial{0u};
  std::uint64_t tieEvents{0u};
  bool healthClean{true};
  std::array<MultiwayLaneStatistics, kBybitBboRaceLaneCount> lanes{};
  std::vector<MultiwayEventSample> eventSamples;
};

enum class RecommendationStatus : std::uint8_t {
  InsufficientEvidence = 0u,
  ObservedCandidate = 1u,
};

struct RaceRecommendation {
  std::uint16_t raceGroupId{0u};
  std::string symbol;
  RecommendationStatus status{RecommendationStatus::InsufficientEvidence};
  std::uint32_t primarySourceId{0u};
  std::uint32_t complementSourceId{0u};
  std::string reason{"missing_strict_four_feed_evidence"};
};

struct AnalysisResult {
  std::uint64_t inputRecords{0u};
  std::uint64_t eligibleRecords{0u};
  std::vector<AnalysisSourceDescriptor> sourceDescriptors;
  std::vector<SourceStatistics> sources;
  std::vector<PairwiseStatistics> perSessionPairs;
  std::vector<PairwiseStatistics> pooledPairs;
  std::vector<MultiwayRaceStatistics> perSessionMultiway;
  std::vector<MultiwayRaceStatistics> pooledMultiway;
  std::vector<RollingRaceWindow> rollingWindows;
  std::vector<RaceRecommendation> recommendations;
};

[[nodiscard]] DistributionSummary summarize_distribution(
    std::vector<std::int64_t> values);
[[nodiscard]] AnalysisResult analyze_records(
    const std::vector<RaceRecord>& records,
    const AnalysisContext& context = {});

}  // namespace exchange_probe::race
