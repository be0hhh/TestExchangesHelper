#pragma once

#include "exchange_probe/race/normalizers.hpp"

#include "race_normalizer_common.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>

namespace exchange_probe::race::binance_like_detail {

namespace hot = cxet::parse::hotjson;
namespace detail = normalizer_detail;

[[nodiscard]] inline bool symbol_matches(
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

[[nodiscard]] inline bool milliseconds_ns(
    std::uint64_t value, std::uint64_t* output) noexcept {
  if (!output || value == 0u ||
      value > std::numeric_limits<std::uint64_t>::max() /
                  numeric::kNanosecondsPerMillisecond) {
    return false;
  }
  *output = value * numeric::kNanosecondsPerMillisecond;
  return true;
}

[[nodiscard]] inline bool classify_control(
    VenueNormalizerState& state, hot::View root,
    NormalizeBatch& output, bool* classified) noexcept {
  *classified = false;
  hot::FieldSlot fields[] = {
      {"result", 6u}, {"id", 2u}, {"code", 4u}, {"msg", 3u}};
  if (!hot::collectObjectFieldsAllUnique(root, fields)) return false;
  if (!fields[1].found || (!fields[0].found && !fields[2].found)) return true;
  *classified = true;
  if (fields[2].found) {
    output.status = FeedStatus::Rejected;
    return true;
  }
  const auto result = hot::trim(fields[0].value);
  if (result.size != 4u ||
      cxet::bytescan::hftMemcmp(result.data, "null", 4u) != 0) {
    output.status = FeedStatus::Rejected;
    return true;
  }
  state.subscriptionAccepted = true;
  output.status = state.family == FeedFamily::IncrementalDepth
                      ? FeedStatus::Synchronizing
                      : FeedStatus::Ready;
  return true;
}

[[nodiscard]] inline bool unwrap(hot::View root, hot::View* payload) noexcept {
  hot::FieldSlot fields[] = {{"stream", 6u}, {"data", 4u}};
  if (!hot::collectObjectFieldsAllUnique(root, fields)) return false;
  if (!fields[0].found && !fields[1].found) {
    *payload = root;
    return true;
  }
  return fields[0].found && hot::slotObject(fields[1], payload);
}

[[nodiscard]] inline bool bbo(
    VenueNormalizerState& state, hot::View object,
    NormalizeBatch& output) noexcept {
  hot::FieldSlot fields[] = {
      {"e", 1u}, {"s", 1u}, {"u", 1u}, {"b", 1u}, {"B", 1u},
      {"a", 1u}, {"A", 1u}, {"T", 1u}, {"E", 1u}};
  char bidPriceScratch[64]{};
  char bidAmountScratch[64]{};
  char askPriceScratch[64]{};
  char askAmountScratch[64]{};
  cxet::StringView bidPrice{};
  cxet::StringView bidAmount{};
  cxet::StringView askPrice{};
  cxet::StringView askAmount{};
  std::uint64_t sequence = 0u;
  std::uint64_t transactionMs = 0u;
  std::uint64_t eventMs = 0u;
  BboState stateValue{};
  if (!hot::collectObjectFieldsAllUnique(object, fields) ||
      !hot::slotStringEquals(fields[0], "bookTicker") ||
      !symbol_matches(state, fields[1]) ||
      !detail::slot_u64(fields[2], &sequence) || sequence == 0u ||
      !detail::slot_text(
          fields[3], bidPriceScratch, sizeof(bidPriceScratch), &bidPrice) ||
      !detail::slot_text(
          fields[4], bidAmountScratch, sizeof(bidAmountScratch), &bidAmount) ||
      !detail::slot_text(
          fields[5], askPriceScratch, sizeof(askPriceScratch), &askPrice) ||
      !detail::slot_text(
          fields[6], askAmountScratch, sizeof(askAmountScratch), &askAmount) ||
      !detail::parse_price(bidPrice, &stateValue.bidPrice) ||
      !detail::parse_amount(bidAmount, &stateValue.bidQuantity) ||
      !detail::parse_price(askPrice, &stateValue.askPrice) ||
      !detail::parse_amount(askAmount, &stateValue.askQuantity) ||
      !detail::slot_u64(fields[7], &transactionMs) ||
      !detail::slot_u64(fields[8], &eventMs)) {
    return false;
  }
  std::uint64_t exchangeNs = 0u;
  std::uint64_t streamNs = 0u;
  const std::size_t first = output.count;
  if (!milliseconds_ns(transactionMs, &exchangeNs) ||
      !milliseconds_ns(eventMs, &streamNs) ||
      !detail::emit_bbo(
          state, output, stateValue, sequence, exchangeNs,
          RecordOrigin::Raw)) {
    return false;
  }
  for (std::size_t index = first; index < output.count; ++index)
    output.records[index].timestamps.streamServiceNs = streamNs;
  return true;
}

[[nodiscard]] inline bool depth(
    VenueNormalizerState& state, hot::View object,
    NormalizeBatch& output) noexcept {
  hot::FieldSlot fields[] = {
      {"e", 1u}, {"s", 1u}, {"U", 1u}, {"u", 1u}, {"pu", 2u},
      {"b", 1u}, {"a", 1u}, {"E", 1u}, {"T", 1u},
      {"lastUpdateId", 12u}, {"bids", 4u}, {"asks", 4u}};
  if (!hot::collectObjectFieldsAllUnique(object, fields)) return false;
  const bool restSnapshot = fields[9].found;
  if (!restSnapshot &&
      (!hot::slotStringEquals(fields[0], "depthUpdate") ||
       !symbol_matches(state, fields[1]))) {
    return false;
  }
  hot::View bids{};
  hot::View asks{};
  std::uint64_t firstSequence = 0u;
  std::uint64_t lastSequence = 0u;
  std::uint64_t previous = 0u;
  std::uint64_t eventMs = 0u;
  std::uint64_t transactionMs = 0u;
  if (restSnapshot) {
    if (!detail::slot_u64(fields[9], &lastSequence) || lastSequence == 0u ||
        !hot::slotArray(fields[10], &bids) ||
        !hot::slotArray(fields[11], &asks)) {
      return false;
    }
    firstSequence = lastSequence;
  } else if (!detail::slot_u64(fields[2], &firstSequence) ||
             !detail::slot_u64(fields[3], &lastSequence) ||
             firstSequence == 0u || lastSequence < firstSequence ||
             !detail::slot_u64(fields[4], &previous) ||
             !hot::slotArray(fields[5], &bids) ||
             !hot::slotArray(fields[6], &asks) ||
             !detail::slot_u64(fields[7], &eventMs) ||
             !detail::slot_u64(fields[8], &transactionMs)) {
    return false;
  }
  const bool snapshot = restSnapshot ||
                        state.family == FeedFamily::SnapshotDepth;
  std::size_t bidCount = 0u;
  std::size_t askCount = 0u;
  if (!detail::parse_levels(
          bids, state.scratchBids.data(), state.scratchBids.size(),
          &bidCount, !snapshot) ||
      !detail::parse_levels(
          asks, state.scratchAsks.data(), state.scratchAsks.size(),
          &askCount, !snapshot)) {
    state.book.invalidate(BookValidity::InvalidGap);
    return false;
  }
  std::uint64_t exchangeNs = 0u;
  std::uint64_t streamNs = 0u;
  if (!restSnapshot &&
      (!milliseconds_ns(transactionMs, &exchangeNs) ||
       !milliseconds_ns(eventMs, &streamNs))) {
    return false;
  }
  const std::size_t firstOutput = output.count;
  bool ok = false;
  if (snapshot) {
    ok = detail::emit_snapshot(
        state, output, bidCount, askCount, lastSequence, exchangeNs);
  } else if (state.book.validity() != BookValidity::Valid) {
    output.status = FeedStatus::Synchronizing;
    return true;
  } else {
    std::size_t mutationCount = 0u;
    for (std::size_t index = 0u; index < bidCount; ++index)
      state.scratchMutations[mutationCount++] =
          {BookSide::Bid, state.scratchBids[index]};
    for (std::size_t index = 0u; index < askCount; ++index)
      state.scratchMutations[mutationCount++] =
          {BookSide::Ask, state.scratchAsks[index]};
    ok = mutationCount != 0u &&
         detail::emit_delta(
             state, output, mutationCount,
             {firstSequence, lastSequence, previous, true}, exchangeNs);
  }
  if (!ok) return false;
  for (std::size_t index = firstOutput; index < output.count; ++index)
    output.records[index].timestamps.streamServiceNs = streamNs;
  return true;
}

[[nodiscard]] inline bool trade(
    VenueNormalizerState& state, hot::View object,
    NormalizeBatch& output) noexcept {
  hot::FieldSlot fields[] = {
      {"e", 1u}, {"s", 1u}, {"a", 1u}, {"t", 1u}, {"p", 1u},
      {"q", 1u}, {"f", 1u}, {"l", 1u}, {"T", 1u}, {"E", 1u},
      {"m", 1u}};
  if (!hot::collectObjectFieldsAllUnique(object, fields) ||
      !symbol_matches(state, fields[1])) {
    return false;
  }
  const bool aggregate = hot::slotStringEquals(fields[0], "aggTrade");
  const bool single = hot::slotStringEquals(fields[0], "trade");
  if (!aggregate && !single) return false;
  char priceScratch[64]{};
  char quantityScratch[64]{};
  cxet::StringView price{};
  cxet::StringView quantity{};
  std::uint64_t identifier = 0u;
  std::uint64_t transactionMs = 0u;
  std::uint64_t eventMs = 0u;
  bool buyerMaker = false;
  RaceRecord record{};
  const auto& identityField = aggregate ? fields[2] : fields[3];
  if (!detail::slot_u64(identityField, &identifier) || identifier == 0u ||
      !detail::slot_text(
          fields[4], priceScratch, sizeof(priceScratch), &price) ||
      !detail::slot_text(
          fields[5], quantityScratch, sizeof(quantityScratch), &quantity) ||
      !detail::parse_price(price, &record.changedLevels[0].price) ||
      !detail::parse_amount(quantity, &record.changedLevels[0].quantity) ||
      !detail::slot_u64(fields[8], &transactionMs) ||
      !detail::slot_u64(fields[9], &eventMs) ||
      !hot::slotBool(fields[10], &buyerMaker) ||
      !milliseconds_ns(transactionMs, &record.timestamps.exchangeEventNs) ||
      !milliseconds_ns(eventMs, &record.timestamps.streamServiceNs)) {
    return false;
  }
  record.eventClass = EventClass::Trade;
  record.validity = BookValidity::Valid;
  record.eventOrdinal = ++state.eventOrdinal;
  record.frameBatchCount = 1u;
  record.trade.aggressorSide = buyerMaker ? 2u : 1u;
  record.changedLevelCount = 1u;
  record.changedLevels[0].side = record.trade.aggressorSide;
  if (single) {
    record.trade.tradeId = identifier;
    record.trade.nativeFirst = identifier;
    record.trade.nativeShape = 1u;
  } else {
    std::uint64_t firstTrade = 0u;
    std::uint64_t lastTrade = 0u;
    if (!detail::slot_u64(fields[6], &firstTrade) ||
        !detail::slot_u64(fields[7], &lastTrade) || firstTrade == 0u ||
        lastTrade < firstTrade ||
        lastTrade - firstTrade >=
            std::numeric_limits<std::uint32_t>::max()) {
      return false;
    }
    record.trade.aggregateId = identifier;
    record.trade.firstTradeId = firstTrade;
    record.trade.lastTradeId = lastTrade;
    record.trade.count =
        static_cast<std::uint32_t>(lastTrade - firstTrade + 1u);
  }
  return detail::append_record(output, record);
}

[[nodiscard]] inline bool normalize(
    VenueNormalizerState& state, const FrameView& frame,
    NormalizeBatch& output) noexcept {
  const auto root = detail::json_view(frame);
  bool control = false;
  if (!classify_control(state, root, output, &control)) return false;
  if (control) return true;
  hot::View payload{};
  if (!unwrap(root, &payload)) return false;
  bool ok = false;
  if (state.family == FeedFamily::DirectBbo)
    ok = bbo(state, payload, output);
  else if (state.family == FeedFamily::Trade)
    ok = trade(state, payload, output);
  else
    ok = depth(state, payload, output);
  if (ok) {
    state.subscriptionAccepted = true;
    output.status = state.family == FeedFamily::IncrementalDepth &&
                            state.book.validity() != BookValidity::Valid
                        ? FeedStatus::Synchronizing
                        : FeedStatus::Ready;
  }
  return ok;
}

}  // namespace exchange_probe::race::binance_like_detail
