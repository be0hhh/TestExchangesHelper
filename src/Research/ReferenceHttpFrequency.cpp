#include "exchange_probe/App.hpp"
#include "exchange_probe/ReferenceHttpFrequency.hpp"

#include <boost/json/array.hpp>
#include <boost/json/serialize.hpp>
#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <filesystem>
#include <limits>
#include <ostream>
#include <set>
#include <string_view>
#include <thread>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#ifdef EXCHANGE_PROBE_WITH_REFERENCE
#include "hft_parser/Reference/QuoteSource.hpp"
#endif

namespace exchange_probe {
namespace reference_frequency {
unsigned request_ceiling(const std::vector<unsigned>& periods, unsigned seconds) noexcept {
  if (!seconds || seconds > 60U || periods.empty() || periods.size() > 5U) return 0U;
  unsigned result = 0U, prior = 1001U;
  for (auto period : periods) {
    if (period < 50U || period > 1000U || period >= prior) return 0U;
    result += (seconds * 1000U + period - 1U) / period + 2U;
    prior = period;
  }
  return result;
}
bool must_stop(const Observation& value) noexcept {
  return value.transport_failed || value.http_status != 200 || !value.accepted ||
         !value.send_ns || value.response_ns < value.send_ns;
}
bool artifact_fits(std::uint64_t used, std::uint64_t size,
                   std::uint64_t limit, bool summary) noexcept {
  const auto reserve = summary ? 0U : kSummaryReserve;
  return limit >= reserve && used <= limit - reserve && size <= limit - reserve - used;
}
boost::json::object observation_json(const Observation& value, unsigned stage) {
  return {{"kind", "response"}, {"stage", stage}, {"send_monotonic_ns", value.send_ns},
          {"full_response_monotonic_ns", value.response_ns}, {"http_status", value.http_status},
          {"body_bytes", value.body_bytes}, {"sequence", value.sequence},
          {"source_timestamp_ms", value.source_timestamp_ms}, {"receive_monotonic_ns", value.receive_ns},
          {"confirmation_monotonic_ns", value.confirmation_ns}, {"accepted", value.accepted},
          {"transport_failed", value.transport_failed}, {"connection_attempts", value.connection_attempts},
          {"retry_after_present", value.retry_after_present},
          {"retry_after_seconds", value.retry_after_seconds_known ? boost::json::value{value.retry_after_seconds} : boost::json::value{nullptr}},
          {"cache_age_present", value.cache_age_present},
          {"cache_age_seconds", value.cache_age_seconds_known ? boost::json::value{value.cache_age_seconds} : boost::json::value{nullptr}},
          {"cache_control_present", value.cache_control_present}, {"source_age_ms", nullptr}};
}
boost::json::object stage_json(const Stage& stage) {
  std::vector<std::uint64_t> latencies;
  latencies.reserve(stage.observations.size());
  std::uint64_t confirmations = 0U, updates = 0U, last_sequence = 0U;
  std::uint64_t requests = 0U, responses = 0U;
  bool have_sequence = false;
  for (const auto& value : stage.observations) {
    requests += value.send_ns != 0U;
    responses += value.response_ns != 0U;
    if (value.send_ns && value.response_ns >= value.send_ns)
      latencies.push_back(value.response_ns - value.send_ns);
    if (value.accepted) {
      ++confirmations;
      if (value.receive_ns >= stage.begin_ns && (!have_sequence || value.sequence != last_sequence)) ++updates;
      have_sequence = true;
      last_sequence = value.sequence;
    }
  }
  std::sort(latencies.begin(), latencies.end());
  auto percentile = [&](unsigned percent) -> boost::json::value {
    if (latencies.empty()) return nullptr;
    return latencies[(latencies.size() * percent + 99U) / 100U - 1U];
  };
  const auto elapsed = stage.end_ns > stage.begin_ns ? stage.end_ns - stage.begin_ns : 0U;
  const double seconds = static_cast<double>(elapsed) / 1e9;
  return {{"period_ms", stage.period_ms}, {"nominal_requests_per_second", 1000.0 / stage.period_ms},
          {"elapsed_ns", elapsed}, {"completed", stage.completed},
          {"requests_started", requests}, {"responses", responses}, {"matching_confirmations", confirmations},
          {"distinct_sample_updates", updates},
          {"achieved_requests_per_second", seconds > 0.0 ? static_cast<double>(requests) / seconds : 0.0},
          {"achieved_responses_per_second", seconds > 0.0 ? static_cast<double>(responses) / seconds : 0.0},
          {"matching_confirmations_per_second", seconds > 0.0 ? static_cast<double>(confirmations) / seconds : 0.0},
          {"distinct_sample_updates_per_second", seconds > 0.0 ? static_cast<double>(updates) / seconds : 0.0},
          {"full_response_latency_ns", boost::json::object{{"p50", percentile(50)}, {"p95", percentile(95)}, {"p99", percentile(99)}}}};
}
} // namespace reference_frequency
namespace {
template<class T> bool number(std::string_view text, T& value) {
  const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
  return !text.empty() && ec == std::errc{} && end == text.data() + text.size();
}
bool env_name(std::string_view value) {
  if (value.empty() || value.size() > 128U || value.front() < 'A' || value.front() > 'Z') return false;
  return std::all_of(value.begin(), value.end(), [](char c) {
    return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
  });
}
bool valid_config(const CliOptions& options) {
  const auto ceiling = reference_frequency::request_ceiling(options.reference_http_frequency.periods_ms,
                                                          options.duration_seconds.value_or(0U));
  return ceiling && env_name(options.reference_http_frequency.endpoint_env) &&
      env_name(options.reference_http_frequency.token_env) && options.ca_file && !options.ca_file->empty() &&
      options.ca_file->native().size() <= 1023U && options.output_dir && !options.output_dir->empty() &&
      options.output_dir->native().size() <= 4096U && options.symbol == "ANTHROPIC" &&
      options.reference_http_frequency.max_requests <= ceiling &&
      options.overall_timeout_seconds > 0U && options.overall_timeout_seconds <= 330U &&
      options.limits.timeout >= std::chrono::milliseconds{100} &&
      options.limits.timeout <= std::chrono::milliseconds{5000} &&
      options.max_artifact_bytes >= 65536U &&
      options.max_artifact_bytes <= reference_frequency::kMaximumArtifactBytes && !options.open_viewer;
}
} // namespace
CliParseResult parse_reference_http_frequency_cli(int argc, char** argv) {
  CliParseResult result;
  auto& o = result.options;
  o.command = Command::Research; o.action = "reference-http-frequency";
  o.open_viewer = false; o.symbol = "ANTHROPIC"; o.duration_seconds = 60U;
  o.overall_timeout_seconds = 330U; o.limits.timeout = std::chrono::milliseconds{1500};
  o.max_artifact_bytes = reference_frequency::kMaximumArtifactBytes;
  std::set<std::string_view> seen;
  for (int i = 3; i < argc; ++i) {
    const std::string_view option{argv[i]};
    const bool allowed = option == "--endpoint-env" || option == "--token-env" || option == "--ca-file" ||
        option == "--output-dir" || option == "--symbol" || option == "--periods-ms" ||
        option == "--duration-seconds" || option == "--overall-timeout-seconds" ||
        option == "--timeout-ms" || option == "--max-artifact-bytes" || option == "--max-requests" || option == "--no-open";
    if (!allowed || !seen.insert(option).second ||
        (option != "--no-open" && (i + 1 >= argc || std::string_view{argv[i+1]}.starts_with("--")))) {
      result.error = "unsupported, missing or repeated reference-http-frequency option"; return result;
    }
    if (option == "--no-open") continue;
    const std::string_view value{argv[++i]};
    bool ok = true;
    if (option == "--endpoint-env") o.reference_http_frequency.endpoint_env = value;
    else if (option == "--token-env") o.reference_http_frequency.token_env = value;
    else if (option == "--ca-file") o.ca_file = std::string{value};
    else if (option == "--output-dir") o.output_dir = std::string{value};
    else if (option == "--symbol") o.symbol = value;
    else if (option == "--periods-ms") {
      o.reference_http_frequency.periods_ms.clear();
      std::size_t start = 0U;
      do {
        const auto comma = value.find(',', start);
        unsigned period{};
        ok = number(value.substr(start, comma == std::string_view::npos ? comma : comma - start), period);
        if (!ok || o.reference_http_frequency.periods_ms.size() >= 5U) { ok = false; break; }
        o.reference_http_frequency.periods_ms.push_back(period);
        if (comma == std::string_view::npos) break;
        start = comma + 1U;
      } while (start <= value.size());
    } else if (option == "--max-artifact-bytes") ok = number(value, o.max_artifact_bytes);
    else {
      unsigned parsed{}; ok = number(value, parsed);
      if (option == "--duration-seconds") o.duration_seconds = parsed;
      else if (option == "--overall-timeout-seconds") o.overall_timeout_seconds = parsed;
      else if (option == "--max-requests") { o.reference_http_frequency.max_requests = parsed; ok = ok && parsed != 0U; }
      else if (option == "--timeout-ms") o.limits.timeout = std::chrono::milliseconds{parsed};
    }
    if (!ok) { result.error = "invalid reference-http-frequency numeric option"; return result; }
  }
  if (!valid_config(o)) { result.error = "invalid reference-http-frequency bounded MOCK configuration"; return result; }
  result.ok = true; return result;
}
#ifdef EXCHANGE_PROBE_WITH_REFERENCE
namespace {
using namespace reference_frequency;
std::uint64_t monotonic_now() noexcept {
  timespec value{};
  if (::clock_gettime(CLOCK_MONOTONIC, &value)) return 0U;
  return static_cast<std::uint64_t>(value.tv_sec) * 1'000'000'000U + static_cast<std::uint64_t>(value.tv_nsec);
}
class Artifact final {
 public:
  ~Artifact() { if (fd_ >= 0) ::close(fd_); }
  bool create(const std::filesystem::path& root, std::uint64_t limit) {
    limit_ = limit;
    std::error_code ec; std::filesystem::create_directories(root, ec);
    if (ec) return false;
    auto pattern = (root / "reference-http-frequency-XXXXXX").string();
    std::vector<char> name(pattern.begin(), pattern.end()); name.push_back('\0');
    const auto* directory = ::mkdtemp(name.data());
    if (!directory) return false;
    directory_ = directory;
    fd_ = ::open((directory_ / "events.jsonl").c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    return fd_ >= 0;
  }
  bool event(const boost::json::object& value, bool summary = false) {
    const auto line = boost::json::serialize(value) + '\n';
    if (!artifact_fits(used_, line.size(), limit_, summary)) return false;
    const auto offset = ::lseek(fd_, 0, SEEK_CUR);
    std::size_t done = 0U;
    while (done < line.size()) {
      const auto n = ::write(fd_, line.data() + done, line.size() - done);
      if (n < 0 && errno == EINTR) continue;
      if (n <= 0) {
        if (offset >= 0) { (void)::ftruncate(fd_, offset); (void)::lseek(fd_, offset, SEEK_SET); }
        return false;
      }
      done += static_cast<std::size_t>(n);
    }
    used_ += line.size(); return true;
  }
 private:
  std::filesystem::path directory_;
  int fd_{-1};
  std::uint64_t used_{}, limit_{};
};
struct Capture final {
  Artifact artifact;
  std::array<Observation, kMaximumRequests> observations{};
  std::size_t count{};
  unsigned max_requests{};
  std::uint64_t begin_ns{}, stage_ns{}, deadline_ns{};
  std::size_t stages{};
  std::atomic<bool> stopped{false};
  // Written only by the worker; read after stopCold() joins it.
  const char* reason{"complete"};
  bool artifact_failed{};
  static bool observe(const hft_parser::reference::QuotePollObservation& value, void* context) noexcept {
    auto& self = *static_cast<Capture*>(context);
    if (self.stopped.load(std::memory_order_acquire)) return false;
    try {
      Observation record{value.sendMonotonicNs, value.responseMonotonicNs, value.sequence,
                         value.sourceTimestampMs, value.receiveMonotonicNs, value.confirmationMonotonicNs,
                         value.responseBodyBytes, value.httpStatus, value.accepted,
                         value.transportFailed, value.connectionAttempts};
      record.retry_after_seconds = value.retryAfterSeconds;
      record.cache_age_seconds = value.cacheAgeSeconds;
      record.retry_after_present = value.retryAfterPresent;
      record.retry_after_seconds_known = value.retryAfterSecondsKnown;
      record.cache_age_present = value.cacheAgePresent;
      record.cache_age_seconds_known = value.cacheAgeSecondsKnown;
      record.cache_control_present = value.cacheControlPresent;
      const auto event_time = record.send_ns ? record.send_ns : monotonic_now();
      const auto stage = event_time >= self.begin_ns ?
          std::min<std::uint64_t>((event_time - self.begin_ns) / self.stage_ns, self.stages - 1U) : 0U;
      if (self.count >= self.max_requests || self.count >= self.observations.size()) {
        self.reason = "request_limit"; self.stopped.store(true, std::memory_order_release); return false;
      }
      self.observations[self.count++] = record;
      if (!self.artifact.event(observation_json(record, static_cast<unsigned>(stage)))) {
        self.artifact_failed = true; self.reason = "artifact_limit_or_io";
      } else if (must_stop(record)) self.reason = record.http_status == 429 ? "http_429" :
          record.http_status == 401 ? "http_401" : record.http_status == 403 ? "http_403" : "transport_or_protocol_failure";
      else if (monotonic_now() >= self.deadline_ns) self.reason = "overall_deadline";
      else if (self.count >= self.max_requests) self.reason = "request_limit";
      else return true;
    } catch (...) { self.artifact_failed = true; self.reason = "resource_failure"; }
    self.stopped.store(true, std::memory_order_release); return false;
  }
};
} // namespace
#endif
int run_reference_http_frequency(const CliOptions& options, std::ostream& output, std::ostream& errors) {
#ifndef EXCHANGE_PROBE_WITH_REFERENCE
  (void)options; (void)output;
  errors << "configuration_error: reference-http-frequency requires the root Parser QuoteSource target\n";
  return 2;
#else
  if (!valid_config(options)) { errors << "configuration_error: invalid bounded MOCK configuration\n"; return 2; }
  const auto& selected = options.reference_http_frequency;
  const char* endpoint = std::getenv(selected.endpoint_env.c_str());
  const char* token = std::getenv(selected.token_env.c_str());
  // Bound the borrowed environment scan. The native source owns validation and
  // copies/wipes its request; neither selected value enters an artifact or log.
  const auto endpoint_size = endpoint ? ::strnlen(endpoint, 2048U) : 0U;
  const auto token_size = token ? ::strnlen(token, 2048U) : 0U;
  if (!endpoint_size || endpoint_size >= 2048U || !token_size || token_size >= 2048U) {
    errors << "configuration_error: missing or oversized reference environment value\n"; return 2;
  }
  Capture capture;
  const auto ceiling = request_ceiling(selected.periods_ms, *options.duration_seconds);
  capture.max_requests = selected.max_requests ? selected.max_requests : ceiling;
  capture.stages = selected.periods_ms.size();
  capture.stage_ns = static_cast<std::uint64_t>(*options.duration_seconds) * 1'000'000'000U;
  if (!capture.artifact.create(*options.output_dir, options.max_artifact_bytes)) {
    errors << "reference_http_frequency_error: artifact_creation_failed\n"; return 2;
  }
  if (!capture.artifact.event({{"kind", "configuration"}, {"source", "MOCK"}, {"source_id", 22},
      {"symbol", "ANTHROPIC"}, {"connections", 1}, {"inflight", 1},
      {"max_requests", capture.max_requests}, {"cache_metadata_available", true},
      {"retry_after_available", true}, {"source_clock_mapping", "unknown"}})) {
    errors << "reference_http_frequency_error: artifact_configuration_failed\n"; return 2;
  }
  hft_parser::reference::QuoteSource source;
  const auto trust = options.ca_file->string();
  hft_parser::reference::QuoteSourceConfig config;
  config.endpointUrl = cxet::StringView{endpoint, endpoint_size};
  config.bearerToken = cxet::StringView{token, token_size};
  config.caFile = cxet::StringView{trust.data(), trust.size()};
  config.intervalMs = selected.periods_ms.front();
  config.timeoutMs = static_cast<std::uint32_t>(options.limits.timeout.count());
  // This diagnostic measures the source, with no strategy consumer. Avoid
  // turning the freshness lease into a shorter request deadline mid-ladder.
  config.maxAgeMs = 330000U;
  config.observer = &Capture::observe; config.observerContext = &capture;
  if (!source.prepareCold(config)) {
    errors << "configuration_error: native MOCK reference preparation refused\n"; return 2;
  }
  capture.begin_ns = monotonic_now();
  capture.deadline_ns = capture.begin_ns + static_cast<std::uint64_t>(options.overall_timeout_seconds) * 1'000'000'000U;
  const auto ladder_end = capture.begin_ns + capture.stage_ns * capture.stages;
  if (!capture.begin_ns || !source.startCold()) {
    errors << "reference_http_frequency_error: source_start_failed\n"; return 2;
  }
  unsigned stage = 0U;
  const char* owner_reason = "complete";
  while (!capture.stopped.load(std::memory_order_acquire)) {
    const auto now = monotonic_now();
    if (!now || now >= capture.deadline_ns) { owner_reason = "overall_deadline"; break; }
    if (now >= ladder_end) break;
    const auto current = static_cast<unsigned>((now - capture.begin_ns) / capture.stage_ns);
    if (current != stage) {
      // One update to the current stage; missed stage boundaries never create
      // catch-up requests. The source schedules each next poll after completion.
      stage = current;
      if (!source.setPollIntervalCold(selected.periods_ms[stage])) { owner_reason = "interval_control_failure"; break; }
    }
    (void)source.pollOwner(now);
    std::this_thread::sleep_for(std::chrono::milliseconds{1});
  }
  capture.stopped.store(true, std::memory_order_release);
  source.stopCold();
  const auto end = monotonic_now();
  const auto requests_started = source.requestsStartedCold();
  std::uint64_t observed_starts = 0U, responses = 0U;
  for (std::size_t i = 0U; i < capture.count; ++i) {
    observed_starts += capture.observations[i].send_ns != 0U;
    responses += capture.observations[i].response_ns != 0U;
  }
  if (std::string_view{capture.reason} != "complete") owner_reason = capture.reason;
  if (std::string_view{owner_reason} == "complete" && requests_started != observed_starts)
    owner_reason = "unobserved_or_cancelled_request_tail";
  const bool complete = std::string_view{owner_reason} == "complete" && end >= ladder_end;
  boost::json::array summaries;
  for (std::size_t i = 0U; i < capture.stages; ++i) {
    Stage summary; summary.period_ms = selected.periods_ms[i];
    summary.begin_ns = capture.begin_ns + i * capture.stage_ns;
    summary.end_ns = std::min(end, summary.begin_ns + capture.stage_ns);
    summary.completed = complete || (summary.end_ns == summary.begin_ns + capture.stage_ns &&
                                    i + 1U < capture.stages &&
                                    std::string_view{owner_reason} != "artifact_limit_or_io");
    for (std::size_t j = 0U; j < capture.count; ++j) {
      const auto& record = capture.observations[j];
      const auto sent = record.send_ns ? record.send_ns : record.response_ns;
      if (sent >= summary.begin_ns && sent < summary.begin_ns + capture.stage_ns) summary.observations.push_back(record);
    }
    summaries.push_back(stage_json(summary));
  }
  boost::json::object report{{"kind", "summary"}, {"status", complete ? "completed_range" : "incomplete"},
      {"reason", owner_reason}, {"source", "MOCK"}, {"responses", responses},
      {"requests_started", requests_started}, {"observed_request_starts", observed_starts},
      {"observed_connection_attempts", capture.count ? capture.observations[capture.count-1U].connection_attempts : 0U},
      {"rate_claim", "observed_range_only_not_maximum_allowed_rate"},
      {"source_age_ms", nullptr}, {"stages", std::move(summaries)}};
  const bool saved = capture.artifact.event(report, true);
  output << boost::json::serialize(report) << '\n';
  if (!saved || capture.artifact_failed) errors << "reference_http_frequency_error: artifact_incomplete\n";
  return complete && saved && !capture.artifact_failed ? 0 : 1;
#endif
}
} // namespace exchange_probe
