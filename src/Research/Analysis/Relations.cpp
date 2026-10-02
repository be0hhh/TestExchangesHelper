#include "ResearchAnalysisInternal.hpp"

namespace exchange_probe::research_analysis {

[[nodiscard]] boost::json::object distribution_json(
    std::vector<std::uint64_t> values) {
  static constexpr std::uint64_t boundaries_ns[]{
      50'000U, 100'000U, 250'000U, 500'000U, 1'000'000U,
      2'000'000U, 5'000'000U, 10'000'000U, 25'000'000U,
      50'000'000U, 100'000'000U, 250'000'000U, 1'000'000'000U,
  };
  boost::json::array buckets;
  if (values.empty()) {
    return {
        {"count", 0},
        {"unit", "ns"},
        {"buckets", std::move(buckets)},
    };
  }
  std::sort(values.begin(), values.end());
  const auto percentile = [&](std::size_t percent) {
    return values[((values.size() - 1U) * percent) / 100U];
  };
  std::size_t previous = 0U;
  for (const auto boundary : boundaries_ns) {
    const auto end = static_cast<std::size_t>(
        std::upper_bound(values.begin(), values.end(), boundary) -
        values.begin());
    buckets.emplace_back(boost::json::object{
        {"le_ns", boundary},
        {"count", end - previous},
    });
    previous = end;
  }
  buckets.emplace_back(boost::json::object{
      {"le_ns", nullptr},
      {"count", values.size() - previous},
  });
  return {
      {"count", values.size()},
      {"unit", "ns"},
      {"min", values.front()},
      {"p50", percentile(50U)},
      {"p95", percentile(95U)},
      {"p99", percentile(99U)},
      {"max", values.back()},
      {"buckets", std::move(buckets)},
  };
}

[[nodiscard]] bool emit_relation(
    std::ofstream& output,
    std::uint64_t& relation_id,
    bool& output_full,
    std::map<std::string, std::vector<std::uint64_t>>& lag_distributions,
    std::string_view mode,
    std::string_view evidence,
    const NormalizedEvent& source,
    const NormalizedEvent& target,
    std::string_view reason) {
  if (relation_id >= kMaximumRelations) output_full = true;
  if (output_full) return false;
  const auto lag =
      target.monotonic_ns >= source.monotonic_ns
          ? static_cast<std::int64_t>(
                target.monotonic_ns - source.monotonic_ns)
          : -static_cast<std::int64_t>(
                source.monotonic_ns - target.monotonic_ns);
  boost::json::object row{
      {"schema", kResearchRelationSchema},
      {"relation_id", relation_id++},
      {"mode", mode},
      {"evidence", evidence},
      {"source_event_id", source.id},
      {"target_event_id", target.id},
      {"receive_lag_ns", lag},
      {"reason", reason},
      {"causality", "not_asserted"},
  };
  const auto line = boost::json::serialize(row) + "\n";
  const auto position = output.tellp();
  const auto offset = static_cast<std::streamoff>(position);
  if (offset < 0 ||
      static_cast<std::uint64_t>(offset) +
              static_cast<std::uint64_t>(line.size()) >
          kMaximumDerivedJsonlBytes) {
    output_full = true;
    return false;
  }
  output << line;
  const auto absolute_lag =
      lag >= 0 ? static_cast<std::uint64_t>(lag)
               : static_cast<std::uint64_t>(-(lag + 1)) + 1U;
  lag_distributions[
      std::string{mode} + ":" + source.channel + "->" + target.channel]
      .push_back(absolute_lag);
  return true;
}

}
