#include "exchange_probe/race/Normalizers.hpp"

#include "RaceNormalizerCommon.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>

namespace exchange_probe::race {
namespace {

namespace hot = cxet::parse::hotjson;
namespace detail = normalizer_detail;

[[nodiscard]] bool timestamp_ns(
    std::uint64_t value, std::uint64_t* output) noexcept {
  if (!output || value == 0u) return false;
  constexpr std::uint64_t kNsFloor = 1'000'000'000'000'000'000ull;
  constexpr std::uint64_t kUsFloor = 1'000'000'000'000'000ull;
  constexpr std::uint64_t kMsFloor = 1'000'000'000'000ull;
  const std::uint64_t multiplier =
      value >= kNsFloor
          ? 1u
          : value >= kUsFloor
                ? numeric::kNanosecondsPerMicrosecond
                : value >= kMsFloor
                      ? numeric::kNanosecondsPerMillisecond
                      : numeric::kNanosecondsPerSecond;
  if (value > std::numeric_limits<std::uint64_t>::max() / multiplier)
    return false;
  *output = value * multiplier;
  return true;
}

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
      {"id", 2u}, {"result", 6u}, {"type", 4u}, {"code", 4u}};
  if (!hot::collectObjectFieldsAllUnique(root, fields)) return false;
  if (fields[2].found && hot::slotStringEquals(fields[2], "pong")) {
    *classified = true;
    output.status = state.subscriptionAccepted ? FeedStatus::Ready
                                               : FeedStatus::Synchronizing;
    return true;
  }
  if (!fields[0].found || !fields[1].found) return true;
  *classified = true;
  bool accepted = false;
  if (!hot::slotBool(fields[1], &accepted)) return false;
  state.subscriptionAccepted = accepted;
  output.status = !accepted
                      ? FeedStatus::Rejected
                      : state.family == FeedFamily::IncrementalDepth
                            ? FeedStatus::Synchronizing
                            : FeedStatus::Ready;
  return true;
}

[[nodiscard]] bool frame_context(
    const VenueNormalizerState& state, hot::View root,
    hot::View* data, cxet::StringView* topic,
    cxet::StringView* updateType, cxet::StringView* depth,
    std::uint64_t* publishNs) noexcept {
  hot::FieldSlot rootFields[] = {
      {"T", 1u}, {"d", 1u}, {"t", 1u}, {"dp", 2u}, {"P", 1u}};
  if (!hot::collectObjectFieldsAllUnique(root, rootFields) ||
      !hot::slotObject(rootFields[1], data)) {
    return false;
  }
  static thread_local char topicScratch[96]{};
  static thread_local char typeScratch[20]{};
  static thread_local char depthScratch[24]{};
  if (!detail::slot_text(
          rootFields[0], topicScratch, sizeof(topicScratch), topic)) {
    return false;
  }
  *updateType = {};
  *depth = {};
  if (rootFields[2].found &&
      !detail::slot_text(
          rootFields[2], typeScratch, sizeof(typeScratch), updateType)) {
    return false;
  }
  if (rootFields[3].found &&
      !detail::slot_text(
          rootFields[3], depthScratch, sizeof(depthScratch), depth)) {
    return false;
  }
  *publishNs = 0u;
  if (rootFields[4].found) {
    std::uint64_t value = 0u;
    if (!detail::slot_u64(rootFields[4], &value) ||
        !timestamp_ns(value, publishNs)) {
      return false;
    }
  }
  hot::FieldSlot dataFields[] = {{"s", 1u}};
  return hot::collectObjectFieldsAllUnique(*data, dataFields) &&
         symbol_matches(state, dataFields[0]);
}

[[nodiscard]] bool normalize_book(
    VenueNormalizerState& state, hot::View data,
    cxet::StringView updateType, std::uint64_t publishNs,
    NormalizeBatch& output) noexcept {
  hot::FieldSlot fields[] = {
      {"C", 1u}, {"U", 1u}, {"b", 1u}, {"a", 1u}, {"M", 1u}};
  hot::View bids{};
  hot::View asks{};
  std::uint64_t sequence = 0u;
  std::uint64_t firstSequence = 0u;
  std::uint64_t timestamp = 0u;
  std::uint64_t exchangeNs = 0u;
  if (!hot::collectObjectFieldsAllUnique(data, fields) ||
      !detail::slot_u64(fields[0], &sequence) || sequence == 0u ||
      !hot::slotArray(fields[2], &bids) ||
      !hot::slotArray(fields[3], &asks) ||
      !detail::slot_u64(fields[4], &timestamp) ||
      !timestamp_ns(timestamp, &exchangeNs)) {
    return false;
  }
  if (!detail::slot_u64(fields[1], &firstSequence)) firstSequence = sequence;
  const bool snapshot =
      updateType == cxet::StringView("snapshot", 8u) ||
      state.family == FeedFamily::DirectBbo ||
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
  const std::size_t firstOutput = output.count;
  bool ok = false;
  if (state.family == FeedFamily::DirectBbo) {
    ok = bidCount == 1u && askCount == 1u &&
         detail::emit_bbo(
             state, output,
             {state.scratchBids[0].price,
              state.scratchBids[0].quantity,
              state.scratchAsks[0].price,
              state.scratchAsks[0].quantity},
             sequence, exchangeNs, RecordOrigin::Raw);
  } else if (snapshot) {
    ok = detail::emit_snapshot(
        state, output, bidCount, askCount, sequence, exchangeNs);
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
             {firstSequence, sequence, 0u, false}, exchangeNs);
  }
  if (!ok) return false;
  for (std::size_t index = firstOutput; index < output.count; ++index)
    output.records[index].timestamps.streamServiceNs = publishNs;
  return true;
}

[[nodiscard]] bool normalize_trade(
    VenueNormalizerState& state, hot::View data,
    std::uint64_t publishNs, NormalizeBatch& output) noexcept {
  hot::FieldSlot fields[] = {
      {"ti", 2u}, {"p", 1u}, {"q", 1u}, {"S", 1u}};
  char priceScratch[64]{};
  char quantityScratch[64]{};
  cxet::StringView price{};
  cxet::StringView quantity{};
  std::uint64_t tradeId = 0u;
  RaceRecord record{};
  if (!hot::collectObjectFieldsAllUnique(data, fields) ||
      !detail::slot_u64(fields[0], &tradeId) || tradeId == 0u ||
      !detail::slot_text(
          fields[1], priceScratch, sizeof(priceScratch), &price) ||
      !detail::slot_text(
          fields[2], quantityScratch, sizeof(quantityScratch), &quantity) ||
      !detail::parse_price(price, &record.changedLevels[0].price) ||
      !detail::parse_amount(quantity, &record.changedLevels[0].quantity) ||
      publishNs == 0u) {
    return false;
  }
  std::uint8_t observedSide = 0u;
  if (hot::slotStringEquals(fields[3], "buy") ||
      hot::slotStringEquals(fields[3], "BUY"))
    observedSide = 1u;
  else if (hot::slotStringEquals(fields[3], "sell") ||
           hot::slotStringEquals(fields[3], "SELL"))
    observedSide = 2u;
  else
    return false;
  record.eventClass = EventClass::Trade;
  record.validity = BookValidity::Valid;
  record.eventOrdinal = ++state.eventOrdinal;
  record.frameBatchCount = 1u;
  record.trade.tradeId = tradeId;
  record.trade.nativeFirst = tradeId;
  record.trade.nativeShape = 1u;
  // KuCoin UTA S is an observed binary trade side, but the project contract
  // does not prove it as maker/taker initiator evidence.
  record.trade.aggressorSide = 0u;
  record.timestamps.exchangeEventNs = publishNs;
  record.changedLevelCount = 1u;
  record.changedLevels[0].side = observedSide;
  return detail::append_record(output, record);
}

}  // namespace

bool normalize_kucoin(
    void* rawState, const FrameView& frame,
    NormalizeBatch& output) noexcept {
  output = NormalizeBatch{};
  auto* state = static_cast<VenueNormalizerState*>(rawState);
  if (!state || state->venue != Venue::Kucoin || !frame.data ||
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
  cxet::StringView topic{};
  cxet::StringView updateType{};
  cxet::StringView depth{};
  std::uint64_t publishNs = 0u;
  if (!frame_context(
          *state, root, &data, &topic, &updateType, &depth, &publishNs)) {
    output.malformed = true;
    return false;
  }
  const bool tradeTopic =
      topic == cxet::StringView("trade.FUTURES", 13u);
  const bool bookTopic =
      topic == cxet::StringView("obu.FUTURES", 11u);
  const bool ok = state->family == FeedFamily::Trade
                      ? tradeTopic &&
                            normalize_trade(*state, data, publishNs, output)
                      : bookTopic && normalize_book(
                                         *state, data, updateType,
                                         publishNs, output);
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
