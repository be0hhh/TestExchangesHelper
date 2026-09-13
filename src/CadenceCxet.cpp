#include "Cadence.hpp"

#include "cxet/Api/Dispatch/BuildDispatch.hpp"
#include "cxet/Api/Market/PublicMarketDataSubscriptionManager.hpp"
#include "cxet/Exchanges/Binance/Fapi/Market/BookTicker/TransformBookTicker.hpp"
#include "cxet/Exchanges/Binance/Fapi/Market/OrderBook/TransformOrderBook.hpp"
#include "cxet/Exchanges/Binance/Fapi/Market/Trades/TransformTrades.hpp"

#include <array>
#include <exception>
#include <memory>
#include <thread>

namespace exchange_probe::cadence {
namespace {
namespace market = cxet::api::market;
namespace parser = cxet::exchanges::transform_layer::binance::fapi;
using Manager = market::PublicMarketDataSubscriptionManager;
using Stream = market::PublicMarketDataStream;

struct CaptureLane {
  std::unique_ptr<Manager> manager;
  Stream stream{Stream::Trades};
  bool prepared{}, connected{}, disabled{}, baselineTaken{};
  std::uint64_t parseBaseline{};
};

void prepared(CaptureLane& state, Window& window) {
  if (state.prepared) return;
  state.prepared = true;
  window.prepared.fetch_add(1, std::memory_order_release);
}

// Inspect the actual prepared route before opening it. Nominal interval and
// depth in DesiredChannel are not proof of the subscription sent by a builder.
bool provenance(Manager& manager, Stream stream, Lane& lane) {
  const auto* route = manager.routeAt(0);
  const auto* config = route ? route->streamConfigs.get(stream) : nullptr;
  if (!route || !config || manager.routeCount() != 1 ||
      manager.channelCount() != 1 || !route->builder ||
      config->transport != cxet::api::RouteTransport::Ws ||
      config->wireFormat != cxet::api::WireFormat::JsonText ||
      !config->buildPayloadFn) {
    lane.error = "selected_route_not_single_json_websocket";
    return false;
  }
  auto payload = std::make_unique<MessageBuffer>();
  if (!config->buildPayloadFn(*route->builder, *payload)) {
    lane.error = "selected_subscription_builder_failed";
    return false;
  }
  lane.subscription.assign(payload->data(), payload->size());
  // These canonical builders emit exactly one topic and numeric request ID 1.
  const std::string expected = "{\"method\":\"SUBSCRIBE\",\"params\":[\"" +
      lane.feed.topic + "\"],\"id\":1}";
  if (lane.subscription != expected) {
    lane.error = "actual_subscription_differs_from_requested_topic";
    return false;
  }
  if (stream == Stream::Trades && config->parseTradesRuntimeFn ==
          &parser::trades::parseWsTradesRuntime &&
      config->parseTradesRuntimeNumericViewFn ==
          &parser::trades::parseWsTradesRuntimeNumericView) {
    lane.parser = "Binance/Fapi/Market/Trades/TransformTrades.hpp:";
    lane.parser += cxet::api::identityRuntimeInstrumentNumeric(route->symbolNumerics[0])
        ? "parseWsTradesRuntime" : "parseWsTradesRuntimeNumericView";
    lane.parser += "; TradeRuntimeV1; E,t exposed; T absent";
  } else if (stream == Stream::BookTicker &&
      config->parseBookTickerSideTapeRuntimeFn ==
          &parser::bookticker::parseBookTickerSideTapeRuntime &&
      config->parseBookTickerSideTapeRuntimeEventNumericViewFn ==
          &parser::bookticker::parseBookTickerSideTapeRuntimeNumericEventView) {
    lane.parser = "Binance/Fapi/Market/BookTicker/TransformBookTicker.hpp:";
    lane.parser += cxet::api::identityRuntimeInstrumentNumeric(route->symbolNumerics[0])
        ? "parseBookTickerSideTapeRuntime" : "parseBookTickerSideTapeRuntimeNumericEventView";
    lane.parser += "; BookTickerSideTapeRuntimeV1; "
        "one observation after final side; E,u exposed; T absent";
  } else if (stream == Stream::Orderbook &&
      config->parseOrderBookTapeRuntimeFn ==
          &parser::orderbook::parseWsOrderBookRuntime &&
      config->parseOrderBookTapeRuntimeViewFn ==
          &parser::orderbook::parseWsOrderBookRuntimeView) {
    lane.parser = "Binance/Fapi/Market/OrderBook/TransformOrderBook.hpp:"
        "parseWsOrderBookRuntime -> parse_layer::parseWsOrderBookRuntimeView; OrderBookTapeRuntimeV1; "
        "E,U,u,pu exposed; T absent; REST snapshot publications excluded";
  } else {
    lane.error = "selected_runtime_parser_provenance_mismatch";
    return false;
  }
  lane.parser += "; frames/control-pings unavailable through public diagnostics; "
      "receive=CLOCK_MONOTONIC_RAW; publish=diagnostic observation time";
  market::PublicMarketDataRouteDiagnostic diagnostic{};
  if (manager.routeDiagnostics(&diagnostic, 1) != 1 ||
      !diagnostic.endpointHost || diagnostic.connectPath[0] == '\0') {
    lane.error = "endpoint_provenance_unavailable";
    return false;
  }
  lane.endpoint = "wss://" + std::string(diagnostic.endpointHost) + ":" +
      std::to_string(diagnostic.endpointPort) + diagnostic.connectPath;
  return true;
}

bool observation(const market::PublicMarketDataDirectPollEvent& event,
                 Record& record) {
  record.receive_ns = event.latencyEvent.localRecvMonoNs.raw;
  record.publish_ns = nowNs();
  if (event.direct.timestampOrigin ==
      market::PublicMarketDataDirectEvent::TimestampOrigin::Exchange)
    record.event_ns = event.direct.exchangeEventTs.raw;
  if (event.direct.trade) {
    record.id = event.direct.trade->eventId.raw;
    record.first_id = record.last_id = record.id;
  } else if (event.direct.bookTickerSide) {
    // This registered Binance parser emits bid and ask for the same wire event.
    // Do not turn those two publications into two receive interval samples.
    if (event.direct.coalesceNextBookTickerSide) return false;
    record.id = event.direct.bookTickerSide->eventId.raw;
  } else if (event.direct.bookTicker) {
    record.id = event.direct.bookTicker->eventId.raw;
  } else if (event.direct.orderbook) {
    if (event.direct.orderbook->isSnapshot) return false;
    record.id = event.direct.orderbook->eventId.raw;
    record.first_id = event.direct.orderbook->firstEventId.raw;
    record.last_id = record.id;
    record.previous_id = event.direct.orderbook->previousEventId.raw;
  } else {
    return false;
  }
  // The normalized payloads above do not expose Binance transaction field T.
  return true;
}
}  // namespace

void captureCxet(const std::string& symbol, Window& window,
                 std::vector<Lane>& lanes) {
  std::array<CaptureLane, laneCount> states{};
  const auto startupDeadline = nowNs() + 30 * second;
  try {
    cxet::initBuildDispatch();
    for (std::size_t i = 0; i < lanes.size() && i < laneCount; ++i) {
      auto& lane = lanes[i];
      auto& state = states[i];
      // Current canonical subscription builders have no aggTrade route and
      // always encode depth@0ms. Never call applyDesired for an ignored variant.
      if (lane.feed.name == "trade") state.stream = Stream::Trades;
      else if (lane.feed.name == "bookTicker") state.stream = Stream::BookTicker;
      else if (lane.feed.depth == 0 && lane.feed.interval == 0 &&
               lane.feed.topic.ends_with("@depth@0ms"))
        state.stream = Stream::Orderbook;
      else {
        lane.status = "unsupported";
        lane.error = lane.feed.name == "aggTrade"
            ? "CXET_public_manager_has_no_aggTrade_subscription_route"
            : "CXET_Binance_Fapi_orderbook_builder_only_emits_depth@0ms";
        state.disabled = true;
        prepared(state, window);
        continue;
      }
      lane.records.reserve(laneRecordLimit);
      state.manager = std::make_unique<Manager>();
      market::PublicMarketDataDesiredChannel desired{};
      desired.exchange = canon::kExchangeIdBinance;
      desired.market = canon::kMarketTypeFutures;
      if (!desired.symbol.copyFrom(symbol.data(), symbol.size()) || symbol.empty()) {
        lane.status = "failed";
        lane.error = "CXET_symbol_empty_or_exceeds_native_capacity";
        state.disabled = true;
        prepared(state, window);
        continue;
      }
      desired.stream = state.stream;
      desired.wirePreference = market::PublicMarketDataWirePreference::Json;
      desired.numeric.quantityKind = cxet::api::RuntimeQuantityKind::DerivativeContracts;
      desired.captureLatency = true;
      desired.pollTimeoutMs = 1;
      desired.wsLanes = 1;
      desired.maxReconnectAttempts = 0;
      desired.ingressDrainPolicy = market::MarketIngressDrainPolicy::PreserveAll;
      market::PublicMarketDataApplyResult result{};
      char error[256]{};
      if (!state.manager->applyDesired(Span<const market::PublicMarketDataDesiredChannel>(
              &desired, 1), &result, error, sizeof(error)) ||
          !provenance(*state.manager, state.stream, lane)) {
        lane.status = "failed";
        if (error[0]) lane.error = error;
        state.disabled = true;
        prepared(state, window);
        continue;
      }
      lane.status = "connecting";
      (void)state.manager->maintainConnectionsOnce();
    }

    std::uint64_t nextMaintenance{};
    while (!window.stop.load(std::memory_order_acquire)) {
      const auto now = nowNs();
      const auto start = window.start_ns.load(std::memory_order_acquire);
      if (start && now >= start + window.duration_seconds * second) break;
      const bool maintain = now >= nextMaintenance;
      if (maintain) nextMaintenance = now + 10'000'000ULL;
      bool drained = false;
      for (std::size_t i = 0; i < lanes.size() && i < laneCount; ++i) {
        auto& state = states[i];
        auto& lane = lanes[i];
        if (state.disabled || !state.manager) continue;
        auto& manager = *state.manager;
        market::PublicMarketDataRouteDiagnostic diagnostic{};
        if (state.connected)
          (void)manager.routeConnectionDiagnostics(&diagnostic, 1);
        else
          (void)manager.routeDiagnostics(&diagnostic, 1);
        if (!state.connected) {
          if (diagnostic.connectDone) {
            // Join only a completed connector and attach its runtime once.
            (void)manager.maintainConnectionsOnce();
            (void)manager.routeConnectionDiagnostics(&diagnostic, 1);
            if (!diagnostic.connected) {
              lane.status = "failed";
              lane.error = "CXET_connect_failed_status_" +
                  std::to_string(static_cast<unsigned>(diagnostic.lastStatus));
              state.disabled = true;
              prepared(state, window);
              continue;
            }
            state.connected = true;
            lane.status = "connected";
            prepared(state, window);
          } else if (now >= startupDeadline) {
            lane.status = "failed";
            lane.error = "CXET_startup_deadline_30s; connector_join_may_outlast_deadline";
            state.disabled = true;
            prepared(state, window);
            continue;
          } else {
            continue;
          }
        }
        if (start && now >= start && !state.baselineTaken) {
          state.baselineTaken = true;
          state.parseBaseline = diagnostic.parseFailures;
          lane.warmup_parse_errors = diagnostic.parseFailures;
        }
        market::PublicMarketDataDirectPollEvent event{};
        // Bounded fairness between independent route managers.
        for (unsigned count = 0; count < 256 && manager.pollAvailableDirectHot(event); ++count) {
          drained = true;
          if (!event.direct.ready) {
            lane.status = "degraded";
            lane.error = "CXET_terminal_publication_status_" +
                std::to_string(static_cast<unsigned>(event.status));
            ++lane.disconnects;
            state.disabled = true;
            break;
          }
          Record record{};
          if (observation(event, record)) (void)append(lane, record, window);
        }
        if (!state.disabled && maintain) {
          // A stopped lane never enters maintenance again: no reconnect or
          // automatic replacement of this measurement's connection epoch.
          (void)manager.routeConnectionDiagnostics(&diagnostic, 1);
          if (!diagnostic.connected) {
            state.disabled = true;
            lane.status = "degraded";
            lane.error = "CXET_connection_lost_no_reconnect";
            ++lane.disconnects;
          } else {
            (void)manager.maintainConnectionsOnce();
          }
        }
        (void)manager.routeConnectionDiagnostics(&diagnostic, 1);
        if (state.baselineTaken)
          lane.parse_errors = diagnostic.parseFailures - state.parseBaseline;
        else
          lane.warmup_parse_errors = diagnostic.parseFailures;
      }
      if (!drained) std::this_thread::yield();
    }
  } catch (const std::exception& error) {
    for (std::size_t i = 0; i < lanes.size() && i < laneCount; ++i) {
      if (!states[i].disabled) {
        lanes[i].status = "failed";
        lanes[i].error = std::string("CXET_capture_exception: ") + error.what();
      }
    }
  }
  for (std::size_t i = 0; i < lanes.size() && i < laneCount; ++i) {
    prepared(states[i], window);
    if (states[i].manager) states[i].manager->closeAll();
    if (lanes[i].status == "connected") {
      if (lanes[i].parse_errors != 0) {
        lanes[i].status = "degraded";
        lanes[i].error = "CXET_parse_failures_during_measurement";
      } else {
        lanes[i].status = lanes[i].records.empty() ? "empty" : "complete";
      }
    }
  }
}
}  // namespace exchange_probe::cadence
