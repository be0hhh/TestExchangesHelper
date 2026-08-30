#include "exchange_probe/race/normalizers.hpp"

#include "race_normalizer_common.hpp"

#include "primitives/buf/RuntimeEventId.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>

namespace exchange_probe::race {
namespace {

namespace hot = cxet::parse::hotjson;
namespace detail = normalizer_detail;

[[nodiscard]] bool symbol_matches(
    const VenueNormalizerState& state, const hot::FieldSlot& slot) noexcept {
  if (state.nativeSymbol.size == 0u) return true;
  char scratch[64]{};
  cxet::StringView symbol{};
  return detail::slot_text(slot, scratch, sizeof(scratch), &symbol) &&
         symbol.size() == state.nativeSymbol.size &&
         cxet::bytescan::hftMemcmp(
             symbol.data(), state.nativeSymbol.bytes.data(), symbol.size()) == 0;
}

[[nodiscard]] bool parse_uuid(
    cxet::StringView token, std::uint64_t* high,
    std::uint64_t* low) noexcept {
  if (!high || !low || token.size() != 36u || token[8] != '-' ||
      token[13] != '-' || token[18] != '-' || token[23] != '-') {
    return false;
  }
  std::uint64_t first = 0u;
  std::uint64_t second = 0u;
  std::size_t nibble = 0u;
  for (std::size_t index = 0u; index < token.size(); ++index) {
    const char value = token[index];
    if (value == '-') continue;
    std::uint8_t hex = 0u;
    if (value >= '0' && value <= '9') {
      hex = static_cast<std::uint8_t>(value - '0');
    } else if (value >= 'a' && value <= 'f') {
      hex = static_cast<std::uint8_t>(value - 'a' + 10);
    } else {
      return false;
    }
    if (nibble < 16u)
      first = (first << 4u) | hex;
    else
      second = (second << 4u) | hex;
    ++nibble;
  }
  if (nibble != 32u) return false;
  *high = first;
  *low = second;
  return true;
}

[[nodiscard]] bool parse_trade_identity(
    const hot::FieldSlot& slot, TradeIdentity* output) noexcept {
  if (!output) return false;
  char scratch[64]{};
  cxet::StringView text{};
  if (!detail::slot_text(slot, scratch, sizeof(scratch), &text)) return false;
  Id projected{};
  if (!cxet::runtimeEventIdFromToken(text, projected) || projected.raw == 0u)
    return false;
  std::uint64_t numeric = 0u;
  if (detail::parse_u64_text(text, &numeric) && numeric != 0u) {
    output->nativeFirst = numeric;
    output->nativeShape = 1u;
  } else if (parse_uuid(
                 text, &output->nativeFirst, &output->nativeSecond)) {
    output->nativeShape = 2u;
  } else {
    return false;
  }
  output->tradeId = projected.raw;
  return true;
}

[[nodiscard]] bool classify_ack(
    VenueNormalizerState& state, hot::View root,
    NormalizeBatch& output, bool* classified) noexcept {
  *classified = false;
  hot::FieldSlot fields[] = {
      {"op", 2u}, {"success", 7u}, {"ret_msg", 7u}};
  if (!hot::collectObjectFieldsAllUnique(root, fields)) return false;
  if (!fields[0].found) return true;
  *classified = true;
  if (!hot::slotStringEquals(fields[0], "subscribe")) {
    output.status = state.subscriptionAccepted ? FeedStatus::Ready
                                               : FeedStatus::Synchronizing;
    return true;
  }
  bool success = false;
  if (!hot::slotBool(fields[1], &success)) return false;
  state.subscriptionAccepted = success;
  output.status = !success
                      ? FeedStatus::Rejected
                      : state.family == FeedFamily::Trade ||
                                state.family == FeedFamily::DirectBbo
                            ? FeedStatus::Ready
                            : FeedStatus::Synchronizing;
  return true;
}

[[nodiscard]] bool normalize_trade(
    VenueNormalizerState& state, hot::View root,
    NormalizeBatch& output) noexcept {
  hot::FieldSlot rootFields[] = {
      {"data", 4u}, {"ts", 2u}, {"topic", 5u}};
  if (!hot::collectObjectFieldsAllUnique(root, rootFields)) return false;
  hot::View trades{};
  if (!hot::slotArray(rootFields[0], &trades)) return false;
  std::uint64_t streamTimestampMs = 0u;
  if (!detail::slot_u64(rootFields[1], &streamTimestampMs)) return false;
  std::size_t position = 1u;
  hot::View trade{};
  const std::size_t firstOutput = output.count;
  while (hot::nextArrayItem(trades, &position, &trade)) {
    hot::FieldSlot fields[] = {
        {"T", 1u}, {"s", 1u}, {"S", 1u}, {"v", 1u},
        {"p", 1u}, {"i", 1u}, {"seq", 3u}};
    if (!hot::collectObjectFieldsAllUnique(trade, fields) ||
        !symbol_matches(state, fields[1])) {
      return false;
    }
    std::uint64_t timestampMs = 0u;
    std::uint64_t sequence = 0u;
    char priceScratch[64]{};
    char quantityScratch[64]{};
    cxet::StringView price{};
    cxet::StringView quantity{};
    RaceRecord record{};
    if (!detail::slot_u64(fields[0], &timestampMs) || timestampMs == 0u ||
        timestampMs > std::numeric_limits<std::uint64_t>::max() /
                          numeric::kNanosecondsPerMillisecond ||
        !detail::slot_u64(fields[6], &sequence) || sequence == 0u ||
        !detail::slot_text(
            fields[4], priceScratch, sizeof(priceScratch), &price) ||
        !detail::slot_text(
            fields[3], quantityScratch, sizeof(quantityScratch), &quantity) ||
        !detail::parse_price(price, &record.changedLevels[0].price) ||
        !detail::parse_amount(quantity, &record.changedLevels[0].quantity) ||
        !parse_trade_identity(fields[5], &record.trade)) {
      return false;
    }
    if (hot::slotStringEquals(fields[2], "Buy"))
      record.trade.aggressorSide = 1u;
    else if (hot::slotStringEquals(fields[2], "Sell"))
      record.trade.aggressorSide = 2u;
    else
      return false;
    record.eventClass = EventClass::Trade;
    record.validity = BookValidity::Valid;
    record.eventOrdinal = ++state.eventOrdinal;
    record.frameBatchCount = 1u;
    record.sequence.sequence = sequence;
    record.timestamps.exchangeEventNs =
        timestampMs * numeric::kNanosecondsPerMillisecond;
    record.timestamps.streamServiceNs =
        streamTimestampMs * numeric::kNanosecondsPerMillisecond;
    record.changedLevelCount = 1u;
    record.changedLevels[0].side = record.trade.aggressorSide;
    if (!detail::append_record(output, record)) return false;
  }
  if (!hot::arrayIterationComplete(trades, position) ||
      output.count == firstOutput) {
    return false;
  }
  const auto batch = static_cast<std::uint16_t>(output.count - firstOutput);
  for (std::size_t index = firstOutput; index < output.count; ++index)
    output.records[index].frameBatchCount = batch;
  state.subscriptionAccepted = true;
  output.status = FeedStatus::Ready;
  return true;
}

[[nodiscard]] bool normalize_book(
    VenueNormalizerState& state, hot::View root,
    NormalizeBatch& output) noexcept {
  hot::FieldSlot rootFields[] = {
      {"topic", 5u}, {"type", 4u}, {"data", 4u}, {"ts", 2u}};
  if (!hot::collectObjectFieldsAllUnique(root, rootFields)) return false;
  hot::View data{};
  if (!hot::slotObject(rootFields[2], &data)) return false;
  hot::FieldSlot fields[] = {
      {"s", 1u}, {"b", 1u}, {"a", 1u}, {"u", 1u},
      {"seq", 3u}, {"cts", 3u}};
  if (!hot::collectObjectFieldsAllUnique(data, fields) ||
      !symbol_matches(state, fields[0])) {
    return false;
  }
  hot::View bids{};
  hot::View asks{};
  std::uint64_t updateId = 0u;
  std::uint64_t crossSequence = 0u;
  std::uint64_t matchingTimestampMs = 0u;
  std::uint64_t streamTimestampMs = 0u;
  if (!hot::slotArray(fields[1], &bids) ||
      !hot::slotArray(fields[2], &asks) ||
      !detail::slot_u64(fields[3], &updateId) || updateId == 0u ||
      !detail::slot_u64(fields[4], &crossSequence) || crossSequence == 0u ||
      !detail::slot_u64(fields[5], &matchingTimestampMs) ||
      !detail::slot_u64(rootFields[3], &streamTimestampMs)) {
    return false;
  }
  const std::uint64_t exchangeNs =
      matchingTimestampMs * numeric::kNanosecondsPerMillisecond;
  const std::uint64_t streamNs =
      streamTimestampMs * numeric::kNanosecondsPerMillisecond;
  const std::size_t firstOutput = output.count;
  if (hot::slotStringEquals(rootFields[1], "snapshot")) {
    std::size_t bidCount = 0u;
    std::size_t askCount = 0u;
    if (!detail::parse_levels(
            bids, state.scratchBids.data(), state.scratchBids.size(),
            &bidCount, false) ||
        !detail::parse_levels(
            asks, state.scratchAsks.data(), state.scratchAsks.size(),
            &askCount, false)) {
      return false;
    }
    if (state.family == FeedFamily::DirectBbo) {
      if (bidCount != 1u || askCount != 1u ||
          !detail::emit_bbo(
              state, output,
              BboState{state.scratchBids[0].price,
                       state.scratchBids[0].quantity,
                       state.scratchAsks[0].price,
                       state.scratchAsks[0].quantity},
              updateId, exchangeNs, RecordOrigin::Raw)) {
        return false;
      }
    } else if (!detail::emit_snapshot(
                   state, output, bidCount, askCount, updateId, exchangeNs)) {
      return false;
    }
  } else if (hot::slotStringEquals(rootFields[1], "delta")) {
    if (state.book.validity() != BookValidity::Valid) {
      output.status = FeedStatus::Synchronizing;
      return true;
    }
    std::size_t bidCount = 0u;
    std::size_t askCount = 0u;
    if (!detail::parse_levels(
            bids, state.scratchBids.data(), state.scratchBids.size(),
            &bidCount, true) ||
        !detail::parse_levels(
            asks, state.scratchAsks.data(), state.scratchAsks.size(),
            &askCount, true)) {
      state.book.invalidate(BookValidity::InvalidGap);
      return false;
    }
    std::size_t mutations = 0u;
    for (std::size_t index = 0u; index < bidCount; ++index)
      state.scratchMutations[mutations++] =
          {BookSide::Bid, state.scratchBids[index]};
    for (std::size_t index = 0u; index < askCount; ++index)
      state.scratchMutations[mutations++] =
          {BookSide::Ask, state.scratchAsks[index]};
    if (mutations == 0u ||
        !detail::emit_delta(
            state, output, mutations,
            SequenceUpdate{updateId, updateId, 0u, false}, exchangeNs)) {
      return false;
    }
  } else {
    return false;
  }
  for (std::size_t index = firstOutput; index < output.count; ++index) {
    output.records[index].sequence.sequence = crossSequence;
    output.records[index].timestamps.streamServiceNs = streamNs;
  }
  state.subscriptionAccepted = true;
  output.status = FeedStatus::Ready;
  return true;
}

}  // namespace

bool normalize_bybit(
    void* rawState, const FrameView& frame,
    NormalizeBatch& output) noexcept {
  output = NormalizeBatch{};
  auto* state = static_cast<VenueNormalizerState*>(rawState);
  if (!state || state->venue != Venue::Bybit || frame.binary ||
      !frame.data || frame.size == 0u) {
    output.malformed = true;
    return false;
  }
  const auto root = detail::json_view(frame);
  bool ack = false;
  if (!classify_ack(*state, root, output, &ack)) {
    output.malformed = true;
    return false;
  }
  if (ack) return true;
  const bool ok = state->family == FeedFamily::Trade
                      ? normalize_trade(*state, root, output)
                      : normalize_book(*state, root, output);
  output.malformed = !ok;
  return ok;
}

void reset_venue_normalizer(void* rawState) noexcept {
  auto* state = static_cast<VenueNormalizerState*>(rawState);
  if (state) normalizer_detail::reset_state(*state);
}

}  // namespace exchange_probe::race
