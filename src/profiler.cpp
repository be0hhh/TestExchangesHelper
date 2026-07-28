#include "exchange_probe/app.hpp"
#include "net_common.hpp"

#include <boost/asio/io_context.hpp>
#include <boost/json/array.hpp>
#include <boost/json/object.hpp>
#include <boost/json/serialize.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <mutex>
#include <optional>
#include <ostream>
#include <sstream>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#if defined(__linux__)
#include <sys/timex.h>
#include <unistd.h>
#endif

namespace exchange_probe {
namespace {

inline constexpr std::string_view kBundleSchema =
    "cxet.exchange_probe.bundle.v2";
inline constexpr std::string_view kPercentileMethod =
    "bounded_log16_subbucket_histogram_upper_bound";
inline constexpr std::size_t kHistogramLinearBuckets = 16U;
inline constexpr std::size_t kHistogramSubBuckets = 16U;
inline constexpr std::size_t kHistogramBuckets =
    kHistogramLinearBuckets + 60U * kHistogramSubBuckets;
inline constexpr std::size_t kMaxPinnedIpsPerTask = 64U;

struct Task {
  const ProductSpec* product{nullptr};
  const RestCase* rest{nullptr};
  const WsCase* ws{nullptr};
  unsigned credential_slot{0U};
  std::optional<std::string> pinned_ip;
  std::string route{"natural"};
  std::string route_error;
};

struct Histogram {
  std::array<std::uint64_t, kHistogramBuckets> buckets{};
  std::uint64_t count{0U};
  std::uint64_t latency_count{0U};
  std::uint64_t failures{0U};
  std::uint64_t min_us{std::numeric_limits<std::uint64_t>::max()};
  std::uint64_t max_us{0U};
  long double sum_us{0.0L};
  long double sum_square_us{0.0L};

  [[nodiscard]] static std::size_t bucket_index(
      std::uint64_t value) noexcept {
    if (value < kHistogramLinearBuckets) {
      return static_cast<std::size_t>(value);
    }
    const auto exponent =
        static_cast<std::size_t>(std::bit_width(value) - 1U);
    const auto base = 1ULL << exponent;
    const auto width = base / kHistogramSubBuckets;
    const auto sub_bucket =
        static_cast<std::size_t>((value - base) / width);
    return kHistogramLinearBuckets +
           (exponent - 4U) * kHistogramSubBuckets +
           std::min(sub_bucket, kHistogramSubBuckets - 1U);
  }

  [[nodiscard]] static std::uint64_t bucket_upper_bound(
      std::size_t index) noexcept {
    if (index < kHistogramLinearBuckets) {
      return static_cast<std::uint64_t>(index);
    }
    const auto relative = index - kHistogramLinearBuckets;
    const auto exponent = 4U + relative / kHistogramSubBuckets;
    const auto sub_bucket = relative % kHistogramSubBuckets;
    if (exponent == 63U &&
        sub_bucket + 1U == kHistogramSubBuckets) {
      return std::numeric_limits<std::uint64_t>::max();
    }
    const auto base = 1ULL << exponent;
    const auto width = base / kHistogramSubBuckets;
    return base +
           static_cast<std::uint64_t>(sub_bucket + 1U) * width - 1U;
  }

  void add(std::uint64_t value, bool success) noexcept {
    ++count;
    failures += success ? 0U : 1U;
    if (!success) {
      return;
    }
    ++latency_count;
    min_us = std::min(min_us, value);
    max_us = std::max(max_us, value);
    sum_us += static_cast<long double>(value);
    sum_square_us +=
        static_cast<long double>(value) * static_cast<long double>(value);
    ++buckets[bucket_index(value)];
  }

  [[nodiscard]] std::uint64_t percentile(unsigned basis_points) const noexcept {
    if (latency_count == 0U) {
      return 0U;
    }
    const auto target =
        ((latency_count - 1U) *
         static_cast<std::uint64_t>(basis_points)) /
        10'000U;
    std::uint64_t seen = 0U;
    for (std::size_t index = 0U; index < buckets.size(); ++index) {
      seen += buckets[index];
      if (seen > target) {
        return bucket_upper_bound(index);
      }
    }
    return max_us;
  }
};

[[nodiscard]] std::string command_name(Command command) {
  switch (command) {
    case Command::Latency: return "latency";
    case Command::Stability: return "stability";
    default: return "unknown";
  }
}

[[nodiscard]] std::string mode_name(PlacementMode mode) {
  switch (mode) {
    case PlacementMode::Low: return "low";
    case PlacementMode::Standard: return "standard";
    case PlacementMode::High: return "high";
  }
  return "unknown";
}

[[nodiscard]] std::string route_name(RouteMode route) {
  switch (route) {
    case RouteMode::Natural: return "natural";
    case RouteMode::Pinned: return "pinned";
    case RouteMode::Both: return "both";
  }
  return "unknown";
}

[[nodiscard]] std::string connection_name(ConnectionMode connection) {
  switch (connection) {
    case ConnectionMode::Cold: return "cold";
    case ConnectionMode::Warm: return "warm";
    case ConnectionMode::Both: return "both";
  }
  return "unknown";
}

[[nodiscard]] unsigned resolved_lanes(const CliOptions& options) {
  if (options.lanes.has_value()) {
    return *options.lanes;
  }
  return options.placement_mode == PlacementMode::Low ? 1U : 4U;
}

[[nodiscard]] unsigned resolved_samples(const CliOptions& options) {
  if (options.samples.has_value()) {
    return *options.samples;
  }
  return options.placement_mode == PlacementMode::Low ? 5U : 100U;
}

[[nodiscard]] unsigned resolved_duration(const CliOptions& options) {
  if (options.duration_seconds.has_value()) {
    return *options.duration_seconds;
  }
  return options.placement_mode == PlacementMode::Low ? 30U : 300U;
}

[[nodiscard]] bool selected_case(
    const CliOptions& options,
    std::string_view name) {
  return options.cases.empty() ||
         std::find(options.cases.begin(), options.cases.end(), name) !=
             options.cases.end();
}

[[nodiscard]] std::filesystem::path default_output_dir(Command command) {
  const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::system_clock::now().time_since_epoch())
                       .count();
  return std::filesystem::path{"results"} /
         (command_name(command) + "-" + std::to_string(now));
}

[[nodiscard]] boost::json::object host_clock_status() {
  boost::json::object result{
      {"source", "unavailable"},
      {"synchronized", false},
      {"offset_ns", nullptr},
      {"max_error_us", nullptr},
      {"estimated_error_us", nullptr},
  };
#if defined(__linux__)
  timex state{};
  const int status = ::adjtimex(&state);
  result["source"] = "linux_adjtimex";
  result["synchronized"] =
      status != TIME_ERROR && (state.status & STA_UNSYNC) == 0;
  const bool nanosecond_mode = (state.status & STA_NANO) != 0;
  result["offset_ns"] =
      nanosecond_mode
          ? static_cast<std::int64_t>(state.offset)
          : static_cast<std::int64_t>(state.offset) * 1000;
  result["kernel_time_unit"] =
      nanosecond_mode ? "nanoseconds" : "microseconds";
  result["max_error_us"] = static_cast<std::int64_t>(state.maxerror);
  result["estimated_error_us"] = static_cast<std::int64_t>(state.esterror);
#endif
  return result;
}

[[nodiscard]] std::string host_name() {
#if defined(__linux__)
  std::array<char, 256> buffer{};
  if (::gethostname(buffer.data(), buffer.size()) == 0) {
    buffer.back() = '\0';
    return buffer.data();
  }
#endif
  return "unknown";
}

[[nodiscard]] boost::json::array strings_json(
    const std::vector<std::string>& values) {
  boost::json::array result;
  result.reserve(values.size());
  for (const auto& value : values) {
    result.emplace_back(value);
  }
  return result;
}

[[nodiscard]] std::string csv_field(std::string_view value) {
  std::string result{"\""};
  result.reserve(value.size() + 2U);
  for (const char character : value) {
    if (character == '"') {
      result.push_back('"');
    }
    result.push_back(character);
  }
  result.push_back('"');
  return result;
}

class BundleWriter {
 public:
  BundleWriter(
      const CliOptions& options,
      Command command,
      std::ostream& output,
      std::ostream& error_output)
      : directory_(
            options.output_dir.value_or(default_output_dir(command))),
        byte_limit_(options.max_artifact_bytes),
        output_(output),
        error_output_(error_output) {
    std::error_code error;
    std::filesystem::create_directories(directory_, error);
    if (error) {
      error_ = "output_directory_create_failed:" + error.message();
      return;
    }
    samples_.open(directory_ / "samples.jsonl", std::ios::binary);
    csv_.open(directory_ / "metrics.csv", std::ios::binary);
    if (!samples_ || !csv_) {
      error_ = "output_files_open_failed";
      return;
    }
    csv_ << "sample,lane,route,venue,product,case,surface,transport,outcome,"
            "total_us,dns_us,tcp_us,tls_us,ttfb_us,ack_us,first_data_us,"
            "remote_ip,ip_family,error\n";
  }

  [[nodiscard]] bool ok() const noexcept { return error_.empty(); }
  [[nodiscard]] const std::string& error() const noexcept { return error_; }

  [[nodiscard]] bool write_manifest(
      const CliOptions& options,
      Command command) {
    boost::json::object manifest{
        {"schema", kBundleSchema},
        {"schema_version", 2},
        {"owner", command_name(command)},
        {"host", host_name()},
        {"mode", mode_name(options.placement_mode)},
        {"surface", to_string(options.surface.value_or(Surface::Public))},
        {"transport",
         to_string(options.transport.value_or(Transport::Rest))},
        {"route", route_name(options.route_mode)},
        {"path",
         options.placement_path == PlacementPath::Direct ? "direct"
                                                         : "proxy"},
        {"connection_requested", connection_name(options.connection_mode)},
        {"connection_measured",
         boost::json::array{"cold"}},
        {"warm_status",
         options.connection_mode == ConnectionMode::Cold
             ? boost::json::value("not_requested")
             : boost::json::value("persistent_session_profile_required")},
        {"lanes", resolved_lanes(options)},
        {"samples", resolved_samples(options)},
        {"duration_seconds", resolved_duration(options)},
        {"compatibility",
         boost::json::object{
             {"mode", mode_name(options.placement_mode)},
             {"surface",
              to_string(options.surface.value_or(Surface::Public))},
             {"transport",
              to_string(options.transport.value_or(Transport::Rest))},
             {"connection", connection_name(options.connection_mode)},
             {"lanes", resolved_lanes(options)},
             {"samples", resolved_samples(options)},
             {"duration_seconds", resolved_duration(options)},
             {"percentile_method", kPercentileMethod},
             {"venues", strings_json(options.venues)},
             {"products", strings_json(options.products)},
             {"cases", strings_json(options.cases)},
         }},
        {"max_artifact_bytes", options.max_artifact_bytes},
        {"raw_public", options.limits.raw_public},
        {"geo_opt_in", !options.geo_providers.empty()},
        {"clock", host_clock_status()},
        {"timing_semantics",
         boost::json::object{
             {"ttfb_us",
              "request_write_complete_to_http_header_complete"},
             {"ack_us", "subscription_complete_to_matching_ack"},
             {"first_data_us",
              "subscription_complete_to_first_matching_data"},
             {"total_us", "probe_call_start_to_classified_result"},
         }},
        {"event_age",
         boost::json::object{
             {"status", "unavailable"},
             {"reason", "exchange_timestamp_contract_not_declared"},
         }},
    };
    std::ofstream file{directory_ / "manifest.json", std::ios::binary};
    file << boost::json::serialize(manifest) << '\n';
    file.flush();
    if (!file) {
      error_ = "manifest_write_failed";
      return false;
    }
    return true;
  }

  void append(
      std::uint64_t sequence,
      unsigned lane,
      const Observation& observation,
      std::string_view connection,
      std::string_view route) {
    auto json = observation_json(observation);
    json["bundle_schema"] = kBundleSchema;
    json["sample"] = sequence;
    json["lane"] = lane;
    json["connection_mode"] = connection;
    json["route"] = route;
    json["event_age"] = boost::json::object{
        {"status", "unavailable"},
        {"reason", "exchange_timestamp_contract_not_declared"},
    };
    const auto line = boost::json::serialize(json) + "\n";
    std::ostringstream csv_line;
    csv_line << sequence << ',' << lane << ',' << route << ','
             << observation.venue << ','
             << observation.product << ',' << observation.case_name << ','
             << to_string(observation.surface) << ','
             << to_string(observation.transport) << ','
             << to_string(observation.outcome) << ','
             << observation.timings.total_us << ','
             << observation.timings.dns_us << ','
             << observation.timings.tcp_connect_us << ','
             << observation.timings.tls_handshake_us << ','
             << observation.timings.ttfb_us << ','
             << observation.timings.ack_us << ','
             << observation.timings.first_data_us << ','
             << observation.transport_metadata.remote_ip << ','
             << observation.transport_metadata.ip_family << ','
             << csv_field(observation.error) << '\n';
    const auto csv_text = csv_line.str();

    std::scoped_lock lock{mutex_};
    histogram_.add(
        observation.timings.total_us,
        observation.expectation_met);
    if (artifact_complete_ &&
        bytes_written_ + line.size() + csv_text.size() <= byte_limit_) {
      samples_ << line;
      csv_ << csv_text;
      if (samples_ && csv_) {
        bytes_written_ += line.size() + csv_text.size();
      } else {
        storage_failed_ = true;
        artifact_complete_ = false;
        ++samples_not_persisted_;
      }
    } else {
      artifact_complete_ = false;
      ++samples_not_persisted_;
    }
  }

  [[nodiscard]] bool finish(Command command) {
    std::scoped_lock lock{mutex_};
    samples_.flush();
    csv_.flush();
    storage_failed_ = storage_failed_ || !samples_ || !csv_;
    const auto successful = histogram_.latency_count;
    const auto mean =
        successful == 0U
            ? 0.0
            : static_cast<double>(
                  histogram_.sum_us /
                  static_cast<long double>(successful));
    const auto variance =
        successful == 0U
            ? 0.0L
            : std::max(
                  0.0L,
                  histogram_.sum_square_us /
                          static_cast<long double>(successful) -
                      static_cast<long double>(mean) *
                          static_cast<long double>(mean));
    boost::json::array buckets;
    for (std::size_t index = 0U; index < histogram_.buckets.size(); ++index) {
      if (histogram_.buckets[index] != 0U) {
        buckets.emplace_back(boost::json::object{
            {"upper_bound_us", Histogram::bucket_upper_bound(index)},
            {"count", histogram_.buckets[index]},
        });
      }
    }
    boost::json::object summary{
        {"schema", kBundleSchema},
        {"schema_version", 2},
        {"owner", command_name(command)},
        {"artifact_complete", artifact_complete_},
        {"samples", histogram_.count},
        {"successful", successful},
        {"failures", histogram_.failures},
        {"samples_not_persisted", samples_not_persisted_},
        {"min_us",
         successful == 0U ? 0U : histogram_.min_us},
        {"max_us", histogram_.max_us},
        {"mean_us", mean},
        {"jitter_stddev_us", std::sqrt(static_cast<double>(variance))},
        {"p50_us", histogram_.percentile(5000U)},
        {"p95_us", histogram_.percentile(9500U)},
        {"p99_us", histogram_.percentile(9900U)},
        {"p999_us",
         successful >= 1000U
             ? boost::json::value(histogram_.percentile(9990U))
             : boost::json::value{}},
        {"percentile_method", kPercentileMethod},
        {"histogram", std::move(buckets)},
    };
    std::ofstream summary_file{
        directory_ / "summary.json", std::ios::binary};
    summary_file << boost::json::serialize(summary) << '\n';

    std::ofstream report{directory_ / "REPORT.md", std::ios::binary};
    report << "# Exchange probe " << command_name(command) << "\n\n"
           << "- Evidence: diagnostic bundle; not production readiness proof.\n"
           << "- Samples: " << histogram_.count << "\n"
           << "- Failures: " << histogram_.failures << "\n"
           << "- Artifact complete: "
           << (artifact_complete_ ? "yes" : "no") << "\n"
           << "- p50/p95/p99 upper bounds: "
           << histogram_.percentile(5000U) << "/"
           << histogram_.percentile(9500U) << "/"
           << histogram_.percentile(9900U) << " us\n"
           << "- Event age: unavailable unless an exchange-owned timestamp "
              "contract and clock uncertainty are present.\n";

    std::ofstream svg{directory_ / "latency_histogram.svg", std::ios::binary};
    svg << "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"1000\" "
           "height=\"420\"><rect width=\"100%\" height=\"100%\" "
           "fill=\"white\"/><text x=\"30\" y=\"30\" "
           "font-family=\"sans-serif\" font-size=\"18\">"
        << command_name(command)
        << " bounded log16-subbucket latency histogram</text>";
    const auto peak = *std::max_element(
        histogram_.buckets.begin(), histogram_.buckets.end());
    for (std::size_t index = 0U; index < histogram_.buckets.size(); ++index) {
      if (histogram_.buckets[index] == 0U || peak == 0U) {
        continue;
      }
      const double height =
          330.0 * static_cast<double>(histogram_.buckets[index]) /
          static_cast<double>(peak);
      const double bar_width =
          940.0 / static_cast<double>(histogram_.buckets.size());
      const double x =
          30.0 + static_cast<double>(index) * bar_width;
      svg << "<rect x=\"" << x << "\" y=\"" << 390.0 - height
          << "\" width=\"" << std::max(0.5, bar_width * 0.85)
          << "\" height=\"" << height
          << "\" fill=\"#2457a6\"/>";
    }
    svg << "</svg>\n";
    summary_file.flush();
    report.flush();
    svg.flush();
    storage_failed_ =
        storage_failed_ || !summary_file || !report || !svg;
    output_ << "bundle=" << directory_.string()
            << " samples=" << histogram_.count
            << " failures=" << histogram_.failures
            << " artifact_complete=" << artifact_complete_
            << " storage_ok=" << !storage_failed_ << '\n';
    if (storage_failed_) {
      error_output_ << "bundle_write_error: artifact_write_failed\n";
    }
    return !storage_failed_;
  }

 private:
  std::filesystem::path directory_;
  std::uint64_t byte_limit_{0U};
  std::uint64_t bytes_written_{0U};
  std::uint64_t samples_not_persisted_{0U};
  bool artifact_complete_{true};
  bool storage_failed_{false};
  std::ofstream samples_;
  std::ofstream csv_;
  Histogram histogram_;
  std::mutex mutex_;
  std::ostream& output_;
  std::ostream& error_output_;
  std::string error_;
};

[[nodiscard]] Observation unsupported_observation(
    const ProductSpec& product,
    Surface surface,
    Transport transport,
    std::string error) {
  return Observation{
      .kind = "latency_sample",
      .venue = product.venue,
      .product = product.product,
      .case_name = "session",
      .surface = surface,
      .transport = transport,
      .outcome = Outcome::Unsupported,
      .stage = "profile",
      .error = std::move(error),
      .evidence = nullptr,
  };
}

[[nodiscard]] std::vector<Task> make_tasks(
    const CliOptions& options,
    const std::vector<const ProductSpec*>& products,
    const Environment& environment) {
  std::vector<Task> tasks;
  const auto surface = options.surface.value_or(Surface::Public);
  const auto transport = options.transport.value_or(Transport::Rest);
  for (const auto* product : products) {
    if (surface == Surface::Public && transport == Transport::Rest) {
      for (const auto& item : product->public_rest) {
        if (selected_case(options, item.name)) {
          tasks.push_back({product, &item, nullptr, 0U, std::nullopt,
                           "natural", {}});
        }
      }
    } else if (surface == Surface::Public &&
               transport == Transport::WebSocket) {
      for (const auto& item : product->public_ws) {
        if (selected_case(options, item.name)) {
          tasks.push_back({product, nullptr, &item, 0U, std::nullopt,
                           "natural", {}});
        }
      }
    } else if (surface == Surface::Private &&
               transport == Transport::Rest) {
      const auto slots =
          configured_slots(environment, product->credential_prefix);
      for (const auto& item : product->private_rest) {
        if (!selected_case(options, item.name)) {
          continue;
        }
        if (slots.empty()) {
          tasks.push_back({product, &item, nullptr, 0U, std::nullopt,
                           "natural", {}});
        } else {
          for (const auto slot : slots) {
            tasks.push_back({product, &item, nullptr, slot, std::nullopt,
                             "natural", {}});
          }
        }
      }
    } else {
      tasks.push_back({product, nullptr, nullptr, 0U, std::nullopt,
                       "natural", {}});
    }
  }
  return tasks;
}

[[nodiscard]] std::vector<Task> expand_routes(
    const CliOptions& options,
    const std::vector<Task>& base) {
  if (options.route_mode == RouteMode::Natural) {
    return base;
  }
  std::vector<Task> result;
  if (options.route_mode == RouteMode::Both) {
    result = base;
  }
  for (const auto& task : base) {
    const std::string host =
        task.rest != nullptr ? task.rest->host
                             : task.ws != nullptr ? task.ws->host : "";
    if (host.empty()) {
      auto unavailable = task;
      unavailable.route = "pinned";
      unavailable.route_error = "pinned_route_requires_network_host";
      result.push_back(std::move(unavailable));
      continue;
    }
    if (options.placement_path == PlacementPath::Proxy) {
      auto unavailable = task;
      unavailable.route = "pinned";
      unavailable.route_error = "proxy_resolves_destination_host";
      result.push_back(std::move(unavailable));
      continue;
    }
    boost::asio::io_context context;
    const auto resolved = net_detail::resolve(
        context,
        host,
        "443",
        std::chrono::steady_clock::now() + options.limits.timeout);
    if (!resolved.ok) {
      auto unavailable = task;
      unavailable.route = "pinned";
      unavailable.route_error = "pinned_dns_failed:" + resolved.error;
      result.push_back(std::move(unavailable));
      continue;
    }
    std::set<std::string> ips;
    for (const auto& endpoint : resolved.endpoints) {
      ips.insert(endpoint.endpoint().address().to_string());
    }
    if (ips.size() > kMaxPinnedIpsPerTask) {
      auto unavailable = task;
      unavailable.route = "pinned";
      unavailable.route_error = "pinned_ip_capacity_exceeded";
      result.push_back(std::move(unavailable));
      continue;
    }
    for (const auto& ip : ips) {
      auto pinned = task;
      pinned.pinned_ip = ip;
      pinned.route = "pinned";
      result.push_back(std::move(pinned));
    }
  }
  return result;
}

[[nodiscard]] Observation execute_task(
    const Task& task,
    const CliOptions& options,
    const Environment& environment) {
  auto limits = options.limits;
  limits.attempts = 1U;
  const std::optional<bool> use_proxy =
      options.placement_path == PlacementPath::Proxy;
  if (!task.route_error.empty()) {
    return unsupported_observation(
        *task.product,
        options.surface.value_or(Surface::Public),
        options.transport.value_or(Transport::Rest),
        task.route_error);
  }
  if (task.rest != nullptr) {
    if (!task.rest->private_case) {
      return run_public_rest(
          *task.product, *task.rest, limits, task.pinned_ip, use_proxy);
    }
    if (task.credential_slot == 0U) {
      return unsupported_observation(
          *task.product, Surface::Private, Transport::Rest,
          "credentials_not_configured");
    }
    auto credentials = resolve_credentials(
        environment,
        task.product->credential_prefix,
        task.credential_slot);
    if (!credentials.has_value()) {
      return unsupported_observation(
          *task.product, Surface::Private, Transport::Rest,
          "credential_slot_incomplete");
    }
    auto result =
        run_private_rest(
            *task.product, *task.rest, *credentials, limits, task.pinned_ip,
            use_proxy);
    result.case_name += "#slot" + std::to_string(task.credential_slot);
    return result;
  }
  if (task.ws != nullptr) {
    return run_public_ws(
        *task.product, *task.ws, limits, task.pinned_ip, use_proxy);
  }
  const auto transport = options.transport.value_or(Transport::Rest);
  return unsupported_observation(
      *task.product,
      options.surface.value_or(Surface::Public),
      transport,
      transport == Transport::WebSocket
          ? "private_ws_session_profile_not_declared"
          : transport == Transport::Fix
                ? "fix_logon_profile_not_declared"
                : "transport_profile_not_declared");
}

[[nodiscard]] int run_profile(
    const CliOptions& options,
    Command command,
    std::ostream& output,
    std::ostream& error_output) {
  if (options.connection_mode != ConnectionMode::Cold) {
    error_output
        << "configuration_error: warm connections require an exchange-owned "
           "persistent-session profile; use --connection cold\n";
    return 2;
  }
  const auto profiles = make_profiles();
  const auto selected = select_products(profiles, options);
  if (selected.empty()) {
    error_output << "configuration_error: filters selected no products\n";
    return 2;
  }

  std::string environment_error;
  Environment environment;
  if (options.surface == Surface::Private) {
    environment = load_environment(options.env_file, environment_error);
    if (!environment_error.empty()) {
      error_output << "configuration_error: " << environment_error << '\n';
      return 2;
    }
  }
  const auto base_tasks = make_tasks(options, selected, environment);
  const auto tasks = expand_routes(options, base_tasks);
  if (tasks.empty()) {
    error_output << "configuration_error: filters selected no probe cases\n";
    return 2;
  }

  BundleWriter bundle{options, command, output, error_output};
  if (!bundle.ok()) {
    error_output << "configuration_error: " << bundle.error() << '\n';
    return 2;
  }
  if (!bundle.write_manifest(options, command)) {
    error_output << "configuration_error: " << bundle.error() << '\n';
    return 2;
  }

  const unsigned lanes = resolved_lanes(options);
  const unsigned sample_limit = resolved_samples(options);
  std::atomic<std::uint64_t> sequence{0U};
  std::atomic<bool> any_failure{false};
  for (const auto& task : tasks) {
    const auto deadline =
        command == Command::Stability || options.duration_seconds.has_value()
            ? std::chrono::steady_clock::now() +
                  std::chrono::seconds{
                      command == Command::Stability
                          ? resolved_duration(options)
                          : *options.duration_seconds}
            : std::chrono::steady_clock::time_point::max();
    std::atomic<unsigned> next_sample{0U};
    std::vector<std::thread> workers;
    workers.reserve(lanes);
    for (unsigned lane = 0U; lane < lanes; ++lane) {
      workers.emplace_back([&, lane] {
        while (std::chrono::steady_clock::now() < deadline) {
          const unsigned sample = next_sample.fetch_add(
              1U, std::memory_order_relaxed);
          if (command == Command::Latency && sample >= sample_limit) {
            break;
          }
          if (command == Command::Stability &&
              options.samples.has_value() && sample >= sample_limit) {
            break;
          }
          auto observation = execute_task(task, options, environment);
          observation.kind =
              command == Command::Latency ? "latency_sample"
                                          : "stability_sample";
          if (!observation.expectation_met) {
            any_failure.store(true, std::memory_order_relaxed);
          }
          bundle.append(
              sequence.fetch_add(1U, std::memory_order_relaxed),
              lane,
              observation,
              "cold",
              task.route);
          if (command == Command::Stability) {
            std::this_thread::sleep_for(std::chrono::milliseconds{100});
          }
        }
      });
    }
    for (auto& worker : workers) {
      worker.join();
    }
  }
  if (!bundle.finish(command)) {
    return 2;
  }
  return any_failure.load(std::memory_order_relaxed) ? 1 : 0;
}

}  // namespace

int run_latency(
    const CliOptions& options,
    std::ostream& output,
    std::ostream& error_output) {
  return run_profile(options, Command::Latency, output, error_output);
}

int run_stability(
    const CliOptions& options,
    std::ostream& output,
    std::ostream& error_output) {
  return run_profile(options, Command::Stability, output, error_output);
}

}  // namespace exchange_probe
