#include "exchange_probe/race/Normalizers.hpp"

#include "RaceNormalizerCommon.hpp"

#include "cxet/Parse/Sbe/SbeBufferView.hpp"
#include "cxet/Parse/Sbe/SbeCursor.hpp"
#include "cxet/Parse/Sbe/SbeScale.hpp"
#include "cxet/Parse/Sbe/SbeTypes.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>

namespace exchange_probe::race {
namespace {

namespace hot = cxet::parse::hotjson;
namespace sbe = cxet::parse::sbe;
namespace detail = normalizer_detail;

constexpr std::uint16_t kSchemaId = 1u;
constexpr std::uint16_t kDepthTemplate = 1001u;
constexpr std::uint16_t kBboTemplate = 1002u;
constexpr std::uint16_t kTradeTemplate = 1003u;

void stamp_schema(
    NormalizeBatch& output, const sbe::SbeMessageHeader& header) noexcept {
  for (std::size_t index = 0u; index < output.count; ++index) {
    output.records[index].wireSchema = {
        header.blockLength, header.templateId, header.schemaId,
        header.schemaVersion};
  }
}

[[nodiscard]] bool symbol_matches(
    const VenueNormalizerState& state, cxet::StringView symbol) noexcept {
  return state.nativeSymbol.size == 0u ||
         (symbol.size() == state.nativeSymbol.size &&
          cxet::bytescan::hftMemcmp(
              symbol.data(), state.nativeSymbol.bytes.data(), symbol.size()) ==
              0);
}

[[nodiscard]] bool read_symbol(
    sbe::SbeCursor& cursor, const VenueNormalizerState& state) noexcept {
  std::uint8_t size = 0u;
  if (!cursor.readU8(&size) || size == 0u || !cursor.canRead(size)) return false;
  const cxet::StringView symbol{
      reinterpret_cast<const char*>(cursor.ptr + cursor.offset()), size};
  return symbol_matches(state, symbol) && cursor.skip(size);
}

[[nodiscard]] bool microseconds_ns(
    std::uint64_t value, std::uint64_t* output) noexcept {
  if (!output || value == 0u ||
      value > std::numeric_limits<std::uint64_t>::max() /
                  numeric::kNanosecondsPerMicrosecond) {
    return false;
  }
  *output = value * numeric::kNanosecondsPerMicrosecond;
  return true;
}

[[nodiscard]] bool json_ack(
    VenueNormalizerState& state, hot::View root,
    NormalizeBatch& output, bool* classified) noexcept {
  *classified = false;
  hot::FieldSlot fields[] = {
      {"event", 5u}, {"code", 4u}, {"msg", 3u}};
  if (!hot::collectObjectFieldsAllUnique(root, fields)) return false;
  if (!fields[0].found) return true;
  *classified = true;
  if (!hot::slotStringEquals(fields[0], "subscribe")) {
    output.status = state.subscriptionAccepted ? FeedStatus::Ready
                                               : FeedStatus::Synchronizing;
    return true;
  }
  if (fields[1].found || fields[2].found) {
    output.status = FeedStatus::Rejected;
    return true;
  }
  state.subscriptionAccepted = true;
  output.status = state.family == FeedFamily::Trade ||
                          state.family == FeedFamily::DirectBbo
                      ? FeedStatus::Ready
                      : FeedStatus::Synchronizing;
  return true;
}

[[nodiscard]] bool json_context(
    const VenueNormalizerState& state, hot::View root,
    hot::View* data, cxet::StringView* action,
    std::uint64_t* streamTimestampMs) noexcept {
  hot::FieldSlot rootFields[] = {
      {"action", 6u}, {"arg", 3u}, {"data", 4u}, {"ts", 2u}};
  if (!hot::collectObjectFieldsAllUnique(root, rootFields) ||
      !hot::slotArray(rootFields[2], data) ||
      !detail::slot_u64(rootFields[3], streamTimestampMs)) {
    return false;
  }
  static thread_local char actionScratch[16]{};
  if (!detail::slot_text(
          rootFields[0], actionScratch, sizeof(actionScratch), action)) {
    return false;
  }
  hot::View argument{};
  if (!hot::slotObject(rootFields[1], &argument)) return false;
  hot::FieldSlot argumentFields[] = {
      {"symbol", 6u}, {"topic", 5u}};
  if (!hot::collectObjectFieldsAllUnique(argument, argumentFields)) return false;
  char symbolScratch[64]{};
  cxet::StringView symbol{};
  return detail::slot_text(
             argumentFields[0], symbolScratch, sizeof(symbolScratch),
             &symbol) &&
         symbol_matches(state, symbol);
}

[[nodiscard]] bool json_book(
    VenueNormalizerState& state, hot::View root,
    NormalizeBatch& output) noexcept {
  hot::View data{};
  cxet::StringView action{};
  std::uint64_t streamMs = 0u;
  if (!json_context(state, root, &data, &action, &streamMs)) return false;
  std::size_t position = 1u;
  hot::View object{};
  if (!hot::nextArrayItem(data, &position, &object) ||
      !hot::arrayIterationComplete(data, position)) {
    return false;
  }
  hot::FieldSlot fields[] = {
      {"b", 1u}, {"a", 1u}, {"seq", 3u}, {"pseq", 4u}, {"ts", 2u}};
  if (!hot::collectObjectFieldsAllUnique(object, fields)) return false;
  hot::View bids{};
  hot::View asks{};
  std::uint64_t sequence = 0u;
  std::uint64_t previous = 0u;
  std::uint64_t exchangeMs = 0u;
  if (!hot::slotArray(fields[0], &bids) ||
      !hot::slotArray(fields[1], &asks) ||
      !detail::slot_u64(fields[2], &sequence) || sequence == 0u ||
      !detail::slot_u64(fields[4], &exchangeMs)) {
    return false;
  }
  const bool snapshot = action == cxet::StringView("snapshot", 8u);
  const bool update = action == cxet::StringView("update", 6u);
  if (!snapshot && !update) return false;
  const std::uint64_t exchangeNs =
      exchangeMs * numeric::kNanosecondsPerMillisecond;
  const std::uint64_t streamNs =
      streamMs * numeric::kNanosecondsPerMillisecond;
  const std::size_t firstOutput = output.count;
  std::size_t bidCount = 0u;
  std::size_t askCount = 0u;
  if (!detail::parse_levels(
          bids, state.scratchBids.data(), state.scratchBids.size(), &bidCount,
          update) ||
      !detail::parse_levels(
          asks, state.scratchAsks.data(), state.scratchAsks.size(), &askCount,
          update)) {
    state.book.invalidate(BookValidity::InvalidGap);
    return false;
  }
  if (state.family == FeedFamily::DirectBbo) {
    if (!snapshot || bidCount != 1u || askCount != 1u ||
        !detail::emit_bbo(
            state, output,
            BboState{state.scratchBids[0].price,
                     state.scratchBids[0].quantity,
                     state.scratchAsks[0].price,
                     state.scratchAsks[0].quantity},
            sequence, exchangeNs, RecordOrigin::Raw)) {
      return false;
    }
  } else if (snapshot) {
    if (!detail::emit_snapshot(
            state, output, bidCount, askCount, sequence, exchangeNs)) {
      return false;
    }
  } else {
    if (state.book.validity() != BookValidity::Valid ||
        !detail::slot_u64(fields[3], &previous) || previous == 0u) {
      output.status = FeedStatus::Synchronizing;
      return true;
    }
    std::size_t mutationCount = 0u;
    for (std::size_t index = 0u; index < bidCount; ++index)
      state.scratchMutations[mutationCount++] =
          {BookSide::Bid, state.scratchBids[index]};
    for (std::size_t index = 0u; index < askCount; ++index)
      state.scratchMutations[mutationCount++] =
          {BookSide::Ask, state.scratchAsks[index]};
    if (mutationCount == 0u ||
        !detail::emit_delta(
            state, output, mutationCount,
            SequenceUpdate{sequence, sequence, previous, true}, exchangeNs)) {
      return false;
    }
  }
  for (std::size_t index = firstOutput; index < output.count; ++index)
    output.records[index].timestamps.streamServiceNs = streamNs;
  state.subscriptionAccepted = true;
  output.status = FeedStatus::Ready;
  return true;
}

[[nodiscard]] bool json_trade(
    VenueNormalizerState& state, hot::View root,
    NormalizeBatch& output) noexcept {
  hot::View data{};
  cxet::StringView action{};
  std::uint64_t streamMs = 0u;
  if (!json_context(state, root, &data, &action, &streamMs)) return false;
  std::size_t position = 1u;
  hot::View object{};
  const std::size_t first = output.count;
  while (hot::nextArrayItem(data, &position, &object)) {
    hot::FieldSlot fields[] = {
        {"p", 1u}, {"v", 1u}, {"S", 1u}, {"T", 1u},
        {"ts", 2u}, {"i", 1u}};
    if (!hot::collectObjectFieldsAllUnique(object, fields)) return false;
    char priceScratch[64]{};
    char amountScratch[64]{};
    cxet::StringView price{};
    cxet::StringView amount{};
    std::uint64_t timestampMs = 0u;
    std::uint64_t executionId = 0u;
    RaceRecord record{};
    if (!detail::slot_text(
            fields[0], priceScratch, sizeof(priceScratch), &price) ||
        !detail::slot_text(
            fields[1], amountScratch, sizeof(amountScratch), &amount) ||
        !detail::parse_price(price, &record.changedLevels[0].price) ||
        !detail::parse_amount(amount, &record.changedLevels[0].quantity) ||
        !(detail::slot_u64(fields[3], &timestampMs) ||
          detail::slot_u64(fields[4], &timestampMs)) ||
        !detail::slot_u64(fields[5], &executionId) || executionId == 0u) {
      return false;
    }
    if (hot::slotStringEquals(fields[2], "buy"))
      record.trade.aggressorSide = 1u;
    else if (hot::slotStringEquals(fields[2], "sell"))
      record.trade.aggressorSide = 2u;
    else
      return false;
    record.eventClass = EventClass::Trade;
    record.validity = BookValidity::Valid;
    record.eventOrdinal = ++state.eventOrdinal;
    record.trade.tradeId = executionId;
    record.trade.nativeFirst = executionId;
    record.trade.nativeShape = 1u;
    record.timestamps.exchangeEventNs =
        timestampMs * numeric::kNanosecondsPerMillisecond;
    record.timestamps.streamServiceNs =
        streamMs * numeric::kNanosecondsPerMillisecond;
    record.changedLevelCount = 1u;
    record.changedLevels[0].side = record.trade.aggressorSide;
    if (!detail::append_record(output, record)) return false;
  }
  if (!hot::arrayIterationComplete(data, position) || output.count == first)
    return false;
  const auto count = static_cast<std::uint16_t>(output.count - first);
  for (std::size_t index = first; index < output.count; ++index)
    output.records[index].frameBatchCount = count;
  state.subscriptionAccepted = true;
  output.status = FeedStatus::Ready;
  return true;
}

[[nodiscard]] bool sbe_bbo(
    VenueNormalizerState& state, const FrameView& frame,
    NormalizeBatch& output) noexcept {
  sbe::SbeCursor cursor{{frame.data, frame.size}};
  sbe::SbeMessageHeader header{};
  std::uint64_t timestampUs = 0u;
  std::int64_t bidPriceMantissa = 0;
  std::int64_t bidSizeMantissa = 0;
  std::int64_t askPriceMantissa = 0;
  std::int64_t askSizeMantissa = 0;
  std::int8_t priceExponent = 0;
  std::int8_t sizeExponent = 0;
  std::uint64_t sequence = 0u;
  std::uint64_t streamTimestampUs = 0u;
  if (!sbe::readMessageHeader(cursor, &header) ||
      header.schemaId != kSchemaId || header.templateId != kBboTemplate ||
      header.blockLength < 50u || !cursor.readU64(&timestampUs) ||
      !cursor.readI64(&bidPriceMantissa) ||
      !cursor.readI64(&bidSizeMantissa) ||
      !cursor.readI64(&askPriceMantissa) ||
      !cursor.readI64(&askSizeMantissa) || !cursor.readI8(&priceExponent) ||
      !cursor.readI8(&sizeExponent) || !cursor.readU64(&sequence) ||
      sequence == 0u) {
    return false;
  }
  std::size_t consumedRoot = 50u;
  if (header.blockLength >= 58u) {
    if (!cursor.readU64(&streamTimestampUs)) return false;
    consumedRoot = 58u;
  }
  if (!cursor.skip(static_cast<std::size_t>(header.blockLength) - consumedRoot) ||
      !read_symbol(cursor, state) || cursor.remaining() != 0u)
    return false;
  Price bidPrice{};
  Price askPrice{};
  Amount bidAmount{};
  Amount askAmount{};
  std::uint64_t exchangeNs = 0u;
  std::uint64_t streamNs = 0u;
  if (!sbe::priceFromMantissa(bidPriceMantissa, priceExponent, &bidPrice) ||
      !sbe::priceFromMantissa(askPriceMantissa, priceExponent, &askPrice) ||
      !sbe::amountFromMantissa(bidSizeMantissa, sizeExponent, &bidAmount) ||
      !sbe::amountFromMantissa(askSizeMantissa, sizeExponent, &askAmount) ||
      !microseconds_ns(timestampUs, &exchangeNs) ||
      (streamTimestampUs != 0u &&
       !microseconds_ns(streamTimestampUs, &streamNs))) {
    return false;
  }
  state.subscriptionAccepted = true;
  output.status = FeedStatus::Ready;
  if (!detail::emit_bbo(
      state, output,
      BboState{bidPrice.raw, bidAmount.raw, askPrice.raw, askAmount.raw},
      sequence, exchangeNs, RecordOrigin::Raw)) {
    return false;
  }
  stamp_schema(output, header);
  for (std::size_t index = 0u; index < output.count; ++index)
    output.records[index].timestamps.streamServiceNs = streamNs;
  return true;
}

[[nodiscard]] bool sbe_depth(
    VenueNormalizerState& state, const FrameView& frame,
    NormalizeBatch& output) noexcept {
  sbe::SbeCursor cursor{{frame.data, frame.size}};
  sbe::SbeMessageHeader header{};
  std::uint64_t timestampUs = 0u;
  std::uint64_t sequence = 0u;
  std::int8_t priceExponent = 0;
  std::int8_t sizeExponent = 0;
  std::uint64_t streamTimestampUs = 0u;
  if (!sbe::readMessageHeader(cursor, &header) ||
      header.schemaId != kSchemaId || header.templateId != kDepthTemplate ||
      header.blockLength < 18u || !cursor.readU64(&timestampUs) ||
      !cursor.readU64(&sequence) || sequence == 0u ||
      !cursor.readI8(&priceExponent) || !cursor.readI8(&sizeExponent)) {
    return false;
  }
  std::size_t consumedRoot = 18u;
  if (header.blockLength >= 26u) {
    if (!cursor.readU64(&streamTimestampUs)) return false;
    consumedRoot = 26u;
  }
  if (!cursor.skip(static_cast<std::size_t>(header.blockLength) - consumedRoot))
    return false;
  std::size_t askCount = 0u;
  std::size_t bidCount = 0u;
  for (unsigned side = 0u; side < 2u; ++side) {
    std::uint16_t blockLength = 0u;
    std::uint16_t count = 0u;
    if (!cursor.readU16(&blockLength) || !cursor.readU16(&count) ||
        blockLength < 16u || count > kMaximumBookLevels) {
      return false;
    }
    auto* levels = side == 0u ? state.scratchAsks.data()
                              : state.scratchBids.data();
    auto& levelCount = side == 0u ? askCount : bidCount;
    for (std::uint16_t index = 0u; index < count; ++index) {
      const auto start = cursor.offset();
      std::int64_t priceMantissa = 0;
      std::int64_t sizeMantissa = 0;
      Price price{};
      Amount amount{};
      if (!cursor.readI64(&priceMantissa) ||
          !cursor.readI64(&sizeMantissa) ||
          !sbe::priceFromMantissa(priceMantissa, priceExponent, &price) ||
          !sbe::amountFromMantissa(sizeMantissa, sizeExponent, &amount) ||
          price.raw <= 0 || amount.raw <= 0 ||
          cursor.offset() > start + blockLength ||
          !cursor.skip(start + blockLength - cursor.offset())) {
        return false;
      }
      levels[levelCount++] = {price.raw, amount.raw};
    }
  }
  std::uint64_t exchangeNs = 0u;
  std::uint64_t streamNs = 0u;
  if (!read_symbol(cursor, state) || cursor.remaining() != 0u ||
      !microseconds_ns(timestampUs, &exchangeNs) ||
      (streamTimestampUs != 0u &&
       !microseconds_ns(streamTimestampUs, &streamNs)) ||
      !detail::emit_snapshot(
          state, output, bidCount, askCount, sequence, exchangeNs)) {
    return false;
  }
  state.subscriptionAccepted = true;
  output.status = FeedStatus::Ready;
  stamp_schema(output, header);
  for (std::size_t index = 0u; index < output.count; ++index)
    output.records[index].timestamps.streamServiceNs = streamNs;
  return true;
}

[[nodiscard]] bool sbe_trades(
    VenueNormalizerState& state, const FrameView& frame,
    NormalizeBatch& output) noexcept {
  sbe::SbeCursor cursor{{frame.data, frame.size}};
  sbe::SbeMessageHeader header{};
  std::int8_t priceExponent = 0;
  std::int8_t sizeExponent = 0;
  std::uint64_t streamTimestampUs = 0u;
  if (!sbe::readMessageHeader(cursor, &header) ||
      header.schemaId != kSchemaId || header.templateId != kTradeTemplate ||
      header.blockLength < 2u || !cursor.readI8(&priceExponent) ||
      !cursor.readI8(&sizeExponent)) {
    return false;
  }
  std::size_t consumedRoot = 2u;
  if (header.blockLength >= 10u) {
    if (!cursor.readU64(&streamTimestampUs)) return false;
    consumedRoot = 10u;
  }
  if (!cursor.skip(static_cast<std::size_t>(header.blockLength) - consumedRoot))
    return false;
  std::uint16_t blockLength = 0u;
  std::uint16_t count = 0u;
  if (!cursor.readU16(&blockLength) || !cursor.readU16(&count) ||
      blockLength < 40u || count == 0u || count > output.records.size()) {
    return false;
  }
  for (std::uint16_t index = 0u; index < count; ++index) {
    const auto start = cursor.offset();
    std::uint64_t timestampUs = 0u;
    std::uint64_t executionId = 0u;
    std::int64_t priceMantissa = 0;
    std::int64_t sizeMantissa = 0;
    std::uint8_t side = 0u;
    std::uint64_t entryStreamTimestampUs = 0u;
    Price price{};
    Amount amount{};
    std::uint64_t exchangeNs = 0u;
    if (!cursor.readU64(&timestampUs) || !cursor.readU64(&executionId) ||
        executionId == 0u || !cursor.readI64(&priceMantissa) ||
        !cursor.readI64(&sizeMantissa) || !cursor.readU8(&side) || side > 1u ||
        !sbe::priceFromMantissa(priceMantissa, priceExponent, &price) ||
        !sbe::amountFromMantissa(sizeMantissa, sizeExponent, &amount) ||
        !microseconds_ns(timestampUs, &exchangeNs)) {
      return false;
    }
    if (header.schemaVersion >= 4u && blockLength >= 43u) {
      std::uint8_t isRpi = 0u;
      std::uint8_t category = 0u;
      if (!cursor.readU8(&isRpi) || isRpi > 1u ||
          !cursor.readU64(&entryStreamTimestampUs) ||
          !cursor.readU8(&category))
        return false;
    }
    if (cursor.offset() > start + blockLength ||
        !cursor.skip(start + blockLength - cursor.offset()))
      return false;
    RaceRecord record{};
    record.eventClass = EventClass::Trade;
    record.validity = BookValidity::Valid;
    record.eventOrdinal = ++state.eventOrdinal;
    record.frameBatchCount = count;
    record.trade.tradeId = executionId;
    record.trade.nativeFirst = executionId;
    record.trade.nativeShape = 1u;
    record.trade.aggressorSide = side == 0u ? 1u : 2u;
    record.timestamps.exchangeEventNs = exchangeNs;
    const auto recordStreamTimestampUs =
        entryStreamTimestampUs != 0u ? entryStreamTimestampUs
                                     : streamTimestampUs;
    if (recordStreamTimestampUs != 0u &&
        !microseconds_ns(
            recordStreamTimestampUs, &record.timestamps.streamServiceNs))
      return false;
    record.changedLevelCount = 1u;
    record.changedLevels[0] =
        {price.raw, amount.raw, record.trade.aggressorSide, {}};
    if (!detail::append_record(output, record)) return false;
  }
  if (!read_symbol(cursor, state) || cursor.remaining() != 0u) return false;
  state.subscriptionAccepted = true;
  output.status = FeedStatus::Ready;
  stamp_schema(output, header);
  return true;
}

}  // namespace

bool normalize_bitget(
    void* rawState, const FrameView& frame,
    NormalizeBatch& output) noexcept {
  output = NormalizeBatch{};
  auto* state = static_cast<VenueNormalizerState*>(rawState);
  if (!state || state->venue != Venue::Bitget || !frame.data ||
      frame.size == 0u) {
    output.malformed = true;
    return false;
  }
  const auto root = detail::json_view(frame);
  const bool jsonPayload = root.size != 0u && root.data[0] == '{';
  if (jsonPayload) {
    bool ack = false;
    if (!json_ack(*state, root, output, &ack)) {
      output.malformed = true;
      return false;
    }
    if (ack) return true;
    if (state->wire == Wire::Sbe) {
      output.malformed = true;
      return false;
    }
    const bool ok = state->family == FeedFamily::Trade
                        ? json_trade(*state, root, output)
                        : json_book(*state, root, output);
    output.malformed = !ok;
    return ok;
  }
  if (!frame.binary || state->wire != Wire::Sbe) {
    output.malformed = true;
    return false;
  }
  const bool ok = state->family == FeedFamily::Trade
                      ? sbe_trades(*state, frame, output)
                      : state->family == FeedFamily::DirectBbo
                            ? sbe_bbo(*state, frame, output)
                            : sbe_depth(*state, frame, output);
  output.malformed = !ok;
  return ok;
}

}  // namespace exchange_probe::race
