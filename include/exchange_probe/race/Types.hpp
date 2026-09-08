#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace exchange_probe::race {

inline constexpr std::uint16_t kRecordSchemaVersion = 2u;
inline constexpr std::size_t kMaximumChangedLevels = 4u;

enum class Venue : std::uint8_t {
  Unknown = 0u,
  Bitget = 1u,
  Bybit = 2u,
  Gate = 3u,
  Okx = 4u,
  Kucoin = 5u,
  BinanceUsdM = 6u,
  Aster = 7u,
};

enum class Wire : std::uint8_t {
  Json = 1u,
  Sbe = 2u,
};

enum class TransportKind : std::uint8_t {
  RuntimeSubscribe = 1u,
  RawStream = 2u,
  CombinedStream = 3u,
  Derived = 4u,
};

enum class EventClass : std::uint8_t {
  None = 0u,
  Trade = 1u,
  Bbo = 2u,
  Top5 = 3u,
  Top50 = 4u,
  Depth = 5u,
  Control = 6u,
  Health = 7u,
};

enum class RecordOrigin : std::uint8_t {
  Raw = 1u,
  DerivedBook = 2u,
  DerivedTradeBbo = 3u,
};

enum class BookValidity : std::uint8_t {
  Uninitialized = 0u,
  Synchronizing = 1u,
  Valid = 2u,
  InvalidGap = 3u,
  InvalidOverflow = 4u,
};

enum class FeedStatus : std::uint8_t {
  Planned = 0u,
  Connecting = 1u,
  Subscribing = 2u,
  Synchronizing = 3u,
  Ready = 4u,
  Unavailable = 5u,
  Rejected = 6u,
  RequiresLogin = 7u,
  VipRequired = 8u,
  UnsupportedSchema = 9u,
  Disconnected = 10u,
  Failed = 11u,
  Degraded = 12u,
};

enum ChangeMask : std::uint8_t {
  kBidPriceChanged = 1u << 0u,
  kBidQuantityChanged = 1u << 1u,
  kAskPriceChanged = 1u << 2u,
  kAskQuantityChanged = 1u << 3u,
};

template <std::size_t Capacity>
struct FixedText {
  std::array<char, Capacity> bytes{};
  std::uint16_t size{0u};

  [[nodiscard]] constexpr bool assign(
      const char* data, std::size_t length) noexcept {
    if (data == nullptr || length > Capacity) return false;
    for (std::size_t i = 0u; i < length; ++i) bytes[i] = data[i];
    for (std::size_t i = length; i < Capacity; ++i) bytes[i] = '\0';
    size = static_cast<std::uint16_t>(length);
    return true;
  }
};

struct SourceIdentity {
  std::uint32_t sourceId{0u};
  std::uint32_t parentSourceId{0u};
  std::uint32_t connectionId{0u};
  std::uint32_t connectionGeneration{0u};
  std::uint16_t sessionId{0u};
  std::uint16_t raceGroupId{0u};
  Venue venue{Venue::Unknown};
  Wire wire{Wire::Json};
  TransportKind transport{TransportKind::RuntimeSubscribe};
  RecordOrigin origin{RecordOrigin::Raw};
};

struct TimestampSet {
  std::uint64_t recvMonoNs{0u};
  std::uint64_t firstReadTsc{0u};
  std::uint64_t exchangeEventNs{0u};
  std::uint64_t streamServiceNs{0u};
};

struct SequenceIdentity {
  std::uint64_t sequence{0u};
  std::uint64_t previousSequence{0u};
  std::uint64_t firstSequence{0u};
  std::uint64_t lastSequence{0u};
};

struct WireSchemaIdentity {
  std::uint16_t blockLength{0u};
  std::uint16_t templateId{0u};
  std::uint16_t schemaId{0u};
  std::uint16_t schemaVersion{0u};
};

struct TradeIdentity {
  std::uint64_t nativeFirst{0u};
  std::uint64_t nativeSecond{0u};
  std::uint64_t tradeId{0u};
  std::uint64_t aggregateId{0u};
  std::uint64_t firstTradeId{0u};
  std::uint64_t lastTradeId{0u};
  std::uint32_t count{0u};
  std::uint8_t aggressorSide{0u};
  std::uint8_t nativeShape{0u};
  std::array<std::uint8_t, 2u> reserved{};
};

struct BboState {
  std::int64_t bidPrice{0};
  std::int64_t bidQuantity{0};
  std::int64_t askPrice{0};
  std::int64_t askQuantity{0};

  [[nodiscard]] constexpr bool operator==(const BboState&) const noexcept =
      default;
};

struct LevelChange {
  std::int64_t price{0};
  std::int64_t quantity{0};
  std::uint8_t side{0u};
  std::array<std::uint8_t, 7u> reserved{};
};

struct RaceRecord {
  std::uint16_t schemaVersion{kRecordSchemaVersion};
  EventClass eventClass{EventClass::None};
  BookValidity validity{BookValidity::Uninitialized};
  std::uint8_t changeMask{0u};
  std::uint8_t changedLevelCount{0u};
  std::uint16_t frameBatchCount{0u};
  std::uint32_t resyncGeneration{0u};
  std::uint64_t eventOrdinal{0u};
  std::uint64_t top5Fingerprint{0u};
  std::uint64_t top50Fingerprint{0u};
  SourceIdentity source{};
  TimestampSet timestamps{};
  WireSchemaIdentity wireSchema{};
  SequenceIdentity sequence{};
  TradeIdentity trade{};
  BboState bbo{};
  std::array<LevelChange, kMaximumChangedLevels> changedLevels{};
};

static_assert(std::is_trivially_copyable_v<RaceRecord>);
static_assert(std::is_standard_layout_v<RaceRecord>);
static_assert(sizeof(RaceRecord) <= 320u);

struct TimeMappingSample {
  std::uint64_t monoBeforeNs{0u};
  std::uint64_t realtimeNs{0u};
  std::uint64_t monoAfterNs{0u};
};

struct CpuLocation {
  unsigned logicalCpu{0u};
  unsigned physicalCore{0u};
  unsigned package{0u};
};

struct AffinityAssignment {
  std::uint32_t connectionId{0u};
  unsigned logicalCpu{0u};
  unsigned physicalCore{0u};
  unsigned package{0u};
  bool oversubscribed{false};
};

}  // namespace exchange_probe::race
