#include "exchange_probe/race/Analysis.hpp"
#include "exchange_probe/race/Report.hpp"

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

exchange_probe::race::RaceRecord bbo_record(
    std::uint32_t sourceId, std::uint16_t sessionId,
    std::uint64_t recvMonoNs, std::uint64_t sequence,
    exchange_probe::race::BboState bbo, std::uint8_t mask,
    exchange_probe::race::Venue venue = exchange_probe::race::Venue::Bitget) {
  using namespace exchange_probe::race;
  RaceRecord record{};
  record.source.sourceId = sourceId;
  record.source.connectionId = sourceId;
  record.source.sessionId = sessionId;
  record.source.raceGroupId = 7u;
  record.source.venue = venue;
  record.source.wire = sourceId == 1u ? Wire::Sbe : Wire::Json;
  record.eventClass = EventClass::Bbo;
  record.validity = BookValidity::Valid;
  record.changeMask = mask;
  record.timestamps.recvMonoNs = recvMonoNs;
  record.sequence.sequence = sequence;
  record.bbo = bbo;
  record.frameBatchCount = 1u;
  return record;
}

}  // namespace

int main() {
  using namespace exchange_probe::race;
  constexpr BboState initial{100, 2, 101, 3};
  constexpr BboState first{100, 1, 101, 2};
  constexpr BboState second{100, 1, 102, 4};
  std::vector<RaceRecord> records{
      bbo_record(1u, 1u, 100u, 10u, initial, kBidPriceChanged),
      bbo_record(2u, 1u, 110u, 10u, initial, kBidPriceChanged),
      bbo_record(
          1u, 1u, 200u, 11u, first,
          kBidQuantityChanged | kAskQuantityChanged),
      bbo_record(
          2u, 1u, 230u, 11u, first,
          kBidQuantityChanged | kAskQuantityChanged),
      bbo_record(
          1u, 1u, 310u, 12u, second,
          kAskPriceChanged | kAskQuantityChanged),
      bbo_record(
          2u, 1u, 300u, 12u, second,
          kAskPriceChanged | kAskQuantityChanged),
      bbo_record(1u, 2u, 1'000u, 20u, initial, kBidPriceChanged),
      bbo_record(2u, 2u, 1'010u, 20u, initial, kBidPriceChanged),
      bbo_record(
          1u, 2u, 1'100u, 21u, first,
          kBidQuantityChanged | kAskQuantityChanged),
      bbo_record(
          2u, 2u, 1'150u, 21u, first,
          kBidQuantityChanged | kAskQuantityChanged),
  };
  auto duplicate = records[2];
  duplicate.timestamps.recvMonoNs = 205u;
  records.push_back(duplicate);
  auto invalid = records[4];
  invalid.timestamps.recvMonoNs = 320u;
  invalid.validity = BookValidity::InvalidGap;
  records.push_back(invalid);
  records.push_back(
      bbo_record(3u, 1u, 120u, 30u, initial, kBidPriceChanged));
  constexpr BboState unmatchedState{99, 8, 103, 9};
  records.push_back(bbo_record(
      3u, 1u, 400u, 31u, unmatchedState,
      kBidPriceChanged | kBidQuantityChanged | kAskPriceChanged |
          kAskQuantityChanged));

  const auto analysis = analyze_records(records);
  assert(analysis.inputRecords == records.size());
  assert(analysis.perSessionPairs.size() == 4u);
  assert(analysis.pooledPairs.size() == 3u);
  const auto pooled = std::find_if(
      analysis.pooledPairs.begin(), analysis.pooledPairs.end(),
      [](const auto& pair) {
        return pair.sourceA == 1u && pair.sourceB == 2u;
      });
  assert(pooled != analysis.pooledPairs.end());
  assert(pooled->matched == 3u);
  assert(pooled->aFirst == 2u && pooled->bFirst == 1u && pooled->ties == 0u);
  assert(pooled->deltaRecvANsMinusB.minimumNs == -50);
  assert(pooled->deltaRecvANsMinusB.maximumNs == 10);
  const auto unmatched = std::find_if(
      analysis.pooledPairs.begin(), analysis.pooledPairs.end(),
      [](const auto& pair) {
        return pair.sourceA == 1u && pair.sourceB == 3u;
      });
  assert(unmatched != analysis.pooledPairs.end());
  assert(unmatched->matched == 0u);
  assert(unmatched->unmatchedA == 2u && unmatched->unmatchedB == 1u);

  // The same old->new transition may occur more than once. Exchange sequence
  // identity must keep those occurrences separate instead of marking the
  // later one as a duplicate of the earlier transition.
  std::vector<RaceRecord> repeated{
      bbo_record(1u, 1u, 1'000u, 40u, initial, kBidPriceChanged),
      bbo_record(2u, 1u, 1'010u, 40u, initial, kBidPriceChanged),
      bbo_record(1u, 1u, 1'100u, 41u, first, kBidQuantityChanged),
      bbo_record(2u, 1u, 1'110u, 41u, first, kBidQuantityChanged),
      bbo_record(1u, 1u, 1'200u, 42u, initial, kBidQuantityChanged),
      bbo_record(2u, 1u, 1'210u, 42u, initial, kBidQuantityChanged),
      bbo_record(1u, 1u, 1'300u, 43u, first, kBidQuantityChanged),
      bbo_record(2u, 1u, 1'310u, 43u, first, kBidQuantityChanged),
  };
  const auto repeatedAnalysis = analyze_records(repeated);
  assert(repeatedAnalysis.pooledPairs.size() == 1u);
  assert(repeatedAnalysis.pooledPairs[0].matched == 3u);
  assert(repeatedAnalysis.sources[0].duplicates == 0u);

  AnalysisContext bybitContext{};
  bybitContext.rollingWindowNs = 100u;
  for (std::uint8_t lane = 0u; lane < kBybitBboRaceLaneCount; ++lane) {
    AnalysisSourceDescriptor descriptor{};
    descriptor.source.sourceId = 10u + lane;
    descriptor.source.sessionId = 9u;
    descriptor.source.raceGroupId = 3u;
    descriptor.source.venue = Venue::Bybit;
    descriptor.symbol = "BTRUSDT";
    descriptor.feedId = "BTRUSDT/orderbook_" +
                        std::to_string(std::array{1, 50, 200, 1000}[lane]);
    descriptor.bboRaceLane = lane;
    bybitContext.sources.push_back(std::move(descriptor));
  }
  bybitContext.sessionWindows.push_back(
      AnalysisSessionWindow{9u, 3u, 1'000u, 2'000u});
  std::vector<RaceRecord> bybitRecords;
  constexpr std::array<std::uint64_t, 4u> initialArrivals{
      1'000u, 1'010u, 1'020u, 1'030u};
  constexpr std::array<std::uint64_t, 4u> eventOneArrivals{
      1'140u, 1'130u, 1'150u, 1'160u};
  constexpr std::array<std::uint64_t, 4u> eventTwoArrivals{
      1'220u, 1'220u, 1'250u, 1'260u};
  for (std::size_t lane = 0u; lane < 4u; ++lane) {
    const auto source = static_cast<std::uint32_t>(10u + lane);
    bybitRecords.push_back(bbo_record(
        source, 9u, initialArrivals[lane], 100u, initial,
        kBidPriceChanged, Venue::Bybit));
    bybitRecords.back().source.raceGroupId = 3u;
    bybitRecords.push_back(bbo_record(
        source, 9u, eventOneArrivals[lane], 101u, first,
        kBidQuantityChanged | kAskQuantityChanged, Venue::Bybit));
    bybitRecords.back().source.raceGroupId = 3u;
    bybitRecords.push_back(bbo_record(
        source, 9u, eventTwoArrivals[lane], 102u, second,
        kAskPriceChanged | kAskQuantityChanged, Venue::Bybit));
    bybitRecords.back().source.raceGroupId = 3u;
  }
  // A valid transition seen by only three feeds stays visible as excluded
  // coverage and can never enter the podium.
  constexpr BboState partial{99, 7, 103, 8};
  for (std::size_t lane = 0u; lane < 3u; ++lane) {
    auto record = bbo_record(
        static_cast<std::uint32_t>(10u + lane), 9u, 1'350u + lane,
        103u, partial,
        kBidPriceChanged | kBidQuantityChanged | kAskPriceChanged |
            kAskQuantityChanged,
        Venue::Bybit);
    record.source.raceGroupId = 3u;
    bybitRecords.push_back(record);
  }
  const auto bybitAnalysis = analyze_records(bybitRecords, bybitContext);
  assert(bybitAnalysis.perSessionMultiway.size() == 1u);
  assert(bybitAnalysis.pooledMultiway.size() == 1u);
  const auto& fourWay = bybitAnalysis.perSessionMultiway.front();
  assert(fourWay.symbol == "BTRUSDT");
  assert(fourWay.unionEvents == 3u);
  assert(fourWay.matchedAll == 2u);
  assert(fourWay.excludedPartial == 1u);
  assert(fourWay.tieEvents == 1u);
  assert(fourWay.lanes[1].wins == 1u);
  assert(fourWay.lanes[0].tiedFirst == 1u);
  assert(fourWay.lanes[1].tiedFirst == 1u);
  assert(fourWay.lanes[1].winningLead.medianNs == 10);
  assert(fourWay.lanes[3].arrivalAfterWinner.maximumNs == 40);
  assert(fourWay.eventSamples.size() == 2u);
  assert(bybitAnalysis.rollingWindows.size() == 2u);
  assert(bybitAnalysis.recommendations.size() == 1u);
  assert(bybitAnalysis.recommendations.front().status ==
         RecommendationStatus::InsufficientEvidence);

  const auto output = std::filesystem::temp_directory_path() /
                      "exchange-feed-race-report-test";
  std::error_code cleanupError;
  std::filesystem::remove_all(output, cleanupError);
  std::string error;
  assert(write_analysis_report(bybitAnalysis, output, error));
  assert(error.empty());
  assert(std::filesystem::exists(output / "report.md"));
  assert(std::filesystem::exists(output / "dashboard.html"));
  assert(std::filesystem::exists(output / "plots/race_overview.svg"));
  assert(std::filesystem::exists(output / "plots/rolling_leader.svg"));
  assert(std::filesystem::exists(output / "plots/coverage_health.svg"));
  assert(std::filesystem::exists(output / "plots/pairwise_matrix.svg"));
  assert(std::filesystem::exists(output / "plots/pairwise_win_rate.svg"));
  assert(std::filesystem::exists(output / "plots/bitget_json_vs_sbe.svg"));
  assert(std::filesystem::exists(output / "plots/bbo_source_origins.svg"));
  std::ifstream report{output / "report.md", std::ios::binary};
  const std::string text{
      std::istreambuf_iterator<char>{report},
      std::istreambuf_iterator<char>{}};
  assert(text.find("recv_A - recv_B") != std::string::npos);
  assert(text.find("Strict 4/4 events: 2") != std::string::npos);
  assert(text.find("Excluded partial events: 1") != std::string::npos);
  assert(text.find("publicTrade") != std::string::npos);
  assert(text.find("insufficient evidence") != std::string::npos);
  std::ifstream dashboard{output / "dashboard.html", std::ios::binary};
  const std::string dashboardText{
      std::istreambuf_iterator<char>{dashboard},
      std::istreambuf_iterator<char>{}};
  assert(dashboardText.find("Where is the race?") != std::string::npos);
  assert(dashboardText.find("Export race PNG") != std::string::npos);
  assert(dashboardText.find("orderbook.1000 - derived BBO") !=
         std::string::npos);
  assert(dashboardText.find("publicTrade is separate") != std::string::npos);
  std::filesystem::remove_all(output, cleanupError);
}
