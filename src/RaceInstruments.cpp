#include "exchange_probe/race/Instruments.hpp"

#include <boost/json/array.hpp>
#include <boost/json/object.hpp>
#include <boost/json/parse.hpp>
#include <boost/system/error_code.hpp>

#include <algorithm>
#include <cctype>
#include <cstddef>

namespace exchange_probe::race {
namespace {

[[nodiscard]] std::string uppercase(std::string value) {
  std::transform(
      value.begin(), value.end(), value.begin(),
      [](unsigned char ch) { return static_cast<char>(std::toupper(ch)); });
  return value;
}

[[nodiscard]] bool perpetual_kind(const std::string& value) {
  const auto kind = uppercase(value);
  return kind == "PERPETUAL" || kind == "SWAP" || kind == "FFWCSX" ||
         kind == "FUTURES" || kind.ends_with("PERPETUAL");
}

[[nodiscard]] bool online_status(const std::string& value) {
  const auto status = uppercase(value);
  return status == "ONLINE" || status == "TRADING" || status == "OPEN" ||
         status == "NORMAL" || status == "LIVE" || status == "ENABLE" ||
         status == "ENABLED";
}

[[nodiscard]] std::string text(
    const boost::json::object& object, const char* key) {
  const auto* value = object.if_contains(key);
  return value != nullptr && value->is_string()
             ? std::string{value->as_string()}
             : std::string{};
}

[[nodiscard]] bool boolean(
    const boost::json::object& object, const char* key,
    bool fallback = false) noexcept {
  const auto* value = object.if_contains(key);
  return value != nullptr && value->is_bool() ? value->as_bool() : fallback;
}

[[nodiscard]] const boost::json::array* venue_rows(
    Venue venue, const boost::json::value& root) noexcept {
  if (venue == Venue::Gate && root.is_array()) return &root.as_array();
  if (!root.is_object()) return nullptr;
  const auto& object = root.as_object();
  const boost::json::value* value = object.if_contains("data");
  if (venue == Venue::Bybit) {
    value = object.if_contains("result");
  } else if (venue == Venue::BinanceUsdM || venue == Venue::Aster) {
    value = object.if_contains("symbols");
  }
  if (value == nullptr) return nullptr;
  if (value->is_array()) return &value->as_array();
  if (!value->is_object()) return nullptr;
  const auto* list = value->as_object().if_contains("list");
  return list != nullptr && list->is_array() ? &list->as_array() : nullptr;
}

[[nodiscard]] std::string prefix_before(
    const std::string& value, const std::string& marker) {
  const auto position = value.find(marker);
  return position == std::string::npos ? std::string{} : value.substr(0u, position);
}

[[nodiscard]] InstrumentRow parse_row(
    Venue venue, const boost::json::object& object) {
  InstrumentRow row{};
  switch (venue) {
    case Venue::Bitget:
      row.nativeSymbol = text(object, "symbol");
      row.base = text(object, "baseCoin");
      row.quote = text(object, "quoteCoin");
      row.contractKind = text(object, "type");
      row.status = text(object, "status");
      break;
    case Venue::Bybit:
      row.nativeSymbol = text(object, "symbol");
      row.base = text(object, "baseCoin");
      row.quote = text(object, "quoteCoin");
      row.contractKind = text(object, "contractType");
      row.status = text(object, "status");
      break;
    case Venue::Gate:
      row.nativeSymbol = text(object, "name");
      row.base = prefix_before(row.nativeSymbol, "_USDT");
      row.quote = row.base.empty() ? std::string{} : "USDT";
      row.contractKind = "PERPETUAL";
      row.status = boolean(object, "in_delisting") ? "DELISTING" : "TRADING";
      break;
    case Venue::Okx: {
      row.nativeSymbol = text(object, "instId");
      const auto family = text(object, "instFamily");
      row.base = prefix_before(family, "-USDT");
      row.quote = row.base.empty() ? std::string{} : "USDT";
      row.contractKind = text(object, "instType");
      row.status = text(object, "state");
      break;
    }
    case Venue::Kucoin:
      row.nativeSymbol = text(object, "symbol");
      row.base = text(object, "baseCurrency");
      if (row.base.empty()) row.base = text(object, "displayBaseCurrency");
      row.quote = text(object, "quoteCurrency");
      if (row.quote.empty() && row.nativeSymbol.ends_with("USDTM")) {
        row.quote = "USDT";
      }
      row.contractKind = "FUTURES";
      row.status = text(object, "status");
      if (row.status.empty()) row.status = text(object, "marketStage");
      if (row.status.empty() && boolean(object, "enableTrading")) {
        row.status = "TRADING";
      }
      break;
    case Venue::BinanceUsdM:
    case Venue::Aster:
      row.nativeSymbol = text(object, "symbol");
      row.base = text(object, "baseAsset");
      row.quote = text(object, "quoteAsset");
      row.contractKind = text(object, "contractType");
      row.status = text(object, "status");
      break;
    default:
      break;
  }
  return row;
}

}  // namespace

InstrumentResolution resolve_usdt_perpetual(
    const std::vector<InstrumentRow>& rows,
    const std::string& requestedBase) {
  InstrumentResolution output{};
  output.requestedBase = uppercase(requestedBase);
  std::vector<const InstrumentRow*> matching;
  for (const auto& row : rows) {
    if (uppercase(row.base) == output.requestedBase &&
        uppercase(row.quote) == "USDT" && perpetual_kind(row.contractKind)) {
      matching.push_back(&row);
    }
  }
  if (matching.empty()) {
    output.reason = "no_usdt_perpetual_for_base";
    return output;
  }
  std::vector<const InstrumentRow*> online;
  for (const auto* row : matching) {
    if (online_status(row->status)) online.push_back(row);
  }
  if (online.size() == 1u) {
    output.availability = InstrumentAvailability::Available;
    output.nativeSymbol = online.front()->nativeSymbol;
    output.reason = "online";
    return output;
  }
  if (online.size() > 1u) {
    output.availability = InstrumentAvailability::Ambiguous;
    output.reason = "multiple_online_usdt_perpetuals";
    return output;
  }
  output.reason = "matched_but_not_online:" + matching.front()->status;
  return output;
}

bool parse_instrument_response(
    Venue venue, const char* data, std::size_t size,
    std::vector<InstrumentRow>& rows, std::string& error) {
  rows.clear();
  if (data == nullptr || size == 0u || size > 16u * 1024u * 1024u) {
    error = "instrument_response_bounds";
    return false;
  }
  boost::system::error_code parseError;
  auto root = boost::json::parse(
      boost::json::string_view{data, size}, parseError);
  if (parseError) {
    error = "instrument_response_json:" + parseError.message();
    return false;
  }
  const auto* array = venue_rows(venue, root);
  if (array == nullptr || array->size() > 100'000u) {
    error = "instrument_response_shape";
    return false;
  }
  rows.reserve(array->size());
  for (const auto& value : *array) {
    if (!value.is_object()) {
      error = "instrument_row_not_object";
      rows.clear();
      return false;
    }
    auto row = parse_row(venue, value.as_object());
    if (row.nativeSymbol.empty()) continue;
    rows.push_back(std::move(row));
  }
  if (rows.empty()) {
    error = "instrument_response_empty";
    return false;
  }
  return true;
}

}  // namespace exchange_probe::race
