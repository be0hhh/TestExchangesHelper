#include "exchange_probe/race/Analysis.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <tuple>
#include <unordered_map>
#include <utility>

namespace exchange_probe::race {
namespace {

struct EventKey {
  Venue venue{Venue::Unknown};
  EventClass eventClass{EventClass::None};
  std::uint16_t sessionId{0u};
  std::uint16_t raceGroupId{0u};
  std::uint8_t identityKind{0u};
  std::uint8_t changeMask{0u};
  std::array<std::uint64_t, 10u> words{};

  [[nodiscard]] bool operator<(const EventKey& rhs) const noexcept {
    return std::tie(
               venue, eventClass, sessionId, raceGroupId, identityKind,
               changeMask, words) <
           std::tie(
               rhs.venue, rhs.eventClass, rhs.sessionId, rhs.raceGroupId,
               rhs.identityKind, rhs.changeMask, rhs.words);
  }
};

struct SourceKey {
  std::uint32_t sourceId{0u};
  std::uint16_t sessionId{0u};
  std::uint16_t raceGroupId{0u};
  Venue venue{Venue::Unknown};
  EventClass eventClass{EventClass::None};

  [[nodiscard]] bool operator<(const SourceKey& rhs) const noexcept {
    return std::tie(
               sourceId, sessionId, raceGroupId, venue, eventClass) <
           std::tie(
               rhs.sourceId, rhs.sessionId, rhs.raceGroupId, rhs.venue,
               rhs.eventClass);
  }
};

struct PairKey {
  std::uint32_t sourceA{0u};
  std::uint32_t sourceB{0u};
  std::uint16_t sessionId{0u};
  std::uint16_t raceGroupId{0u};
  Venue venue{Venue::Unknown};
  EventClass eventClass{EventClass::None};

  [[nodiscard]] bool operator<(const PairKey& rhs) const noexcept {
    return std::tie(
               sourceA, sourceB, sessionId, raceGroupId, venue, eventClass) <
           std::tie(
               rhs.sourceA, rhs.sourceB, rhs.sessionId, rhs.raceGroupId,
               rhs.venue, rhs.eventClass);
  }
};

struct ObservedEvent {
  const RaceRecord* record{nullptr};
  EventKey key{};
};

struct MultiwayContextKey {
  std::uint16_t sessionId{0u};
  std::uint16_t raceGroupId{0u};

  [[nodiscard]] bool operator<(const MultiwayContextKey& rhs) const noexcept {
    return std::tie(sessionId, raceGroupId) <
           std::tie(rhs.sessionId, rhs.raceGroupId);
  }
};

struct MultiwayAccumulator {
  MultiwayRaceStatistics statistics{};
  std::array<std::uint32_t, kBybitBboRaceLaneCount> sourceIds{};
  std::array<std::vector<std::int64_t>, kBybitBboRaceLaneCount>
      arrivalAfterWinner;
  std::array<std::vector<std::int64_t>, kBybitBboRaceLaneCount> winningLead;
  std::map<std::uint64_t, RollingRaceWindow> rolling;
  std::uint64_t measuredStartMonoNs{0u};
  std::uint64_t measuredEndMonoNs{0u};
  std::uint64_t rollingWindowNs{10'000'000'000u};
  std::uint64_t sampleOrdinal{0u};
  std::uint64_t sampleStride{1u};
};

[[nodiscard]] std::uint64_t signed_bits(std::int64_t value) noexcept {
  return static_cast<std::uint64_t>(value);
}

[[nodiscard]] bool make_event_key(
    const RaceRecord& record, const BboState* previousBbo,
    EventKey& output) noexcept {
  output.venue = record.source.venue;
  output.eventClass = record.eventClass;
  output.sessionId = record.source.sessionId;
  output.raceGroupId = record.source.raceGroupId;
  if (record.eventClass == EventClass::Trade) {
    if (record.trade.nativeShape != 0u) {
      output.identityKind = 1u;
      output.words[0] = record.trade.nativeShape;
      output.words[1] = record.trade.nativeFirst;
      output.words[2] = record.trade.nativeSecond;
      return true;
    }
    if (record.trade.tradeId != 0u) {
      output.identityKind = 2u;
      output.words[0] = record.trade.tradeId;
      return true;
    }
    if (record.trade.aggregateId != 0u) {
      output.identityKind = 3u;
      output.words[0] = record.trade.aggregateId;
      output.words[1] = record.trade.firstTradeId;
      output.words[2] = record.trade.lastTradeId;
      output.words[3] = record.trade.count;
      return true;
    }
    return false;
  }
  if (record.eventClass == EventClass::Bbo) {
    if (record.changeMask == 0u || previousBbo == nullptr) return false;
    output.identityKind = 4u;
    output.changeMask = record.changeMask;
    output.words[0] = signed_bits(previousBbo->bidPrice);
    output.words[1] = signed_bits(previousBbo->bidQuantity);
    output.words[2] = signed_bits(previousBbo->askPrice);
    output.words[3] = signed_bits(previousBbo->askQuantity);
    output.words[4] = signed_bits(record.bbo.bidPrice);
    output.words[5] = signed_bits(record.bbo.bidQuantity);
    output.words[6] = signed_bits(record.bbo.askPrice);
    output.words[7] = signed_bits(record.bbo.askQuantity);
    // Repeated transitions are distinct market events. When the venue exposes
    // an update sequence, require it to match instead of collapsing all equal
    // old->new states into one global event.
    if (record.sequence.sequence != 0u) {
      output.identityKind = 7u;
      output.words[8] = record.sequence.sequence;
    }
    return true;
  }
  if (record.eventClass == EventClass::Top5 &&
      record.top5Fingerprint != 0u) {
    output.identityKind = 5u;
    output.words[0] = record.top5Fingerprint;
    return true;
  }
  if (record.eventClass == EventClass::Top50 &&
      record.top50Fingerprint != 0u) {
    output.identityKind = 6u;
    output.words[0] = record.top50Fingerprint;
    return true;
  }
  return false;
}

[[nodiscard]] std::int64_t saturating_delta(
    std::uint64_t lhs, std::uint64_t rhs) noexcept {
  if (lhs >= rhs) {
    const auto magnitude = lhs - rhs;
    return magnitude > static_cast<std::uint64_t>(
                           std::numeric_limits<std::int64_t>::max())
               ? std::numeric_limits<std::int64_t>::max()
               : static_cast<std::int64_t>(magnitude);
  }
  const auto magnitude = rhs - lhs;
  if (magnitude > static_cast<std::uint64_t>(
                      std::numeric_limits<std::int64_t>::max()))
    return std::numeric_limits<std::int64_t>::min();
  return -static_cast<std::int64_t>(magnitude);
}

[[nodiscard]] std::int64_t percentile(
    const std::vector<std::int64_t>& values, long double probability) {
  if (values.empty()) return 0;
  const auto position = probability *
                        static_cast<long double>(values.size() - 1u);
  const auto index = static_cast<std::size_t>(std::floor(position));
  return values[index];
}

void finalize_pair(PairwiseStatistics& pair) {
  pair.matched = pair.signedDeltasNs.size();
  for (const auto delta : pair.signedDeltasNs) {
    if (delta < 0)
      ++pair.aFirst;
    else if (delta > 0)
      ++pair.bFirst;
    else
      ++pair.ties;
  }
  pair.deltaRecvANsMinusB = summarize_distribution(pair.signedDeltasNs);
}

[[nodiscard]] const AnalysisSessionWindow* find_session_window(
    const AnalysisContext& context, std::uint16_t sessionId,
    std::uint16_t raceGroupId) noexcept {
  for (const auto& window : context.sessionWindows) {
    if (window.sessionId == sessionId &&
        window.raceGroupId == raceGroupId) {
      return &window;
    }
  }
  return nullptr;
}

void append_event_sample(
    MultiwayAccumulator& accumulator, MultiwayEventSample sample) {
  ++accumulator.sampleOrdinal;
  if (accumulator.sampleOrdinal % accumulator.sampleStride != 0u) return;
  auto& samples = accumulator.statistics.eventSamples;
  if (samples.size() == kMaximumDashboardEventSamples) {
    std::size_t output = 0u;
    for (std::size_t index = 1u; index < samples.size(); index += 2u)
      samples[output++] = std::move(samples[index]);
    samples.resize(output);
    accumulator.sampleStride *= 2u;
    if (accumulator.sampleOrdinal % accumulator.sampleStride != 0u) return;
  }
  samples.push_back(std::move(sample));
}

[[nodiscard]] std::uint8_t unique_winner_lane(
    const MultiwayRaceStatistics& statistics) noexcept {
  std::uint64_t maximum = 0u;
  std::uint8_t winner = kNoBboRaceLane;
  bool tied = false;
  for (std::uint8_t lane = 0u; lane < kBybitBboRaceLaneCount; ++lane) {
    const auto wins = statistics.lanes[lane].wins;
    if (wins > maximum) {
      maximum = wins;
      winner = lane;
      tied = false;
    } else if (wins == maximum && wins != 0u) {
      tied = true;
    }
  }
  return maximum == 0u || tied ? kNoBboRaceLane : winner;
}

void finalize_multiway(MultiwayAccumulator& accumulator) {
  for (std::size_t lane = 0u; lane < kBybitBboRaceLaneCount; ++lane) {
    accumulator.statistics.lanes[lane].arrivalAfterWinner =
        summarize_distribution(std::move(accumulator.arrivalAfterWinner[lane]));
    accumulator.statistics.lanes[lane].winningLead =
        summarize_distribution(std::move(accumulator.winningLead[lane]));
  }
}

void merge_event_samples(
    MultiwayAccumulator& output,
    const std::vector<MultiwayEventSample>& samples) {
  for (const auto& sample : samples) append_event_sample(output, sample);
}

void build_recommendations(AnalysisResult& result) {
  for (const auto& pooled : result.pooledMultiway) {
    RaceRecommendation recommendation{};
    recommendation.raceGroupId = pooled.raceGroupId;
    recommendation.symbol = pooled.symbol;
    std::vector<const MultiwayRaceStatistics*> sessions;
    for (const auto& session : result.perSessionMultiway) {
      if (session.raceGroupId == pooled.raceGroupId &&
          session.symbol == pooled.symbol) {
        sessions.push_back(&session);
      }
    }
    if (sessions.size() < 3u) {
      recommendation.reason = "requires_three_sessions";
      result.recommendations.push_back(std::move(recommendation));
      continue;
    }
    bool evidenceClean = pooled.healthClean;
    std::uint8_t primaryLane = kNoBboRaceLane;
    for (const auto* session : sessions) {
      evidenceClean = evidenceClean && session->healthClean &&
                      session->matchedAll >= 1'000u;
      const auto winner = unique_winner_lane(*session);
      if (winner == kNoBboRaceLane) {
        evidenceClean = false;
      } else if (primaryLane == kNoBboRaceLane) {
        primaryLane = winner;
      } else if (winner != primaryLane) {
        evidenceClean = false;
      }
    }
    if (!evidenceClean || primaryLane == kNoBboRaceLane) {
      recommendation.reason = "health_sample_or_session_leader_gate";
      result.recommendations.push_back(std::move(recommendation));
      continue;
    }
    std::uint8_t complementLane = kNoBboRaceLane;
    std::uint64_t rescueWins = 0u;
    for (std::uint8_t lane = 0u; lane < kBybitBboRaceLaneCount; ++lane) {
      if (lane == primaryLane) continue;
      const auto wins = pooled.lanes[lane].wins;
      if (complementLane == kNoBboRaceLane || wins > rescueWins) {
        complementLane = lane;
        rescueWins = wins;
      }
    }
    if (complementLane == kNoBboRaceLane) {
      recommendation.reason = "no_complement_candidate";
      result.recommendations.push_back(std::move(recommendation));
      continue;
    }
    recommendation.status = RecommendationStatus::ObservedCandidate;
    recommendation.primarySourceId = pooled.lanes[primaryLane].sourceId;
    recommendation.complementSourceId =
        pooled.lanes[complementLane].sourceId;
    recommendation.reason = "consistent_three_session_strict_cohort";
    result.recommendations.push_back(std::move(recommendation));
  }
}

}  // namespace

DistributionSummary summarize_distribution(std::vector<std::int64_t> values) {
  DistributionSummary output{};
  if (values.empty()) return output;
  std::sort(values.begin(), values.end());
  output.samples = values.size();
  output.minimumNs = values.front();
  output.p10Ns = percentile(values, 0.10L);
  output.p25Ns = percentile(values, 0.25L);
  output.medianNs = percentile(values, 0.50L);
  output.p75Ns = percentile(values, 0.75L);
  output.p90Ns = percentile(values, 0.90L);
  output.p95Ns = percentile(values, 0.95L);
  output.p99Ns = percentile(values, 0.99L);
  output.p999Ns = percentile(values, 0.999L);
  output.maximumNs = values.back();
  long double sum = 0.0L;
  for (const auto value : values) sum += static_cast<long double>(value);
  output.meanNs = sum / static_cast<long double>(values.size());
  return output;
}

AnalysisResult analyze_records(
    const std::vector<RaceRecord>& records,
    const AnalysisContext& context) {
  AnalysisResult result{};
  result.inputRecords = records.size();
  result.sourceDescriptors = context.sources;
  std::map<SourceKey, BboState> previousBbo;
  std::map<EventKey, std::map<std::uint32_t, const RaceRecord*>> events;
  std::map<SourceKey, SourceStatistics> sourceStats;
  std::map<SourceKey, std::set<EventKey>> sourceEvents;
  std::map<SourceKey, std::uint64_t> lastArrival;
  std::map<SourceKey, std::vector<std::int64_t>> interArrivals;
  std::map<SourceKey, std::vector<std::int64_t>> batchSizes;
  std::map<SourceKey, std::uint32_t> lastResync;

  for (const auto& record : records) {
    SourceKey sourceKey{
        record.source.sourceId, record.source.sessionId,
        record.source.raceGroupId, record.source.venue, record.eventClass};
    auto& source = sourceStats[sourceKey];
    source.source = record.source;
    source.eventClass = record.eventClass;
    ++source.records;
    if (record.validity != BookValidity::Valid)
      ++source.staleOrInvalid;
    if (record.validity == BookValidity::InvalidGap)
      ++source.sequenceGaps;
    const auto resync = lastResync.find(sourceKey);
    if (resync == lastResync.end()) {
      lastResync.emplace(sourceKey, record.resyncGeneration);
    } else if (record.resyncGeneration > resync->second) {
      ++source.resyncs;
      resync->second = record.resyncGeneration;
    }
    if (record.timestamps.recvMonoNs != 0u) {
      auto [position, inserted] = lastArrival.emplace(
          sourceKey, record.timestamps.recvMonoNs);
      if (!inserted) {
        interArrivals[sourceKey].push_back(saturating_delta(
            record.timestamps.recvMonoNs, position->second));
        position->second = record.timestamps.recvMonoNs;
      }
    }
    batchSizes[sourceKey].push_back(record.frameBatchCount);

    EventKey eventKey{};
    const auto previous = previousBbo.find(sourceKey);
    const BboState* prior = previous == previousBbo.end()
                                ? nullptr
                                : &previous->second;
    const bool eligible = record.validity == BookValidity::Valid &&
                          make_event_key(record, prior, eventKey);
    if (record.eventClass == EventClass::Bbo)
      previousBbo[sourceKey] = record.bbo;
    if (!eligible) continue;
    ++result.eligibleRecords;
    auto& bySource = events[eventKey];
    const auto existing = bySource.find(record.source.sourceId);
    if (existing != bySource.end()) {
      ++source.duplicates;
      if (record.timestamps.recvMonoNs <
          existing->second->timestamps.recvMonoNs)
        existing->second = &record;
      continue;
    }
    bySource.emplace(record.source.sourceId, &record);
    sourceEvents[sourceKey].insert(eventKey);
  }

  std::map<PairKey, PairwiseStatistics> sessionPairs;
  std::map<std::tuple<std::uint16_t, std::uint16_t, Venue, EventClass>,
           std::vector<std::uint32_t>> contextSources;
  for (const auto& [sourceKey, eventSet] : sourceEvents) {
    if (eventSet.empty()) continue;
    contextSources[{sourceKey.sessionId, sourceKey.raceGroupId,
                    sourceKey.venue, sourceKey.eventClass}]
        .push_back(sourceKey.sourceId);
  }
  for (auto& [sourceContext, sources] : contextSources) {
    std::sort(sources.begin(), sources.end());
    sources.erase(std::unique(sources.begin(), sources.end()), sources.end());
    for (std::size_t lhs = 0u; lhs < sources.size(); ++lhs) {
      for (std::size_t rhs = lhs + 1u; rhs < sources.size(); ++rhs) {
        const PairKey key{
            sources[lhs], sources[rhs], std::get<0>(sourceContext),
            std::get<1>(sourceContext), std::get<2>(sourceContext),
            std::get<3>(sourceContext)};
        auto& pair = sessionPairs[key];
        pair.sourceA = key.sourceA;
        pair.sourceB = key.sourceB;
        pair.sessionId = key.sessionId;
        pair.raceGroupId = key.raceGroupId;
        pair.venue = key.venue;
        pair.eventClass = key.eventClass;
      }
    }
  }
  for (const auto& [eventKey, bySource] : events) {
    if (bySource.empty()) continue;
    auto first = std::min_element(
        bySource.begin(), bySource.end(), [](const auto& lhs, const auto& rhs) {
          return lhs.second->timestamps.recvMonoNs <
                 rhs.second->timestamps.recvMonoNs;
        });
    std::size_t earliestCount = 0u;
    for (const auto& candidate : bySource)
      if (candidate.second->timestamps.recvMonoNs ==
          first->second->timestamps.recvMonoNs)
        ++earliestCount;
    if (earliestCount == 1u) {
      SourceKey winningKey{
          first->first, eventKey.sessionId, eventKey.raceGroupId,
          eventKey.venue, eventKey.eventClass};
      ++sourceStats[winningKey].firstArrivals;
    }
    for (auto lhs = bySource.begin(); lhs != bySource.end(); ++lhs) {
      for (auto rhs = std::next(lhs); rhs != bySource.end(); ++rhs) {
        const PairKey key{
            lhs->first, rhs->first, eventKey.sessionId,
            eventKey.raceGroupId, eventKey.venue, eventKey.eventClass};
        auto& pair = sessionPairs[key];
        pair.sourceA = key.sourceA;
        pair.sourceB = key.sourceB;
        pair.sessionId = key.sessionId;
        pair.raceGroupId = key.raceGroupId;
        pair.venue = key.venue;
        pair.eventClass = key.eventClass;
        pair.signedDeltasNs.push_back(saturating_delta(
            lhs->second->timestamps.recvMonoNs,
            rhs->second->timestamps.recvMonoNs));
      }
    }
  }

  for (auto& [key, pair] : sessionPairs) {
    const SourceKey a{
        key.sourceA, key.sessionId, key.raceGroupId, key.venue,
        key.eventClass};
    const SourceKey b{
        key.sourceB, key.sessionId, key.raceGroupId, key.venue,
        key.eventClass};
    const auto& aEvents = sourceEvents[a];
    const auto& bEvents = sourceEvents[b];
    pair.unmatchedA = aEvents.size();
    pair.unmatchedB = bEvents.size();
    for (const auto& event : aEvents)
      if (bEvents.contains(event)) --pair.unmatchedA;
    for (const auto& event : bEvents)
      if (aEvents.contains(event)) --pair.unmatchedB;
    finalize_pair(pair);
    result.perSessionPairs.push_back(pair);
  }

  std::map<PairKey, PairwiseStatistics> pooled;
  for (const auto& session : result.perSessionPairs) {
    const PairKey key{
        session.sourceA, session.sourceB, 0u, session.raceGroupId,
        session.venue, session.eventClass};
    auto& pair = pooled[key];
    pair.sourceA = key.sourceA;
    pair.sourceB = key.sourceB;
    pair.raceGroupId = key.raceGroupId;
    pair.venue = key.venue;
    pair.eventClass = key.eventClass;
    pair.unmatchedA += session.unmatchedA;
    pair.unmatchedB += session.unmatchedB;
    pair.signedDeltasNs.insert(
        pair.signedDeltasNs.end(), session.signedDeltasNs.begin(),
        session.signedDeltasNs.end());
  }
  for (auto& [key, pair] : pooled) {
    (void)key;
    finalize_pair(pair);
    result.pooledPairs.push_back(std::move(pair));
  }

  for (auto& [key, source] : sourceStats) {
    source.uniqueEvents = sourceEvents[key].size();
    source.interArrival = summarize_distribution(interArrivals[key]);
    source.frameBatchSize = summarize_distribution(batchSizes[key]);
    result.sources.push_back(std::move(source));
  }

  using ParticipantArray =
      std::array<const AnalysisSourceDescriptor*, kBybitBboRaceLaneCount>;
  std::map<MultiwayContextKey, ParticipantArray> participants;
  std::set<MultiwayContextKey> invalidParticipantSets;
  for (const auto& descriptor : context.sources) {
    if (descriptor.source.venue != Venue::Bybit ||
        descriptor.bboRaceLane >= kBybitBboRaceLaneCount) {
      continue;
    }
    const MultiwayContextKey key{
        descriptor.source.sessionId, descriptor.source.raceGroupId};
    auto& lanes = participants[key];
    auto& lane = lanes[descriptor.bboRaceLane];
    if (lane != nullptr) {
      invalidParticipantSets.insert(key);
      continue;
    }
    lane = &descriptor;
  }

  std::map<MultiwayContextKey, MultiwayAccumulator> multiway;
  for (const auto& [key, lanes] : participants) {
    if (invalidParticipantSets.contains(key) ||
        std::any_of(lanes.begin(), lanes.end(), [](const auto* lane) {
          return lane == nullptr;
        })) {
      continue;
    }
    MultiwayAccumulator accumulator{};
    accumulator.statistics.sessionId = key.sessionId;
    accumulator.statistics.raceGroupId = key.raceGroupId;
    accumulator.statistics.venue = Venue::Bybit;
    accumulator.statistics.symbol = lanes[0]->symbol;
    accumulator.rollingWindowNs =
        context.rollingWindowNs == 0u ? 10'000'000'000u
                                      : context.rollingWindowNs;
    if (const auto* window = find_session_window(
            context, key.sessionId, key.raceGroupId)) {
      accumulator.measuredStartMonoNs = window->measuredStartMonoNs;
      accumulator.measuredEndMonoNs = window->measuredEndMonoNs;
    }
    std::set<std::uint32_t> uniqueSources;
    for (std::size_t lane = 0u; lane < lanes.size(); ++lane) {
      const auto& descriptor = *lanes[lane];
      accumulator.sourceIds[lane] = descriptor.source.sourceId;
      accumulator.statistics.lanes[lane].sourceId =
          descriptor.source.sourceId;
      accumulator.statistics.lanes[lane].feedId = descriptor.feedId;
      accumulator.statistics.healthClean =
          accumulator.statistics.healthClean && descriptor.healthClean &&
          descriptor.symbol == accumulator.statistics.symbol &&
          uniqueSources.insert(descriptor.source.sourceId).second;
      const SourceKey sourceKey{
          descriptor.source.sourceId, descriptor.source.sessionId,
          descriptor.source.raceGroupId, Venue::Bybit, EventClass::Bbo};
      accumulator.statistics.lanes[lane].eligibleEvents =
          sourceEvents[sourceKey].size();
      for (const auto& [statisticsKey, source] : sourceStats) {
        if (statisticsKey.sourceId == descriptor.source.sourceId &&
            statisticsKey.sessionId == descriptor.source.sessionId &&
            statisticsKey.raceGroupId == descriptor.source.raceGroupId &&
            (source.staleOrInvalid != 0u || source.sequenceGaps != 0u ||
             source.resyncs != 0u)) {
          accumulator.statistics.healthClean = false;
        }
      }
    }
    multiway.emplace(key, std::move(accumulator));
  }

  for (const auto& [eventKey, bySource] : events) {
    if (eventKey.venue != Venue::Bybit ||
        eventKey.eventClass != EventClass::Bbo) {
      continue;
    }
    const MultiwayContextKey key{eventKey.sessionId, eventKey.raceGroupId};
    const auto position = multiway.find(key);
    if (position == multiway.end()) continue;
    auto& accumulator = position->second;
    std::array<const RaceRecord*, kBybitBboRaceLaneCount> laneRecords{};
    std::size_t present = 0u;
    for (std::size_t lane = 0u; lane < laneRecords.size(); ++lane) {
      const auto found = bySource.find(accumulator.sourceIds[lane]);
      if (found != bySource.end()) {
        laneRecords[lane] = found->second;
        ++present;
      }
    }
    if (present == 0u) continue;
    ++accumulator.statistics.unionEvents;
    if (present != kBybitBboRaceLaneCount) {
      ++accumulator.statistics.excludedPartial;
      continue;
    }
    ++accumulator.statistics.matchedAll;
    std::array<std::uint64_t, kBybitBboRaceLaneCount> arrivals{};
    for (std::size_t lane = 0u; lane < arrivals.size(); ++lane)
      arrivals[lane] = laneRecords[lane]->timestamps.recvMonoNs;
    const auto minimum = *std::min_element(arrivals.begin(), arrivals.end());
    auto ordered = arrivals;
    std::sort(ordered.begin(), ordered.end());
    std::uint8_t firstMask = 0u;
    for (std::size_t lane = 0u; lane < arrivals.size(); ++lane) {
      const auto delta = saturating_delta(arrivals[lane], minimum);
      accumulator.arrivalAfterWinner[lane].push_back(delta);
      if (arrivals[lane] == minimum)
        firstMask |= static_cast<std::uint8_t>(1u <<
                                              static_cast<unsigned>(lane));
    }
    const auto firstCount = std::popcount(firstMask);
    if (firstCount == 1) {
      const auto winner = static_cast<std::uint8_t>(std::countr_zero(firstMask));
      ++accumulator.statistics.lanes[winner].wins;
      accumulator.winningLead[winner].push_back(
          saturating_delta(ordered[1], minimum));
    } else {
      ++accumulator.statistics.tieEvents;
      for (std::size_t lane = 0u; lane < arrivals.size(); ++lane) {
        if ((firstMask & static_cast<std::uint8_t>(
                             1u << static_cast<unsigned>(lane))) != 0u)
          ++accumulator.statistics.lanes[lane].tiedFirst;
      }
    }
    if (accumulator.measuredStartMonoNs == 0u)
      accumulator.measuredStartMonoNs = minimum;
    const auto offset = minimum >= accumulator.measuredStartMonoNs
                            ? minimum - accumulator.measuredStartMonoNs
                            : 0u;
    const auto bucket = offset / accumulator.rollingWindowNs;
    auto& rolling = accumulator.rolling[bucket];
    if (rolling.matchedAll == 0u) {
      rolling.sessionId = key.sessionId;
      rolling.raceGroupId = key.raceGroupId;
      rolling.startOffsetNs = bucket * accumulator.rollingWindowNs;
      rolling.endOffsetNs = rolling.startOffsetNs +
                            accumulator.rollingWindowNs;
      for (std::size_t lane = 0u; lane < rolling.lanes.size(); ++lane)
        rolling.lanes[lane].sourceId = accumulator.sourceIds[lane];
    }
    ++rolling.matchedAll;
    for (std::size_t lane = 0u; lane < arrivals.size(); ++lane) {
      if ((firstMask & static_cast<std::uint8_t>(
                           1u << static_cast<unsigned>(lane))) == 0u)
        continue;
      if (firstCount == 1)
        ++rolling.lanes[lane].wins;
      else
        ++rolling.lanes[lane].tiedFirst;
    }
    MultiwayEventSample sample{};
    sample.sessionId = key.sessionId;
    sample.raceGroupId = key.raceGroupId;
    sample.sequence = laneRecords[0]->sequence.sequence;
    sample.offsetFromMeasuredStartNs = offset;
    sample.bbo = laneRecords[0]->bbo;
    sample.firstLaneMask = firstMask;
    for (std::size_t lane = 0u; lane < arrivals.size(); ++lane) {
      sample.arrivalAfterWinnerNs[lane] =
          saturating_delta(arrivals[lane], minimum);
    }
    append_event_sample(accumulator, std::move(sample));
  }

  std::map<std::uint16_t, MultiwayAccumulator> pooledMultiway;
  for (auto& [key, accumulator] : multiway) {
    (void)key;
    for (auto& [bucket, rolling] : accumulator.rolling) {
      (void)bucket;
      std::uint64_t maximum = 0u;
      std::uint8_t leader = kNoBboRaceLane;
      bool tied = false;
      for (std::uint8_t lane = 0u; lane < rolling.lanes.size(); ++lane) {
        const auto wins = rolling.lanes[lane].wins;
        if (wins > maximum) {
          maximum = wins;
          leader = lane;
          tied = false;
        } else if (wins == maximum && wins != 0u) {
          tied = true;
        }
      }
      rolling.leaderLane = maximum == 0u || tied ? kNoBboRaceLane : leader;
      result.rollingWindows.push_back(rolling);
    }
    auto& pooledAccumulator =
        pooledMultiway[accumulator.statistics.raceGroupId];
    if (pooledAccumulator.statistics.venue == Venue::Unknown) {
      pooledAccumulator.statistics.raceGroupId =
          accumulator.statistics.raceGroupId;
      pooledAccumulator.statistics.venue = accumulator.statistics.venue;
      pooledAccumulator.statistics.symbol = accumulator.statistics.symbol;
      pooledAccumulator.sourceIds = accumulator.sourceIds;
      for (std::size_t lane = 0u; lane < kBybitBboRaceLaneCount; ++lane) {
        pooledAccumulator.statistics.lanes[lane].sourceId =
            accumulator.statistics.lanes[lane].sourceId;
        pooledAccumulator.statistics.lanes[lane].feedId =
            accumulator.statistics.lanes[lane].feedId;
      }
    }
    pooledAccumulator.statistics.unionEvents +=
        accumulator.statistics.unionEvents;
    pooledAccumulator.statistics.matchedAll +=
        accumulator.statistics.matchedAll;
    pooledAccumulator.statistics.excludedPartial +=
        accumulator.statistics.excludedPartial;
    pooledAccumulator.statistics.tieEvents +=
        accumulator.statistics.tieEvents;
    pooledAccumulator.statistics.healthClean =
        pooledAccumulator.statistics.healthClean &&
        accumulator.statistics.healthClean;
    for (std::size_t lane = 0u; lane < kBybitBboRaceLaneCount; ++lane) {
      pooledAccumulator.statistics.lanes[lane].eligibleEvents +=
          accumulator.statistics.lanes[lane].eligibleEvents;
      pooledAccumulator.statistics.lanes[lane].wins +=
          accumulator.statistics.lanes[lane].wins;
      pooledAccumulator.statistics.lanes[lane].tiedFirst +=
          accumulator.statistics.lanes[lane].tiedFirst;
      auto& pooledArrivals = pooledAccumulator.arrivalAfterWinner[lane];
      auto& pooledLeads = pooledAccumulator.winningLead[lane];
      pooledArrivals.insert(
          pooledArrivals.end(), accumulator.arrivalAfterWinner[lane].begin(),
          accumulator.arrivalAfterWinner[lane].end());
      pooledLeads.insert(
          pooledLeads.end(), accumulator.winningLead[lane].begin(),
          accumulator.winningLead[lane].end());
    }
    merge_event_samples(
        pooledAccumulator, accumulator.statistics.eventSamples);
    finalize_multiway(accumulator);
    result.perSessionMultiway.push_back(std::move(accumulator.statistics));
  }
  for (auto& [group, accumulator] : pooledMultiway) {
    (void)group;
    finalize_multiway(accumulator);
    result.pooledMultiway.push_back(std::move(accumulator.statistics));
  }
  build_recommendations(result);
  return result;
}

}  // namespace exchange_probe::race
