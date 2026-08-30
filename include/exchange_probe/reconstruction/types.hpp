#pragma once

#include "primitives/buf/Symbol.hpp"
#include "primitives/raw/counts/ApiProtocolProfile.hpp"
#include "primitives/raw/counts/ExchangeId.hpp"
#include "canon/Subtypes.hpp"
#include "api/config/ExchangeObjectConfig.hpp"
#include "api/market/PublicMarketDataTypes.hpp"

#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace exchange_probe::reconstruction {

inline constexpr std::uint32_t kCaptureMagic = 0x42424f52u;  // BBOR
inline constexpr std::uint16_t kCaptureSchemaVersion = 2u;
inline constexpr std::uint32_t kCaptureCompleteMagic = 0x454e4442u;
inline constexpr std::size_t kMaximumProducts = 256u;
inline constexpr std::size_t kMaximumSelectedSymbols = 3u;
inline constexpr std::size_t kMaximumCampaignSessions = 16u;
inline constexpr std::int64_t kMaximumCaptureSeconds = 86'400;
inline constexpr std::uint64_t kMaximumCaptureRecords = 10'000'000u;

enum class ObservationKind : std::uint8_t {
  BookTickerSide = 1u,
  Trade = 2u,
  DepthBbo = 3u,
  Reset = 4u,
};

enum class TimestampOrigin : std::uint8_t {
  Unknown = 0u,
  Exchange = 1u,
  Receive = 2u,
};

enum class ReconstructionPolicy : std::uint8_t {
  StrictExchange = 1u,
  HybridEligibility = 2u,
  ReceiveOrderCounterfactual = 3u,
};

enum class ReconstructionMode : std::uint8_t {
  None = 0u,
  Direct = 1u,
  Reverse = 2u,
  Nibbling = 3u,
};

enum class CaptureParserContract : std::uint8_t {
  None = 0u,
  BookTickerRuntimeV1 = 1u,
  TradeRuntimeV1 = 2u,
  DepthRuntimeV1 = 3u,
};

enum class ProductEligibilityReason : std::uint8_t {
  Eligible = 0u,
  CatalogUnavailable,
  MissingBookTickerLane,
  MissingTradeLane,
  MissingBookTickerRuntime,
  MissingTradeRuntime,
  CatalogCapacityExceeded,
};

struct ProductEligibility final {
  ExchangeId exchange{};
  canon::MarketType market{};
  ApiProtocolProfile apiProtocolProfile{};
  ProductEligibilityReason reason{
      ProductEligibilityReason::CatalogUnavailable};
  bool ticker24hAvailable{false};
  bool instrumentRulesAvailable{false};
  bool depthRuntimeAvailable{false};
  bool bookTickerTimestampRequired{false};
  bool tradeTimestampRequired{false};
  cxet::api::market::PublicMarketDataWirePreference bookTickerWire{
      cxet::api::market::PublicMarketDataWirePreference::Auto};
  cxet::api::market::PublicMarketDataWirePreference tradeWire{
      cxet::api::market::PublicMarketDataWirePreference::Auto};
  cxet::api::market::PublicMarketDataWirePreference depthWire{
      cxet::api::market::PublicMarketDataWirePreference::Auto};
  cxet::api::RouteTransport bookTickerTransport{
      cxet::api::RouteTransport::None};
  cxet::api::RouteTransport tradeTransport{
      cxet::api::RouteTransport::None};
  cxet::api::RouteTransport depthTransport{
      cxet::api::RouteTransport::None};
  std::uint16_t bookTickerLaneId{0u};
  std::uint16_t tradeLaneId{0u};
  std::uint16_t depthLaneId{0u};

  [[nodiscard]] constexpr bool eligible() const noexcept {
    return reason == ProductEligibilityReason::Eligible;
  }
};

struct CaptureHeader final {
  std::uint32_t magic{kCaptureMagic};
  std::uint16_t schemaVersion{kCaptureSchemaVersion};
  std::uint16_t headerBytes{static_cast<std::uint16_t>(sizeof(CaptureHeader))};
  std::uint16_t recordBytes{0u};
  std::uint8_t exchangeRaw{0u};
  std::uint8_t marketRaw{0u};
  std::uint8_t apiProtocolProfileRaw{0u};
  std::uint8_t bookTickerWireRaw{0u};
  std::uint8_t tradeWireRaw{0u};
  std::uint8_t depthWireRaw{0u};
  std::int64_t tickSizeRaw{0};
  std::uint64_t sessionId{0u};
  std::uint64_t startedRealtimeNs{0u};
  std::uint16_t bookTickerLaneId{0u};
  std::uint16_t tradeLaneId{0u};
  std::uint16_t depthLaneId{0u};
  std::uint8_t bookTickerTransportRaw{0u};
  std::uint8_t tradeTransportRaw{0u};
  std::uint8_t depthTransportRaw{0u};
  std::uint8_t bookTickerParserContractRaw{0u};
  std::uint8_t tradeParserContractRaw{0u};
  std::uint8_t depthParserContractRaw{0u};
  std::uint16_t captureFlags{0u};
  std::uint32_t reserved0{0u};
  std::uint32_t reserved1{0u};
  Symbol symbol{};
};

struct CaptureFooter final {
  std::uint32_t magic{kCaptureCompleteMagic};
  std::uint16_t schemaVersion{kCaptureSchemaVersion};
  std::uint16_t footerBytes{static_cast<std::uint16_t>(sizeof(CaptureFooter))};
  std::uint64_t recordCount{0u};
  std::uint64_t completedRealtimeNs{0u};
  std::uint64_t reserved[5]{};
};

struct alignas(64) Observation final {
  std::uint64_t sequence{0u};
  std::uint64_t eventId{0u};
  std::uint64_t exchangeTimestampNs{0u};
  std::uint64_t receiveMonotonicNs{0u};
  std::int64_t bidPriceRaw{0};
  std::int64_t bidQtyRaw{0};
  std::int64_t askPriceRaw{0};
  std::int64_t askQtyRaw{0};
  std::int64_t priceRaw{0};
  std::int64_t qtyRaw{0};
  ObservationKind kind{ObservationKind::BookTickerSide};
  TimestampOrigin timestampOrigin{TimestampOrigin::Unknown};
  // 1=bid/buy, 2=ask/sell. BookTickerSide action: 1=upsert, 2=delete.
  std::uint8_t side{0u};
  std::uint8_t action{0u};
  std::uint8_t coalesceNext{0u};
  std::uint8_t reserved[43]{};
};

static_assert(std::is_standard_layout_v<CaptureHeader>);
static_assert(std::is_trivially_copyable_v<CaptureHeader>);
static_assert(sizeof(CaptureHeader) == 96u);
static_assert(std::is_standard_layout_v<CaptureFooter>);
static_assert(std::is_trivially_copyable_v<CaptureFooter>);
static_assert(sizeof(CaptureFooter) == 64u);
static_assert(std::is_standard_layout_v<Observation>);
static_assert(std::is_trivially_copyable_v<Observation>);
static_assert(sizeof(Observation) == 128u);
inline constexpr std::uint64_t kMaximumCaptureBytes =
    sizeof(CaptureHeader) +
    kMaximumCaptureRecords * sizeof(Observation) +
    sizeof(CaptureFooter);

struct AnalysisCounters final {
  std::uint64_t observations{0u};
  std::uint64_t terminalResets{0u};
  std::uint64_t rawBookTickerUpdates{0u};
  std::uint64_t trades{0u};
  std::uint64_t acceptedTrades{0u};
  std::uint64_t rejectedTrades{0u};
  std::uint64_t direct{0u};
  std::uint64_t reverse{0u};
  std::uint64_t nibbling{0u};
  std::uint64_t fullNibbling{0u};
  std::uint64_t missingTimestamp{0u};
  std::uint64_t equalBookTickerTimestamp{0u};
  std::uint64_t timestampRollback{0u};
  std::uint64_t unknownAggressor{0u};
  std::uint64_t nextBboConfirmations{0u};
  std::uint64_t nextBboContradictions{0u};
  std::uint64_t depthConfirmations{0u};
  std::uint64_t depthContradictions{0u};
};

struct InstrumentCandidate final {
  Symbol symbol{};
  char baseAsset[32]{};
  char quoteAsset[32]{};
  std::int64_t tickSizeRaw{0};
  std::int64_t quoteVolumeRaw{0};
  bool active{false};
  bool exactQuoteVolume{false};
};

struct InstrumentSelection final {
  InstrumentCandidate rows[kMaximumSelectedSymbols]{};
  std::size_t count{0u};
  bool ethAvailable{false};
  bool volumeFallbackToEthOnly{false};
};

[[nodiscard]] const char* productEligibilityReasonName(
    ProductEligibilityReason reason) noexcept;
[[nodiscard]] const char* reconstructionPolicyName(
    ReconstructionPolicy policy) noexcept;

}  // namespace exchange_probe::reconstruction
