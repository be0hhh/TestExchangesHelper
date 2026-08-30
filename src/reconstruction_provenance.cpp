#include "exchange_probe/reconstruction/provenance.hpp"

#include "api/builder/UnifiedBuilder.hpp"
#include "api/connector/ExchangeProductConnector.hpp"
#include "api/market/StagedPublicMarketCatalogV2.hpp"
#include "api/object/SubscribeObject.hpp"

#include <array>
#include <cstring>

namespace exchange_probe::reconstruction {
namespace {

using Descriptor =
    cxet::api::market_v2::PublicMarketLaneDescriptorV2;
using Object = cxet::api::market_v2::PublicMarketObjectV2;
using Registry =
    cxet::api::market_v2::StagedPublicMarketRegistryV2;

[[nodiscard]] cxet::api::market::PublicMarketDataWirePreference wire(
    cxet::api::market_v2::PublicMarketWireFormatV2 value) noexcept {
  using Wire = cxet::api::market_v2::PublicMarketWireFormatV2;
  if (value == Wire::JsonText)
    return cxet::api::market::PublicMarketDataWirePreference::Json;
  if (value == Wire::SbeBinary || value == Wire::FixSbeBinary)
    return cxet::api::market::PublicMarketDataWirePreference::Sbe;
  if (value == Wire::ProtobufBinary)
    return cxet::api::market::PublicMarketDataWirePreference::Protobuf;
  return cxet::api::market::PublicMarketDataWirePreference::Auto;
}

[[nodiscard]] cxet::api::RouteTransport transport(
    cxet::api::market_v2::PublicMarketTransportV2 value) noexcept {
  using Transport = cxet::api::market_v2::PublicMarketTransportV2;
  if (value == Transport::WebSocket) return cxet::api::RouteTransport::Ws;
  if (value == Transport::FixSbe) return cxet::api::RouteTransport::Fix;
  if (value == Transport::Grpc) return cxet::api::RouteTransport::Grpc;
  return cxet::api::RouteTransport::None;
}

[[nodiscard]] cxet::api::market::PublicMarketDataWirePreference wire(
    cxet::api::WireFormat value) noexcept {
  using Wire = cxet::api::WireFormat;
  if (value == Wire::JsonText)
    return cxet::api::market::PublicMarketDataWirePreference::Json;
  if (value == Wire::SbeBinary || value == Wire::FixSbeBinary)
    return cxet::api::market::PublicMarketDataWirePreference::Sbe;
  if (value == Wire::ProtobufBinary)
    return cxet::api::market::PublicMarketDataWirePreference::Protobuf;
  return cxet::api::market::PublicMarketDataWirePreference::Auto;
}

[[nodiscard]] bool sameTradeParser(
    const cxet::api::TradeRuntimeParserContractV2& left,
    const cxet::api::TradeRuntimeParserContractV2& right) noexcept {
  return left.abiVersion == right.abiVersion &&
      left.descriptorSize == right.descriptorSize &&
      left.mode == right.mode &&
      left.callbackMask == right.callbackMask &&
      left.reserved[0] == right.reserved[0] &&
      left.reserved[1] == right.reserved[1] &&
      left.parseTradesRuntimeViewFnV2 == right.parseTradesRuntimeViewFnV2 &&
      left.parseTradesRuntimeNumericViewFnV2 ==
          right.parseTradesRuntimeNumericViewFnV2 &&
      left.parseTradeRuntimeEventNumericViewFnV2 ==
          right.parseTradeRuntimeEventNumericViewFnV2 &&
      left.parseTradesRuntimeViewFn == right.parseTradesRuntimeViewFn &&
      left.parseTradesRuntimeNumericViewFn ==
          right.parseTradesRuntimeNumericViewFn &&
      left.parseTradeRuntimeEventNumericViewFn ==
          right.parseTradeRuntimeEventNumericViewFn;
}

[[nodiscard]] bool descriptorMatchesProduct(
    const Descriptor& descriptor,
    const ProductEligibility& product,
    Object object) noexcept {
  if (descriptor.key.exchangeRaw != product.exchange.raw ||
      descriptor.key.marketRaw != product.market.raw ||
      descriptor.key.apiProtocolProfileRaw !=
          product.apiProtocolProfile.raw ||
      descriptor.key.object != object) {
    return false;
  }
  if (object == Object::BookTicker) {
    return descriptor.key.laneId == product.bookTickerLaneId &&
        wire(descriptor.key.wire) == product.bookTickerWire &&
        transport(descriptor.key.transport) ==
            product.bookTickerTransport &&
        (descriptor.parseBookTickerRuntime ||
         descriptor.parseBookTickerEventRuntime ||
         descriptor.parseBookTickerEventNumericRuntime);
  }
  if (object == Object::Trade) {
    return descriptor.key.laneId == product.tradeLaneId &&
        wire(descriptor.key.wire) == product.tradeWire &&
        transport(descriptor.key.transport) == product.tradeTransport &&
        cxet::api::validTradeRuntimeParserContractV2(
            descriptor.tradeRuntimeParserV2);
  }
  return object == Object::Depth &&
      descriptor.key.laneId == product.depthLaneId &&
      wire(descriptor.key.wire) == product.depthWire &&
      transport(descriptor.key.transport) == product.depthTransport &&
      descriptor.parseDepthRuntime;
}

[[nodiscard]] bool canonicalSubscribeObjectRaw(
    Object object, std::uint8_t& output) noexcept {
  using SubscribeObject =
      cxet::composite::out::SubscribeObject;
  if (object == Object::BookTicker) {
    output = static_cast<std::uint8_t>(SubscribeObject::BookTicker);
    return true;
  }
  if (object == Object::Trade) {
    output = static_cast<std::uint8_t>(SubscribeObject::Trades);
    return true;
  }
  if (object == Object::Depth) {
    output = static_cast<std::uint8_t>(SubscribeObject::Orderbook);
    return true;
  }
  return false;
}

[[nodiscard]] bool exactRuntimeRouteShape(
    const cxet::api::ObjectConfig& config,
    const Descriptor& descriptor) noexcept {
  return config.exchangeRaw == descriptor.key.exchangeRaw &&
      config.marketRaw == descriptor.key.marketRaw &&
      config.apiProtocolProfileRaw ==
          descriptor.key.apiProtocolProfileRaw &&
      config.opRaw == static_cast<std::uint8_t>(
          cxet::UnifiedRequestBuilder::Operation::Subscribe) &&
      transport(descriptor.key.transport) == config.transport &&
      wire(descriptor.key.wire) == wire(config.wireFormat);
}

[[nodiscard]] bool actualRuntimeParserPresent(
    const cxet::api::ObjectConfig& config,
    Object object) noexcept {
  if (object == Object::BookTicker) {
    return config.parseBookTickerSideTapeRuntimeFn ||
        config.parseBookTickerSideTapeRuntimeEventNumericViewFn;
  }
  if (object == Object::Trade) {
    return config.parseTradesRuntimeFn ||
        config.parseTradeRuntimeEventFn ||
        config.parseTradeRuntimeEventNumericViewFn;
  }
  return object == Object::Depth &&
      config.parseOrderBookTapeRuntimeFn;
}

[[nodiscard]] const cxet::api::ObjectConfig* canonicalRuntimeConfig(
    const Descriptor& descriptor) noexcept {
  const auto* connector = cxet::api::findExchangeProductConnector(
      ExchangeId{descriptor.key.exchangeRaw},
      canon::MarketType{descriptor.key.marketRaw},
      ApiProtocolProfile{descriptor.key.apiProtocolProfileRaw});
  if (!connector || !connector->routes) return nullptr;
  std::uint8_t objectRaw = 0u;
  if (!canonicalSubscribeObjectRaw(descriptor.key.object, objectRaw)) {
    return nullptr;
  }
  const cxet::api::ObjectConfig* match = nullptr;
  for (std::size_t index = 0u; index < connector->routeCount; ++index) {
    const cxet::api::ObjectConfig* candidate = connector->routes[index];
    if (!candidate || candidate->objectRaw != objectRaw ||
        !exactRuntimeRouteShape(*candidate, descriptor) ||
        !actualRuntimeParserPresent(*candidate, descriptor.key.object)) {
      continue;
    }
    if (match) return nullptr;
    match = candidate;
  }
  return match;
}

[[nodiscard]] bool runtimeParserMatches(
    const cxet::api::ObjectConfig& config,
    const Descriptor& descriptor,
    const cxet::api::ObjectConfig* canonicalConfig,
    cxet::api::market::PublicMarketDataStream stream) noexcept {
  if (&config != canonicalConfig || !canonicalConfig) return false;
  using Stream = cxet::api::market::PublicMarketDataStream;
  if (stream == Stream::BookTicker) {
    return actualRuntimeParserPresent(config, Object::BookTicker) &&
        config.parseBookTickerSideTapeRuntimeViewFn ==
               descriptor.parseBookTickerRuntime &&
        config.parseBookTickerSideTapeRuntimeEventViewFn ==
            descriptor.parseBookTickerEventRuntime &&
        config.parseBookTickerSideTapeRuntimeEventNumericViewFn ==
            descriptor.parseBookTickerEventNumericRuntime;
  }
  if (stream == Stream::Trades) {
    return actualRuntimeParserPresent(config, Object::Trade) &&
        sameTradeParser(config.tradeRuntimeParserV2,
                           descriptor.tradeRuntimeParserV2);
  }
  return stream == Stream::Orderbook &&
      actualRuntimeParserPresent(config, Object::Depth) &&
      config.parseOrderBookTapeRuntimeViewFn == descriptor.parseDepthRuntime;
}

[[nodiscard]] bool routeMatches(
    const cxet::api::market::PublicMarketDataSubscriptionManager& manager,
    std::size_t channelIndex,
    cxet::api::market::PublicMarketDataStream stream,
    cxet::api::market::PublicMarketDataWirePreference expectedWire,
    cxet::api::RouteTransport expectedTransport,
    const Symbol& symbol,
    const Descriptor& descriptor,
    const cxet::api::ObjectConfig* canonicalConfig) noexcept {
  const auto* channel = manager.channelAt(channelIndex);
  if (!channel || channel->stream != stream ||
      channel->wirePreference != expectedWire) {
    return false;
  }
  const auto* route = manager.routeBySlot(channel->routeSlot);
  const cxet::api::ObjectConfig* config =
      route ? route->streamConfigs.get(stream) : nullptr;
  if (!route || !config || route->wirePreference != expectedWire ||
      (route->streamMask &
       cxet::api::market::publicMarketDataStreamBit(stream)) == 0u ||
      !runtimeParserMatches(
          *config, descriptor, canonicalConfig, stream)) {
    return false;
  }
  std::array<cxet::api::market::PublicMarketDataRouteDiagnostic,
             cxet::api::market::kMaxManagedMarketDataRoutes>
      diagnostics{};
  const std::size_t count = manager.routeDiagnostics(
      diagnostics.data(), diagnostics.size());
  if (count > diagnostics.size()) return false;
  for (std::size_t index = 0u; index < count; ++index) {
    const auto& diagnostic = diagnostics[index];
    if (diagnostic.exchange.raw == channel->exchange.raw &&
        diagnostic.market.raw == channel->market.raw &&
        diagnostic.apiProtocolProfile.raw ==
            channel->apiProtocolProfile.raw &&
        diagnostic.wirePreference == expectedWire &&
        diagnostic.transport == expectedTransport &&
        (diagnostic.streamMask &
         cxet::api::market::publicMarketDataStreamBit(stream)) != 0u &&
        std::strncmp(diagnostic.firstSymbol.data, symbol.data,
                     Symbol::capacity) == 0) {
      return true;
    }
  }
  return false;
}

[[nodiscard]] bool hasRuntimeRoute(
    const cxet::api::market::PublicMarketDataSubscriptionManager& manager,
    cxet::api::market::PublicMarketDataStream stream,
    cxet::api::market::PublicMarketDataWirePreference expectedWire,
    cxet::api::RouteTransport expectedTransport,
    const Symbol& symbol,
    const Descriptor& descriptor,
    const cxet::api::ObjectConfig* canonicalConfig) noexcept {
  for (std::size_t index = 0u; index < manager.channelCount(); ++index) {
    if (routeMatches(manager, index, stream, expectedWire,
                     expectedTransport, symbol, descriptor,
                     canonicalConfig)) {
      return true;
    }
  }
  return false;
}

}  // namespace

bool validateCaptureProvenance(
    const ProductEligibility& product,
    const Symbol& symbol,
    bool includeDepth,
    const cxet::api::market::PublicMarketDataSubscriptionManager& manager)
    noexcept {
  Registry registry{};
  if (cxet::api::market_v2::buildStagedPublicMarketCatalogV2(&registry) !=
      cxet::api::market_v2::StagedPublicMarketCatalogStatusV2::Ready) {
    return false;
  }
  const Descriptor* book = registry.select(
      product.exchange.raw, product.market.raw, Object::BookTicker, 0u,
      product.apiProtocolProfile.raw);
  const Descriptor* trade = registry.select(
      product.exchange.raw, product.market.raw, Object::Trade, 0u,
      product.apiProtocolProfile.raw);
  const Descriptor* depth = includeDepth
      ? registry.select(product.exchange.raw, product.market.raw,
                        Object::Depth, 0u,
                        product.apiProtocolProfile.raw)
      : nullptr;
  if (!book || !trade || (includeDepth && !depth) ||
      !descriptorMatchesProduct(*book, product, Object::BookTicker) ||
      !descriptorMatchesProduct(*trade, product, Object::Trade) ||
      (includeDepth &&
       !descriptorMatchesProduct(*depth, product, Object::Depth))) {
    return false;
  }
  const cxet::api::ObjectConfig* const bookConfig =
      canonicalRuntimeConfig(*book);
  const cxet::api::ObjectConfig* const tradeConfig =
      canonicalRuntimeConfig(*trade);
  const cxet::api::ObjectConfig* const depthConfig =
      includeDepth ? canonicalRuntimeConfig(*depth) : nullptr;
  if (!bookConfig || !tradeConfig || (includeDepth && !depthConfig)) {
    return false;
  }
  using Stream = cxet::api::market::PublicMarketDataStream;
  return hasRuntimeRoute(manager, Stream::BookTicker,
                         product.bookTickerWire,
                         product.bookTickerTransport, symbol, *book,
                         bookConfig) &&
      hasRuntimeRoute(manager, Stream::Trades, product.tradeWire,
                      product.tradeTransport, symbol, *trade,
                      tradeConfig) &&
      (!includeDepth ||
      hasRuntimeRoute(manager, Stream::Orderbook, product.depthWire,
                       product.depthTransport, symbol, *depth,
                       depthConfig));
}

bool validateCaptureHeaderProvenance(
    const CaptureHeader& header) noexcept {
  ProductEligibility product{};
  product.exchange.raw = header.exchangeRaw;
  product.market.raw = header.marketRaw;
  product.apiProtocolProfile.raw = header.apiProtocolProfileRaw;
  product.bookTickerWire = static_cast<
      cxet::api::market::PublicMarketDataWirePreference>(
          header.bookTickerWireRaw);
  product.tradeWire = static_cast<
      cxet::api::market::PublicMarketDataWirePreference>(
          header.tradeWireRaw);
  product.depthWire = static_cast<
      cxet::api::market::PublicMarketDataWirePreference>(
          header.depthWireRaw);
  product.bookTickerTransport =
      static_cast<cxet::api::RouteTransport>(
          header.bookTickerTransportRaw);
  product.tradeTransport = static_cast<cxet::api::RouteTransport>(
      header.tradeTransportRaw);
  product.depthTransport = static_cast<cxet::api::RouteTransport>(
      header.depthTransportRaw);
  product.bookTickerLaneId = header.bookTickerLaneId;
  product.tradeLaneId = header.tradeLaneId;
  product.depthLaneId = header.depthLaneId;
  const bool includeDepth = (header.captureFlags & 1u) != 0u;

  Registry registry{};
  if (cxet::api::market_v2::buildStagedPublicMarketCatalogV2(&registry) !=
      cxet::api::market_v2::StagedPublicMarketCatalogStatusV2::Ready) {
    return false;
  }
  const Descriptor* book = registry.select(
      product.exchange.raw, product.market.raw, Object::BookTicker, 0u,
      product.apiProtocolProfile.raw);
  const Descriptor* trade = registry.select(
      product.exchange.raw, product.market.raw, Object::Trade, 0u,
      product.apiProtocolProfile.raw);
  const Descriptor* depth = includeDepth
      ? registry.select(product.exchange.raw, product.market.raw,
                        Object::Depth, 0u,
                        product.apiProtocolProfile.raw)
      : nullptr;
  return book && trade && (!includeDepth || depth) &&
      descriptorMatchesProduct(*book, product, Object::BookTicker) &&
      descriptorMatchesProduct(*trade, product, Object::Trade) &&
      (!includeDepth ||
       descriptorMatchesProduct(*depth, product, Object::Depth));
}

}  // namespace exchange_probe::reconstruction
