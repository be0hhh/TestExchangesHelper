#include "exchange_probe/race/Capture.hpp"

#include "exchange_probe/Net.hpp"
#include "exchange_probe/race/Instruments.hpp"
#include "exchange_probe/race/Normalizers.hpp"
#include "exchange_probe/race/Run.hpp"

#include "RaceCaptureInternal.hpp"

#include <boost/json/serialize.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace exchange_probe::race {
namespace capture_detail {
namespace {

struct BitgetFeed {
  const char* id;
  const char* topic;
  Wire wire;
  FeedFamily family;
  EventClass eventClass;
  unsigned depth;
};

constexpr std::array<BitgetFeed, 8u> kBitgetFeeds{{
    {"json_books", "books", Wire::Json, FeedFamily::IncrementalDepth,
     EventClass::Depth, 0u},
    {"json_books1", "books1", Wire::Json, FeedFamily::DirectBbo,
     EventClass::Bbo, 1u},
    {"json_books5", "books5", Wire::Json, FeedFamily::SnapshotDepth,
     EventClass::Top5, 5u},
    {"json_books50", "books50", Wire::Json, FeedFamily::SnapshotDepth,
     EventClass::Top50, 50u},
    {"json_public_trade", "publicTrade", Wire::Json, FeedFamily::Trade,
     EventClass::Trade, 0u},
    {"sbe_books1", "books1", Wire::Sbe, FeedFamily::DirectBbo,
     EventClass::Bbo, 1u},
    {"sbe_books50", "books50", Wire::Sbe, FeedFamily::SnapshotDepth,
     EventClass::Top50, 50u},
    {"sbe_public_trade", "publicTrade", Wire::Sbe, FeedFamily::Trade,
     EventClass::Trade, 0u},
}};

[[nodiscard]] std::string subscribe_payload(
    const char* topic, const std::string& symbol) {
  return "{\"op\":\"subscribe\",\"args\":[{\"instType\":\"usdt-futures\",\"topic\":\"" +
         std::string{topic} + "\",\"symbol\":\"" + symbol + "\"}]}";
}

[[nodiscard]] bool discover_bitget(
    const std::vector<std::string>& requested,
    std::vector<InstrumentResolution>& resolutions, std::string& error) {
  exchange_probe::RestCase probe{};
  probe.name = "bitget_uta_usdt_futures_instruments";
  probe.host = "api.bitget.com";
  probe.path = "/api/v3/market/instruments?category=USDT-FUTURES";
  const auto result = exchange_probe::execute_http(
      probe, std::chrono::steady_clock::now() + std::chrono::seconds{20});
  if (result.status != 200u || !result.error.empty() || !result.json_present) {
    error = "bitget_instruments:" + result.stage + ':' + result.error;
    return false;
  }
  const auto body = boost::json::serialize(result.json);
  std::vector<InstrumentRow> rows;
  if (!parse_instrument_response(
          Venue::Bitget, body.data(), body.size(), rows, error)) {
    error = "bitget_instruments_parse:" + error;
    return false;
  }
  resolutions.clear();
  resolutions.reserve(requested.size());
  for (const auto& base : requested)
    resolutions.push_back(resolve_usdt_perpetual(rows, base));
  return true;
}

}  // namespace

PublicCaptureResult capture_bitget(const PublicCaptureOptions& options) {
  PublicCaptureResult result{};
  for (unsigned session = 1u; session <= options.sessions; ++session) {
    std::vector<InstrumentResolution> resolutions;
    if (!discover_bitget(options.requestedBases, resolutions, result.error))
      return result;
    const auto venueDirectory = options.outputDirectory / "bitget";
    if (!write_availability(
            venueDirectory, "bitget", resolutions, result.error))
      return result;
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
      result.error = "bitget_no_available_requested_instrument";
      return result;
    }
    const std::size_t connectionCount = symbols.size() * kBitgetFeeds.size();
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
      const auto symbolIndex = ordinal / kBitgetFeeds.size();
      const auto feedIndex = ordinal % kBitgetFeeds.size();
      const auto& symbol = symbols[symbolIndex];
      const auto& feed = kBitgetFeeds[feedIndex];
      auto& state = states[ordinal];
      state.venue = Venue::Bitget;
      state.wire = feed.wire;
      state.family = feed.family;
      state.depthEventClass = feed.eventClass;
      state.configuredDepth = feed.depth;
      if (!state.nativeSymbol.assign(symbol.data(), symbol.size())) {
        result.error = "bitget_symbol_bound";
        return result;
      }
      SourceIdentity source{};
      source.sourceId = static_cast<std::uint32_t>(ordinal + 1u);
      source.connectionId = ++connectionId;
      source.sessionId = static_cast<std::uint16_t>(session);
      source.raceGroupId = static_cast<std::uint16_t>(symbolIndex + 1u);
      source.venue = Venue::Bitget;
      source.wire = feed.wire;
      source.transport = TransportKind::RuntimeSubscribe;
      source.origin = RecordOrigin::Raw;
      ConnectionSpec spec{};
      spec.source = source;
      spec.feedId = symbol + '/' + feed.id;
      spec.host = "ws.bitget.com";
      spec.path = feed.wire == Wire::Sbe ? "/v3/ws/public/sbe"
                                         : "/v3/ws/public";
      spec.subscribe = subscribe_payload(feed.topic, symbol);
      spec.logicalCpu = logical_cpu(ordinal);
      spec.maximumReconnectAttempts = options.validation ? 0u : 3u;
      connections.push_back(GroupConnection{
          .spec = std::move(spec),
          .normalizer = &normalize_bitget,
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
      result.error = "bitget_session_" + std::to_string(session) + ':' +
                     sessionResult.error;
      return result;
    }
    ++result.sessionsCompleted;
  }
  result.complete = true;
  return result;
}

}  // namespace capture_detail
}  // namespace exchange_probe::race
