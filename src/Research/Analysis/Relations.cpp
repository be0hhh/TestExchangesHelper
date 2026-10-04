#include "ResearchAnalysisInternal.hpp"

namespace exchange_probe::research_analysis {

[[nodiscard]] bool emit_relation(
    std::ofstream& output,
    std::uint64_t& relation_id,
    bool& output_full,
    std::string_view mode,
    std::string_view evidence,
    const NormalizedEvent& source,
    const NormalizedEvent& target,
    std::string_view reason) {
  if (relation_id >= kMaximumRelations) output_full = true;
  if (output_full) return false;
  boost::json::object row{
      {"schema", kResearchRelationSchema},
      {"relation_id", relation_id++},
      {"mode", mode},
      {"evidence", evidence},
      {"source_event_id", source.id},
      {"target_event_id", target.id},
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
  return true;
}

}
