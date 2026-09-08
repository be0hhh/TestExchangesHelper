#pragma once

#include "exchange_probe/race/Book.hpp"
#include "exchange_probe/race/Transport.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace exchange_probe::race {

enum class FeedFamily : std::uint8_t {
  DirectBbo = 1u,
  SnapshotDepth = 2u,
  IncrementalDepth = 3u,
  Trade = 4u,
};

struct VenueNormalizerState {
  Venue venue{Venue::Unknown};
  Wire wire{Wire::Json};
  FeedFamily family{FeedFamily::DirectBbo};
  EventClass depthEventClass{EventClass::Depth};
  unsigned configuredDepth{0u};
  FixedText<64u> nativeSymbol{};
  BoundedLocalBook book{};
  BboState lastBbo{};
  std::uint64_t lastTop5Fingerprint{0u};
  std::uint64_t lastTop50Fingerprint{0u};
  std::uint64_t eventOrdinal{0u};
  bool subscriptionAccepted{false};
  std::array<BookLevel, kMaximumBookLevels> scratchBids{};
  std::array<BookLevel, kMaximumBookLevels> scratchAsks{};
  std::array<BookMutation, kMaximumBookLevels * 2u> scratchMutations{};
};

void reset_venue_normalizer(void* state) noexcept;

[[nodiscard]] bool normalize_bitget(
    void* state, const FrameView& frame, NormalizeBatch& output) noexcept;
[[nodiscard]] bool normalize_bybit(
    void* state, const FrameView& frame, NormalizeBatch& output) noexcept;
[[nodiscard]] bool normalize_gate(
    void* state, const FrameView& frame, NormalizeBatch& output) noexcept;
[[nodiscard]] bool normalize_okx(
    void* state, const FrameView& frame, NormalizeBatch& output) noexcept;
[[nodiscard]] bool normalize_kucoin(
    void* state, const FrameView& frame, NormalizeBatch& output) noexcept;
[[nodiscard]] bool normalize_binance_usdm(
    void* state, const FrameView& frame, NormalizeBatch& output) noexcept;
[[nodiscard]] bool normalize_aster(
    void* state, const FrameView& frame, NormalizeBatch& output) noexcept;

}  // namespace exchange_probe::race
