#pragma once

#include "exchange_probe/race/Normalizers.hpp"

#include "cxet/Parse/DecimalToScaled.hpp"
#include "cxet/Parse/HotJsonScan.hpp"
#include "cxet/Primitives/Buf/CanonConstants.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace exchange_probe::race::normalizer_detail {

namespace hot = cxet::parse::hotjson;

[[nodiscard]] inline hot::View json_view(const FrameView& frame) noexcept {
  return hot::trim({reinterpret_cast<const char*>(frame.data), frame.size});
}

[[nodiscard]] inline bool slot_text(
    const hot::FieldSlot& slot, char* scratch, std::size_t scratchSize,
    cxet::StringView* output) noexcept {
  if (!output || !slot.found) return false;
  return hot::tokenPayloadDecoded(
      slot.value, scratch, scratchSize, output);
}

[[nodiscard]] inline bool parse_u64_text(
    cxet::StringView text, std::uint64_t* output) noexcept {
  if (!output || text.empty()) return false;
  std::uint64_t value = 0u;
  for (std::size_t index = 0u; index < text.size(); ++index) {
    const char byte = text[index];
    if (byte < '0' || byte > '9') return false;
    const auto digit = static_cast<std::uint64_t>(byte - '0');
    if (value > (std::numeric_limits<std::uint64_t>::max() - digit) / 10u)
      return false;
    value = value * 10u + digit;
  }
  *output = value;
  return true;
}

[[nodiscard]] inline bool slot_u64(
    const hot::FieldSlot& slot, std::uint64_t* output) noexcept {
  if (!slot.found || !output) return false;
  std::uint64_t direct = 0u;
  if (hot::slotUint64(slot, &direct)) {
    *output = direct;
    return true;
  }
  char scratch[32]{};
  cxet::StringView text{};
  return slot_text(slot, scratch, sizeof(scratch), &text) &&
         parse_u64_text(text, output);
}

[[nodiscard]] inline bool parse_price(
    cxet::StringView text, std::int64_t* out) noexcept {
  return cxet::parse::parseUnsignedScaledDecimalStrict(
      text, numeric::kPriceScaleDigits, out);
}

[[nodiscard]] inline bool parse_amount(
    cxet::StringView text, std::int64_t* out) noexcept {
  return cxet::parse::parseUnsignedScaledDecimalStrict(
      text, numeric::kAmountScaleDigits, out);
}

[[nodiscard]] inline bool parse_level(
    hot::View row, BookLevel* output) noexcept {
  if (!output) return false;
  row = hot::trim(row);
  cxet::StringView price{};
  cxet::StringView quantity{};
  if (row.size < 2u || row.data[0] != '[' ||
      !hot::readFirstTwoArrayValues(row, &price, &quantity) ||
      !parse_price(price, &output->price) ||
      !parse_amount(quantity, &output->quantity) ||
      output->price <= 0 || output->quantity < 0) {
    return false;
  }
  return true;
}

[[nodiscard]] inline bool parse_levels(
    hot::View levels, BookLevel* output, std::size_t capacity,
    std::size_t* count, bool allowZeroQuantity) noexcept {
  if (!output || !count) return false;
  *count = 0u;
  std::size_t position = 1u;
  hot::View row{};
  while (hot::nextArrayItem(levels, &position, &row)) {
    if (*count == capacity || !parse_level(row, &output[*count]) ||
        (!allowZeroQuantity && output[*count].quantity == 0)) {
      return false;
    }
    ++*count;
  }
  return hot::arrayIterationComplete(levels, position);
}

[[nodiscard]] inline bool append_record(
    NormalizeBatch& output, const RaceRecord& record) noexcept {
  if (output.count >= output.records.size()) {
    output.malformed = true;
    return false;
  }
  output.records[output.count++] = record;
  return true;
}

[[nodiscard]] inline bool emit_bbo(
    VenueNormalizerState& state, NormalizeBatch& output,
    const BboState& bbo, std::uint64_t sequence,
    std::uint64_t exchangeNs, RecordOrigin origin,
    BookValidity validity = BookValidity::Valid) noexcept {
  if (bbo.bidPrice <= 0 || bbo.askPrice <= 0 ||
      bbo.bidPrice >= bbo.askPrice || bbo.bidQuantity < 0 ||
      bbo.askQuantity < 0) {
    return false;
  }
  const auto change = bbo_change_mask(state.lastBbo, bbo);
  if (change == 0u) return true;
  RaceRecord record{};
  record.eventClass = EventClass::Bbo;
  record.validity = validity;
  record.changeMask = change;
  record.eventOrdinal = ++state.eventOrdinal;
  record.sequence.sequence = sequence;
  record.timestamps.exchangeEventNs = exchangeNs;
  record.bbo = bbo;
  record.source.origin = origin;
  if (!append_record(output, record)) return false;
  state.lastBbo = bbo;
  return true;
}

[[nodiscard]] inline bool emit_snapshot(
    VenueNormalizerState& state, NormalizeBatch& output,
    std::size_t bidCount, std::size_t askCount,
    std::uint64_t sequence, std::uint64_t exchangeNs) noexcept {
  const BboState before = state.lastBbo;
  if (!state.book.apply_snapshot(
          state.scratchBids.data(), bidCount,
          state.scratchAsks.data(), askCount, sequence)) {
    return false;
  }
  const auto top5 = state.book.top_fingerprint(5u);
  const auto top50 = state.book.top_fingerprint(50u);
  const auto current = state.book.bbo();
  const bool topChanged =
      (state.depthEventClass == EventClass::Top5
           ? state.lastTop5Fingerprint != top5
           : state.lastTop50Fingerprint != top50);
  if (topChanged) {
    RaceRecord depth{};
    depth.eventClass = state.depthEventClass;
    depth.validity = BookValidity::Valid;
    depth.eventOrdinal = ++state.eventOrdinal;
    depth.resyncGeneration = state.book.resync_generation();
    depth.sequence.sequence = sequence;
    depth.timestamps.exchangeEventNs = exchangeNs;
    depth.bbo = current;
    depth.top5Fingerprint = top5;
    depth.top50Fingerprint = top50;
    depth.source.origin = RecordOrigin::Raw;
    if (!append_record(output, depth)) return false;
  }
  state.lastTop5Fingerprint = top5;
  state.lastTop50Fingerprint = top50;
  state.lastBbo = before;
  return emit_bbo(
      state, output, current, sequence, exchangeNs,
      RecordOrigin::DerivedBook);
}

[[nodiscard]] inline bool emit_delta(
    VenueNormalizerState& state, NormalizeBatch& output,
    std::size_t mutationCount, const SequenceUpdate& sequence,
    std::uint64_t exchangeNs) noexcept {
  const auto applied = state.book.apply_delta_batch(
      state.scratchMutations.data(), mutationCount, sequence);
  if (applied.gap) {
    RaceRecord health{};
    health.eventClass = EventClass::Health;
    health.validity = BookValidity::InvalidGap;
    health.eventOrdinal = ++state.eventOrdinal;
    health.sequence.firstSequence = sequence.first;
    health.sequence.lastSequence = sequence.last;
    health.sequence.previousSequence = sequence.previous;
    health.resyncGeneration = state.book.resync_generation();
    return append_record(output, health);
  }
  if (!applied.accepted) return false;
  if (applied.top5Changed || applied.top50Changed) {
    RaceRecord depth{};
    depth.eventClass = state.depthEventClass;
    depth.validity = BookValidity::Valid;
    depth.eventOrdinal = ++state.eventOrdinal;
    depth.resyncGeneration = state.book.resync_generation();
    depth.sequence.firstSequence = sequence.first;
    depth.sequence.lastSequence = sequence.last;
    depth.sequence.previousSequence = sequence.previous;
    depth.timestamps.exchangeEventNs = exchangeNs;
    depth.bbo = applied.after;
    depth.top5Fingerprint = applied.top5Fingerprint;
    depth.top50Fingerprint = applied.top50Fingerprint;
    depth.source.origin = RecordOrigin::Raw;
    if (!append_record(output, depth)) return false;
  }
  if (!applied.bboChanged) return true;
  state.lastBbo = applied.before;
  return emit_bbo(
      state, output, applied.after, sequence.last, exchangeNs,
      RecordOrigin::DerivedBook);
}

inline void reset_state(VenueNormalizerState& state) noexcept {
  const auto venue = state.venue;
  const auto wire = state.wire;
  const auto family = state.family;
  const auto eventClass = state.depthEventClass;
  const auto depth = state.configuredDepth;
  const auto symbol = state.nativeSymbol;
  state = VenueNormalizerState{};
  state.venue = venue;
  state.wire = wire;
  state.family = family;
  state.depthEventClass = eventClass;
  state.configuredDepth = depth;
  state.nativeSymbol = symbol;
}

}  // namespace exchange_probe::race::normalizer_detail
