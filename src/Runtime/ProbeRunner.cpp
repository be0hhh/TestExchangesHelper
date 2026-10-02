#include "exchange_probe/App.hpp"

#include "exchange_probe/Contracts.hpp"
#include "exchange_probe/Credentials.hpp"
#include "exchange_probe/Net.hpp"

#include <boost/json/object.hpp>
#include <boost/json/serialize.hpp>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <ostream>
#include <string>
#include <string_view>
#include <vector>

namespace exchange_probe {
namespace {

[[nodiscard]] std::string first_capability(
    const std::vector<std::string>& capabilities) {
  return capabilities.empty() ? std::string{} : capabilities.front();
}

[[nodiscard]] Outcome transport_outcome(
    std::string_view stage,
    std::string_view error) {
  if (error.find("capacity_exceeded") != std::string_view::npos) {
    return Outcome::CapacityExceeded;
  }
  if (error.find("timeout") != std::string_view::npos) {
    return Outcome::Timeout;
  }
  if (stage.find("proxy") != std::string_view::npos) {
    return Outcome::ProxyError;
  }
  if (stage.find("tls") != std::string_view::npos) {
    return Outcome::TlsError;
  }
  return Outcome::TransportError;
}

[[nodiscard]] boost::json::object contract_evidence_json(
    const ContractEvidence& evidence) {
  boost::json::array missing;
  for (const auto& field : evidence.missing_fields) {
    missing.emplace_back(field);
  }
  return {
      {"contract", evidence.contract},
      {"logical_success", evidence.logical_success},
      {"schema_success", evidence.schema_success},
      {"symbol_success", evidence.symbol_success},
      {"row_count", evidence.row_count},
      {"native_symbol", evidence.native_symbol},
      {"api_code", evidence.api_code},
      {"units", evidence.units},
      {"missing_fields", std::move(missing)},
      {"error", evidence.error},
  };
}

[[nodiscard]] bool expected_status(
    const RestCase& probe_case,
    unsigned status) {
  return std::find(
             probe_case.expected_rejection_statuses.begin(),
             probe_case.expected_rejection_statuses.end(),
             status) != probe_case.expected_rejection_statuses.end();
}

[[nodiscard]] bool expected_code(
    const RestCase& probe_case,
    std::string_view code) {
  return !code.empty() &&
         std::find(
             probe_case.expected_rejection_codes.begin(),
             probe_case.expected_rejection_codes.end(),
             code) != probe_case.expected_rejection_codes.end();
}

[[nodiscard]] Observation classify_http(
    const ProductSpec& product,
    const RestCase& probe_case,
    const HttpResult& result,
    const RunLimits& limits) {
  Observation observation{
      .kind = "observation",
      .venue = product.venue,
      .product = product.product,
      .case_name = probe_case.name,
      .capability = first_capability(probe_case.capabilities),
      .surface = probe_case.private_case ? Surface::Private : Surface::Public,
      .transport = Transport::Rest,
      .wire = Wire::Json,
      .selection = probe_case.selection,
      .expectation = probe_case.expectation,
      .outcome = Outcome::NotRun,
      .tls_ok = result.tls_verified,
      .http_status = result.status,
      .elapsed_ms = result.elapsed_ms,
      .payload_bytes = result.body_bytes,
      .timings = result.timings,
      .transport_metadata = result.transport,
      .stage = result.stage,
      .error = result.error,
      .evidence = nullptr,
      .raw_public = std::nullopt,
  };
  if (observation.timings.total_us == 0U && result.elapsed_ms != 0U) {
    observation.timings.total_us = result.elapsed_ms * 1000U;
  }
  observation.transport_ok = result.status != 0 && result.tls_verified;
  observation.http_ok = result.status >= 200 && result.status < 300;
  if (!result.json_present) {
    if (result.status != 0) {
      observation.outcome =
          observation.http_ok ? Outcome::SchemaError : Outcome::HttpError;
    } else {
      observation.outcome = transport_outcome(result.stage, result.error);
    }
    if (observation.error.empty()) {
      observation.error = "response_json_missing";
    }
    return observation;
  }

  const auto evidence = validate_rest_contract(
      probe_case.contract,
      result.json,
      probe_case.expected_symbol);
  observation.evidence = contract_evidence_json(evidence);
  if (!probe_case.private_case && limits.raw_public) {
    const auto serialized = boost::json::serialize(result.json);
    observation.raw_public = bounded_public_frame(serialized);
  }

  if (probe_case.expectation == Expectation::SymbolRejected) {
    const bool server_or_rate_limit =
        result.status == 429 || result.status >= 500;
    const bool rejected =
        !server_or_rate_limit &&
        (expected_status(probe_case, result.status) ||
         expected_code(probe_case, evidence.api_code));
    observation.logical_ok = rejected;
    observation.schema_ok = rejected;
    observation.expectation_met = rejected;
    observation.outcome =
        rejected ? Outcome::ExpectedRejection
                 : result.status >= 500 ? Outcome::HttpError
                                        : Outcome::LogicalError;
    if (!rejected && observation.error.empty()) {
      observation.error = "unexpected_negative_case_response";
    }
    return observation;
  }

  if (!observation.http_ok) {
    observation.outcome = Outcome::HttpError;
    if (observation.error.empty()) {
      observation.error = "http_status_not_2xx";
    }
    return observation;
  }
  if (probe_case.contract == RestContract::None) {
    observation.logical_ok = true;
    observation.schema_ok = true;
    observation.expectation_met = true;
    observation.outcome = Outcome::Success;
    observation.evidence = boost::json::object{
        {"contract", "undeclared"},
        {"semantic_proof", false},
        {"json_shape", shape_of(result.json)},
    };
    return observation;
  }
  observation.logical_ok = evidence.logical_success;
  observation.schema_ok = evidence.schema_success;
  observation.expectation_met =
      observation.logical_ok && observation.schema_ok;
  observation.outcome =
      !observation.logical_ok
          ? Outcome::LogicalError
          : !observation.schema_ok ? Outcome::SchemaError : Outcome::Success;
  if (!observation.expectation_met && observation.error.empty()) {
    observation.error = evidence.error;
  }
  return observation;
}

[[nodiscard]] bool successful(const Observation& observation) {
  return observation.expectation_met;
}

[[nodiscard]] Observation run_http_attempts(
    const ProductSpec& product,
    const RestCase& probe_case,
    const RunLimits& limits,
    const std::optional<SignedRequest>& signed_request,
    const std::optional<std::string>& pinned_ip,
    const std::optional<bool>& use_proxy) {
  Observation last;
  const auto attempts = std::max(1U, limits.attempts);
  for (unsigned attempt = 1; attempt <= attempts; ++attempt) {
    const auto result = execute_http(
        probe_case,
        std::chrono::steady_clock::now() + limits.timeout,
        signed_request,
        pinned_ip,
        use_proxy);
    last = classify_http(product, probe_case, result, limits);
    last.attempts_allowed = attempts;
    last.attempts_used = attempt;
    if (successful(last)) {
      break;
    }
  }
  return last;
}

[[nodiscard]] Observation configuration_observation(
    const ProductSpec& product,
    const RestCase& probe_case,
    std::string error) {
  return {
      .kind = "observation",
      .venue = product.venue,
      .product = product.product,
      .case_name = probe_case.name,
      .capability = first_capability(probe_case.capabilities),
      .surface = probe_case.private_case ? Surface::Private : Surface::Public,
      .transport = Transport::Rest,
      .wire = Wire::Json,
      .selection = probe_case.selection,
      .expectation = probe_case.expectation,
      .outcome = Outcome::ConfigurationError,
      .stage = "configuration",
      .error = std::move(error),
      .evidence = nullptr,
      .raw_public = std::nullopt,
  };
}

}  // namespace
Observation run_public_rest(
    const ProductSpec& product,
    const RestCase& probe_case,
    const RunLimits& limits,
    const std::optional<std::string>& pinned_ip,
    const std::optional<bool>& use_proxy) {
  return run_http_attempts(
      product, probe_case, limits, std::nullopt, pinned_ip, use_proxy);
}

Observation run_private_rest(
    const ProductSpec& product,
    const RestCase& probe_case,
    const Credentials& credentials,
    const RunLimits& limits,
    const std::optional<std::string>& pinned_ip,
    const std::optional<bool>& use_proxy) {
  std::string sign_error;
  const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::system_clock::now().time_since_epoch())
                       .count();
  const auto signed_request =
      sign_request(probe_case, credentials, now, sign_error);
  if (!signed_request.has_value()) {
    return configuration_observation(
        product,
        probe_case,
        "signing_failed:" + sign_error);
  }
  return run_http_attempts(
      product, probe_case, limits, signed_request, pinned_ip, use_proxy);
}

Observation run_public_ws(
    const ProductSpec& product,
    const WsCase& probe_case,
    const RunLimits& limits,
    const std::optional<std::string>& pinned_ip,
    const std::optional<bool>& use_proxy) {
  Observation last;
  const auto attempts = std::max(1U, limits.attempts);
  for (unsigned attempt = 1; attempt <= attempts; ++attempt) {
    const auto result = observe_ws(
        probe_case,
        std::chrono::steady_clock::now() + limits.timeout,
        pinned_ip,
        use_proxy);
    last = Observation{
        .kind = "observation",
        .venue = product.venue,
        .product = product.product,
        .case_name = probe_case.name,
        .capability = first_capability(probe_case.capabilities),
        .surface = Surface::Public,
        .transport = Transport::WebSocket,
        .wire = probe_case.wire,
        .selection = probe_case.selection,
        .expectation = Expectation::LogicalSuccess,
        .outcome = Outcome::NotRun,
        .transport_ok = result.connected,
        .tls_ok = result.tls_verified,
        .logical_ok = result.ack_complete || !probe_case.require_ack,
        .schema_ok = result.data_complete || !probe_case.require_data,
        .elapsed_ms = result.elapsed_ms,
        .payload_bytes = result.payload_bytes,
        .attempts_allowed = attempts,
        .attempts_used = attempt,
        .timings = result.timings,
        .transport_metadata = result.transport,
        .stage = result.protocol_stage,
        .error = result.error,
        .evidence = boost::json::object{
            {"ack_complete", result.ack_complete},
            {"data_complete", result.data_complete},
            {"binary", result.binary},
            {"control_pings", result.control_pings},
            {"json_shape",
             result.json_present ? shape_of(result.json)
                                 : boost::json::value(
                                       boost::json::string{"not_json"})},
        },
        .raw_public = std::nullopt,
    };
    last.expectation_met =
        last.transport_ok && last.tls_ok &&
        last.logical_ok && last.schema_ok && last.error.empty();
    if (last.timings.total_us == 0U && result.elapsed_ms != 0U) {
      last.timings.total_us = result.elapsed_ms * 1000U;
    }
    if (last.expectation_met) {
      last.outcome = Outcome::Success;
    } else if (result.protocol_stage == "decompression") {
      last.outcome =
          result.error.find("capacity_exceeded") != std::string::npos
              ? Outcome::CapacityExceeded
              : Outcome::SchemaError;
    } else if (!last.error.empty()) {
      last.outcome =
          transport_outcome(result.protocol_stage, result.error);
    } else {
      last.outcome =
          !last.logical_ok ? Outcome::LogicalError : Outcome::SchemaError;
    }
    if (limits.raw_public && !result.payload.empty()) {
      last.raw_public = bounded_public_frame(result.payload);
    }
    if (last.expectation_met) {
      break;
    }
  }
  return last;
}

}  // namespace exchange_probe
