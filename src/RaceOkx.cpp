#include "exchange_probe/race/Normalizers.hpp"

#include "RaceNormalizerCommon.hpp"

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
             symbol.data(), state.nativeSymbol.bytes.data(), symbol.size()) ==
             0;
}

[[nodiscard]] bool classify_control(
    VenueNormalizerState& state, hot::View root,
    NormalizeBatch& output, bool* classified) noexcept {
  *classified = false;
  hot::FieldSlot fields[] = {
      {"event", 5u}, {"code", 4u}, {"msg", 3u}};
  if (!hot::collectObjectFieldsAllUnique(root, fields)) return false;
  if (!fields[0].found) return true;
  *classified = true;
  if (hot::slotStringEquals(fields[0], "subscribe")) {
    state.subscriptionAccepted = true;
    output.status = state.family == FeedFamily::Trade ||
                            state.family == FeedFamily::DirectBbo ||
                            state.family == FeedFamily::SnapshotDepth
                        ? FeedStatus::Ready
                        : FeedStatus::Synchronizing;
    return true;
  }
  if (!hot::slotStringEquals(fields[0], "error")) {
    output.status = state.subscriptionAccepted ? FeedStatus::Ready
                                               : FeedStatus::Synchronizing;
    return true;
  }
  char codeScratch[24]{};
  cxet::StringView code{};
  if (!detail::slot_text(
          fields[1], codeScratch, sizeof(codeScratch), &code)) {
    output.status = FeedStatus::Rejected;
    return true;
  }
  if (code == cxet::StringView("60011", 5u))
    output.status = FeedStatus::RequiresLogin;
  else if (code == cxet::StringView("60029", 5u) ||
           code == cxet::StringView("60030", 5u))
    output.status = FeedStatus::VipRequired;
  else
    output.status = FeedStatus::Rejected;
  return true;
}

[[nodiscard]] bool data_context(
    const VenueNormalizerState& state, hot::View root,
    hot::View* data, cxet::StringView* channel,
    cxet::StringView* action) noexcept {
  hot::FieldSlot rootFields[] = {
      {"arg", 3u}, {"action", 6u}, {"data", 4u}};
  if (!hot::collectObjectFieldsAllUnique(root, rootFields) ||
      !hot::slotArray(rootFields[2], data)) {
    return false;
  }
  static thread_local char actionScratch[16]{};
  if (rootFields[1].found) {
    if (!detail::slot_text(
            rootFields[1], actionScratch, sizeof(actionScratch), action)) {
      return false;
    }
  } else {
    *action = {};
  }
  hot::View argument{};
  hot::FieldSlot argumentFields[] = {
      {"channel", 7u}, {"instId", 6u}};
  static thread_local char channelScratch[32]{};
  return hot::slotObject(rootFields[0], &argument) &&
         hot::collectObjectFieldsAllUnique(argument, argumentFields) &&
         detail::slot_text(
             argumentFields[0], channelScratch, sizeof(channelScratch),
             channel) &&
         symbol_matches(state, argumentFields[1]);
}

[[nodiscard]] bool first_object(hot::View data, hot::View* object) noexcept {
  std::size_t position = 1u;
  return hot::nextArrayItem(data, &position, object) &&
         hot::arrayIterationComplete(data, position);
}

[[nodiscard]] bool normalize_book(
    VenueNormalizerState& state, hot::View data,
    cxet::StringView action, NormalizeBatch& output) noexcept {
  hot::View object{};
  if (!first_object(data, &object)) return false;
  hot::FieldSlot fields[] = {
      {"bids", 4u}, {"asks", 4u}, {"ts", 2u},
      {"seqId", 5u}, {"prevSeqId", 9u}};
  hot::View bids{};
  hot::View asks{};
  std::uint64_t timestampMs = 0u;
  std::uint64_t sequence = 0u;
  std::uint64_t previous = 0u;
  if (!hot::collectObjectFieldsAllUnique(object, fields) ||
      !hot::slotArray(fields[0], &bids) ||
      !hot::slotArray(fields[1], &asks) ||
      !detail::slot_u64(fields[2], &timestampMs) ||
      timestampMs > std::numeric_limits<std::uint64_t>::max() /
                        numeric::kNanosecondsPerMillisecond ||
      !detail::slot_u64(fields[3], &sequence) || sequence == 0u) {
    return false;
  }
  const std::uint64_t exchangeNs =
      timestampMs * numeric::kNanosecondsPerMillisecond;
  std::size_t bidCount = 0u;
  std::size_t askCount = 0u;
  const bool snapshot =
      action == cxet::StringView("snapshot", 8u) ||
      state.family == FeedFamily::SnapshotDepth ||
      state.family == FeedFamily::DirectBbo;
  if (!detail::parse_levels(
          bids, state.scratchBids.data(), state.scratchBids.size(),
          &bidCount, !snapshot) ||
      !detail::parse_levels(
          asks, state.scratchAsks.data(), state.scratchAsks.size(),
          &askCount, !snapshot)) {
    state.book.invalidate(BookValidity::InvalidGap);
    return false;
  }
  if (state.family == FeedFamily::DirectBbo) {
    return bidCount == 1u && askCount == 1u &&
           detail::emit_bbo(
               state, output,
               {state.scratchBids[0].price,
                state.scratchBids[0].quantity,
                state.scratchAsks[0].price,
                state.scratchAsks[0].quantity},
               sequence, exchangeNs, RecordOrigin::Raw);
  }
  if (snapshot) {
    return detail::emit_snapshot(
        state, output, bidCount, askCount, sequence, exchangeNs);
  }
  if (action != cxet::StringView("update", 6u)) return false;
  if (state.book.validity() != BookValidity::Valid) {
    output.status = FeedStatus::Synchronizing;
    return true;
  }
  if (!detail::slot_u64(fields[4], &previous) || previous == 0u)
    return false;
  std::size_t mutationCount = 0u;
  for (std::size_t index = 0u; index < bidCount; ++index)
    state.scratchMutations[mutationCount++] =
        {BookSide::Bid, state.scratchBids[index]};
  for (std::size_t index = 0u; index < askCount; ++index)
    state.scratchMutations[mutationCount++] =
        {BookSide::Ask, state.scratchAsks[index]};
  return mutationCount != 0u &&
         detail::emit_delta(
             state, output, mutationCount,
             {sequence, sequence, previous, true}, exchangeNs);
}

[[nodiscard]] bool normalize_trades(
    VenueNormalizerState& state, hot::View data,
    cxet::StringView channel, NormalizeBatch& output) noexcept {
  const bool individual = channel == cxet::StringView("trades-all", 10u);
  std::size_t position = 1u;
  hot::View object{};
  const std::size_t first = output.count;
  while (hot::nextArrayItem(data, &position, &object)) {
    hot::FieldSlot fields[] = {
        {"instId", 6u}, {"tradeId", 7u}, {"px", 2u},
        {"sz", 2u}, {"side", 4u}, {"ts", 2u}, {"count", 5u}};
    char priceScratch[64]{};
    char quantityScratch[64]{};
    cxet::StringView price{};
    cxet::StringView quantity{};
    std::uint64_t tradeId = 0u;
    std::uint64_t timestampMs = 0u;
    std::uint64_t count = 1u;
    RaceRecord record{};
    if (!hot::collectObjectFieldsAllUnique(object, fields) ||
        !symbol_matches(state, fields[0]) ||
        !detail::slot_u64(fields[1], &tradeId) || tradeId == 0u ||
        !detail::slot_text(
            fields[2], priceScratch, sizeof(priceScratch), &price) ||
        !detail::slot_text(
            fields[3], quantityScratch, sizeof(quantityScratch), &quantity) ||
        !detail::parse_price(price, &record.changedLevels[0].price) ||
        !detail::parse_amount(quantity, &record.changedLevels[0].quantity) ||
        !detail::slot_u64(fields[5], &timestampMs) ||
        timestampMs > std::numeric_limits<std::uint64_t>::max() /
                          numeric::kNanosecondsPerMillisecond) {
      return false;
    }
    if (fields[6].found &&
        (!detail::slot_u64(fields[6], &count) || count == 0u ||
         count > std::numeric_limits<std::uint32_t>::max())) {
      return false;
    }
    if (hot::slotStringEquals(fields[4], "buy"))
      record.trade.aggressorSide = 1u;
    else if (hot::slotStringEquals(fields[4], "sell"))
      record.trade.aggressorSide = 2u;
    else
      return false;
    record.eventClass = EventClass::Trade;
    record.validity = BookValidity::Valid;
    record.eventOrdinal = ++state.eventOrdinal;
    record.timestamps.exchangeEventNs =
        timestampMs * numeric::kNanosecondsPerMillisecond;
    record.changedLevelCount = 1u;
    record.changedLevels[0].side = record.trade.aggressorSide;
    if (individual || count == 1u) {
      record.trade.tradeId = tradeId;
      record.trade.nativeFirst = tradeId;
      record.trade.nativeShape = 1u;
    } else {
      // OKX trades can aggregate maker fills.  Without a documented native
      // first/last range, preserve only exact aggregate identity and never
      // claim one-to-one equality with trades-all.
      record.trade.aggregateId = tradeId;
      record.trade.count = static_cast<std::uint32_t>(count);
    }
    if (!detail::append_record(output, record)) return false;
  }
  if (!hot::arrayIterationComplete(data, position) || output.count == first)
    return false;
  const auto batch = static_cast<std::uint16_t>(output.count - first);
  for (std::size_t index = first; index < output.count; ++index)
    output.records[index].frameBatchCount = batch;
  return true;
}

}  // namespace

bool normalize_okx(
    void* rawState, const FrameView& frame,
    NormalizeBatch& output) noexcept {
  output = NormalizeBatch{};
  auto* state = static_cast<VenueNormalizerState*>(rawState);
  if (!state || state->venue != Venue::Okx || frame.binary || !frame.data ||
      frame.size == 0u) {
    output.malformed = true;
    return false;
  }
  const auto root = detail::json_view(frame);
  bool control = false;
  if (!classify_control(*state, root, output, &control)) {
    output.malformed = true;
    return false;
  }
  if (control) return true;
  hot::View data{};
  cxet::StringView channel{};
  cxet::StringView action{};
  if (!data_context(*state, root, &data, &channel, &action)) {
    output.malformed = true;
    return false;
  }
  const bool tradeChannel =
      channel == cxet::StringView("trades", 6u) ||
      channel == cxet::StringView("trades-all", 10u);
  const bool ok = state->family == FeedFamily::Trade
                      ? tradeChannel &&
                            normalize_trades(*state, data, channel, output)
                      : !tradeChannel &&
                            normalize_book(*state, data, action, output);
  if (ok) {
    state->subscriptionAccepted = true;
    output.status = state->family == FeedFamily::IncrementalDepth &&
                            state->book.validity() != BookValidity::Valid
                        ? FeedStatus::Synchronizing
                        : FeedStatus::Ready;
  }
  output.malformed = !ok;
  return ok;
}

}  // namespace exchange_probe::race
