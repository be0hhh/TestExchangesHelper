#include "exchange_probe/App.hpp"

#include <boost/json/array.hpp>
#include <boost/json/object.hpp>
#include <boost/json/parse.hpp>
#include <boost/json/serialize.hpp>
#include <boost/system/error_code.hpp>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <ostream>
#include <string>
#include <utility>
#include <vector>

namespace exchange_probe {
namespace {

inline constexpr std::size_t kMaxCompareFileBytes = 8U * 1024U * 1024U;
inline constexpr std::string_view kBundleSchema =
    "exchange.api_probe.bundle.v2";

struct Candidate {
  std::filesystem::path bundle;
  std::string owner;
  std::string host;
  std::string route;
  std::string connection;
  std::string compatibility;
  std::uint64_t samples{0U};
  std::uint64_t successful{0U};
  std::uint64_t failures{0U};
  std::uint64_t p50_us{0U};
  std::uint64_t p99_us{0U};
  double jitter_us{0.0};
  bool artifact_complete{false};
  bool eligible{false};
  unsigned pareto_front{0U};
  std::string exclusion_reason;
};

[[nodiscard]] std::optional<boost::json::object> read_object(
    const std::filesystem::path& path,
    std::string& error) {
  std::error_code size_error;
  const auto size = std::filesystem::file_size(path, size_error);
  if (size_error || size > kMaxCompareFileBytes) {
    error = size_error ? "file_size_failed:" + size_error.message()
                       : "file_capacity_exceeded";
    return std::nullopt;
  }
  std::ifstream input{path, std::ios::binary};
  if (!input) {
    error = "open_failed";
    return std::nullopt;
  }
  const std::string text{
      std::istreambuf_iterator<char>{input},
      std::istreambuf_iterator<char>{}};
  boost::system::error_code parse_error;
  auto value = boost::json::parse(text, parse_error);
  if (parse_error || !value.is_object()) {
    error = "invalid_json";
    return std::nullopt;
  }
  return value.as_object();
}

[[nodiscard]] std::string string_member(
    const boost::json::object& object,
    std::string_view key) {
  const auto* value = object.if_contains(key);
  return value != nullptr && value->is_string()
             ? std::string{value->as_string()}
             : std::string{};
}

[[nodiscard]] std::uint64_t u64_member(
    const boost::json::object& object,
    std::string_view key) {
  const auto* value = object.if_contains(key);
  if (value == nullptr) {
    return 0U;
  }
  if (value->is_uint64()) {
    return value->as_uint64();
  }
  if (value->is_int64() && value->as_int64() >= 0) {
    return static_cast<std::uint64_t>(value->as_int64());
  }
  return 0U;
}

[[nodiscard]] double double_member(
    const boost::json::object& object,
    std::string_view key) {
  const auto* value = object.if_contains(key);
  if (value == nullptr) {
    return 0.0;
  }
  if (value->is_double()) {
    return value->as_double();
  }
  if (value->is_uint64()) {
    return static_cast<double>(value->as_uint64());
  }
  if (value->is_int64()) {
    return static_cast<double>(value->as_int64());
  }
  return 0.0;
}

[[nodiscard]] bool bool_member(
    const boost::json::object& object,
    std::string_view key) {
  const auto* value = object.if_contains(key);
  return value != nullptr && value->is_bool() && value->as_bool();
}

[[nodiscard]] std::optional<Candidate> load_candidate(
    const std::filesystem::path& input,
    std::string& error) {
  const auto directory =
      std::filesystem::is_directory(input) ? input : input.parent_path();
  const auto summary_path =
      std::filesystem::is_directory(input) ? input / "summary.json" : input;
  const auto manifest_path = directory / "manifest.json";
  auto summary = read_object(summary_path, error);
  if (!summary.has_value()) {
    return std::nullopt;
  }
  auto manifest = read_object(manifest_path, error);
  if (!manifest.has_value()) {
    return std::nullopt;
  }
  if (string_member(*summary, "schema") != kBundleSchema ||
      string_member(*manifest, "schema") != kBundleSchema) {
    error = "incompatible_bundle_schema";
    return std::nullopt;
  }

  Candidate result;
  result.bundle = directory;
  result.owner = string_member(*summary, "owner");
  result.host = string_member(*manifest, "host");
  result.route = string_member(*manifest, "route");
  result.connection = string_member(*manifest, "connection_requested");
  if (const auto* compatibility = manifest->if_contains("compatibility");
      compatibility != nullptr && compatibility->is_object()) {
    result.compatibility = boost::json::serialize(*compatibility);
  }
  result.samples = u64_member(*summary, "samples");
  result.successful = u64_member(*summary, "successful");
  result.failures = u64_member(*summary, "failures");
  result.p50_us = u64_member(*summary, "p50_us");
  result.p99_us = u64_member(*summary, "p99_us");
  result.jitter_us = double_member(*summary, "jitter_stddev_us");
  result.artifact_complete = bool_member(*summary, "artifact_complete");
  if (!result.artifact_complete) {
    result.exclusion_reason = "artifact_incomplete";
  } else if (result.samples == 0U) {
    result.exclusion_reason = "no_samples";
  } else if (result.successful == 0U) {
    result.exclusion_reason = "no_successful_samples";
  } else if (result.route == "both") {
    result.exclusion_reason = "mixed_route_bundle";
  } else {
    result.eligible = true;
  }
  return result;
}

[[nodiscard]] double failure_rate(const Candidate& candidate) noexcept {
  return candidate.samples == 0U
             ? 1.0
             : static_cast<double>(candidate.failures) /
                   static_cast<double>(candidate.samples);
}

[[nodiscard]] bool dominates(
    const Candidate& lhs,
    const Candidate& rhs) noexcept {
  const auto lhs_failure = failure_rate(lhs);
  const auto rhs_failure = failure_rate(rhs);
  const bool no_worse =
      lhs_failure <= rhs_failure &&
      lhs.p99_us <= rhs.p99_us &&
      lhs.jitter_us <= rhs.jitter_us &&
      lhs.p50_us <= rhs.p50_us;
  const bool strictly_better =
      lhs_failure < rhs_failure ||
      lhs.p99_us < rhs.p99_us ||
      lhs.jitter_us < rhs.jitter_us ||
      lhs.p50_us < rhs.p50_us;
  return no_worse && strictly_better;
}

void assign_pareto_fronts(std::vector<Candidate>& candidates) {
  std::vector<std::size_t> remaining;
  for (std::size_t index = 0U; index < candidates.size(); ++index) {
    if (candidates[index].eligible) {
      remaining.push_back(index);
    }
  }
  unsigned front = 1U;
  while (!remaining.empty()) {
    std::vector<std::size_t> current;
    for (const auto candidate_index : remaining) {
      bool dominated = false;
      for (const auto other_index : remaining) {
        if (candidate_index != other_index &&
            dominates(candidates[other_index], candidates[candidate_index])) {
          dominated = true;
          break;
        }
      }
      if (!dominated) {
        current.push_back(candidate_index);
      }
    }
    if (current.empty()) {
      break;
    }
    for (const auto index : current) {
      candidates[index].pareto_front = front;
    }
    remaining.erase(
        std::remove_if(
            remaining.begin(),
            remaining.end(),
            [&](std::size_t index) {
              return std::find(current.begin(), current.end(), index) !=
                     current.end();
            }),
        remaining.end());
    ++front;
  }
}

[[nodiscard]] boost::json::object candidate_json(
    const Candidate& candidate) {
  return {
      {"bundle", candidate.bundle.string()},
      {"owner", candidate.owner},
      {"host", candidate.host},
      {"route", candidate.route},
      {"connection", candidate.connection},
      {"samples", candidate.samples},
      {"successful", candidate.successful},
      {"failures", candidate.failures},
      {"failure_rate", failure_rate(candidate)},
      {"p50_us", candidate.p50_us},
      {"p99_us", candidate.p99_us},
      {"jitter_stddev_us", candidate.jitter_us},
      {"artifact_complete", candidate.artifact_complete},
      {"eligible", candidate.eligible},
      {"pareto_front",
       candidate.eligible ? boost::json::value(candidate.pareto_front)
                          : boost::json::value{}},
      {"exclusion_reason", candidate.exclusion_reason},
  };
}

}  // namespace

int run_compare(
    const CliOptions& options,
    std::ostream& output,
    std::ostream& error_output) {
  std::vector<Candidate> candidates;
  for (const auto& input : options.inputs) {
    std::string error;
    auto candidate = load_candidate(input, error);
    if (!candidate.has_value()) {
      error_output << "compare_input_error: " << input.string() << ':'
                   << error << '\n';
      return 2;
    }
    candidates.push_back(std::move(*candidate));
  }
  const auto owner = candidates.front().owner;
  const auto compatibility = candidates.front().compatibility;
  if (std::any_of(
          candidates.begin(),
          candidates.end(),
          [&](const Candidate& candidate) {
            return candidate.owner != owner ||
                   candidate.compatibility.empty() ||
                   candidate.compatibility != compatibility;
          })) {
    error_output
        << "compare_input_error: owner_or_measurement_contract_mismatch\n";
    return 2;
  }
  assign_pareto_fronts(candidates);
  std::stable_sort(
      candidates.begin(),
      candidates.end(),
      [](const Candidate& lhs, const Candidate& rhs) {
        if (lhs.eligible != rhs.eligible) {
          return lhs.eligible > rhs.eligible;
        }
        if (lhs.pareto_front != rhs.pareto_front) {
          return lhs.pareto_front < rhs.pareto_front;
        }
        return lhs.bundle.string() < rhs.bundle.string();
      });

  boost::json::array rows;
  for (const auto& candidate : candidates) {
    rows.emplace_back(candidate_json(candidate));
  }
  boost::json::object comparison{
      {"schema", "exchange.api_probe.compare.v1"},
      {"schema_version", 1},
      {"owner", owner},
      {"selection_policy", "pareto_no_automatic_winner"},
      {"candidates", std::move(rows)},
  };
  if (options.output_dir.has_value()) {
    std::error_code directory_error;
    std::filesystem::create_directories(
        *options.output_dir, directory_error);
    if (directory_error) {
      error_output << "compare_output_error: "
                   << directory_error.message() << '\n';
      return 2;
    }
    std::ofstream json{
        *options.output_dir / "compare.json", std::ios::binary};
    json << boost::json::serialize(comparison) << '\n';
    std::ofstream report{
        *options.output_dir / "REPORT.md", std::ios::binary};
    report << "# Exchange probe comparison\n\n"
           << "No automatic winner is selected. Candidates are grouped by "
              "Pareto front over failure rate, p99, jitter and p50.\n\n";
    for (const auto& candidate : candidates) {
      report << "- front="
             << (candidate.eligible
                     ? std::to_string(candidate.pareto_front)
                     : std::string{"excluded"})
             << " bundle=" << candidate.bundle.string()
             << " failure_rate=" << failure_rate(candidate)
             << " p99_us=" << candidate.p99_us
             << " jitter_us=" << candidate.jitter_us;
      if (!candidate.exclusion_reason.empty()) {
        report << " reason=" << candidate.exclusion_reason;
      }
      report << '\n';
    }
  }
  output << boost::json::serialize(comparison) << '\n';
  return 0;
}

}  // namespace exchange_probe
