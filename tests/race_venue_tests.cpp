#include "exchange_probe/race/normalizers.hpp"

#include <cassert>
#include <cstdint>
#include <cstring>
#include <string_view>
#include <type_traits>
#include <vector>

namespace {

exchange_probe::race::FrameView text_frame(const char* text) {
  return {
      .data = reinterpret_cast<const std::uint8_t*>(text),
      .size = std::strlen(text),
      .recvMonoNs = 10u,
      .binary = false,
  };
}

exchange_probe::race::FrameView binary_json_frame(const char* text) {
  auto frame = text_frame(text);
  frame.binary = true;
  return frame;
}

template <typename UInt>
void append_le(std::vector<std::uint8_t>& output, UInt value) {
  using Unsigned = std::make_unsigned_t<UInt>;
  Unsigned bits = static_cast<Unsigned>(value);
  for (std::size_t index = 0u; index < sizeof(UInt); ++index) {
    output.push_back(static_cast<std::uint8_t>(bits & 0xffu));
    if constexpr (sizeof(UInt) > 1u) bits >>= 8u;
  }
}

void append_text(std::vector<std::uint8_t>& output, std::string_view text) {
  assert(text.size() <= 255u);
  output.push_back(static_cast<std::uint8_t>(text.size()));
  for (const char byte : text)
    output.push_back(static_cast<std::uint8_t>(byte));
}

std::vector<std::uint8_t> bitget_bbo_sbe() {
  std::vector<std::uint8_t> output;
  output.reserve(80u);
  append_le<std::uint16_t>(output, 64u);
  append_le<std::uint16_t>(output, 1002u);
  append_le<std::uint16_t>(output, 1u);
  append_le<std::uint16_t>(output, 4u);
  append_le<std::uint64_t>(output, 4'000'000u);
  append_le<std::int64_t>(output, 100);
  append_le<std::int64_t>(output, 2);
  append_le<std::int64_t>(output, 101);
  append_le<std::int64_t>(output, 3);
  append_le<std::int8_t>(output, 0);
  append_le<std::int8_t>(output, 0);
  append_le<std::uint64_t>(output, 50u);
  append_le<std::uint64_t>(output, 4'000'001u);
  append_le<std::uint8_t>(output, 1u);
  for (unsigned index = 0u; index < 5u; ++index) output.push_back(0u);
  output.push_back(7u);
  for (const char byte : std::string_view{"BTCUSDT"})
    output.push_back(static_cast<std::uint8_t>(byte));
  return output;
}

std::vector<std::uint8_t> gate_bbo_sbe() {
  std::vector<std::uint8_t> output;
  append_le<std::uint16_t>(output, 59u);
  append_le<std::uint16_t>(output, 1u);
  append_le<std::uint16_t>(output, 1u);
  append_le<std::uint16_t>(output, 1u);
  append_le<std::int64_t>(output, 5'000'000);
  append_le<std::int8_t>(output, 2);
  append_le<std::int64_t>(output, 4'999'000);
  append_le<std::int64_t>(output, 60);
  append_le<std::int8_t>(output, 0);
  append_le<std::int8_t>(output, 0);
  append_le<std::int64_t>(output, 101);
  append_le<std::int64_t>(output, 3);
  append_le<std::int64_t>(output, 100);
  append_le<std::int64_t>(output, 2);
  append_text(output, "futures.book_ticker");
  append_text(output, "BTC_USDT");
  return output;
}

std::vector<std::uint8_t> gate_obu_sbe() {
  std::vector<std::uint8_t> output;
  append_le<std::uint16_t>(output, 36u);
  append_le<std::uint16_t>(output, 3u);
  append_le<std::uint16_t>(output, 1u);
  append_le<std::uint16_t>(output, 1u);
  append_le<std::int64_t>(output, 6'000'000);
  append_le<std::int8_t>(output, 2);
  append_le<std::int64_t>(output, 5'999'000);
  append_le<std::uint8_t>(output, 1u);
  append_le<std::int64_t>(output, 100);
  append_le<std::int64_t>(output, 100);
  append_le<std::int8_t>(output, 0);
  append_le<std::int8_t>(output, 0);
  append_le<std::uint16_t>(output, 16u);
  append_le<std::uint16_t>(output, 2u);
  append_le<std::int64_t>(output, 100);
  append_le<std::int64_t>(output, 2);
  append_le<std::int64_t>(output, 99);
  append_le<std::int64_t>(output, 4);
  append_le<std::uint16_t>(output, 16u);
  append_le<std::uint16_t>(output, 2u);
  append_le<std::int64_t>(output, 101);
  append_le<std::int64_t>(output, 3);
  append_le<std::int64_t>(output, 102);
  append_le<std::int64_t>(output, 5);
  append_text(output, "futures.obu");
  append_text(output, "BTC_USDT");
  return output;
}

}  // namespace

int main() {
  using namespace exchange_probe::race;

  VenueNormalizerState bbo;
  bbo.venue = Venue::Bybit;
  bbo.family = FeedFamily::DirectBbo;
  bbo.depthEventClass = EventClass::Bbo;
  assert(bbo.nativeSymbol.assign("BTCUSDT", 7u));
  NormalizeBatch output;
  constexpr char ack[] =
      R"({"success":true,"ret_msg":"","op":"subscribe","req_id":"1"})";
  assert(normalize_bybit(&bbo, text_frame(ack), output));
  assert(output.status == FeedStatus::Ready && output.count == 0u);

  constexpr char l1[] = R"({"topic":"orderbook.1.BTCUSDT","type":"snapshot","ts":1001,"data":{"s":"BTCUSDT","b":[["100","2"]],"a":[["101","3"]],"u":10,"seq":20,"cts":1000}})";
  assert(normalize_bybit(&bbo, text_frame(l1), output));
  assert(output.status == FeedStatus::Ready && output.count == 1u);
  assert(output.records[0].eventClass == EventClass::Bbo);
  assert(output.records[0].sequence.sequence == 20u);
  assert(output.records[0].timestamps.exchangeEventNs == 1'000'000'000u);
  assert(output.records[0].timestamps.streamServiceNs == 1'001'000'000u);
  assert(normalize_bybit(&bbo, text_frame(l1), output));
  assert(output.count == 0u);

  VenueNormalizerState depth;
  depth.venue = Venue::Bybit;
  depth.family = FeedFamily::IncrementalDepth;
  depth.depthEventClass = EventClass::Top50;
  assert(depth.nativeSymbol.assign("BTCUSDT", 7u));
  constexpr char snapshot[] = R"({"topic":"orderbook.50.BTCUSDT","type":"snapshot","ts":2001,"data":{"s":"BTCUSDT","b":[["100","2"],["99","4"]],"a":[["101","3"],["102","5"]],"u":100,"seq":200,"cts":2000}})";
  assert(normalize_bybit(&depth, text_frame(snapshot), output));
  assert(depth.book.validity() == BookValidity::Valid);
  assert(output.count == 2u);
  constexpr char delta[] = R"({"topic":"orderbook.50.BTCUSDT","type":"delta","ts":2002,"data":{"s":"BTCUSDT","b":[["100","1"]],"a":[["101","2"]],"u":101,"seq":201,"cts":2001}})";
  assert(normalize_bybit(&depth, text_frame(delta), output));
  assert(depth.book.validity() == BookValidity::Valid);
  assert(depth.book.last_sequence() == 101u);
  assert(output.count == 2u);

  VenueNormalizerState trades;
  trades.venue = Venue::Bybit;
  trades.family = FeedFamily::Trade;
  assert(trades.nativeSymbol.assign("BTCUSDT", 7u));
  constexpr char trade[] = R"({"topic":"publicTrade.BTCUSDT","type":"snapshot","ts":3001,"data":[{"T":3000,"s":"BTCUSDT","S":"Buy","v":"0.5","p":"100.25","i":"01234567-89ab-cdef-0123-456789abcdef","seq":77}]})";
  assert(normalize_bybit(&trades, text_frame(trade), output));
  assert(output.count == 1u && output.records[0].trade.nativeShape == 2u);
  assert(output.records[0].trade.nativeFirst != 0u);
  assert(output.records[0].trade.nativeSecond != 0u);
  assert(output.records[0].sequence.sequence == 77u);

  VenueNormalizerState bitgetJson;
  bitgetJson.venue = Venue::Bitget;
  bitgetJson.wire = Wire::Json;
  bitgetJson.family = FeedFamily::DirectBbo;
  assert(bitgetJson.nativeSymbol.assign("BTCUSDT", 7u));
  constexpr char bitgetBook[] = R"({"action":"snapshot","arg":{"instType":"USDT-FUTURES","topic":"books1","symbol":"BTCUSDT"},"data":[{"b":[["100","2"]],"a":[["101","3"]],"seq":"50","pseq":"49","ts":"4000"}],"ts":4001})";
  assert(normalize_bitget(&bitgetJson, text_frame(bitgetBook), output));
  assert(output.count == 1u);
  const auto jsonBbo = output.records[0].bbo;

  VenueNormalizerState bitgetSbe;
  bitgetSbe.venue = Venue::Bitget;
  bitgetSbe.wire = Wire::Sbe;
  bitgetSbe.family = FeedFamily::DirectBbo;
  assert(bitgetSbe.nativeSymbol.assign("BTCUSDT", 7u));
  const auto binary = bitget_bbo_sbe();
  const FrameView binaryFrame{
      .data = binary.data(),
      .size = binary.size(),
      .recvMonoNs = 20u,
      .binary = true,
  };
  assert(normalize_bitget(&bitgetSbe, binaryFrame, output));
  assert(output.count == 1u);
  assert(output.records[0].bbo == jsonBbo);
  assert(output.records[0].sequence.sequence == 50u);
  assert(output.records[0].wireSchema.templateId == 1002u);
  assert(output.records[0].wireSchema.schemaVersion == 4u);
  assert(output.records[0].timestamps.exchangeEventNs == 4'000'000'000u);
  assert(output.records[0].timestamps.streamServiceNs == 4'000'001'000u);

  VenueNormalizerState gateJson;
  gateJson.venue = Venue::Gate;
  gateJson.wire = Wire::Json;
  gateJson.family = FeedFamily::DirectBbo;
  assert(gateJson.nativeSymbol.assign("BTC_USDT", 8u));
  constexpr char gateBook[] = R"({"time_ms":5000,"channel":"futures.book_ticker","event":"update","result":{"t":4999,"u":60,"s":"BTC_USDT","b":"100","B":"2","a":"101","A":"3"}})";
  assert(normalize_gate(&gateJson, text_frame(gateBook), output));
  assert(output.count == 1u);
  const auto gateJsonBbo = output.records[0].bbo;
  assert(output.records[0].timestamps.streamServiceNs == 5'000'000'000u);

  VenueNormalizerState gateSbe;
  gateSbe.venue = Venue::Gate;
  gateSbe.wire = Wire::Sbe;
  gateSbe.family = FeedFamily::DirectBbo;
  assert(gateSbe.nativeSymbol.assign("BTC_USDT", 8u));
  constexpr char gateAck[] = R"({"id":1,"event":"subscribe","channel":"futures.book_ticker","result":{"status":"success"}})";
  assert(normalize_gate(&gateSbe, binary_json_frame(gateAck), output));
  assert(output.status == FeedStatus::Ready && output.count == 0u);
  const auto gateBinary = gate_bbo_sbe();
  const FrameView gateBinaryFrame{
      .data = gateBinary.data(),
      .size = gateBinary.size(),
      .recvMonoNs = 30u,
      .binary = true,
  };
  assert(normalize_gate(&gateSbe, gateBinaryFrame, output));
  assert(output.count == 1u);
  assert(output.records[0].bbo == gateJsonBbo);
  assert(output.records[0].sequence.sequence == 60u);
  assert(output.records[0].wireSchema.templateId == 1u);
  assert(output.records[0].wireSchema.schemaVersion == 1u);
  assert(output.records[0].timestamps.exchangeEventNs == 4'999'000'000u);
  assert(output.records[0].timestamps.streamServiceNs == 5'000'000'000u);

  VenueNormalizerState gateDepth;
  gateDepth.venue = Venue::Gate;
  gateDepth.wire = Wire::Json;
  gateDepth.family = FeedFamily::IncrementalDepth;
  gateDepth.depthEventClass = EventClass::Top50;
  assert(gateDepth.nativeSymbol.assign("BTC_USDT", 8u));
  constexpr char gateSnapshot[] = R"({"time_ms":6000,"channel":"futures.obu","event":"update","result":{"t":5999,"full":true,"contract":"BTC_USDT","U":100,"u":100,"bids":[{"p":"100","s":"2"},{"p":"99","s":"4"}],"asks":[{"p":"101","s":"3"},{"p":"102","s":"5"}]}})";
  assert(normalize_gate(&gateDepth, text_frame(gateSnapshot), output));
  assert(gateDepth.book.validity() == BookValidity::Valid);
  assert(gateDepth.book.last_sequence() == 100u);
  assert(output.count == 2u);
  const auto gateJsonDepthBbo = gateDepth.book.bbo();
  const auto gateJsonTop50 = gateDepth.book.top_fingerprint(50u);

  VenueNormalizerState gateSbeDepth;
  gateSbeDepth.venue = Venue::Gate;
  gateSbeDepth.wire = Wire::Sbe;
  gateSbeDepth.family = FeedFamily::IncrementalDepth;
  gateSbeDepth.depthEventClass = EventClass::Top50;
  assert(gateSbeDepth.nativeSymbol.assign("BTC_USDT", 8u));
  const auto gateObuBinary = gate_obu_sbe();
  const FrameView gateObuFrame{
      .data = gateObuBinary.data(),
      .size = gateObuBinary.size(),
      .recvMonoNs = 31u,
      .binary = true,
  };
  assert(normalize_gate(&gateSbeDepth, gateObuFrame, output));
  assert(output.count == 2u);
  assert(gateSbeDepth.book.bbo() == gateJsonDepthBbo);
  assert(gateSbeDepth.book.top_fingerprint(50u) == gateJsonTop50);
  assert(output.records[0].wireSchema.templateId == 3u);
  constexpr char gateDelta[] = R"({"time_ms":6001,"channel":"futures.obu","event":"update","result":{"t":6000,"full":false,"contract":"BTC_USDT","U":101,"u":101,"bids":[{"p":"100","s":"1"}],"asks":[{"p":"101","s":"2"}]}})";
  assert(normalize_gate(&gateDepth, text_frame(gateDelta), output));
  assert(gateDepth.book.validity() == BookValidity::Valid);
  assert(gateDepth.book.last_sequence() == 101u);
  assert(output.count == 2u);

  VenueNormalizerState okxBbo;
  okxBbo.venue = Venue::Okx;
  okxBbo.family = FeedFamily::DirectBbo;
  assert(okxBbo.nativeSymbol.assign("BTC-USDT-SWAP", 13u));
  constexpr char okxAck[] = R"({"event":"subscribe","arg":{"channel":"bbo-tbt","instId":"BTC-USDT-SWAP"}})";
  assert(normalize_okx(&okxBbo, text_frame(okxAck), output));
  assert(output.status == FeedStatus::Ready);
  constexpr char okxBook[] = R"({"arg":{"channel":"bbo-tbt","instId":"BTC-USDT-SWAP"},"action":"snapshot","data":[{"asks":[["101","3","0","1"]],"bids":[["100","2","0","1"]],"ts":"7000","seqId":50}]})";
  assert(normalize_okx(&okxBbo, text_frame(okxBook), output));
  assert(output.count == 1u && output.records[0].sequence.sequence == 50u);

  VenueNormalizerState okxDepth;
  okxDepth.venue = Venue::Okx;
  okxDepth.family = FeedFamily::IncrementalDepth;
  okxDepth.depthEventClass = EventClass::Top50;
  assert(okxDepth.nativeSymbol.assign("BTC-USDT-SWAP", 13u));
  constexpr char okxSnapshot[] = R"({"arg":{"channel":"books","instId":"BTC-USDT-SWAP"},"action":"snapshot","data":[{"asks":[["101","3","0","1"],["102","5","0","1"]],"bids":[["100","2","0","1"],["99","4","0","1"]],"ts":"7100","seqId":100,"prevSeqId":-1}]})";
  assert(normalize_okx(&okxDepth, text_frame(okxSnapshot), output));
  assert(okxDepth.book.validity() == BookValidity::Valid);
  constexpr char okxDelta[] = R"({"arg":{"channel":"books","instId":"BTC-USDT-SWAP"},"action":"update","data":[{"asks":[["101","2","0","1"]],"bids":[["100","1","0","1"]],"ts":"7101","seqId":101,"prevSeqId":100}]})";
  assert(normalize_okx(&okxDepth, text_frame(okxDelta), output));
  assert(okxDepth.book.last_sequence() == 101u && output.count == 2u);

  VenueNormalizerState okxTrades;
  okxTrades.venue = Venue::Okx;
  okxTrades.family = FeedFamily::Trade;
  assert(okxTrades.nativeSymbol.assign("BTC-USDT-SWAP", 13u));
  constexpr char okxAggregated[] = R"({"arg":{"channel":"trades","instId":"BTC-USDT-SWAP"},"data":[{"instId":"BTC-USDT-SWAP","tradeId":"900","px":"100","sz":"3","side":"buy","ts":"7200","count":"3"}]})";
  assert(normalize_okx(&okxTrades, text_frame(okxAggregated), output));
  assert(output.count == 1u && output.records[0].trade.tradeId == 0u);
  assert(output.records[0].trade.aggregateId == 900u);
  assert(output.records[0].trade.count == 3u);
  constexpr char okxVip[] = R"({"event":"error","code":"60029","msg":"VIP required"})";
  assert(normalize_okx(&okxTrades, text_frame(okxVip), output));
  assert(output.status == FeedStatus::VipRequired && output.count == 0u);

  VenueNormalizerState kucoinBbo;
  kucoinBbo.venue = Venue::Kucoin;
  kucoinBbo.family = FeedFamily::DirectBbo;
  assert(kucoinBbo.nativeSymbol.assign("BTCUSDTM", 8u));
  constexpr char kucoinAck[] = R"({"id":"cxet-kucoin-1","result":true})";
  assert(normalize_kucoin(&kucoinBbo, binary_json_frame(kucoinAck), output));
  assert(output.status == FeedStatus::Ready);
  constexpr char kucoinBook[] = R"({"T":"obu.FUTURES","dp":"1","t":"snapshot","P":1700000000000,"d":{"s":"BTCUSDTM","C":200,"b":[["100","2"]],"a":[["101","3"]],"M":1700000000000000000}})";
  assert(normalize_kucoin(&kucoinBbo, binary_json_frame(kucoinBook), output));
  assert(output.count == 1u && output.records[0].sequence.sequence == 200u);

  VenueNormalizerState kucoinDepth;
  kucoinDepth.venue = Venue::Kucoin;
  kucoinDepth.family = FeedFamily::IncrementalDepth;
  kucoinDepth.depthEventClass = EventClass::Top50;
  assert(kucoinDepth.nativeSymbol.assign("BTCUSDTM", 8u));
  constexpr char kucoinSnapshot[] = R"({"T":"obu.FUTURES","dp":"increment@10ms","t":"snapshot","P":1700000000000,"d":{"s":"BTCUSDTM","C":300,"b":[["100","2"],["99","4"]],"a":[["101","3"],["102","5"]],"M":1700000000000000000}})";
  assert(normalize_kucoin(&kucoinDepth, binary_json_frame(kucoinSnapshot), output));
  assert(kucoinDepth.book.validity() == BookValidity::Valid);
  constexpr char kucoinDelta[] = R"({"T":"obu.FUTURES","dp":"increment@10ms","t":"increment","P":1700000000001,"d":{"s":"BTCUSDTM","U":301,"C":301,"b":[["100","1"]],"a":[["101","2"]],"M":1700000000000001000}})";
  assert(normalize_kucoin(&kucoinDepth, binary_json_frame(kucoinDelta), output));
  assert(kucoinDepth.book.last_sequence() == 301u && output.count == 2u);

  VenueNormalizerState kucoinTrade;
  kucoinTrade.venue = Venue::Kucoin;
  kucoinTrade.family = FeedFamily::Trade;
  assert(kucoinTrade.nativeSymbol.assign("BTCUSDTM", 8u));
  constexpr char kucoinTradeFrame[] = R"({"T":"trade.FUTURES","P":1700000000000002000,"d":{"s":"BTCUSDTM","ti":400,"p":"100","q":"0.5","S":"buy"}})";
  assert(normalize_kucoin(
      &kucoinTrade, binary_json_frame(kucoinTradeFrame), output));
  assert(output.count == 1u && output.records[0].trade.tradeId == 400u);
  assert(output.records[0].trade.aggressorSide == 0u);
  constexpr char kucoinRemoved[] = R"({"id":"cxet-kucoin-old","result":false})";
  assert(normalize_kucoin(
      &kucoinTrade, binary_json_frame(kucoinRemoved), output));
  assert(output.status == FeedStatus::Rejected && output.count == 0u);

  VenueNormalizerState binanceBbo;
  binanceBbo.venue = Venue::BinanceUsdM;
  binanceBbo.family = FeedFamily::DirectBbo;
  assert(binanceBbo.nativeSymbol.assign("BTCUSDT", 7u));
  constexpr char binanceAck[] = R"({"result":null,"id":1})";
  assert(normalize_binance_usdm(
      &binanceBbo, text_frame(binanceAck), output));
  assert(output.status == FeedStatus::Ready && output.count == 0u);
  constexpr char binanceBookTicker[] = R"({"e":"bookTicker","u":50,"s":"BTCUSDT","b":"100","B":"2","a":"101","A":"3","T":8000,"E":8001})";
  assert(normalize_binance_usdm(
      &binanceBbo, text_frame(binanceBookTicker), output));
  assert(output.count == 1u && output.records[0].sequence.sequence == 50u);
  assert(output.records[0].timestamps.exchangeEventNs == 8'000'000'000u);
  assert(output.records[0].timestamps.streamServiceNs == 8'001'000'000u);

  VenueNormalizerState binanceCombinedBbo;
  binanceCombinedBbo.venue = Venue::BinanceUsdM;
  binanceCombinedBbo.family = FeedFamily::DirectBbo;
  assert(binanceCombinedBbo.nativeSymbol.assign("BTCUSDT", 7u));
  constexpr char binanceCombined[] = R"({"stream":"btcusdt@bookTicker","data":{"e":"bookTicker","u":50,"s":"BTCUSDT","b":"100","B":"2","a":"101","A":"3","T":8000,"E":8001}})";
  assert(normalize_binance_usdm(
      &binanceCombinedBbo, text_frame(binanceCombined), output));
  assert(output.count == 1u && output.records[0].bbo ==
                                      binanceBbo.lastBbo);

  VenueNormalizerState binanceDepth;
  binanceDepth.venue = Venue::BinanceUsdM;
  binanceDepth.family = FeedFamily::IncrementalDepth;
  binanceDepth.depthEventClass = EventClass::Top50;
  assert(binanceDepth.nativeSymbol.assign("BTCUSDT", 7u));
  constexpr char binanceSnapshot[] = R"({"lastUpdateId":100,"bids":[["100","2"],["99","4"]],"asks":[["101","3"],["102","5"]]})";
  assert(normalize_binance_usdm(
      &binanceDepth, text_frame(binanceSnapshot), output));
  assert(binanceDepth.book.validity() == BookValidity::Valid);
  constexpr char binanceDelta[] = R"({"e":"depthUpdate","E":8101,"T":8100,"s":"BTCUSDT","U":101,"u":101,"pu":100,"b":[["100","1"]],"a":[["101","2"]]})";
  assert(normalize_binance_usdm(
      &binanceDepth, text_frame(binanceDelta), output));
  assert(binanceDepth.book.last_sequence() == 101u && output.count == 2u);

  VenueNormalizerState binanceTrade;
  binanceTrade.venue = Venue::BinanceUsdM;
  binanceTrade.family = FeedFamily::Trade;
  assert(binanceTrade.nativeSymbol.assign("BTCUSDT", 7u));
  constexpr char binanceAggTrade[] = R"({"e":"aggTrade","E":8201,"a":700,"s":"BTCUSDT","p":"100","q":"3","f":1000,"l":1002,"T":8200,"m":false})";
  assert(normalize_binance_usdm(
      &binanceTrade, text_frame(binanceAggTrade), output));
  assert(output.count == 1u && output.records[0].trade.tradeId == 0u);
  assert(output.records[0].trade.aggregateId == 700u);
  assert(output.records[0].trade.firstTradeId == 1000u);
  assert(output.records[0].trade.lastTradeId == 1002u);
  assert(output.records[0].trade.count == 3u);
  assert(output.records[0].trade.aggressorSide == 1u);
  constexpr char binanceRejected[] =
      R"({"code":-1121,"msg":"Invalid stream","id":2})";
  assert(normalize_binance_usdm(
      &binanceTrade, text_frame(binanceRejected), output));
  assert(output.status == FeedStatus::Rejected && output.count == 0u);

  VenueNormalizerState asterBbo;
  asterBbo.venue = Venue::Aster;
  asterBbo.family = FeedFamily::DirectBbo;
  assert(asterBbo.nativeSymbol.assign("BTCUSDT", 7u));
  assert(normalize_aster(
      &asterBbo, text_frame(binanceCombined), output));
  assert(output.count == 1u && output.records[0].venue == Venue::Aster);

  VenueNormalizerState asterDepth;
  asterDepth.venue = Venue::Aster;
  asterDepth.family = FeedFamily::IncrementalDepth;
  asterDepth.depthEventClass = EventClass::Top50;
  assert(asterDepth.nativeSymbol.assign("BTCUSDT", 7u));
  assert(normalize_aster(
      &asterDepth, text_frame(binanceSnapshot), output));
  assert(normalize_aster(&asterDepth, text_frame(binanceDelta), output));
  assert(asterDepth.book.validity() == BookValidity::Valid);
  assert(asterDepth.book.last_sequence() == 101u);

  VenueNormalizerState asterTrade;
  asterTrade.venue = Venue::Aster;
  asterTrade.family = FeedFamily::Trade;
  assert(asterTrade.nativeSymbol.assign("BTCUSDT", 7u));
  assert(normalize_aster(
      &asterTrade, text_frame(binanceAggTrade), output));
  assert(output.count == 1u && output.records[0].venue == Venue::Aster);
  assert(output.records[0].trade.aggregateId == 700u);
}
