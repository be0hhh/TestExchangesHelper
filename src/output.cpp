#include "exchange_probe/app.hpp"

#include <boost/json/serialize.hpp>

#include <algorithm>
#include <iomanip>
#include <ostream>
#include <string>

namespace exchange_probe {
namespace {

[[nodiscard]] std::string joined(
    const std::vector<std::string>& values,
    std::string_view separator = ",") {
  std::string result;
  for (const auto& value : values) {
    if (!result.empty()) {
      result += separator;
    }
    result += value;
  }
  return result;
}

void emit_json_line(
    const boost::json::value& value,
    std::ostream& output) {
  output << boost::json::serialize(value) << '\n';
}

[[nodiscard]] boost::json::object stage_timings_json(
    const StageTimings& timings) {
  return {
      {"dns_us", timings.dns_us},
      {"tcp_connect_us", timings.tcp_connect_us},
      {"proxy_connect_us", timings.proxy_connect_us},
      {"tls_handshake_us", timings.tls_handshake_us},
      {"request_write_us", timings.request_write_us},
      {"ttfb_us", timings.ttfb_us},
      {"body_read_us", timings.body_read_us},
      {"json_parse_us", timings.json_parse_us},
      {"ws_handshake_us", timings.ws_handshake_us},
      {"welcome_us", timings.welcome_us},
      {"subscribe_write_us", timings.subscribe_write_us},
      {"ack_us", timings.ack_us},
      {"first_data_us", timings.first_data_us},
      {"total_us", timings.total_us},
  };
}

[[nodiscard]] boost::json::object transport_metadata_json(
    const TransportMetadata& metadata) {
  return {
      {"remote_ip", metadata.remote_ip},
      {"ip_family", metadata.ip_family},
      {"tls_version", metadata.tls_version},
      {"tls_cipher", metadata.tls_cipher},
      {"alpn", metadata.alpn},
      {"certificate_not_after", metadata.certificate_not_after},
      {"tls_session_reused", metadata.tls_session_reused},
      {"tcp_info_available", metadata.tcp_info_available},
      {"tcp_rtt_us", metadata.tcp_rtt_us},
      {"tcp_rtt_variance_us", metadata.tcp_rtt_variance_us},
      {"tcp_retransmits", metadata.tcp_retransmits},
      {"tcp_congestion_window", metadata.tcp_congestion_window},
      {"tcp_mss", metadata.tcp_mss},
  };
}

}  // namespace

boost::json::object capability_json(
    const ProductSpec& product,
    const CapabilityRow& capability) {
  return {
      {"schema_version", 3},
      {"kind", "capability"},
      {"venue", product.venue},
      {"product", product.product},
      {"capability", capability.name},
      {"surface", to_string(capability.surface)},
      {"transport", to_string(capability.transport)},
      {"wire", to_string(capability.wire)},
      {"selection", to_string(capability.selection)},
      {"profile_status", to_string(capability.profile_status)},
      {"core_status", to_string(capability.core_status)},
      {"requires_confirmation", capability.requires_confirmation},
      {"native", capability.native},
  };
}

boost::json::object observation_json(const Observation& observation) {
  boost::json::object result{
      {"schema_version", 3},
      {"kind", observation.kind},
      {"venue", observation.venue},
      {"product", observation.product},
      {"case", observation.case_name},
      {"capability", observation.capability},
      {"surface", to_string(observation.surface)},
      {"transport", to_string(observation.transport)},
      {"wire", to_string(observation.wire)},
      {"selection", to_string(observation.selection)},
      {"expectation", to_string(observation.expectation)},
      {"outcome", to_string(observation.outcome)},
      {"expectation_met", observation.expectation_met},
      {"transport_ok", observation.transport_ok},
      {"tls_ok", observation.tls_ok},
      {"http_ok", observation.http_ok},
      {"logical_ok", observation.logical_ok},
      {"schema_ok", observation.schema_ok},
      {"http_status", observation.http_status},
      {"elapsed_ms", observation.elapsed_ms},
      {"payload_bytes", observation.payload_bytes},
      {"attempts_allowed", observation.attempts_allowed},
      {"attempts_used", observation.attempts_used},
      {"stage_timings", stage_timings_json(observation.timings)},
      {"transport_metadata",
       transport_metadata_json(observation.transport_metadata)},
      {"stage", observation.stage},
      {"error", observation.error},
      {"evidence", observation.evidence},
  };
  if (observation.raw_public.has_value()) {
    result["raw_public"] = *observation.raw_public;
  }
  return result;
}

void emit_observations(
    const std::vector<Observation>& observations,
    bool jsonl,
    std::ostream& output) {
  if (jsonl) {
    for (const auto& observation : observations) {
      emit_json_line(observation_json(observation), output);
    }
    return;
  }

  output << std::left
         << std::setw(13) << "VENUE"
         << std::setw(10) << "PRODUCT"
         << std::setw(20) << "CASE"
         << std::setw(9) << "SURFACE"
         << std::setw(8) << "WIRE"
         << std::setw(22) << "OUTCOME"
         << std::setw(9) << "TIME_MS"
         << "DETAIL\n";
  for (const auto& observation : observations) {
    output << std::left
           << std::setw(13) << observation.venue
           << std::setw(10) << observation.product
           << std::setw(20) << observation.case_name
           << std::setw(9) << to_string(observation.surface)
           << std::setw(8) << to_string(observation.wire)
           << std::setw(22) << to_string(observation.outcome)
           << std::setw(9) << observation.elapsed_ms
           << (observation.error.empty() ? observation.stage
                                         : observation.error)
           << '\n';
  }
}

void emit_matrix(
    const std::vector<const ProductSpec*>& products,
    bool jsonl,
    std::ostream& output) {
  if (jsonl) {
    for (const auto* product : products) {
      for (const auto& capability : product->capabilities) {
        emit_json_line(capability_json(*product, capability), output);
      }
    }
    return;
  }

  output << std::left
         << std::setw(13) << "VENUE"
         << std::setw(10) << "PRODUCT"
         << std::setw(24) << "CAPABILITY"
         << std::setw(9) << "SURFACE"
         << std::setw(8) << "VIA"
         << std::setw(18) << "WIRE"
         << std::setw(27) << "SELECTION"
         << "STATUS\n";
  for (const auto* product : products) {
    for (const auto& capability : product->capabilities) {
      output << std::left
             << std::setw(13) << product->venue
             << std::setw(10) << product->product
             << std::setw(24) << capability.name
             << std::setw(9) << to_string(capability.surface)
             << std::setw(8) << to_string(capability.transport)
             << std::setw(18) << to_string(capability.wire)
             << std::setw(27) << to_string(capability.selection)
             << to_string(capability.profile_status)
             << '\n';
    }
    if (!product->notes.empty()) {
      output << "  notes: " << joined(product->notes, "; ") << '\n';
    }
  }
}

void emit_audit(
    const boost::json::object& audit,
    bool jsonl,
    std::ostream& output) {
  if (jsonl) {
    emit_json_line(audit, output);
    return;
  }
  const auto summary = audit.if_contains("summary");
  output << "source audit: "
         << (audit.at("ok").as_bool() ? "PASS" : "FAIL") << '\n';
  if (summary != nullptr && summary->is_object()) {
    for (const auto& item : summary->as_object()) {
      output << "  " << item.key() << ": "
             << boost::json::serialize(item.value()) << '\n';
    }
  }
  if (const auto* issues = audit.if_contains("issues");
      issues != nullptr && issues->is_array()) {
    for (const auto& issue : issues->as_array()) {
      output << "  - " << boost::json::serialize(issue) << '\n';
    }
  }
}

}  // namespace exchange_probe
