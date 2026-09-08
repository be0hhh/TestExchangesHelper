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

constexpr std::uint16_t kGateSchemaId = 1u;
constexpr std::uint16_t kBboTemplate = 1u;
constexpr std::uint16_t kTradeTemplate = 2u;
constexpr std::uint16_t kObuTemplate = 3u;
constexpr std::uint8_t kUpdateEvent = 2u;

void stamp_schema(
    NormalizeBatch& output, const sbe::SbeMessageHeader& header) noexcept {
  for (std::size_t index = 0u; index < output.count; ++index) {
    output.records[index].wireSchema = {
        header.blockLength, header.templateId, header.schemaId,
        header.schemaVersion};
  }
}

[[nodiscard]] bool same_text(
    cxet::StringView value, const char* expected,
    std::size_t expectedSize) noexcept {
  return value.size() == expectedSize &&
         cxet::bytescan::hftMemcmp(value.data(), expected, expectedSize) == 0;
}

[[nodiscard]] bool symbol_matches(
    const VenueNormalizerState& state, cxet::StringView symbol) noexcept {
  return state.nativeSymbol.size == 0u ||
         (symbol.size() == state.nativeSymbol.size &&
          cxet::bytescan::hftMemcmp(
              symbol.data(), state.nativeSymbol.bytes.data(), symbol.size()) ==
              0);
}

[[nodiscard]] bool read_var_text(
    sbe::SbeCursor& cursor, cxet::StringView* output) noexcept {
  if (!output) return false;
  std::uint8_t size = 0u;
  if (!cursor.readU8(&size) || size == 0u || !cursor.canRead(size))
    return false;
  *output = cxet::StringView(
      reinterpret_cast<const char*>(cursor.ptr + cursor.offset()), size);
  return cursor.skip(size);
}

[[nodiscard]] bool validate_sbe_envelope(
    sbe::SbeCursor& cursor, const VenueNormalizerState& state,
    const char* channel, std::size_t channelSize) noexcept {
  cxet::StringView actualChannel{};
  cxet::StringView symbol{};
  return read_var_text(cursor, &actualChannel) &&
         read_var_text(cursor, &symbol) && cursor.remaining() == 0u &&
         same_text(actualChannel, channel, channelSize) &&
         symbol_matches(state, symbol);
}

[[nodiscard]] bool microseconds_ns(
    std::int64_t value, std::uint64_t* output) noexcept {
  if (!output || value <= 0 ||
      static_cast<std::uint64_t>(value) >
          std::numeric_limits<std::uint64_t>::max() /
              numeric::kNanosecondsPerMicrosecond) {
    return false;
  }
  *output = static_cast<std::uint64_t>(value) *
            numeric::kNanosecondsPerMicrosecond;
  return true;
}

[[nodiscard]] bool milliseconds_ns(
    std::uint64_t value, std::uint64_t* output) noexcept {
  if (!output || value == 0u ||
      value > std::numeric_limits<std::uint64_t>::max() /
                  numeric::kNanosecondsPerMillisecond) {
    return false;
  }
  *output = value * numeric::kNanosecondsPerMillisecond;
  return true;
}

[[nodiscard]] bool decimal_seconds_ns(
    cxet::StringView text, std::uint64_t* output) noexcept {
  if (!output || text.empty()) return false;
  std::uint64_t seconds = 0u;
  std::uint64_t fraction = 0u;
  std::uint64_t scale = 100'000'000u;
  bool afterDot = false;
  bool sawDigit = false;
  for (const char byte : text) {
    if (byte == '.') {
      if (afterDot || !sawDigit) return false;
      afterDot = true;
      continue;
    }
    if (byte < '0' || byte > '9') return false;
    sawDigit = true;
    const auto digit = static_cast<std::uint64_t>(byte - '0');
    if (!afterDot) {
      if (seconds >
          (std::numeric_limits<std::uint64_t>::max() - digit) / 10u) {
        return false;
      }
      seconds = seconds * 10u + digit;
    } else if (scale != 0u) {
      fraction += digit * scale;
      scale /= 10u;
    }
  }
  if (!afterDot || seconds >
          (std::numeric_limits<std::uint64_t>::max() - fraction) /
              numeric::kNanosecondsPerSecond) {
    return false;
  }
  *output = seconds * numeric::kNanosecondsPerSecond + fraction;
  return true;
}

[[nodiscard]] bool slot_gate_timestamp_ns(
    const hot::FieldSlot& slot, std::uint64_t* output) noexcept {
  std::uint64_t milliseconds = 0u;
  if (detail::slot_u64(slot, &milliseconds))
    return milliseconds_ns(milliseconds, output);
  char scratch[48]{};
  cxet::StringView text{};
  return detail::slot_text(slot, scratch, sizeof(scratch), &text) &&
         decimal_seconds_ns(text, output);
}

[[nodiscard]] bool signed_decimal(
    cxet::StringView text, std::int64_t* magnitude,
    bool* negative) noexcept {
  if (!magnitude || !negative || text.empty()) return false;
  *negative = text[0] == '-';
  if (*negative) text.remove_prefix(1u);
  return !text.empty() && detail::parse_amount(text, magnitude);
}

[[nodiscard]] bool slot_signed_amount(
    const hot::FieldSlot& slot, std::int64_t* magnitude,
    bool* negative) noexcept {
  char scratch[64]{};
  cxet::StringView text{};
  return detail::slot_text(slot, scratch, sizeof(scratch), &text) &&
         signed_decimal(text, magnitude, negative);
}

[[nodiscard]] bool classify_ack(
    VenueNormalizerState& state, hot::View root,
    NormalizeBatch& output, bool* classified) noexcept {
  *classified = false;
  hot::FieldSlot fields[] = {
      {"event", 5u}, {"result", 6u}, {"error", 5u}};
  if (!hot::collectObjectFieldsAllUnique(root, fields)) return false;
  if (!fields[0].found || !hot::slotStringEquals(fields[0], "subscribe"))
    return true;
  *classified = true;
  if (fields[2].found) {
    output.status = FeedStatus::Rejected;
    return true;
  }
  hot::View result{};
  hot::FieldSlot status[] = {{"status", 6u}};
  if (!hot::slotObject(fields[1], &result) ||
      !hot::collectObjectFieldsAllUnique(result, status) ||
      !hot::slotStringEquals(status[0], "success")) {
    output.status = FeedStatus::Rejected;
    return true;
  }
  state.subscriptionAccepted = true;
  output.status = state.family == FeedFamily::DirectBbo ||
                          state.family == FeedFamily::Trade
                      ? FeedStatus::Ready
                      : FeedStatus::Synchronizing;
  return true;
}

[[nodiscard]] bool json_context(
    const VenueNormalizerState& state, hot::View root,
    cxet::StringView* channel, hot::View* result,
    std::uint64_t* streamNs) noexcept {
  hot::FieldSlot rootFields[] = {
      {"channel", 7u}, {"event", 5u}, {"result", 6u},
      {"time_ms", 7u}, {"time", 4u}};
  if (!hot::collectObjectFieldsAllUnique(root, rootFields) ||
      !hot::slotStringEquals(rootFields[1], "update")) {
    return false;
  }
  static thread_local char channelScratch[48]{};
  if (!detail::slot_text(
          rootFields[0], channelScratch, sizeof(channelScratch), channel)) {
    return false;
  }
  std::uint64_t value = 0u;
  if (detail::slot_u64(rootFields[3], &value)) {
    if (!milliseconds_ns(value, streamNs)) return false;
  } else if (detail::slot_u64(rootFields[4], &value)) {
    if (value > std::numeric_limits<std::uint64_t>::max() /
                    numeric::kNanosecondsPerSecond) {
      return false;
    }
    *streamNs = value * numeric::kNanosecondsPerSecond;
  } else {
    return false;
  }
  *result = hot::trim(rootFields[2].value);
  return result->size >= 2u && result->data[0] != '\0' &&
         (state.nativeSymbol.size != 0u || state.venue == Venue::Gate);
}

[[nodiscard]] bool json_symbol_matches(
    const VenueNormalizerState& state,
    const hot::FieldSlot& slot) noexcept {
  char scratch[64]{};
  cxet::StringView symbol{};
  return detail::slot_text(slot, scratch, sizeof(scratch), &symbol) &&
         symbol_matches(state, symbol);
}

[[nodiscard]] bool parse_gate_level(
    hot::View item, BookLevel* output) noexcept {
  item = hot::trim(item);
  if (!output || item.size < 2u) return false;
  if (item.data[0] == '[') return detail::parse_level(item, output);
  if (item.data[0] != '{') return false;
  hot::FieldSlot fields[] = {{"p", 1u}, {"s", 1u}};
  char priceScratch[64]{};
  char quantityScratch[64]{};
  cxet::StringView price{};
  cxet::StringView quantity{};
  bool negative = false;
  return hot::collectObjectFieldsAllUnique(item, fields) &&
         detail::slot_text(
             fields[0], priceScratch, sizeof(priceScratch), &price) &&
         detail::slot_text(
             fields[1], quantityScratch, sizeof(quantityScratch), &quantity) &&
         detail::parse_price(price, &output->price) &&
         signed_decimal(quantity, &output->quantity, &negative) && !negative &&
         output->price > 0 && output->quantity >= 0;
}

[[nodiscard]] bool parse_gate_levels(
    hot::View levels, BookLevel* output, std::size_t capacity,
    std::size_t* count, bool allowZero) noexcept {
  if (!output || !count) return false;
  *count = 0u;
  std::size_t position = 1u;
  hot::View item{};
  while (hot::nextArrayItem(levels, &position, &item)) {
    if (*count == capacity || !parse_gate_level(item, &output[*count]) ||
        (!allowZero && output[*count].quantity == 0)) {
      return false;
    }
    ++*count;
  }
  return hot::arrayIterationComplete(levels, position);
}

[[nodiscard]] bool json_bbo(
    VenueNormalizerState& state, hot::View result,
    std::uint64_t streamNs, NormalizeBatch& output) noexcept {
  hot::FieldSlot fields[] = {
      {"s", 1u}, {"b", 1u}, {"B", 1u}, {"a", 1u},
      {"A", 1u}, {"u", 1u}, {"t", 1u}};
  char bidPriceScratch[64]{};
  char bidAmountScratch[64]{};
  char askPriceScratch[64]{};
  char askAmountScratch[64]{};
  cxet::StringView bidPrice{};
  cxet::StringView bidAmount{};
  cxet::StringView askPrice{};
  cxet::StringView askAmount{};
  std::uint64_t sequence = 0u;
  std::uint64_t timestampMs = 0u;
  BboState bbo{};
  if (!hot::collectObjectFieldsAllUnique(result, fields) ||
      !json_symbol_matches(state, fields[0]) ||
      !detail::slot_text(
          fields[1], bidPriceScratch, sizeof(bidPriceScratch), &bidPrice) ||
      !detail::slot_text(
          fields[2], bidAmountScratch, sizeof(bidAmountScratch), &bidAmount) ||
      !detail::slot_text(
          fields[3], askPriceScratch, sizeof(askPriceScratch), &askPrice) ||
      !detail::slot_text(
          fields[4], askAmountScratch, sizeof(askAmountScratch), &askAmount) ||
      !detail::parse_price(bidPrice, &bbo.bidPrice) ||
      !detail::parse_amount(bidAmount, &bbo.bidQuantity) ||
      !detail::parse_price(askPrice, &bbo.askPrice) ||
      !detail::parse_amount(askAmount, &bbo.askQuantity) ||
      !detail::slot_u64(fields[5], &sequence) || sequence == 0u ||
      !detail::slot_u64(fields[6], &timestampMs)) {
    return false;
  }
  std::uint64_t exchangeNs = 0u;
  const std::size_t first = output.count;
  if (!milliseconds_ns(timestampMs, &exchangeNs) ||
      !detail::emit_bbo(
          state, output, bbo, sequence, exchangeNs, RecordOrigin::Raw)) {
    return false;
  }
  for (std::size_t index = first; index < output.count; ++index)
    output.records[index].timestamps.streamServiceNs = streamNs;
  return true;
}

[[nodiscard]] bool json_trades(
    VenueNormalizerState& state, hot::View result,
    std::uint64_t streamNs, NormalizeBatch& output) noexcept {
  std::size_t position = result.data[0] == '[' ? 1u : 0u;
  hot::View trade = result;
  const std::size_t first = output.count;
  do {
    if (result.data[0] == '[' &&
        !hot::nextArrayItem(result, &position, &trade)) {
      break;
    }
    hot::FieldSlot fields[] = {
        {"id", 2u}, {"contract", 8u}, {"price", 5u},
        {"size", 4u}, {"create_time_ms", 14u}};
    char priceScratch[64]{};
    cxet::StringView price{};
    std::uint64_t id = 0u;
    bool negative = false;
    RaceRecord record{};
    if (!hot::collectObjectFieldsAllUnique(trade, fields) ||
        !json_symbol_matches(state, fields[1]) ||
        !detail::slot_u64(fields[0], &id) || id == 0u ||
        !detail::slot_text(
            fields[2], priceScratch, sizeof(priceScratch), &price) ||
        !detail::parse_price(price, &record.changedLevels[0].price) ||
        !slot_signed_amount(
            fields[3], &record.changedLevels[0].quantity, &negative) ||
        !slot_gate_timestamp_ns(
            fields[4], &record.timestamps.exchangeEventNs)) {
      return false;
    }
    record.eventClass = EventClass::Trade;
    record.validity = BookValidity::Valid;
    record.eventOrdinal = ++state.eventOrdinal;
    record.trade.tradeId = id;
    record.trade.nativeFirst = id;
    record.trade.nativeShape = 1u;
    record.trade.aggressorSide = negative ? 2u : 1u;
    record.timestamps.streamServiceNs = streamNs;
    record.changedLevelCount = 1u;
    record.changedLevels[0].side = record.trade.aggressorSide;
    if (!detail::append_record(output, record)) return false;
    if (result.data[0] != '[') break;
  } while (true);
  if ((result.data[0] == '[' &&
       !hot::arrayIterationComplete(result, position)) ||
      output.count == first) {
    return false;
  }
  const auto batch = static_cast<std::uint16_t>(output.count - first);
  for (std::size_t index = first; index < output.count; ++index)
    output.records[index].frameBatchCount = batch;
  return true;
}

[[nodiscard]] bool json_book(
    VenueNormalizerState& state, hot::View result,
    std::uint64_t streamNs, NormalizeBatch& output) noexcept {
  hot::FieldSlot fields[] = {
      {"contract", 8u}, {"bids", 4u}, {"asks", 4u},
      {"b", 1u}, {"a", 1u}, {"id", 2u}, {"U", 1u},
      {"u", 1u}, {"t", 1u}, {"full", 4u}};
  if (!hot::collectObjectFieldsAllUnique(result, fields) ||
      !json_symbol_matches(state, fields[0])) {
    return false;
  }
  hot::View bids{};
  hot::View asks{};
  if (!(hot::slotArray(fields[1], &bids) &&
        hot::slotArray(fields[2], &asks)) &&
      !(hot::slotArray(fields[3], &bids) &&
        hot::slotArray(fields[4], &asks))) {
    return false;
  }
  std::uint64_t last = 0u;
  std::uint64_t firstSequence = 0u;
  std::uint64_t timestampMs = 0u;
  if (!(detail::slot_u64(fields[7], &last) ||
        detail::slot_u64(fields[5], &last)) ||
      last == 0u || !detail::slot_u64(fields[8], &timestampMs)) {
    return false;
  }
  (void)detail::slot_u64(fields[6], &firstSequence);
  if (firstSequence == 0u) firstSequence = last;
  bool full = false;
  const bool explicitFull = fields[9].found && hot::slotBool(fields[9], &full);
  const bool snapshot = state.family == FeedFamily::SnapshotDepth ||
                        (explicitFull && full);
  const bool allowZero = !snapshot;
  std::size_t bidCount = 0u;
  std::size_t askCount = 0u;
  if (!parse_gate_levels(
          bids, state.scratchBids.data(), state.scratchBids.size(),
          &bidCount, allowZero) ||
      !parse_gate_levels(
          asks, state.scratchAsks.data(), state.scratchAsks.size(),
          &askCount, allowZero)) {
    state.book.invalidate(BookValidity::InvalidGap);
    return false;
  }
  std::uint64_t exchangeNs = 0u;
  if (!milliseconds_ns(timestampMs, &exchangeNs)) return false;
  const std::size_t firstOutput = output.count;
  if (snapshot) {
    if (!detail::emit_snapshot(
            state, output, bidCount, askCount, last, exchangeNs)) {
      return false;
    }
  } else if (state.book.validity() != BookValidity::Valid) {
    output.status = FeedStatus::Synchronizing;
    return true;
  } else {
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
            SequenceUpdate{firstSequence, last, 0u, false}, exchangeNs)) {
      return false;
    }
  }
  for (std::size_t index = firstOutput; index < output.count; ++index)
    output.records[index].timestamps.streamServiceNs = streamNs;
  return true;
}

[[nodiscard]] bool sbe_bbo(
    VenueNormalizerState& state, const FrameView& frame,
    NormalizeBatch& output) noexcept {
  sbe::SbeCursor cursor(sbe::SbeBufferView{frame.data, frame.size});
  sbe::SbeMessageHeader header{};
  std::int64_t serverTimeUs = 0;
  std::int8_t event = 0;
  std::int64_t bookTimeUs = 0;
  std::int64_t updateId = 0;
  std::int8_t priceExponent = 0;
  std::int8_t sizeExponent = 0;
  std::int64_t askPriceMantissa = 0;
  std::int64_t askSizeMantissa = 0;
  std::int64_t bidPriceMantissa = 0;
  std::int64_t bidSizeMantissa = 0;
  if (!sbe::readMessageHeader(cursor, &header) ||
      header.schemaId != kGateSchemaId || header.templateId != kBboTemplate ||
      header.blockLength < 59u || !cursor.readI64(&serverTimeUs) ||
      !cursor.readI8(&event) || !cursor.readI64(&bookTimeUs) ||
      !cursor.readI64(&updateId) || updateId <= 0 ||
      !cursor.readI8(&priceExponent) || !cursor.readI8(&sizeExponent) ||
      !cursor.readI64(&askPriceMantissa) ||
      !cursor.readI64(&askSizeMantissa) ||
      !cursor.readI64(&bidPriceMantissa) ||
      !cursor.readI64(&bidSizeMantissa) ||
      !cursor.skip(static_cast<std::size_t>(header.blockLength) - 59u) ||
      event != static_cast<std::int8_t>(kUpdateEvent) ||
      !validate_sbe_envelope(
          cursor, state, "futures.book_ticker", 19u)) {
    return false;
  }
  Price bidPrice{};
  Price askPrice{};
  Amount bidAmount{};
  Amount askAmount{};
  std::uint64_t exchangeNs = 0u;
  std::uint64_t streamNs = 0u;
  const std::size_t first = output.count;
  if (!sbe::priceFromMantissa(bidPriceMantissa, priceExponent, &bidPrice) ||
      !sbe::priceFromMantissa(askPriceMantissa, priceExponent, &askPrice) ||
      !sbe::amountFromMantissa(bidSizeMantissa, sizeExponent, &bidAmount) ||
      !sbe::amountFromMantissa(askSizeMantissa, sizeExponent, &askAmount) ||
      !microseconds_ns(bookTimeUs, &exchangeNs) ||
      !microseconds_ns(serverTimeUs, &streamNs) ||
      !detail::emit_bbo(
          state, output,
          {bidPrice.raw, bidAmount.raw, askPrice.raw, askAmount.raw},
          static_cast<std::uint64_t>(updateId), exchangeNs,
          RecordOrigin::Raw)) {
    return false;
  }
  for (std::size_t index = first; index < output.count; ++index)
    output.records[index].timestamps.streamServiceNs = streamNs;
  stamp_schema(output, header);
  return true;
}

[[nodiscard]] bool sbe_trades(
    VenueNormalizerState& state, const FrameView& frame,
    NormalizeBatch& output) noexcept {
  sbe::SbeCursor cursor(sbe::SbeBufferView{frame.data, frame.size});
  sbe::SbeMessageHeader header{};
  std::int64_t serverTimeUs = 0;
  std::int8_t event = 0;
  std::int8_t priceExponent = 0;
  std::int8_t sizeExponent = 0;
  if (!sbe::readMessageHeader(cursor, &header) ||
      header.schemaId != kGateSchemaId ||
      header.templateId != kTradeTemplate || header.blockLength < 11u ||
      !cursor.readI64(&serverTimeUs) || !cursor.readI8(&event) ||
      !cursor.readI8(&priceExponent) || !cursor.readI8(&sizeExponent) ||
      !cursor.skip(static_cast<std::size_t>(header.blockLength) - 11u) ||
      event != static_cast<std::int8_t>(kUpdateEvent)) {
    return false;
  }
  std::uint16_t blockLength = 0u;
  std::uint16_t count = 0u;
  std::uint64_t streamNs = 0u;
  if (!cursor.readU16(&blockLength) || !cursor.readU16(&count) ||
      blockLength < 32u || count == 0u || count > output.records.size() ||
      !microseconds_ns(serverTimeUs, &streamNs)) {
    return false;
  }
  for (std::uint16_t index = 0u; index < count; ++index) {
    const auto start = cursor.offset();
    std::int64_t tradeTimeUs = 0;
    std::uint64_t tradeId = 0u;
    std::int64_t sizeMantissa = 0;
    std::int64_t priceMantissa = 0;
    Price price{};
    Amount amount{};
    std::uint64_t exchangeNs = 0u;
    if (!cursor.readI64(&tradeTimeUs) || !cursor.readU64(&tradeId) ||
        tradeId == 0u || !cursor.readI64(&sizeMantissa) ||
        !cursor.readI64(&priceMantissa) ||
        sizeMantissa == std::numeric_limits<std::int64_t>::min() ||
        !sbe::priceFromMantissa(priceMantissa, priceExponent, &price) ||
        !sbe::amountFromMantissa(
            sizeMantissa < 0 ? -sizeMantissa : sizeMantissa,
            sizeExponent, &amount) ||
        !microseconds_ns(tradeTimeUs, &exchangeNs) ||
        cursor.offset() > start + blockLength ||
        !cursor.skip(start + blockLength - cursor.offset())) {
      return false;
    }
    RaceRecord record{};
    record.eventClass = EventClass::Trade;
    record.validity = BookValidity::Valid;
    record.eventOrdinal = ++state.eventOrdinal;
    record.frameBatchCount = count;
    record.trade.tradeId = tradeId;
    record.trade.nativeFirst = tradeId;
    record.trade.nativeShape = 1u;
    // Gate's current SBE schema proves signed size, but does not prove that
    // its sign is aggressor-side evidence.  Keep trade identity exact and
    // leave trade-derived BBO disabled for this source.
    record.trade.aggressorSide = 0u;
    record.timestamps.exchangeEventNs = exchangeNs;
    record.timestamps.streamServiceNs = streamNs;
    record.changedLevelCount = 1u;
    record.changedLevels[0] = {
        price.raw, amount.raw,
        static_cast<std::uint8_t>(sizeMantissa < 0 ? 2u : 1u), {}};
    if (!detail::append_record(output, record)) return false;
  }
  if (!validate_sbe_envelope(cursor, state, "futures.trades", 14u))
    return false;
  stamp_schema(output, header);
  return true;
}

[[nodiscard]] bool read_sbe_levels(
    sbe::SbeCursor& cursor, std::int8_t priceExponent,
    std::int8_t sizeExponent, BookLevel* levels,
    std::size_t capacity, std::size_t* count) noexcept {
  std::uint16_t blockLength = 0u;
  std::uint16_t groupCount = 0u;
  if (!count || !cursor.readU16(&blockLength) ||
      !cursor.readU16(&groupCount) || blockLength < 16u ||
      groupCount > capacity) {
    return false;
  }
  *count = 0u;
  for (std::uint16_t index = 0u; index < groupCount; ++index) {
    const auto start = cursor.offset();
    std::int64_t priceMantissa = 0;
    std::int64_t sizeMantissa = 0;
    Price price{};
    Amount amount{};
    if (!cursor.readI64(&priceMantissa) ||
        !cursor.readI64(&sizeMantissa) ||
        !sbe::priceFromMantissa(priceMantissa, priceExponent, &price) ||
        !sbe::amountFromMantissa(sizeMantissa, sizeExponent, &amount) ||
        price.raw <= 0 || amount.raw < 0 ||
        cursor.offset() > start + blockLength ||
        !cursor.skip(start + blockLength - cursor.offset())) {
      return false;
    }
    levels[(*count)++] = {price.raw, amount.raw};
  }
  return true;
}

[[nodiscard]] bool sbe_obu(
    VenueNormalizerState& state, const FrameView& frame,
    NormalizeBatch& output) noexcept {
  sbe::SbeCursor cursor(sbe::SbeBufferView{frame.data, frame.size});
  sbe::SbeMessageHeader header{};
  std::int64_t serverTimeUs = 0;
  std::int8_t event = 0;
  std::int64_t bookTimeUs = 0;
  std::uint8_t fullRaw = 0u;
  std::int64_t firstId = 0;
  std::int64_t lastId = 0;
  std::int8_t priceExponent = 0;
  std::int8_t sizeExponent = 0;
  if (!sbe::readMessageHeader(cursor, &header) ||
      header.schemaId != kGateSchemaId || header.templateId != kObuTemplate ||
      header.blockLength < 36u || !cursor.readI64(&serverTimeUs) ||
      !cursor.readI8(&event) || !cursor.readI64(&bookTimeUs) ||
      !cursor.readU8(&fullRaw) || fullRaw > 1u ||
      !cursor.readI64(&firstId) || !cursor.readI64(&lastId) ||
      firstId <= 0 || lastId < firstId ||
      !cursor.readI8(&priceExponent) || !cursor.readI8(&sizeExponent) ||
      !cursor.skip(static_cast<std::size_t>(header.blockLength) - 36u) ||
      event != static_cast<std::int8_t>(kUpdateEvent)) {
    return false;
  }
  std::size_t bidCount = 0u;
  std::size_t askCount = 0u;
  if (!read_sbe_levels(
          cursor, priceExponent, sizeExponent, state.scratchBids.data(),
          state.scratchBids.size(), &bidCount) ||
      !read_sbe_levels(
          cursor, priceExponent, sizeExponent, state.scratchAsks.data(),
          state.scratchAsks.size(), &askCount) ||
      !validate_sbe_envelope(cursor, state, "futures.obu", 11u)) {
    return false;
  }
  std::uint64_t exchangeNs = 0u;
  std::uint64_t streamNs = 0u;
  if (!microseconds_ns(bookTimeUs, &exchangeNs) ||
      !microseconds_ns(serverTimeUs, &streamNs)) {
    return false;
  }
  const std::size_t firstOutput = output.count;
  if (fullRaw != 0u) {
    if (!detail::emit_snapshot(
            state, output, bidCount, askCount,
            static_cast<std::uint64_t>(lastId), exchangeNs)) {
      return false;
    }
  } else if (state.book.validity() != BookValidity::Valid) {
    output.status = FeedStatus::Synchronizing;
    return true;
  } else {
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
            {static_cast<std::uint64_t>(firstId),
             static_cast<std::uint64_t>(lastId), 0u, false},
            exchangeNs)) {
      return false;
    }
  }
  for (std::size_t index = firstOutput; index < output.count; ++index)
    output.records[index].timestamps.streamServiceNs = streamNs;
  stamp_schema(output, header);
  return true;
}

}  // namespace

bool normalize_gate(
    void* rawState, const FrameView& frame,
    NormalizeBatch& output) noexcept {
  output = NormalizeBatch{};
  auto* state = static_cast<VenueNormalizerState*>(rawState);
  if (!state || state->venue != Venue::Gate || !frame.data ||
      frame.size == 0u) {
    output.malformed = true;
    return false;
  }
  bool ok = false;
  const auto root = detail::json_view(frame);
  const bool jsonPayload = root.size != 0u && root.data[0] == '{';
  if (jsonPayload) {
    bool ack = false;
    if (!classify_ack(*state, root, output, &ack)) {
      output.malformed = true;
      return false;
    }
    if (ack) return true;
    if (state->wire == Wire::Sbe) {
      output.malformed = true;
      return false;
    }
    cxet::StringView channel{};
    hot::View result{};
    std::uint64_t streamNs = 0u;
    if (!json_context(*state, root, &channel, &result, &streamNs)) {
      output.malformed = true;
      return false;
    }
    if (same_text(channel, "futures.book_ticker", 19u) &&
        state->family == FeedFamily::DirectBbo) {
      ok = json_bbo(*state, result, streamNs, output);
    } else if (same_text(channel, "futures.trades", 14u) &&
               state->family == FeedFamily::Trade) {
      ok = json_trades(*state, result, streamNs, output);
    } else if ((same_text(channel, "futures.order_book", 18u) ||
                same_text(channel, "futures.order_book_update", 25u) ||
                same_text(channel, "futures.obu", 11u)) &&
               state->family != FeedFamily::DirectBbo &&
               state->family != FeedFamily::Trade) {
      ok = json_book(*state, result, streamNs, output);
    }
  } else if (frame.binary && state->wire == Wire::Sbe) {
    ok = state->family == FeedFamily::DirectBbo
             ? sbe_bbo(*state, frame, output)
             : state->family == FeedFamily::Trade
                   ? sbe_trades(*state, frame, output)
                   : sbe_obu(*state, frame, output);
  }
  if (ok) {
    state->subscriptionAccepted = true;
    output.status = FeedStatus::Ready;
  }
  output.malformed = !ok;
  return ok;
}

}  // namespace exchange_probe::race
