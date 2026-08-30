#include "exchange_probe/race/report.hpp"

#include "race_dashboard.hpp"
#include "race_svg.hpp"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <map>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace exchange_probe::race {
namespace {

[[nodiscard]] const char* venue_name(Venue venue) noexcept {
  switch (venue) {
    case Venue::Bitget: return "Bitget";
    case Venue::Bybit: return "Bybit";
    case Venue::Gate: return "Gate";
    case Venue::Okx: return "OKX";
    case Venue::Kucoin: return "KuCoin";
    case Venue::BinanceUsdM: return "Binance USD-M";
    case Venue::Aster: return "Aster";
    default: return "Unknown";
  }
}

[[nodiscard]] const char* event_name(EventClass eventClass) noexcept {
  switch (eventClass) {
    case EventClass::Trade: return "trade";
    case EventClass::Bbo: return "bbo";
    case EventClass::Top5: return "top5";
    case EventClass::Top50: return "top50";
    case EventClass::Depth: return "depth";
    default: return "other";
  }
}

[[nodiscard]] std::string feed_name(const std::string& feedId) {
  const auto slash = feedId.find('/');
  const auto suffix = slash == std::string::npos
                          ? feedId
                          : feedId.substr(slash + 1u);
  if (suffix == "orderbook_1") return "orderbook.1 (direct BBO)";
  if (suffix == "orderbook_50") return "orderbook.50 (derived BBO)";
  if (suffix == "orderbook_200") return "orderbook.200 (derived BBO)";
  if (suffix == "orderbook_1000") return "orderbook.1000 (derived BBO)";
  if (suffix == "public_trade") return "publicTrade (separate)";
  return suffix;
}

[[nodiscard]] std::string source_label(
    const AnalysisResult& analysis, std::uint32_t sourceId,
    std::uint16_t raceGroupId) {
  for (const auto& descriptor : analysis.sourceDescriptors) {
    if (descriptor.source.sourceId == sourceId &&
        descriptor.source.raceGroupId == raceGroupId) {
      return feed_name(descriptor.feedId);
    }
  }
  return "source " + std::to_string(sourceId);
}

[[nodiscard]] std::string pair_label(const PairwiseStatistics& pair) {
  return std::to_string(pair.sourceA) + "-" +
         std::to_string(pair.sourceB) + "/" + event_name(pair.eventClass);
}

[[nodiscard]] long double microseconds(std::int64_t nanoseconds) noexcept {
  return static_cast<long double>(nanoseconds) / 1'000.0L;
}

[[nodiscard]] long double win_percent(const PairwiseStatistics& pair) noexcept {
  if (pair.matched == 0u) return 0.0L;
  return 100.0L * static_cast<long double>(pair.aFirst) /
         static_cast<long double>(pair.matched);
}

[[nodiscard]] std::vector<std::pair<std::string, long double>> pair_values(
    const std::vector<PairwiseStatistics>& pairs,
    long double (*value)(const PairwiseStatistics&) noexcept,
    Venue filter = Venue::Unknown) {
  std::vector<std::pair<std::string, long double>> output;
  for (const auto& pair : pairs) {
    if (filter != Venue::Unknown && pair.venue != filter) continue;
    output.emplace_back(pair_label(pair), value(pair));
    if (output.size() == 32u) break;
  }
  return output;
}

[[nodiscard]] long double median_us(
    const PairwiseStatistics& pair) noexcept {
  return microseconds(pair.deltaRecvANsMinusB.medianNs);
}

[[nodiscard]] long double p99_us(
    const PairwiseStatistics& pair) noexcept {
  return microseconds(pair.deltaRecvANsMinusB.p99Ns);
}

[[nodiscard]] std::vector<std::pair<std::string, long double>>
cross_wire_medians(
    const AnalysisResult& analysis, Venue venue) {
  std::map<std::uint32_t, Wire> wires;
  for (const auto& source : analysis.sources)
    wires[source.source.sourceId] = source.source.wire;
  std::vector<std::pair<std::string, long double>> output;
  for (const auto& pair : analysis.pooledPairs) {
    if (pair.venue != venue || !wires.contains(pair.sourceA) ||
        !wires.contains(pair.sourceB) ||
        wires[pair.sourceA] == wires[pair.sourceB]) {
      continue;
    }
    output.emplace_back(pair_label(pair), median_us(pair));
    if (output.size() == 32u) break;
  }
  return output;
}

[[nodiscard]] bool plot_all(
    const AnalysisResult& analysis, const std::filesystem::path& plots,
    std::string& error) {
  if (!svg::write_bars(
          plots / "pairwise_win_rate.svg", "Pairwise A-first win rate",
          "percent; ties remain in denominator",
          pair_values(analysis.pooledPairs, win_percent), error) ||
      !svg::write_bars(
          plots / "pairwise_median_lead.svg", "Pairwise signed median",
          "microseconds; negative means source A arrived first",
          pair_values(analysis.pooledPairs, median_us), error)) {
    return false;
  }
  const std::vector<std::int64_t> empty;
  const auto& mainDeltas = analysis.pooledPairs.empty()
                               ? empty
                               : analysis.pooledPairs.front().signedDeltasNs;
  if (!svg::write_ecdf(
          plots / "ecdf_main_pair.svg", "Signed latency ECDF: first pair",
          mainDeltas, error) ||
      !svg::write_bars(
          plots / "percentile_comparison.svg", "Pairwise signed p99",
          "microseconds; sign preserved",
          pair_values(analysis.pooledPairs, p99_us), error)) {
    return false;
  }
  std::vector<std::pair<std::string, long double>> shares;
  std::vector<std::pair<std::string, long double>> arrivals;
  std::vector<std::pair<std::string, long double>> reliability;
  for (const auto& source : analysis.sources) {
    const auto label = std::to_string(source.source.sourceId) + "/" +
                       event_name(source.eventClass);
    shares.emplace_back(label, source.firstArrivals);
    arrivals.emplace_back(
        label, microseconds(source.interArrival.medianNs));
    reliability.emplace_back(
        label, static_cast<long double>(source.sequenceGaps + source.resyncs));
  }
  if (!svg::write_bars(
          plots / "first_arrival_share.svg", "First-arrival counts",
          "matched event wins (ties resolve only for display)", shares,
          error) ||
      !svg::write_bars(
          plots / "inter_arrival_batching.svg", "Median inter-arrival",
          "microseconds", arrivals, error) ||
      !svg::write_bars(
          plots / "gaps_resync.svg", "Sequence gaps and resyncs",
          "count", reliability, error)) {
    return false;
  }
  std::vector<std::pair<std::string, long double>> sessions;
  for (const auto& pair : analysis.perSessionPairs) {
    sessions.emplace_back(
        "s" + std::to_string(pair.sessionId) + "/" + pair_label(pair),
        median_us(pair));
    if (sessions.size() == 48u) break;
  }
  if (!svg::write_bars(
          plots / "per_session.svg", "Per-session signed median",
          "microseconds", sessions, error)) {
    return false;
  }
  std::vector<std::pair<std::string, std::int64_t>> timeline;
  if (!analysis.pooledPairs.empty()) {
    auto values = analysis.pooledPairs.front().signedDeltasNs;
    std::sort(values.begin(), values.end());
    const auto limit = std::min<std::size_t>(values.size(), 12u);
    for (std::size_t index = 0u; index < limit; ++index)
      timeline.emplace_back("event " + std::to_string(index + 1u), values[index]);
  }
  if (!svg::write_timeline(
          plots / "event_timeline.svg", "Example matched-event deltas",
          timeline, error) ||
      !svg::write_bars(
          plots / "bitget_json_vs_sbe.svg", "Bitget source pairs",
          "signed median, microseconds",
          cross_wire_medians(analysis, Venue::Bitget), error) ||
      !svg::write_bars(
          plots / "gate_json_vs_sbe.svg", "Gate source pairs",
          "signed median, microseconds",
          cross_wire_medians(analysis, Venue::Gate), error)) {
    return false;
  }
  std::vector<std::pair<std::string, long double>> origins;
  for (const auto& source : analysis.sources) {
    if (source.eventClass != EventClass::Bbo) continue;
    origins.emplace_back(
        std::to_string(source.source.sourceId) + "/origin" +
            std::to_string(static_cast<unsigned>(source.source.origin)),
        source.firstArrivals);
  }
  return svg::write_bars(
      plots / "bbo_source_origins.svg",
      "Direct vs depth-derived vs trade-derived BBO",
      "first-arrival count by source origin", origins, error);
}

}  // namespace

bool write_analysis_report(
    const AnalysisResult& analysis, const std::filesystem::path& directory,
    std::string& error) {
  std::error_code filesystemError;
  const auto plots = directory / "plots";
  std::filesystem::create_directories(plots, filesystemError);
  if (filesystemError) {
    error = "report_directory:" + filesystemError.message();
    return false;
  }
  if (!plot_all(analysis, plots, error)) return false;
  if (!write_race_dashboard(analysis, directory, error)) return false;
  std::ofstream report{directory / "report.md", std::ios::binary};
  if (!report) {
    error = "report_open_failed";
    return false;
  }
  report << "# Exchange feed race report\n\n"
         << "Open the [interactive dashboard](dashboard.html) first. It shows "
            "the four Bybit BBO lanes, event scrubber, rolling leader, podium, "
            "coverage and capture health.\n\n"
         << "A strict race event is the same valid BBO transition and Bybit "
            "sequence observed in all four orderbook feeds in one session. "
            "Partial events are counted as excluded coverage and never enter "
            "the podium. Public trades are reported separately.\n\n"
         << "- Input records: " << analysis.inputRecords << "\n"
         << "- Eligible exact/transition records: " << analysis.eligibleRecords
         << "\n- Per-session pairs: " << analysis.perSessionPairs.size()
         << "\n- Pooled pairs: " << analysis.pooledPairs.size() << "\n\n";
  report << std::fixed << std::setprecision(3);
  report << "## Strict Bybit four-feed BBO race\n\n";
  if (analysis.pooledMultiway.empty()) {
    report << "No complete four-feed Bybit source set was found. This is "
              "insufficient evidence, not a zero-latency tie.\n\n";
  }
  for (const auto& race : analysis.pooledMultiway) {
    const auto coverage = race.unionEvents == 0u
        ? 0.0L
        : 100.0L * static_cast<long double>(race.matchedAll) /
              static_cast<long double>(race.unionEvents);
    report << "### " << race.symbol << "\n\n"
           << "- Strict 4/4 events: " << race.matchedAll << "\n"
           << "- Union events: " << race.unionEvents << "\n"
           << "- Excluded partial events: " << race.excludedPartial << "\n"
           << "- Strict coverage: " << coverage << "%\n"
           << "- Capture health: "
           << (race.healthClean ? "clean" : "degraded") << "\n"
           << "- Tie events: " << race.tieEvents << "\n\n"
           << "| Lane | Wins | Tied first | Eligible events | Delay after winner p50 us | p95 us | p99 us | Winning lead p50 us |\n"
           << "|---|---:|---:|---:|---:|---:|---:|---:|\n";
    for (const auto& lane : race.lanes) {
      report << "| " << feed_name(lane.feedId) << " | " << lane.wins
             << " | " << lane.tiedFirst << " | " << lane.eligibleEvents
             << " | " << microseconds(lane.arrivalAfterWinner.medianNs)
             << " | " << microseconds(lane.arrivalAfterWinner.p95Ns)
             << " | " << microseconds(lane.arrivalAfterWinner.p99Ns)
             << " | " << microseconds(lane.winningLead.medianNs)
             << " |\n";
    }
    const auto recommendation = std::find_if(
        analysis.recommendations.begin(), analysis.recommendations.end(),
        [&race](const auto& candidate) {
          return candidate.raceGroupId == race.raceGroupId &&
                 candidate.symbol == race.symbol;
        });
    report << "\n**Decision:** ";
    if (recommendation != analysis.recommendations.end() &&
        recommendation->status == RecommendationStatus::ObservedCandidate) {
      report << "observed candidate only; primary `"
             << source_label(
                    analysis, recommendation->primarySourceId,
                    recommendation->raceGroupId)
             << "`, complement `"
             << source_label(
                    analysis, recommendation->complementSourceId,
                    recommendation->raceGroupId)
             << "`. This is not a production selection.\n\n";
    } else {
      report << "insufficient evidence (`"
             << (recommendation == analysis.recommendations.end()
                     ? "missing_strict_four_feed_evidence"
                     : recommendation->reason)
             << "`).\n\n";
    }
  }
  report << "## Pooled pairwise technical matrix\n\n"
         << "Signed delta is `recv_A - recv_B`; negative means A arrived "
            "first. Pairwise results are diagnostic and do not replace the "
            "strict 4/4 podium.\n\n"
         << "| Venue | Group | Event | A | B | Matched | Unmatched A | Unmatched B | A first | B first | Ties | Median us | p99 us |\n"
         << "|---|---:|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|\n";
  for (const auto& pair : analysis.pooledPairs) {
    report << "| " << venue_name(pair.venue) << " | " << pair.raceGroupId
           << " | " << event_name(pair.eventClass) << " | "
           << source_label(analysis, pair.sourceA, pair.raceGroupId)
           << " | "
           << source_label(analysis, pair.sourceB, pair.raceGroupId)
           << " | " << pair.matched << " | "
           << pair.unmatchedA << " | " << pair.unmatchedB << " | "
           << pair.aFirst << " | " << pair.bFirst << " | " << pair.ties
           << " | " << microseconds(pair.deltaRecvANsMinusB.medianNs)
           << " | " << microseconds(pair.deltaRecvANsMinusB.p99Ns)
           << " |\n";
  }
  report << "\n## Session stability\n\n"
         << "| Session | Venue | Group | Event | A | B | Matched | Median us | p10 | p25 | p75 | p90 | p95 | p99 | p99.9 | Min | Max |\n"
         << "|---:|---|---:|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|\n";
  for (const auto& pair : analysis.perSessionPairs) {
    const auto& d = pair.deltaRecvANsMinusB;
    report << "| " << pair.sessionId << " | " << venue_name(pair.venue)
           << " | " << pair.raceGroupId << " | "
           << event_name(pair.eventClass) << " | " << pair.sourceA << " | "
           << pair.sourceB << " | " << pair.matched << " | "
           << microseconds(d.medianNs) << " | " << microseconds(d.p10Ns)
           << " | " << microseconds(d.p25Ns) << " | "
           << microseconds(d.p75Ns) << " | " << microseconds(d.p90Ns)
           << " | " << microseconds(d.p95Ns) << " | "
           << microseconds(d.p99Ns) << " | " << microseconds(d.p999Ns)
           << " | " << microseconds(d.minimumNs) << " | "
           << microseconds(d.maximumNs) << " |\n";
  }
  report << "\n## Source reliability and batching\n\n"
         << "| Source | Session | Group | Event | Records | Unique | Duplicates | Invalid/stale | Gaps | Resyncs | First arrivals | Inter-arrival p50 us | Batch p50 | Batch p99 |\n"
         << "|---:|---:|---:|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|\n";
  for (const auto& source : analysis.sources) {
    report << "| " << source.source.sourceId << " | "
           << source.source.sessionId << " | " << source.source.raceGroupId
           << " | " << event_name(source.eventClass) << " | "
           << source.records << " | " << source.uniqueEvents << " | "
           << source.duplicates << " | " << source.staleOrInvalid << " | "
           << source.sequenceGaps << " | " << source.resyncs << " | "
           << source.firstArrivals << " | "
           << microseconds(source.interArrival.medianNs) << " | "
           << source.frameBatchSize.medianNs << " | "
           << source.frameBatchSize.p99Ns << " |\n";
  }
  report << "\n## Plots\n\n"
         << "- [Strict BBO podium](plots/race_overview.svg)\n"
         << "- [Rolling leader](plots/rolling_leader.svg)\n"
         << "- [Coverage and health](plots/coverage_health.svg)\n"
         << "- [Readable pairwise matrix](plots/pairwise_matrix.svg)\n"
         << "- [Pairwise win rate](plots/pairwise_win_rate.svg)\n"
         << "- [Pairwise median lead](plots/pairwise_median_lead.svg)\n"
         << "- [ECDF](plots/ecdf_main_pair.svg)\n"
         << "- [Percentiles](plots/percentile_comparison.svg)\n"
         << "- [First arrival share](plots/first_arrival_share.svg)\n"
         << "- [Per session](plots/per_session.svg)\n"
         << "- [Inter-arrival and batching](plots/inter_arrival_batching.svg)\n"
         << "- [Gaps and resync](plots/gaps_resync.svg)\n"
         << "- [Event timeline](plots/event_timeline.svg)\n"
         << "- [Bitget JSON vs SBE](plots/bitget_json_vs_sbe.svg)\n"
         << "- [Gate JSON vs SBE](plots/gate_json_vs_sbe.svg)\n"
         << "- [BBO source origins](plots/bbo_source_origins.svg)\n\n"
         << "Observed arrival order does not prove exchange matcher latency, "
            "future stability, execution quality or profitability.\n";
  report.flush();
  if (!report) {
    error = "report_write_failed";
    return false;
  }
  return true;
}

}  // namespace exchange_probe::race
