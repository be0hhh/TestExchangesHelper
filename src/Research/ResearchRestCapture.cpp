#include "exchange_probe/App.hpp"
#include "exchange_probe/Net.hpp"
#include "exchange_probe/Contracts.hpp"

#include <boost/json.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <latch>
#include <ostream>
#include <string>
#include <thread>
#include <utility>
#include <sys/utsname.h>
#include <unistd.h>

namespace exchange_probe {
namespace {
using Clock = std::chrono::steady_clock;
constexpr std::uint64_t kManifestReserve = 64U * 1024U;
constexpr unsigned kWarmups = 5U;
constexpr auto kCadence = std::chrono::seconds{5};

std::uint64_t utc_ns() {
  return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::system_clock::now().time_since_epoch()).count());
}

boost::json::object environment() {
  boost::json::object result{{"origin", "capture_process_local"},
      {"aws_region", "not_observed"}, {"placement", "not_inferred"},
      {"compiler", __VERSION__}, {"cplusplus", static_cast<unsigned long long>(__cplusplus)}};
  utsname machine{};
  if (uname(&machine) == 0) {
    result["sysname"] = machine.sysname;
    result["release"] = machine.release;
    result["machine"] = machine.machine;
    result["node"] = machine.nodename;
  }
  const auto processors = sysconf(_SC_NPROCESSORS_ONLN);
  if (processors > 0) result["online_processors"] = processors;
  std::ifstream cpu{"/proc/cpuinfo"};
  std::string row;
  for (unsigned index = 0; index < 256 && std::getline(cpu, row); ++index) {
    if (row.starts_with("model name")) {
      const auto colon = row.find(':');
      if (colon != std::string::npos) result["cpu_model"] = row.substr(colon + 1, 256);
      break;
    }
  }
  return result;
}

bool same_semantics(const RestCase& a, const RestCase& b) {
  return a.path == b.path && a.contract == b.contract &&
      a.capabilities == b.capabilities && a.expected_symbol == b.expected_symbol &&
      a.expectation == b.expectation && a.expected_rejection_statuses == b.expected_rejection_statuses &&
      a.expected_rejection_codes == b.expected_rejection_codes && a.native == b.native;
}

std::string select_pair(const CliOptions& options, const ProductSpec& profile,
                       std::array<const RestCase*, 2>& cases) {
  if (options.surface == Surface::Private || options.env_file ||
      options.confirm_private || options.allow_private_adapter)
    return "public_only";
  if (options.transport && options.transport != Transport::Rest) return "rest_only";
  if (options.capture_pcap || options.capture_tls_keys) return "unsupported_capture_option";
  if (!options.symbol.empty()) return "symbol_override_not_supported_for_frozen_rest_cases";
  if (profile.venue != "htx" || (profile.product != "spot" && profile.product != "futures"))
    return "htx_only";
  if (options.cases.size() != 2) return "exactly_two_cases";
  if (options.cases[0] == options.cases[1]) return "distinct_cases";
  const auto seconds = options.duration_seconds.value_or(300U);
  if (options.rounds == 0 || options.rounds > 3 || seconds == 0 || seconds > 300 ||
      options.limits.timeout <= std::chrono::milliseconds{0} ||
      options.limits.timeout > std::chrono::seconds{30} ||
      options.limits.attempts != 1 || options.max_artifact_bytes <= kManifestReserve)
    return "invalid_limits";
  for (std::size_t index = 0; index != cases.size(); ++index) {
    for (const auto& candidate : profile.private_rest)
      if (candidate.name == options.cases[index]) return "public_only";
    for (const auto& candidate : profile.public_rest) {
      if (candidate.name != options.cases[index]) continue;
      if (cases[index] != nullptr) return "duplicate_case_identity";
      cases[index] = &candidate;
    }
    if (cases[index] == nullptr) return "unknown_public_case";
    if (cases[index]->private_case || cases[index]->auth != AuthKind::None) return "public_only";
    if (cases[index]->expectation != Expectation::LogicalSuccess) return "successful_public_case_required";
    if (cases[index]->path.empty() || cases[index]->path.front() != '/' ||
        cases[index]->path.find_first_of("\r\n") != std::string::npos) return "invalid_public_path";
  }
  if (!same_semantics(*cases[0], *cases[1])) return "case_semantics_mismatch";
  const bool spot = profile.product == "spot";
  const std::string standard_host = spot ? "api.huobi.pro" : "api.hbdm.com";
  const std::string aws_host = spot ? "api-aws.huobi.pro" : "api.hbdm.vn";
  if (cases[0]->host == aws_host && cases[1]->host == standard_host) std::swap(cases[0], cases[1]);
  if (cases[0]->host != standard_host || cases[1]->host != aws_host)
    return "documented_htx_host_pair_required";
  return {};
}

boost::json::object result_json(const RestCase& request, const HttpResult& result,
                               unsigned round, unsigned pair, bool warmup,
                               std::uint64_t pair_utc,
                               std::uint64_t offset) {
  boost::json::object timing;
  const auto add = [&](const char* name, const std::optional<std::uint64_t>& value) {
    timing[name] = value ? boost::json::value{*value} : boost::json::value{nullptr};
  };
  add("dns_ns", result.dns_ns);
  add("tcp_connect_ns", result.tcp_connect_ns);
  add("tls_handshake_ns", result.tls_handshake_ns);
  add("request_response_ns", result.request_response_ns);
  add("request_failure_ns", result.request_failure_ns);
  add("request_failure_ns", result.request_failure_ns);
  boost::json::array answers;
  for (const auto& answer : result.dns_answers) answers.emplace_back(answer);
  boost::json::object headers;
  for (const auto& [name, value] : result.public_response_headers) headers[name] = value;
  const auto contract = result.json_present
      ? validate_rest_contract(request.contract, result.json, request.expected_symbol)
      : ContractEvidence{};
  return {{"schema", "exchange.api_probe.rest_receipt.v1"},
      {"round", round}, {"pair", pair}, {"phase", warmup ? "warmup" : "measured"},
      {"case", request.name}, {"host", request.host}, {"path", request.path},
      {"pair_utc_ns", pair_utc}, {"dns_utc_ns", result.dns_utc_ns},
      {"dns_answers", std::move(answers)}, {"timing", std::move(timing)},
      {"request_start_monotonic_ns", result.request_start_monotonic_ns},
      {"response_receipt_monotonic_ns", result.response_receipt_monotonic_ns},
      {"connection_reused", result.connection_reused},
      {"connection_available", result.connection_available},
      {"http_status", result.status}, {"json_present", result.json_present},
      {"logical_success", contract.logical_success}, {"schema_success", contract.schema_success},
      {"symbol_success", contract.symbol_success}, {"contract_error", contract.error},
      {"contract_scope", to_string(request.contract)},
      {"symbol_evidence", request.expected_symbol.empty() ? "not_requested" : "declared_contract"},
      {"tls_verified", result.tls_verified}, {"stage", result.stage}, {"error", result.error},
      {"response_headers", std::move(headers)},
      {"transport", boost::json::object{{"remote_ip", result.transport.remote_ip},
          {"ip_family", result.transport.ip_family}, {"tls_version", result.transport.tls_version},
          {"tls_cipher", result.transport.tls_cipher}, {"alpn", result.transport.alpn},
          {"certificate_not_after", result.transport.certificate_not_after},
          {"tls_session_reused", result.transport.tls_session_reused}}},
      {"body_offset", offset}, {"body_length", result.raw_body.size()},
      {"response_complete", result.response_receipt_monotonic_ns != 0},
      {"timing_boundary", "request_write_start_to_complete_http_response_before_json_parse"}};
}

std::array<HttpResult, 2> get_pair(PublicHttpConnection& standard,
                                 PublicHttpConnection& aws,
                                 Clock::time_point deadline) {
  std::latch ready{2};
  std::latch start{1};
  bool released = false, aborted = false;
  const auto get = [&](PublicHttpConnection& connection) {
    ready.count_down();
    start.wait();
    if (aborted) {
      HttpResult result{};
      result.stage = "configuration";
      result.error = "paired_worker_start_failed";
      return result;
    }
    return connection.get(deadline);
  };
  auto first = std::async(std::launch::async, [&] { return get(standard); });
  try {
    auto second = std::async(std::launch::async, [&] { return get(aws); });
    ready.wait();
    start.count_down();
    released = true;
    return {first.get(), second.get()};
  } catch (...) {
    // Release the first worker even if starting the second thread failed.
    if (!released) { aborted = true; start.count_down(); }
    throw;
  }
}
}  // namespace

int capture_paired_rest_research(const CliOptions& options, const ProductSpec& profile,
                                const std::filesystem::path& directory,
                                std::ostream& output, std::ostream& error_output) {
  std::array<const RestCase*, 2> cases{};
  const auto admission_error = select_pair(options, profile, cases);
  if (!admission_error.empty()) {
    error_output << "rest_research_error: " << admission_error << '\n';
    return 2;
  }
  std::error_code filesystem_error;
  if (std::filesystem::exists(directory, filesystem_error) || filesystem_error) {
    error_output << "rest_research_error: output_directory_exists_or_unavailable\n";
    return 2;
  }
  if (!std::filesystem::create_directories(directory, filesystem_error) || filesystem_error) {
    error_output << "rest_research_error: output_directory_create_failed\n";
    return 2;
  }
  std::ofstream bodies{directory / "response_bodies.bin", std::ios::binary};
  std::ofstream receipts{directory / "requests.jsonl", std::ios::binary};
  if (!bodies || !receipts) {
    error_output << "rest_research_error: artifact_open_failed\n";
    return 2;
  }
  boost::json::object manifest{{"schema", "exchange.api_probe.rest_bundle.v1"},
      {"venue", profile.venue}, {"product", profile.product}, {"status", "incomplete"},
      {"started_utc_ns", utc_ns()}, {"environment", environment()},
      {"rounds_requested", options.rounds}, {"duration_seconds_per_round", options.duration_seconds.value_or(300U)},
      {"warmup_pairs_per_round", kWarmups}, {"cadence_seconds", 5},
      {"retry_policy", "none"}, {"reconnect_policy", "new_connection_only_at_explicit_round_boundary"},
      {"scope", "public_read_only_diagnostic"}, {"aws_region", "not_observed"},
      {"conclusions", "no_relative_latency_or_placement_claim"}};
  boost::json::array selected;
  for (const auto* request : cases)
    selected.emplace_back(boost::json::object{{"case", request->name}, {"host", request->host},
        {"path", request->path}, {"expected_symbol", request->expected_symbol},
        {"contract", to_string(request->contract)}});
  manifest["cases"] = std::move(selected);
  boost::json::array round_rows;
  std::uint64_t body_offset = 0, artifact_bytes = 0;
  unsigned requests = 0, recorded_requests = 0, warm_requests = 0, cold_requests = 0, unissued_requests = 0;
  unsigned errors = 0, peer_closes = 0, http_errors = 0, parse_errors = 0;
  unsigned completed_rounds = 0;
  bool any_failure = false, artifact_ok = true;
  const auto write_result = [&](const RestCase& request, const HttpResult& result,
                                unsigned round, unsigned pair, bool warmup,
                                std::uint64_t pair_utc) {
    ++requests;
    if (result.connection_reused) ++warm_requests;
    else if (result.dns_ns || result.tcp_connect_ns) ++cold_requests;
    else ++unissued_requests;
    if (!result.error.empty()) ++errors;
    if (result.response_receipt_monotonic_ns != 0 && !result.connection_available) ++peer_closes;
    if (result.status != 0 && (result.status < 200 || result.status >= 300)) ++http_errors;
    if (result.error.starts_with("invalid_json:")) ++parse_errors;
    if (!artifact_ok) return false;
    const auto row = boost::json::serialize(result_json(
        request, result, round, pair, warmup, pair_utc, body_offset)) + '\n';
    const std::uint64_t added = static_cast<std::uint64_t>(result.raw_body.size() + row.size());
    if (added > options.max_artifact_bytes - kManifestReserve - artifact_bytes) {
      artifact_ok = false;
      error_output << "rest_research_error: artifact_capacity_exceeded\n";
      return false;
    }
    bodies.write(result.raw_body.data(), static_cast<std::streamsize>(result.raw_body.size()));
    receipts << row;
    if (!bodies || !receipts) { artifact_ok = false; return false; }
    body_offset += result.raw_body.size(); artifact_bytes += added;
    ++recorded_requests;
    return true;
  };
  try {
    for (unsigned round = 1; round <= options.rounds && artifact_ok; ++round) {
      PublicHttpConnection standard{*cases[0]}, aws{*cases[1]};
      unsigned pair = 0, measured_pairs = 0, skipped_slots = 0;
      std::string stop_reason;
      const auto sample = [&](bool warmup, Clock::time_point round_deadline) {
        const auto deadline = std::min(round_deadline, Clock::now() + options.limits.timeout);
        const auto pair_utc = utc_ns();
        auto results = get_pair(standard, aws, deadline);
        ++pair;
        bool valid = true;
        for (std::size_t index = 0; index != results.size(); ++index) {
          const auto& result = results[index];
          const auto contract = result.json_present
              ? validate_rest_contract(cases[index]->contract, result.json, cases[index]->expected_symbol)
              : ContractEvidence{};
          const bool contract_ok = contract.logical_success && contract.schema_success && contract.symbol_success;
          if (!write_result(*cases[index], result, round, pair, warmup, pair_utc)) valid = false;
          if (!result.error.empty() || !result.connection_available ||
              result.status < 200 || result.status >= 300 || !contract_ok ||
              (!warmup && !result.connection_reused)) {
            valid = false;
            stop_reason = !result.error.empty() ? result.error :
                !result.connection_available ? "peer_closed_connection" :
                !warmup && !result.connection_reused ? "measured_request_was_not_reused" :
                !contract_ok ? "public_response_contract_failure" : "http_status_failure";
          }
        }
        return valid;
      };
      auto warmup_slot = Clock::now();
      for (unsigned warmup = 0; warmup < kWarmups; ++warmup) {
        std::this_thread::sleep_until(warmup_slot);
        if (!sample(true, Clock::now() + options.limits.timeout)) {
          any_failure = true;
          break;
        }
        warmup_slot = Clock::now() + kCadence;
      }
      if (stop_reason.empty() && artifact_ok) {
        const auto end = warmup_slot + std::chrono::seconds{options.duration_seconds.value_or(300U)};
        auto next = warmup_slot;
        while (next < end) {
          std::this_thread::sleep_until(next);
          if (Clock::now() >= end) break;
          if (!sample(false, end)) { any_failure = true; break; }
          ++measured_pairs;
          next += kCadence;
          while (next < Clock::now()) { next += kCadence; ++skipped_slots; }
        }
        if (stop_reason.empty() && artifact_ok) ++completed_rounds;
      }
      round_rows.emplace_back(boost::json::object{{"round", round}, {"pairs", pair},
          {"measured_pairs", measured_pairs}, {"skipped_cadence_slots", skipped_slots},
          {"stop_reason", stop_reason}, {"status", stop_reason.empty() && artifact_ok ? "complete" : "incomplete"}});
    }
  } catch (const std::exception& exception) {
    any_failure = true;
    manifest["error"] = std::string{"exception:"} + exception.what();
    error_output << "rest_research_error: capture_exception\n";
  }
  bodies.flush(); receipts.flush();
  artifact_ok = artifact_ok && bodies.good() && receipts.good();
  const bool complete = !any_failure && artifact_ok && completed_rounds == options.rounds;
  manifest["status"] = complete ? "complete" : "incomplete";
  manifest["finished_utc_ns"] = utc_ns();
  manifest["rounds"] = std::move(round_rows);
  manifest["counters"] = boost::json::object{{"requests", requests},
      {"recorded_requests", recorded_requests},
      {"cold_requests", cold_requests}, {"reused_requests", warm_requests},
      {"unissued_requests", unissued_requests},
      {"errors", errors}, {"peer_closes", peer_closes}, {"http_errors", http_errors},
      {"json_parse_errors", parse_errors}, {"completed_rounds", completed_rounds}};
  const auto manifest_text = boost::json::serialize(manifest) + '\n';
  if (manifest_text.size() > kManifestReserve) {
    error_output << "rest_research_error: manifest_capacity_exceeded\n";
    return 1;
  }
  std::ofstream final_manifest{directory / "manifest.json", std::ios::binary};
  final_manifest << manifest_text; final_manifest.flush();
  if (!final_manifest.good()) {
    error_output << "rest_research_error: manifest_write_failed\n";
    return 1;
  }
  output << boost::json::serialize(boost::json::object{
      {"schema", "exchange.api_probe.rest_capture_result.v1"},
      {"directory", directory.string()}, {"status", complete ? "complete" : "incomplete"}}) << '\n';
  return complete ? 0 : 1;
}
}  // namespace exchange_probe
