#pragma once

#include "exchange_probe/Model.hpp"

#include <initializer_list>
#include <string>
#include <utility>

namespace exchange_probe::profile_factory {

[[nodiscard]] inline SourceAnchor anchor(
    std::string path,
    std::string symbol = {},
    std::initializer_list<std::string> literals = {}) {
  return {
      .path = std::move(path),
      .symbol = std::move(symbol),
      .literals = {literals.begin(), literals.end()},
  };
}

[[nodiscard]] inline RestCase public_rest(
    std::string name,
    std::string host,
    std::string path,
    std::initializer_list<std::string> capabilities = {},
    RestContract contract = RestContract::None,
    std::string expected_symbol = {},
    SourceAnchor core_anchor = {},
    SourceAnchor parser_anchor = {}) {
  return {
      .name = std::move(name),
      .host = std::move(host),
      .path = std::move(path),
      .capabilities = {capabilities.begin(), capabilities.end()},
      .selection = core_anchor.path.empty()
                       ? Selection::DiagnosticVariant
                       : Selection::CoreSelected,
      .contract = contract,
      .expected_symbol = std::move(expected_symbol),
      .core_anchor = std::move(core_anchor),
      .parser_anchor = std::move(parser_anchor),
  };
}

[[nodiscard]] inline RestCase negative_rest(
    RestCase probe_case,
    std::initializer_list<unsigned> statuses,
    std::initializer_list<std::string> codes) {
  probe_case.expectation = Expectation::SymbolRejected;
  probe_case.expected_rejection_statuses = {statuses.begin(), statuses.end()};
  probe_case.expected_rejection_codes = {codes.begin(), codes.end()};
  return probe_case;
}

[[nodiscard]] inline RestCase private_rest(
    std::string name,
    std::string host,
    std::string path,
    std::initializer_list<std::string> capabilities,
    RestContract contract,
    AuthKind auth,
    Selection selection = Selection::DiagnosticVariant) {
  return {
      .name = std::move(name),
      .host = std::move(host),
      .path = std::move(path),
      .capabilities = {capabilities.begin(), capabilities.end()},
      .selection = selection,
      .contract = contract,
      .auth = auth,
      .private_case = true,
  };
}

[[nodiscard]] inline WsCase json_ws(
    std::string name,
    std::string host,
    std::string path,
    std::string subscribe,
    std::initializer_list<std::string> capabilities,
    SourceAnchor core_anchor,
    WsAckKind ack_kind,
    std::string expected_topic,
    std::string expected_symbol,
    std::string expected_request_id = {}) {
  return {
      .name = std::move(name),
      .host = std::move(host),
      .path = std::move(path),
      .subscribe = std::move(subscribe),
      .capabilities = {capabilities.begin(), capabilities.end()},
      .selection = core_anchor.path.empty()
                       ? Selection::DiagnosticVariant
                       : Selection::CoreSelected,
      .wire = Wire::Json,
      .ack_kind = ack_kind,
      .data_kind = WsDataKind::TopicJson,
      .expected_symbol = std::move(expected_symbol),
      .expected_topic = std::move(expected_topic),
      .expected_request_id = std::move(expected_request_id),
      .require_ack = ack_kind != WsAckKind::None,
      .require_data = true,
      .core_anchor = std::move(core_anchor),
  };
}

[[nodiscard]] inline WsCase kucoin_ws(
    std::string name,
    std::string host,
    std::string subscribe,
    std::initializer_list<std::string> capabilities,
    std::string expected_topic,
    std::string expected_symbol,
    SourceAnchor core_anchor) {
  return {
      .name = std::move(name),
      .host = std::move(host),
      .path = "/",
      .subscribe = std::move(subscribe),
      .capabilities = {capabilities.begin(), capabilities.end()},
      .selection = Selection::CoreSelected,
      .wire = Wire::BinaryJson,
      .ack_kind = WsAckKind::KucoinSubscription,
      .data_kind = WsDataKind::KucoinBinaryJson,
      .expected_symbol = std::move(expected_symbol),
      .expected_topic = std::move(expected_topic),
      .expected_request_id = "probe-kucoin-1",
      .subscribe_binary = true,
      .inbound_binary = true,
      .read_welcome = true,
      .require_ack = true,
      .require_data = true,
      .core_anchor = std::move(core_anchor),
  };
}

[[nodiscard]] inline WsCase gate_sbe(
    std::string name,
    std::string channel,
    std::string payload_item,
    std::string capability,
    std::uint16_t expected_template,
    SourceAnchor core_anchor) {
  WsCase result{
      .name = std::move(name),
      .host = "fx-ws.gateio.ws",
      .path = "/v4/ws/usdt/sbe?sbe_schema_id=1",
      .subscribe =
          "{\"time\":1,\"channel\":\"" + channel +
          "\",\"event\":\"subscribe\",\"payload\":[\"" +
          payload_item + "\"]}",
      .capabilities = {std::move(capability)},
      .selection = Selection::CoreSelected,
      .wire = Wire::Sbe,
      .ack_kind = WsAckKind::GateSubscription,
      .data_kind = WsDataKind::SbeHeader,
      .handshake_headers = {{"X-Gate-Size-Decimal", "1"}},
      .expected_symbol = "BTC_USDT",
      .expected_topic = std::move(channel),
      .expected_sbe_schema = 1,
      .inbound_binary = true,
      .require_ack = true,
      .require_data = true,
      .core_anchor = core_anchor,
      .parser_anchor = std::move(core_anchor),
  };
  if (expected_template != 0) {
    result.expected_sbe_templates.push_back(expected_template);
  }
  return result;
}

[[nodiscard]] inline WsCase bitget_sbe(
    std::string name,
    std::string inst_type,
    std::string topic,
    std::string capability,
    std::uint16_t expected_template,
    SourceAnchor core_anchor) {
  WsCase result{
      .name = std::move(name),
      .host = "ws.bitget.com",
      .path = "/v3/ws/public/sbe",
      .subscribe =
          "{\"op\":\"subscribe\",\"args\":[{\"instType\":\"" +
          inst_type + "\",\"topic\":\"" + topic +
          "\",\"symbol\":\"BTCUSDT\"}]}",
      .capabilities = {std::move(capability)},
      .selection = Selection::CoreSelected,
      .wire = Wire::Sbe,
      .ack_kind = WsAckKind::BitgetSubscription,
      .data_kind = WsDataKind::SbeHeader,
      .handshake_headers = {
          {"Origin", "https://www.bitget.com"},
          {"Referer", "https://www.bitget.com/"},
      },
      .expected_symbol = "BTCUSDT",
      .expected_topic = std::move(topic),
      .expected_sbe_schema = 1,
      .inbound_binary = true,
      .require_ack = true,
      .require_data = true,
      .core_anchor = core_anchor,
      .parser_anchor = std::move(core_anchor),
  };
  if (expected_template != 0) {
    result.expected_sbe_templates.push_back(expected_template);
  }
  return result;
}

[[nodiscard]] inline SourceAnchor credential_anchor(
    std::string path,
    std::string prefix) {
  return anchor(
      std::move(path),
      {},
      {".stagedCredentialEnvPrefix", std::move(prefix)});
}

}  // namespace exchange_probe::profile_factory
