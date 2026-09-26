#include "exchange_probe/App.hpp"

#include <boost/json/array.hpp>
#include <boost/json/object.hpp>
#include <boost/json/parse.hpp>
#include <boost/json/serialize.hpp>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>

namespace {

void require(bool condition, const char* message) {
  if (!condition) {
    std::cerr << "compare test failed: " << message << '\n';
    std::exit(1);
  }
}

[[nodiscard]] std::uint64_t nonnegative_integer(
    const boost::json::value& value) {
  if (value.is_uint64()) {
    return value.as_uint64();
  }
  if (value.is_int64() && value.as_int64() >= 0) {
    return static_cast<std::uint64_t>(value.as_int64());
  }
  return 0U;
}

void write_json(
    const std::filesystem::path& path,
    const boost::json::object& value) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream output{path, std::ios::binary};
  output << boost::json::serialize(value) << '\n';
}

boost::json::object compatibility() {
  return {
      {"mode", "low"},
      {"surface", "public"},
      {"transport", "rest"},
      {"connection", "cold"},
      {"lanes", 1},
      {"samples", 5},
      {"duration_seconds", 30},
      {"percentile_method",
       "bounded_log16_subbucket_histogram_upper_bound"},
      {"venues", boost::json::array{"binance"}},
      {"products", boost::json::array{"spot"}},
      {"cases", boost::json::array{}},
  };
}

void write_bundle(
    const std::filesystem::path& directory,
    std::string_view route,
    std::uint64_t p50_us,
    std::uint64_t p99_us,
    double jitter_us,
    std::uint64_t schema_version = 3U) {
  const auto schema = schema_version == 3U
                          ? "exchange.api_probe.bundle.v3"
                          : "exchange.api_probe.bundle.v2";
  write_json(
      directory / "manifest.json",
      {
          {"schema", schema},
          {"schema_version", schema_version},
          {"run_id", directory.string()},
          {"owner", "latency"},
          {"status", "complete"},
          {"artifact_complete", true},
          {"clock_domains",
           boost::json::object{
               {"local_monotonic", "nanoseconds_from_run_start"},
               {"local_utc", "unix_epoch_nanoseconds"},
               {"exchange_event", "profile_declared_or_null"},
               {"exchange_transaction", "profile_declared_or_null"},
           }},
          {"host", "test-host"},
          {"route", std::string{route}},
          {"connection_requested", "cold"},
          {"compatibility", compatibility()},
      });
  write_json(
      directory / "summary.json",
      {
          {"schema", schema},
          {"schema_version", schema_version},
          {"owner", "latency"},
          {"artifact_complete", true},
          {"samples", 5},
          {"successful", 5},
          {"failures", 0},
          {"p50_us", p50_us},
          {"p99_us", p99_us},
          {"jitter_stddev_us", jitter_us},
      });
}

}  // namespace

int main() {
  using namespace exchange_probe;
  const auto suffix =
      std::chrono::steady_clock::now().time_since_epoch().count();
  const auto root =
      std::filesystem::temp_directory_path() /
      ("exchange-probe-compare-" + std::to_string(suffix));
  const auto natural = root / "natural";
  const auto pinned = root / "pinned";
  write_bundle(natural, "natural", 50U, 100U, 10.0);
  write_bundle(pinned, "pinned", 40U, 120U, 8.0);

  CliOptions options;
  options.inputs = {natural, pinned};
  std::ostringstream output;
  std::ostringstream errors;
  require(run_compare(options, output, errors) == 0, "comparison succeeds");
  require(errors.str().empty(), "comparison has no errors");

  boost::system::error_code parse_error;
  const auto parsed = boost::json::parse(output.str(), parse_error);
  require(!parse_error && parsed.is_object(), "comparison is JSON");
  const auto& object = parsed.as_object();
  require(
      object.at("selection_policy").as_string() ==
          "pareto_no_automatic_winner",
      "no winner policy");
  const auto& candidates = object.at("candidates").as_array();
  require(candidates.size() == 2U, "two candidates");
  require(
      nonnegative_integer(
          candidates[0].as_object().at("pareto_front")) == 1U &&
          nonnegative_integer(
              candidates[1].as_object().at("pareto_front")) == 1U,
      "trade-off candidates share first front");

  const auto old_bundle = root / "old_bundle";
  write_bundle(old_bundle, "natural", 50U, 100U, 10.0, 2U);
  options.inputs = {old_bundle, natural};
  output.str({});
  errors.str({});
  require(run_compare(options, output, errors) == 2, "old bundle is rejected");
  require(errors.str().find("incompatible_bundle_schema") != std::string::npos,
          "old bundle reports incompatible schema");

  std::error_code cleanup_error;
  std::filesystem::remove_all(root, cleanup_error);
  return 0;
}
