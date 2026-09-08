#include "exchange_probe/reconstruction/Catalog.hpp"

#include "cxet/Api/Connector/ExchangeProductConnector.hpp"
#include "cxet/Api/Connector/ExchangeProductDataCapabilities.hpp"
#include "cxet/Api/Market/StagedPublicMarketCatalogV2.hpp"

namespace exchange_probe::reconstruction {
namespace {

using cxet::api::market_v2::PublicMarketExchangeTimestampPolicy;
using cxet::api::market_v2::PublicMarketObjectV2;
using cxet::api::market_v2::StagedPublicMarketCatalogStatusV2;
using cxet::api::market_v2::StagedPublicMarketRegistryV2;

struct CatalogBuildContext final {
  ProductCatalog* output{nullptr};
  const StagedPublicMarketRegistryV2* registry{nullptr};
  bool overflow{false};
};

[[nodiscard]] cxet::api::market::PublicMarketDataWirePreference wirePreference(
    cxet::api::market_v2::PublicMarketWireFormatV2 wire) noexcept {
  using Wire = cxet::api::market_v2::PublicMarketWireFormatV2;
  if (wire == Wire::JsonText)
    return cxet::api::market::PublicMarketDataWirePreference::Json;
  if (wire == Wire::SbeBinary || wire == Wire::FixSbeBinary)
    return cxet::api::market::PublicMarketDataWirePreference::Sbe;
  if (wire == Wire::ProtobufBinary)
    return cxet::api::market::PublicMarketDataWirePreference::Protobuf;
  return cxet::api::market::PublicMarketDataWirePreference::Auto;
}

[[nodiscard]] cxet::api::RouteTransport routeTransport(
    cxet::api::market_v2::PublicMarketTransportV2 transport) noexcept {
  using Transport = cxet::api::market_v2::PublicMarketTransportV2;
  if (transport == Transport::WebSocket) return cxet::api::RouteTransport::Ws;
  if (transport == Transport::FixSbe) return cxet::api::RouteTransport::Fix;
  if (transport == Transport::Grpc) return cxet::api::RouteTransport::Grpc;
  return cxet::api::RouteTransport::None;
}

void visitConnector(const cxet::api::ExchangeProductConnector& connector,
                    void* opaque) noexcept {
  auto& context = *static_cast<CatalogBuildContext*>(opaque);
  if (!context.output || !context.registry) {
    return;
  }
  if (context.output->count == kMaximumProducts) {
    context.overflow = true;
    return;
  }

  ProductEligibility row{};
  row.exchange = connector.exchange;
  row.market = connector.market;
  row.apiProtocolProfile = connector.apiProtocolProfile;

  const auto* book = context.registry->select(
      connector.exchange.raw, connector.market.raw,
      PublicMarketObjectV2::BookTicker, 0u,
      connector.apiProtocolProfile.raw);
  const auto* trade = context.registry->select(
      connector.exchange.raw, connector.market.raw,
      PublicMarketObjectV2::Trade, 0u,
      connector.apiProtocolProfile.raw);
  if (!book) {
    row.reason = ProductEligibilityReason::MissingBookTickerLane;
  } else if (!trade) {
    row.reason = ProductEligibilityReason::MissingTradeLane;
  } else if (!book->parseBookTickerRuntime &&
             !book->parseBookTickerEventRuntime &&
             !book->parseBookTickerEventNumericRuntime) {
    row.reason = ProductEligibilityReason::MissingBookTickerRuntime;
  } else if (!cxet::api::validTradeRuntimeParserContractV2(
                 trade->tradeRuntimeParserV2)) {
    row.reason = ProductEligibilityReason::MissingTradeRuntime;
  } else {
    row.reason = ProductEligibilityReason::Eligible;
  }

  if (book) {
    row.bookTickerWire = wirePreference(book->key.wire);
    row.bookTickerTransport = routeTransport(book->key.transport);
    row.bookTickerLaneId = book->key.laneId;
    row.bookTickerTimestampRequired =
        book->exchangeTimestampPolicy ==
        PublicMarketExchangeTimestampPolicy::Required;
  }
  if (trade) {
    row.tradeWire = wirePreference(trade->key.wire);
    row.tradeTransport = routeTransport(trade->key.transport);
    row.tradeLaneId = trade->key.laneId;
    row.tradeTimestampRequired =
        trade->exchangeTimestampPolicy ==
        PublicMarketExchangeTimestampPolicy::Required;
  }
  const auto* depth = context.registry->select(
      connector.exchange.raw, connector.market.raw,
      PublicMarketObjectV2::Depth, 0u,
      connector.apiProtocolProfile.raw);
  row.depthRuntimeAvailable = depth && depth->parseDepthRuntime;
  if (depth) {
    row.depthWire = wirePreference(depth->key.wire);
    row.depthTransport = routeTransport(depth->key.transport);
    row.depthLaneId = depth->key.laneId;
  }

  const auto capabilities = cxet::api::exchangeProductDataCapabilities(
      connector.exchange, connector.market, connector.apiProtocolProfile);
  row.instrumentRulesAvailable = capabilities.available(
      cxet::api::kExchangeDataInstrumentRules);
  row.ticker24hAvailable = capabilities.available(
      cxet::api::kExchangeDataTicker24h);
  context.output->rows[context.output->count++] = row;
}

}  // namespace

ProductCatalog buildProductCatalog() noexcept {
  ProductCatalog output{};
  StagedPublicMarketRegistryV2 registry{};
  if (cxet::api::market_v2::buildStagedPublicMarketCatalogV2(&registry) !=
      StagedPublicMarketCatalogStatusV2::Ready) {
    output.failure = ProductEligibilityReason::CatalogUnavailable;
    return output;
  }
  output.failure = ProductEligibilityReason::Eligible;
  CatalogBuildContext context{&output, &registry, false};
  cxet::api::forEachExchangeProductConnector(&visitConnector, &context);
  if (context.overflow) {
    output.failure = ProductEligibilityReason::CatalogCapacityExceeded;
  }
  return output;
}

const char* productEligibilityReasonName(
    ProductEligibilityReason reason) noexcept {
  switch (reason) {
    case ProductEligibilityReason::Eligible: return "eligible";
    case ProductEligibilityReason::CatalogUnavailable:
      return "catalog_unavailable";
    case ProductEligibilityReason::MissingBookTickerLane:
      return "missing_book_ticker_lane";
    case ProductEligibilityReason::MissingTradeLane:
      return "missing_trade_lane";
    case ProductEligibilityReason::MissingBookTickerRuntime:
      return "missing_book_ticker_runtime";
    case ProductEligibilityReason::MissingTradeRuntime:
      return "missing_trade_runtime";
    case ProductEligibilityReason::CatalogCapacityExceeded:
      return "catalog_capacity_exceeded";
  }
  return "unknown";
}

const char* reconstructionPolicyName(ReconstructionPolicy policy) noexcept {
  switch (policy) {
    case ReconstructionPolicy::StrictExchange: return "strict_exchange";
    case ReconstructionPolicy::HybridEligibility:
      return "hybrid_eligibility";
    case ReconstructionPolicy::ReceiveOrderCounterfactual:
      return "receive_order_counterfactual";
  }
  return "unknown";
}

}  // namespace exchange_probe::reconstruction
