#include "OfflineCase.hpp"
#include "exchange_probe/App.hpp"
#include "exchange_probe/ReferenceHttpFrequency.hpp"
#include <boost/json/serialize.hpp>
#include <initializer_list>
#include <string>
#include <vector>

namespace {
using namespace exchange_probe;
using namespace exchange_probe::reference_frequency;
CliParseResult cli(std::initializer_list<const char*> extra) {
  std::vector<std::string> args{"probe", "research", "reference-http-frequency", "--endpoint-env", "MOCK_REFERENCE_URL",
      "--token-env", "MOCK_REFERENCE_TOKEN", "--ca-file", "ca.pem", "--output-dir", "out"};
  for (auto* value : extra) args.emplace_back(value);
  std::vector<char*> argv; for (auto& value : args) argv.push_back(value.data());
  return parse_cli(static_cast<int>(argv.size()), argv.data());
}
void limits() {
  auto parsed = cli({});
  CXET_CHECK(parsed.ok && !parsed.options.open_viewer && *parsed.options.duration_seconds == 60U);
  CXET_CHECK(request_ceiling(parsed.options.reference_http_frequency.periods_ms, 60U) == 2230U);
  CXET_CHECK(!cli({"--periods-ms", "1000,49"}).ok);
  CXET_CHECK(!cli({"--periods-ms", "1000,500,500"}).ok);
  CXET_CHECK(!cli({"--periods-ms", "50,100"}).ok);
  CXET_CHECK(!cli({"--periods-ms", "1000,"}).ok);
  CXET_CHECK(!cli({"--duration-seconds", "61"}).ok);
  CXET_CHECK(!cli({"--overall-timeout-seconds", "331"}).ok);
  CXET_CHECK(!cli({"--max-requests", "2231"}).ok);
  CXET_CHECK(!cli({"--max-requests", "0"}).ok);
  CXET_CHECK(!cli({"--max-artifact-bytes", "4194305"}).ok);
  CXET_CHECK(!cli({"--symbol", "OTHER"}).ok);
  CXET_CHECK(!cli({"--connections", "2"}).ok);
  CXET_CHECK(!cli({"--token", "synthetic-secret"}).ok);
  CXET_CHECK(!cli({"--timeout-ms", "5001"}).ok);
  CXET_CHECK(cli({"--periods-ms", "1000,100", "--duration-seconds", "1", "--max-requests", "1"}).ok);
}
void aggregation() {
  Stage stage; stage.period_ms = 100U; stage.begin_ns = 1U; stage.end_ns = 1'000'000'001U;
  stage.observations = {{1U, 100U, 7U, 10U, 50U, 100U, 30U, 200, true},
                        {101U, 300U, 7U, 10U, 50U, 300U, 30U, 200, true},
                        {301U, 600U, 8U, 11U, 600U, 600U, 30U, 200, true}};
  const auto result = stage_json(stage);
  CXET_CHECK(result.at("responses").as_uint64() == 3U);
  CXET_CHECK(result.at("matching_confirmations").as_uint64() == 3U);
  CXET_CHECK(result.at("distinct_sample_updates").as_uint64() == 2U);
  const auto& latency = result.at("full_response_latency_ns").as_object();
  CXET_CHECK(latency.at("p50").as_uint64() == 199U && latency.at("p99").as_uint64() == 299U);
  CXET_CHECK(result.at("nominal_requests_per_second").as_double() == 10.0);
  CXET_CHECK(result.at("achieved_responses_per_second").as_double() == 3.0);
  Stage next; next.period_ms = 50U; next.begin_ns = 1000U; next.end_ns = 2000U;
  next.observations = {{1001U, 1100U, 8U, 11U, 600U, 1100U, 30U, 200, true},
                       {1101U, 1200U, 9U, 12U, 1200U, 1200U, 30U, 200, true}};
  const auto continued = stage_json(next);
  CXET_CHECK(continued.at("matching_confirmations").as_uint64() == 2U);
  CXET_CHECK(continued.at("distinct_sample_updates").as_uint64() == 1U);
}
void fail_stop() {
  Observation accepted{1U, 2U, 1U, 3U, 2U, 2U, 30U, 200, true};
  CXET_CHECK(!must_stop(accepted));
  for (int status : {401,403,429,500,302}) { auto bad = accepted; bad.http_status = status; CXET_CHECK(must_stop(bad)); }
  auto invalid = accepted; invalid.accepted = false; CXET_CHECK(must_stop(invalid));
  invalid = accepted; invalid.transport_failed = true; CXET_CHECK(must_stop(invalid));
  invalid = accepted; invalid.response_ns = 0U; CXET_CHECK(must_stop(invalid));
}
void artifact_and_redaction() {
  CXET_CHECK(artifact_fits(0U, 65536U-kSummaryReserve, 65536U, false));
  CXET_CHECK(!artifact_fits(1U, 65536U-kSummaryReserve, 65536U, false));
  CXET_CHECK(artifact_fits(65536U-kSummaryReserve, kSummaryReserve, 65536U, true));
  CXET_CHECK(!artifact_fits(UINT64_MAX, 1U, 65536U, true));
  const auto encoded = boost::json::serialize(observation_json({}, 0U));
  CXET_CHECK(encoded.find("source_age_ms\":null") != std::string::npos);
  CXET_CHECK(encoded.find("Bearer") == std::string::npos && encoded.find("endpoint") == std::string::npos);
  Observation retry; retry.retry_after_present = true; retry.retry_after_seconds_known = true;
  retry.retry_after_seconds = 120U; retry.cache_control_present = true;
  const auto metadata = observation_json(retry, 0U);
  CXET_CHECK(metadata.at("retry_after_seconds").as_uint64() == 120U);
  retry.retry_after_seconds_known = false; // HTTP-date/invalid/duplicate: presence only.
  CXET_CHECK(observation_json(retry, 0U).at("retry_after_seconds").is_null());
  const auto rejected = cli({"--token", "synthetic-secret"});
  CXET_CHECK(rejected.error.find("synthetic-secret") == std::string::npos);
}
}
int main(int argc, char** argv) {
  const cxet::testing::Case cases[]{
      {"helper.reference_http_frequency_limits", limits},
      {"helper.reference_http_frequency_aggregation", aggregation},
      {"helper.reference_http_frequency_fail_stop", fail_stop},
      {"helper.reference_http_frequency_artifact_redaction", artifact_and_redaction},
  };
  return cxet::testing::runCases(argc, argv, cases);
}
