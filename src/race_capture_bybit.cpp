#include "exchange_probe/race/capture.hpp"

#include "exchange_probe/net.hpp"
#include "exchange_probe/race/instruments.hpp"
#include "exchange_probe/race/normalizers.hpp"
#include "exchange_probe/race/run.hpp"

#include "race_capture_internal.hpp"

#include <boost/json/object.hpp>
#include <boost/json/serialize.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace exchange_probe::race::capture_detail {
namespace {

struct BybitFeed {
  const char* id;
  const char* topic;
  FeedFamily family;
  EventClass eventClass;
  unsigned depth;
};

constexpr std::array<BybitFeed, 5u> kBybitFeeds{{
    {"orderbook_1", "orderbook.1", FeedFamily::DirectBbo,
     EventClass::Bbo, 1u},
    {"orderbook_50", "orderbook.50", FeedFamily::IncrementalDepth,
     EventClass::Top50, 50u},
    {"orderbook_200", "orderbook.200", FeedFamily::IncrementalDepth,
     EventClass::Depth, 200u},
    {"orderbook_1000", "orderbook.1000", FeedFamily::IncrementalDepth,
     EventClass::Depth, 1000u},
    {"public_trade", "publicTrade", FeedFamily::Trade,
     EventClass::Trade, 0u},
}};

[[nodiscard]] std::string subscribe_payload(
    const BybitFeed& feed, const std::string& symbol) {
  return "{\"op\":\"subscribe\",\"args\":[\"" +
         std::string{feed.topic} + '.' + symbol + "\"]}";
}

[[nodiscard]] std::string percent_encode_query(const std::string& value) {
  constexpr char kHex[] = "0123456789ABCDEF";
  std::string output;
  output.reserve(value.size() * 3u);
  for (const char rawByte : value) {
    const auto byte = static_cast<unsigned char>(rawByte);
    const bool unreserved =
        (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') ||
        (byte >= '0' && byte <= '9') || byte == '-' || byte == '_' ||
        byte == '.' || byte == '~';
    if (unreserved) {
      output.push_back(static_cast<char>(byte));
    } else {
      output.push_back('%');
      output.push_back(kHex[byte >> 4u]);
      output.push_back(kHex[byte & 0x0fu]);
    }
  }
  return output;
}

[[nodiscard]] bool bybit_cursor(
    const boost::json::value& root, std::string& cursor,
    std::string& error) {
  if (!root.is_object()) {
    error = "bybit_instruments_root";
    return false;
  }
  const auto& object = root.as_object();
  const auto* code = object.if_contains("retCode");
  const bool success = code != nullptr &&
                       ((code->is_int64() && code->as_int64() == 0) ||
                        (code->is_uint64() && code->as_uint64() == 0u));
  const auto* result = object.if_contains("result");
  if (!success || result == nullptr || !result->is_object()) {
    error = "bybit_instruments_result";
    return false;
  }
  const auto* next = result->as_object().if_contains("nextPageCursor");
  if (next == nullptr || next->is_null()) {
    cursor.clear();
    return true;
  }
  if (!next->is_string() || next->as_string().size() > 1024u) {
    error = "bybit_instruments_cursor";
    return false;
  }
  cursor.assign(next->as_string().data(), next->as_string().size());
  return true;
}

[[nodiscard]] bool discover_bybit(
    const std::vector<std::string>& requested,
    std::vector<InstrumentResolution>& resolutions, std::string& error) {
  constexpr unsigned kMaximumPages = 8u;
  constexpr std::size_t kMaximumRows = 8'000u;
  std::vector<InstrumentRow> rows;
  std::set<std::string> nativeSymbols;
  std::string cursor;
  for (unsigned page = 0u; page < kMaximumPages; ++page) {
    exchange_probe::RestCase probe{};
    probe.name = "bybit_linear_instruments";
    probe.host = "api.bybit.com";
    probe.path = "/v5/market/instruments-info?category=linear&limit=1000";
    if (!cursor.empty()) {
      probe.path += "&cursor=" + percent_encode_query(cursor);
    }
    const auto result = exchange_probe::execute_http(
        probe, std::chrono::steady_clock::now() + std::chrono::seconds{20});
    if (result.status != 200u || !result.error.empty() ||
        !result.json_present) {
      error = "bybit_instruments:" + result.stage + ':' + result.error;
      return false;
    }
    const auto body = boost::json::serialize(result.json);
    std::vector<InstrumentRow> pageRows;
    if (!parse_instrument_response(
            Venue::Bybit, body.data(), body.size(), pageRows, error)) {
      error = "bybit_instruments_parse:" + error;
      return false;
    }
    for (auto& row : pageRows) {
      if (nativeSymbols.insert(row.nativeSymbol).second) {
        if (rows.size() == kMaximumRows) {
          error = "bybit_instruments_row_bound";
          return false;
        }
        rows.push_back(std::move(row));
      }
    }
    std::string next;
    if (!bybit_cursor(result.json, next, error)) return false;
    if (next.empty()) break;
    if (next == cursor) {
      error = "bybit_instruments_cursor_stalled";
      return false;
    }
    cursor = std::move(next);
    if (page + 1u == kMaximumPages) {
      error = "bybit_instruments_page_bound";
      return false;
    }
  }
  resolutions.clear();
  resolutions.reserve(requested.size());
  for (const auto& base : requested)
    resolutions.push_back(resolve_usdt_perpetual(rows, base));
  return true;
}

}  // namespace

PublicCaptureResult capture_bybit(const PublicCaptureOptions& options) {
  PublicCaptureResult result{};
  for (unsigned session = 1u; session <= options.sessions; ++session) {
    std::vector<InstrumentResolution> resolutions;
    if (!discover_bybit(options.requestedBases, resolutions, result.error))
      return result;
    const auto venueDirectory = options.outputDirectory / "bybit";
    if (!write_availability(
            venueDirectory, "bybit", resolutions, result.error)) {
      return result;
    }
    std::vector<std::string> symbols;
    for (const auto& resolution : resolutions) {
      if (resolution.availability == InstrumentAvailability::Available) {
        symbols.push_back(resolution.nativeSymbol);
        ++result.availableInstruments;
      } else {
        ++result.unavailableInstruments;
      }
    }
    if (symbols.empty()) {
      result.error = "bybit_no_available_requested_instrument";
      return result;
    }
    const std::size_t connectionCount = symbols.size() * kBybitFeeds.size();
    std::vector<VenueNormalizerState> states(connectionCount);
    std::vector<GroupConnection> connections;
    connections.reserve(connectionCount);
    std::vector<std::size_t> order(connectionCount);
    for (std::size_t index = 0u; index < connectionCount; ++index)
      order[index] = index;
    const std::size_t rotation = (session - 1u) % connectionCount;
    std::rotate(
        order.begin(),
        order.begin() + static_cast<std::ptrdiff_t>(rotation), order.end());
    std::uint32_t connectionId = session * 100'000u;
    for (const auto ordinal : order) {
      const auto symbolIndex = ordinal / kBybitFeeds.size();
      const auto feedIndex = ordinal % kBybitFeeds.size();
      const auto& symbol = symbols[symbolIndex];
      const auto& feed = kBybitFeeds[feedIndex];
      auto& state = states[ordinal];
      state.venue = Venue::Bybit;
      state.wire = Wire::Json;
      state.family = feed.family;
      state.depthEventClass = feed.eventClass;
      state.configuredDepth = feed.depth;
      if (!state.nativeSymbol.assign(symbol.data(), symbol.size())) {
        result.error = "bybit_symbol_bound";
        return result;
      }
      SourceIdentity source{};
      source.sourceId = static_cast<std::uint32_t>(ordinal + 1u);
      source.connectionId = ++connectionId;
      source.sessionId = static_cast<std::uint16_t>(session);
      source.raceGroupId = static_cast<std::uint16_t>(symbolIndex + 1u);
      source.venue = Venue::Bybit;
      source.wire = Wire::Json;
      source.transport = TransportKind::RuntimeSubscribe;
      source.origin = RecordOrigin::Raw;
      ConnectionSpec spec{};
      spec.source = source;
      spec.feedId = symbol + '/' + feed.id;
      spec.host = "stream.bybit.com";
      spec.path = "/v5/public/linear";
      spec.subscribe = subscribe_payload(feed, symbol);
      spec.logicalCpu = logical_cpu(ordinal);
      spec.maximumReconnectAttempts = options.validation ? 0u : 3u;
      connections.push_back(GroupConnection{
          .spec = std::move(spec),
          .normalizer = &normalize_bybit,
          .resetNormalizer = &reset_venue_normalizer,
          .normalizerState = &state,
      });
    }
    GroupRunOptions groupOptions{};
    groupOptions.warmup = options.warmup;
    groupOptions.measured = options.measured;
    groupOptions.readyTimeout = std::chrono::seconds{45};
    groupOptions.statusInterval = std::chrono::seconds{2};
    groupOptions.outputDirectory =
        venueDirectory / ("session-" + std::to_string(session));
    const auto sessionResult = run_group_session(groupOptions, connections);
    result.degraded = result.degraded || sessionResult.degraded;
    if (!sessionResult.complete) {
      result.error = "bybit_session_" + std::to_string(session) + ':' +
                     sessionResult.error;
      return result;
    }
    ++result.sessionsCompleted;
  }
  result.complete = true;
  return result;
}

}  // namespace exchange_probe::race::capture_detail
