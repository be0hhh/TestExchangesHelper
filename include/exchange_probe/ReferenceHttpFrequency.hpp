#pragma once

#include <boost/json/object.hpp>
#include <cstdint>
#include <iosfwd>
#include <string>
#include <vector>

namespace exchange_probe {
struct CliOptions;
struct CliParseResult;

struct ReferenceHttpFrequencyOptions {
  std::vector<unsigned> periods_ms{1000U, 500U, 250U, 100U, 50U};
  std::string endpoint_env;
  std::string token_env;
  unsigned max_requests{}; // Zero selects the derived, bounded ladder ceiling.
};

namespace reference_frequency {
inline constexpr unsigned kMaximumRequests = 6010U;
inline constexpr std::uint64_t kMaximumArtifactBytes = 4U * 1024U * 1024U;
inline constexpr std::uint64_t kSummaryReserve = 16384U;

// Numeric projection only: no URL, token, HTTP headers or response body can enter
// the report through this type. Source age needs a clock mapping, absent here.
struct Observation {
  std::uint64_t send_ns{}, response_ns{}, sequence{}, source_timestamp_ms{};
  std::uint64_t receive_ns{}, confirmation_ns{}, body_bytes{};
  int http_status{};
  bool accepted{};
  bool transport_failed{};
  std::uint64_t connection_attempts{};
  std::uint64_t retry_after_seconds{}, cache_age_seconds{};
  bool retry_after_present{}, retry_after_seconds_known{};
  bool cache_age_present{}, cache_age_seconds_known{}, cache_control_present{};
};
struct Stage {
  unsigned period_ms{};
  std::uint64_t begin_ns{}, end_ns{};
  std::vector<Observation> observations;
  bool completed{};
};
[[nodiscard]] unsigned request_ceiling(const std::vector<unsigned>& periods,
                                      unsigned seconds) noexcept;
[[nodiscard]] bool must_stop(const Observation&) noexcept;
[[nodiscard]] bool artifact_fits(std::uint64_t used, std::uint64_t size,
                                 std::uint64_t limit, bool summary) noexcept;
[[nodiscard]] boost::json::object observation_json(const Observation&, unsigned stage);
[[nodiscard]] boost::json::object stage_json(const Stage&);
} // namespace reference_frequency

[[nodiscard]] CliParseResult parse_reference_http_frequency_cli(int argc, char** argv);
[[nodiscard]] int run_reference_http_frequency(const CliOptions&, std::ostream&, std::ostream&);
} // namespace exchange_probe
