#include "exchange_probe/Contracts.hpp"

#include <boost/json/array.hpp>
#include <boost/json/object.hpp>
#include <boost/json/serialize.hpp>
#include <boost/system/error_code.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <set>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace exchange_probe {
namespace {

enum class Envelope {
  Plain,
  Binance,
  Bybit,
  Okx,
  Gate,
  KucoinUta,
  KucoinFutures,
  Bitget,
};

struct RestSpec {
  Envelope envelope{Envelope::Plain};
  std::string_view symbol_field;
  std::span<const std::string_view> required;
  std::span<const std::string_view> numeric;
  std::string_view units;
};

constexpr std::array<std::string_view, 2> kBinanceTickerRequired{
    "quoteVolume",
    "priceChangePercent",
};
constexpr std::array<std::string_view, 2> kBinanceFundingRequired{
    "lastFundingRate",
    "nextFundingTime",
};
constexpr std::array<std::string_view, 2> kBybitTickerRequired{
    "turnover24h",
    "price24hPcnt",
};
constexpr std::array<std::string_view, 2> kBybitFundingRequired{
    "fundingRate",
    "nextFundingTime",
};
constexpr std::array<std::string_view, 3> kOkxTickerRequired{
    "volCcy24h",
    "last",
    "open24h",
};
constexpr std::array<std::string_view, 2> kOkxFundingRequired{
    "fundingRate",
    "nextFundingTime",
};
constexpr std::array<std::string_view, 2> kGateTickerRequired{
    "volume_24h_quote",
    "change_percentage",
};
constexpr std::array<std::string_view, 3> kGateFundingRequired{
    "funding_rate",
    "funding_next_apply",
    "funding_interval",
};
constexpr std::array<std::string_view, 2> kKucoinTickerRequired{
    "quoteVolume",
    "priceChangePercent",
};
constexpr std::array<std::string_view, 2> kKucoinFundingRequired{
    "fundingFeeRate",
    "nextFundingRateDateTime",
};
constexpr std::array<std::string_view, 2> kBitgetTickerRequired{
    "turnover24h",
    "price24hPcnt",
};
constexpr std::array<std::string_view, 3> kBitgetFundingRequired{
    "fundingRate",
    "fundingRateInterval",
    "nextUpdate",
};

[[nodiscard]] const boost::json::value* member(
    const boost::json::object& object,
    std::string_view name) {
  const auto found = object.find(name);
  return found == object.end() ? nullptr : &found->value();
}

[[nodiscard]] std::string scalar_text(const boost::json::value* value) {
  if (value == nullptr) {
    return {};
  }
  if (value->is_string()) {
    return std::string{value->as_string()};
  }
  if (value->is_int64()) {
    return std::to_string(value->as_int64());
  }
  if (value->is_uint64()) {
    return std::to_string(value->as_uint64());
  }
  return {};
}

[[nodiscard]] bool nonempty(const boost::json::value* value) {
  if (value == nullptr || value->is_null()) {
    return false;
  }
  if (value->is_string()) {
    return !value->as_string().empty();
  }
  if (value->is_array()) {
    return !value->as_array().empty();
  }
  if (value->is_object()) {
    return !value->as_object().empty();
  }
  return true;
}

[[nodiscard]] bool decimal_text(std::string_view text) {
  if (text.empty()) {
    return false;
  }
  std::size_t cursor = 0;
  if (text[cursor] == '+' || text[cursor] == '-') {
    ++cursor;
  }
  bool digits = false;
  while (cursor < text.size() &&
         text[cursor] >= '0' &&
         text[cursor] <= '9') {
    digits = true;
    ++cursor;
  }
  if (cursor < text.size() && text[cursor] == '.') {
    ++cursor;
    while (cursor < text.size() &&
           text[cursor] >= '0' &&
           text[cursor] <= '9') {
      digits = true;
      ++cursor;
    }
  }
  if (!digits) {
    return false;
  }
  if (cursor < text.size() &&
      (text[cursor] == 'e' || text[cursor] == 'E')) {
    ++cursor;
    if (cursor < text.size() &&
        (text[cursor] == '+' || text[cursor] == '-')) {
      ++cursor;
    }
    const auto exponent_begin = cursor;
    while (cursor < text.size() &&
           text[cursor] >= '0' &&
           text[cursor] <= '9') {
      ++cursor;
    }
    if (cursor == exponent_begin) {
      return false;
    }
  }
  return cursor == text.size();
}

[[nodiscard]] bool numeric_scalar(const boost::json::value* value) {
  if (value == nullptr) {
    return false;
  }
  if (value->is_int64() || value->is_uint64()) {
    return true;
  }
  if (value->is_double()) {
    return std::isfinite(value->as_double());
  }
  return value->is_string() && decimal_text(value->as_string());
}

[[nodiscard]] std::string api_code(Envelope envelope, const boost::json::value& value) {
  if (!value.is_object()) {
    return {};
  }
  const auto& object = value.as_object();
  switch (envelope) {
    case Envelope::Binance:
      return scalar_text(member(object, "code"));
    case Envelope::Bybit:
      return scalar_text(member(object, "retCode"));
    case Envelope::Okx:
    case Envelope::Bitget:
    case Envelope::KucoinUta:
    case Envelope::KucoinFutures:
      return scalar_text(member(object, "code"));
    case Envelope::Gate:
      return scalar_text(member(object, "label"));
    case Envelope::Plain:
      return {};
  }
  return {};
}

[[nodiscard]] bool envelope_success(Envelope envelope, const boost::json::value& value) {
  switch (envelope) {
    case Envelope::Plain:
      return value.is_object() || value.is_array();
    case Envelope::Binance:
      return !(value.is_object() && member(value.as_object(), "code") != nullptr &&
               member(value.as_object(), "symbol") == nullptr);
    case Envelope::Bybit:
      return value.is_object() && scalar_text(member(value.as_object(), "retCode")) == "0";
    case Envelope::Okx:
      return value.is_object() && scalar_text(member(value.as_object(), "code")) == "0";
    case Envelope::Gate:
      return !(value.is_object() &&
               (member(value.as_object(), "label") != nullptr ||
                member(value.as_object(), "message") != nullptr));
    case Envelope::KucoinUta:
    case Envelope::KucoinFutures:
      return value.is_object() &&
             scalar_text(member(value.as_object(), "code")) == "200000";
    case Envelope::Bitget:
      return value.is_object() &&
             scalar_text(member(value.as_object(), "code")) == "00000";
  }
  return false;
}

[[nodiscard]] std::vector<const boost::json::object*> rows_for(
    Envelope envelope,
    const boost::json::value& value) {
  const boost::json::value* raw = &value;
  if (envelope == Envelope::Bybit && value.is_object()) {
    const auto* result = member(value.as_object(), "result");
    raw = result != nullptr && result->is_object()
              ? member(result->as_object(), "list")
              : nullptr;
  } else if ((envelope == Envelope::Okx || envelope == Envelope::Bitget) &&
             value.is_object()) {
    raw = member(value.as_object(), "data");
  } else if (envelope == Envelope::KucoinUta && value.is_object()) {
    const auto* data = member(value.as_object(), "data");
    raw = data != nullptr && data->is_object()
              ? member(data->as_object(), "list")
              : nullptr;
  } else if (envelope == Envelope::KucoinFutures && value.is_object()) {
    raw = member(value.as_object(), "data");
  }

  std::vector<const boost::json::object*> rows;
  if (raw == nullptr) {
    return rows;
  }
  if (raw->is_object()) {
    rows.push_back(&raw->as_object());
    return rows;
  }
  if (!raw->is_array()) {
    return rows;
  }
  for (const auto& item : raw->as_array()) {
    if (item.is_object()) {
      rows.push_back(&item.as_object());
    }
  }
  return rows;
}

[[nodiscard]] RestSpec spec_for(RestContract contract) {
  using C = RestContract;
  switch (contract) {
    case C::BinanceTicker24h:
      return {
          Envelope::Binance,
          "symbol",
          kBinanceTickerRequired,
          kBinanceTickerRequired,
          "quoteVolume=quote;priceChangePercent=percentage_points",
      };
    case C::BinanceFunding:
      return {
          Envelope::Binance,
          "symbol",
          kBinanceFundingRequired,
          kBinanceFundingRequired,
          "lastFundingRate=ratio;nextFundingTime=unix_ms",
      };
    case C::BybitTicker24h:
      return {
          Envelope::Bybit,
          "symbol",
          kBybitTickerRequired,
          kBybitTickerRequired,
          "turnover24h=quote;price24hPcnt=ratio",
      };
    case C::BybitFunding:
      return {
          Envelope::Bybit,
          "symbol",
          kBybitFundingRequired,
          kBybitFundingRequired,
          "fundingRate=ratio;nextFundingTime=unix_ms",
      };
    case C::OkxTicker24h:
      return {
          Envelope::Okx,
          "instId",
          kOkxTickerRequired,
          kOkxTickerRequired,
          "volCcy24h=base;quoteVolume=derived;change=last/open24h",
      };
    case C::OkxFunding:
      return {
          Envelope::Okx,
          "instId",
          kOkxFundingRequired,
          kOkxFundingRequired,
          "fundingRate=ratio;nextFundingTime=unix_ms",
      };
    case C::GateTicker24h:
      return {
          Envelope::Gate,
          "contract",
          kGateTickerRequired,
          kGateTickerRequired,
          "volume_24h_quote=quote;change_percentage=percentage_points",
      };
    case C::GateFunding:
      return {
          Envelope::Gate,
          "name",
          kGateFundingRequired,
          kGateFundingRequired,
          "funding_rate=ratio;funding_next_apply=unix_s;funding_interval=s",
      };
    case C::KucoinTicker24h:
      return {
          Envelope::KucoinUta,
          "symbol",
          kKucoinTickerRequired,
          kKucoinTickerRequired,
          "quoteVolume=quote;priceChangePercent=percentage_points",
      };
    case C::KucoinFunding:
      return {
          Envelope::KucoinFutures,
          "symbol",
          kKucoinFundingRequired,
          kKucoinFundingRequired,
          "fundingFeeRate=ratio;nextFundingRateDateTime=unix_ms",
      };
    case C::BitgetTicker24h:
      return {
          Envelope::Bitget,
          "symbol",
          kBitgetTickerRequired,
          kBitgetTickerRequired,
          "turnover24h=quote;price24hPcnt=ratio",
      };
    case C::BitgetFunding:
      return {
          Envelope::Bitget,
          "symbol",
          kBitgetFundingRequired,
          kBitgetFundingRequired,
          "fundingRate=ratio;fundingRateInterval=hours;nextUpdate=unix_ms",
      };
    case C::BinancePrivate:
      return {Envelope::Binance, "", {}, {}, ""};
    case C::BybitPrivate:
      return {Envelope::Bybit, "", {}, {}, ""};
    case C::OkxPrivate:
      return {Envelope::Okx, "", {}, {}, ""};
    case C::GatePrivate:
      return {Envelope::Gate, "", {}, {}, ""};
    case C::KucoinPrivate:
      return {Envelope::KucoinUta, "", {}, {}, ""};
    case C::BitgetPrivate:
      return {Envelope::Bitget, "", {}, {}, ""};
    case C::None:
      return {};
  }
  return {};
}

[[nodiscard]] bool is_private_contract(RestContract contract) noexcept {
  return contract >= RestContract::BinancePrivate;
}

[[nodiscard]] bool json_string_equals(
    const boost::json::object& object,
    std::string_view key,
    std::string_view expected) {
  const auto* value = member(object, key);
  return value != nullptr && value->is_string() &&
         value->as_string() == expected;
}

[[nodiscard]] bool json_bool_true(
    const boost::json::object& object,
    std::string_view key) {
  const auto* value = member(object, key);
  return value != nullptr && value->is_bool() && value->as_bool();
}

[[nodiscard]] bool value_matches_id(
    const boost::json::value* value,
    std::string_view expected) {
  if (expected.empty()) {
    return value != nullptr;
  }
  return scalar_text(value) == expected;
}

[[nodiscard]] bool contains_text(
    const boost::json::value& value,
    std::string_view expected,
    unsigned depth = 0) {
  if (expected.empty()) {
    return true;
  }
  if (depth > 6) {
    return false;
  }
  if (value.is_string()) {
    return value.as_string() == expected ||
           value.as_string().find(expected) != boost::json::string::npos;
  }
  if (value.is_object()) {
    for (const auto& item : value.as_object()) {
      if (contains_text(item.value(), expected, depth + 1)) {
        return true;
      }
    }
  } else if (value.is_array()) {
    for (const auto& item : value.as_array()) {
      if (contains_text(item, expected, depth + 1)) {
        return true;
      }
    }
  }
  return false;
}

[[nodiscard]] bool contains_nonempty_data(
    const boost::json::value& value,
    unsigned depth = 0) {
  if (depth > 6) {
    return false;
  }
  if (value.is_object()) {
    const auto& object = value.as_object();
    if (const auto* data = member(object, "data"); data != nullptr) {
      return nonempty(data);
    }
    if (const auto* tick = member(object, "tick"); tick != nullptr) {
      return nonempty(tick);
    }
    for (const auto& item : object) {
      if (contains_nonempty_data(item.value(), depth + 1)) {
        return true;
      }
    }
    return !object.empty();
  } else if (value.is_array()) {
    if (!value.as_array().empty()) {
      return true;
    }
  }
  return false;
}

[[nodiscard]] std::uint16_t little_u16(
    std::string_view bytes,
    std::size_t offset) noexcept {
  return static_cast<std::uint16_t>(
      static_cast<unsigned char>(bytes[offset]) |
      static_cast<unsigned>(
          static_cast<unsigned char>(bytes[offset + 1]))
          << 8U);
}

[[nodiscard]] bool read_varint(
    std::string_view payload,
    std::size_t& cursor,
    std::uint64_t& value) {
  value = 0;
  for (unsigned shift = 0; shift < 64 && cursor < payload.size(); shift += 7) {
    const auto byte = static_cast<unsigned char>(payload[cursor++]);
    value |= static_cast<std::uint64_t>(byte & 0x7fU) << shift;
    if ((byte & 0x80U) == 0) {
      return true;
    }
  }
  return false;
}

[[nodiscard]] bool valid_protobuf_wire(std::string_view payload) {
  std::size_t cursor = 0;
  bool saw_field = false;
  while (cursor < payload.size()) {
    std::uint64_t key = 0;
    if (!read_varint(payload, cursor, key) || key == 0) {
      return false;
    }
    saw_field = true;
    switch (key & 0x7U) {
      case 0: {
        std::uint64_t ignored = 0;
        if (!read_varint(payload, cursor, ignored)) {
          return false;
        }
        break;
      }
      case 1:
        if (payload.size() - cursor < 8) {
          return false;
        }
        cursor += 8;
        break;
      case 2: {
        std::uint64_t size = 0;
        if (!read_varint(payload, cursor, size) ||
            size > payload.size() - cursor) {
          return false;
        }
        cursor += static_cast<std::size_t>(size);
        break;
      }
      case 5:
        if (payload.size() - cursor < 4) {
          return false;
        }
        cursor += 4;
        break;
      default:
        return false;
    }
  }
  return saw_field;
}

}  // namespace

ContractEvidence validate_rest_contract(
    RestContract contract,
    const boost::json::value& value,
    std::string_view expected_symbol) {
  ContractEvidence evidence;
  evidence.contract = std::string{to_string(contract)};
  if (contract == RestContract::None) {
    evidence.error = "contract_not_declared";
    return evidence;
  }

  const auto spec = spec_for(contract);
  evidence.units = std::string{spec.units};
  evidence.api_code = api_code(spec.envelope, value);
  evidence.logical_success = envelope_success(spec.envelope, value);
  if (!evidence.logical_success) {
    evidence.error = "exchange_logical_error";
    return evidence;
  }
  if (is_private_contract(contract)) {
    evidence.schema_success = value.is_object() || value.is_array();
    evidence.symbol_success = true;
    if (!evidence.schema_success) {
      evidence.error = "private_response_not_object_or_array";
    }
    return evidence;
  }

  const auto rows = rows_for(spec.envelope, value);
  evidence.row_count = rows.size();
  if (rows.empty()) {
    evidence.error = "empty_or_invalid_rows";
    return evidence;
  }

  const boost::json::object* selected = nullptr;
  for (const auto* row : rows) {
    if (expected_symbol.empty() ||
        scalar_text(member(*row, spec.symbol_field)) == expected_symbol) {
      selected = row;
      break;
    }
  }
  if (selected == nullptr) {
    evidence.error = "expected_symbol_missing";
    return evidence;
  }
  evidence.native_symbol = scalar_text(member(*selected, spec.symbol_field));
  evidence.symbol_success =
      expected_symbol.empty() || evidence.native_symbol == expected_symbol;

  for (const auto field : spec.required) {
    if (!nonempty(member(*selected, field))) {
      evidence.missing_fields.emplace_back(field);
    }
  }
  for (const auto field : spec.numeric) {
    if (!numeric_scalar(member(*selected, field))) {
      const std::string marker = std::string{field} + ":non_numeric";
      if (std::find(
              evidence.missing_fields.begin(),
              evidence.missing_fields.end(),
              marker) == evidence.missing_fields.end()) {
        evidence.missing_fields.push_back(marker);
      }
    }
  }

  if (contract == RestContract::KucoinFunding) {
    const auto current =
        scalar_text(member(*selected, "currentFundingRateGranularity"));
    const auto configured =
        scalar_text(member(*selected, "fundingRateGranularity"));
    if (current.empty() || configured.empty() || current != configured ||
        !decimal_text(current)) {
      evidence.missing_fields.emplace_back(
          "consistent_funding_granularity");
    }
  }

  evidence.schema_success =
      evidence.symbol_success && evidence.missing_fields.empty();
  if (!evidence.schema_success) {
    evidence.error = "required_schema_mismatch";
  }
  return evidence;
}

WsValidation validate_ws_ack(
    const WsCase& probe_case,
    const boost::json::value& value,
    bool json_present,
    bool binary) {
  if (probe_case.ack_kind == WsAckKind::None) {
    return {.matched = !probe_case.require_ack};
  }
  if (!json_present || !value.is_object()) {
    return {.matched = false, .error = "ack_not_json_object"};
  }
  const auto& object = value.as_object();
  switch (probe_case.ack_kind) {
    case WsAckKind::None:
      return {.matched = true};
    case WsAckKind::KeysOnly:
      return {
          .matched = value_matches_id(
              member(object, "id"),
              probe_case.expected_request_id),
          .error = "ack_id_mismatch",
      };
    case WsAckKind::BinanceSubscription: {
      const auto* result = member(object, "result");
      return {
          .matched = !binary && result != nullptr && result->is_null() &&
                     value_matches_id(
                         member(object, "id"),
                         probe_case.expected_request_id),
          .error = "binance_ack_mismatch",
      };
    }
    case WsAckKind::BybitSubscription:
      return {
          .matched = !binary && json_bool_true(object, "success") &&
                     json_string_equals(object, "op", "subscribe"),
          .error = "bybit_ack_mismatch",
      };
    case WsAckKind::OkxSubscription: {
      const auto* argument = member(object, "arg");
      const bool topic_ok =
          argument != nullptr && argument->is_object() &&
          (probe_case.expected_topic.empty() ||
           contains_text(*argument, probe_case.expected_topic));
      const bool symbol_ok =
          argument != nullptr && argument->is_object() &&
          (probe_case.expected_symbol.empty() ||
           contains_text(*argument, probe_case.expected_symbol));
      return {
          .matched = !binary &&
                     json_string_equals(object, "event", "subscribe") &&
                     topic_ok && symbol_ok,
          .error = "okx_ack_mismatch",
      };
    }
    case WsAckKind::GateSubscription: {
      const auto* result = member(object, "result");
      const bool result_ok =
          result != nullptr && result->is_object() &&
          json_string_equals(result->as_object(), "status", "success");
      return {
          .matched = !binary &&
                     json_string_equals(object, "event", "subscribe") &&
                     (probe_case.expected_topic.empty() ||
                      json_string_equals(
                          object,
                          "channel",
                          probe_case.expected_topic)) &&
                     result_ok,
          .error = "gate_ack_mismatch",
      };
    }
    case WsAckKind::BitgetSubscription: {
      const auto* argument = member(object, "arg");
      return {
          .matched = !binary &&
                     json_string_equals(object, "event", "subscribe") &&
                     argument != nullptr && argument->is_object() &&
                     contains_text(*argument, probe_case.expected_topic) &&
                     contains_text(*argument, probe_case.expected_symbol),
          .error = "bitget_ack_mismatch",
      };
    }
    case WsAckKind::KucoinSubscription:
      return {
          .matched = binary &&
                     value_matches_id(
                         member(object, "id"),
                         probe_case.expected_request_id) &&
                     json_bool_true(object, "result"),
          .error = "kucoin_ack_mismatch",
      };
    case WsAckKind::PhemexSubscription: {
      const auto* error = member(object, "error");
      const bool no_error =
          error == nullptr || error->is_null() ||
          (error->is_object() && error->as_object().empty());
      return {
          .matched = !binary &&
                     value_matches_id(
                         member(object, "id"),
                         probe_case.expected_request_id) &&
                     no_error,
          .error = "phemex_ack_mismatch",
      };
    }
    case WsAckKind::HyperliquidSubscription: {
      const auto* data = member(object, "data");
      return {
          .matched = !binary &&
                     json_string_equals(
                         object,
                         "channel",
                         "subscriptionResponse") &&
                     data != nullptr && data->is_object() &&
                     json_string_equals(
                         data->as_object(),
                         "method",
                         "subscribe") &&
                     member(data->as_object(), "subscription") != nullptr,
          .error = "hyperliquid_ack_mismatch",
      };
    }
    case WsAckKind::BingxSubscription:
      return {
          .matched = scalar_text(member(object, "code")) == "0",
          .error = "bingx_ack_mismatch",
      };
  }
  return {.matched = false, .error = "unknown_ack_contract"};
}

WsValidation validate_ws_data(
    const WsCase& probe_case,
    const boost::json::value& value,
    bool json_present,
    bool binary,
    std::string_view payload) {
  switch (probe_case.data_kind) {
    case WsDataKind::AnyJson:
      return {
          .matched = json_present &&
                     ((value.is_object() && !value.as_object().empty()) ||
                      (value.is_array() && !value.as_array().empty())),
          .error = "empty_or_non_json_data",
      };
    case WsDataKind::TopicJson:
      return {
          .matched = json_present && contains_nonempty_data(value) &&
                     contains_text(value, probe_case.expected_topic) &&
                     contains_text(value, probe_case.expected_symbol),
          .error = "topic_or_symbol_data_mismatch",
      };
    case WsDataKind::KucoinBinaryJson:
      return {
          .matched = binary && json_present &&
                     !contains_text(value, probe_case.expected_request_id) &&
                     (contains_nonempty_data(value) ||
                      contains_text(value, probe_case.expected_symbol)),
          .error = "kucoin_binary_data_mismatch",
      };
    case WsDataKind::SbeHeader: {
      if (!binary || payload.size() < 8) {
        return {.matched = false, .error = "sbe_header_missing"};
      }
      const auto block_length = little_u16(payload, 0);
      const auto template_id = little_u16(payload, 2);
      const auto schema_id = little_u16(payload, 4);
      const auto version = little_u16(payload, 6);
      const bool template_ok =
          probe_case.expected_sbe_templates.empty() ||
          std::find(
              probe_case.expected_sbe_templates.begin(),
              probe_case.expected_sbe_templates.end(),
              template_id) != probe_case.expected_sbe_templates.end();
      return {
          .matched = block_length > 0 && template_id > 0 &&
                     schema_id == probe_case.expected_sbe_schema &&
                     version > 0 && template_ok,
          .error = "sbe_header_contract_mismatch",
      };
    }
    case WsDataKind::ProtobufEnvelope:
      return {
          .matched = binary && valid_protobuf_wire(payload) &&
                     (probe_case.expected_symbol.empty() ||
                      payload.find(probe_case.expected_symbol) !=
                          std::string_view::npos),
          .error = "protobuf_wire_contract_mismatch",
      };
    case WsDataKind::BingxTrades:
      if (!json_present || !value.is_object()) {
        return {.matched = false, .error = "bingx_data_not_json"};
      }
      return {
          .matched = json_string_equals(
                         value.as_object(),
                         "dataType",
                         probe_case.expected_topic) &&
                     nonempty(member(value.as_object(), "data")),
          .error = "bingx_trade_data_mismatch",
      };
  }
  return {.matched = false, .error = "unknown_data_contract"};
}

}  // namespace exchange_probe
