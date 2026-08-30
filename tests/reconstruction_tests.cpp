#include "exchange_probe/reconstruction/analysis.hpp"
#include "exchange_probe/reconstruction/catalog.hpp"
#include "exchange_probe/reconstruction/report.hpp"
#include "exchange_probe/reconstruction/selection.hpp"

#include <cassert>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>

namespace {

using namespace exchange_probe::reconstruction;

Observation side(std::uint64_t sequence, std::uint64_t timestamp,
                 std::uint8_t bookSide, std::int64_t price,
                 std::int64_t qty, bool coalesceNext) {
  Observation row{};
  row.sequence = sequence;
  row.eventId = sequence;
  row.exchangeTimestampNs = timestamp;
  row.receiveMonotonicNs = sequence;
  row.kind = ObservationKind::BookTickerSide;
  row.timestampOrigin = TimestampOrigin::Exchange;
  row.side = bookSide;
  row.action = 1u;
  row.coalesceNext = coalesceNext ? 1u : 0u;
  row.priceRaw = price;
  row.qtyRaw = qty;
  return row;
}

Observation trade(std::uint64_t sequence, std::uint64_t timestamp,
                  std::uint8_t aggressor, std::int64_t price,
                  std::int64_t qty) {
  Observation row{};
  row.sequence = sequence;
  row.eventId = sequence;
  row.exchangeTimestampNs = timestamp;
  row.receiveMonotonicNs = sequence;
  row.kind = ObservationKind::Trade;
  row.timestampOrigin = TimestampOrigin::Exchange;
  row.side = aggressor;
  row.priceRaw = price;
  row.qtyRaw = qty;
  return row;
}

Observation depth(std::uint64_t sequence, std::uint64_t timestamp,
                  std::int64_t bid, std::int64_t ask) {
  Observation row{};
  row.sequence = sequence;
  row.eventId = sequence;
  row.exchangeTimestampNs = timestamp;
  row.receiveMonotonicNs = sequence;
  row.kind = ObservationKind::DepthBbo;
  row.timestampOrigin = TimestampOrigin::Exchange;
  row.bidPriceRaw = bid;
  row.bidQtyRaw = 1;
  row.askPriceRaw = ask;
  row.askQtyRaw = 1;
  return row;
}

void seed(Analyzer& analyzer, std::int64_t bid, std::int64_t bidQty,
          std::int64_t ask, std::int64_t askQty) {
  assert(analyzer.apply(side(1u, 100u, 1u, bid, bidQty, true)));
  assert(analyzer.apply(side(2u, 100u, 2u, ask, askQty, false)));
}

void testStrictModes() {
  Analyzer reverse{};
  assert(reverse.configure(1, ReconstructionPolicy::StrictExchange));
  seed(reverse, 100, 10, 101, 10);
  assert(reverse.apply(trade(3u, 101u, 2u, 102, 1)));
  assert(reverse.view().bookTicker.bid.px.raw == 102);
  assert(reverse.view().bookTicker.ask.px.raw == 103);
  assert(reverse.counters().reverse == 1u);
  assert(reverse.apply(depth(4u, 100u, 100, 101)));
  assert(reverse.counters().depthConfirmations == 0u);
  assert(reverse.counters().depthContradictions == 0u);
  assert(reverse.apply(side(5u, 102u, 1u, 102, 8, true)));
  assert(reverse.counters().nextBboConfirmations == 0u);
  assert(reverse.apply(side(6u, 102u, 2u, 103, 8, false)));
  assert(reverse.counters().nextBboConfirmations == 1u);

  Analyzer direct{};
  assert(direct.configure(1, ReconstructionPolicy::StrictExchange));
  seed(direct, 100, 10, 101, 10);
  assert(direct.apply(trade(3u, 101u, 2u, 99, 1)));
  assert(direct.view().bookTicker.bid.px.raw == 99);
  assert(direct.counters().direct == 1u);

  Analyzer nibbling{};
  assert(nibbling.configure(1, ReconstructionPolicy::StrictExchange));
  seed(nibbling, 100, 10, 101, 6);
  assert(nibbling.apply(trade(3u, 101u, 1u, 101, 4)));
  assert(nibbling.view().bookTicker.ask.qty.raw == 2);
  assert(nibbling.apply(trade(4u, 101u, 1u, 101, 2)));
  assert(nibbling.view().bookTicker.ask.px.raw == 102);
  assert(nibbling.view().askQtyKnown.raw == 0u);
  assert(nibbling.counters().nibbling == 2u);
  assert(nibbling.counters().fullNibbling == 1u);
}

void testTimestampPolicies() {
  Analyzer strict{};
  assert(strict.configure(1, ReconstructionPolicy::StrictExchange));
  seed(strict, 100, 10, 101, 10);
  auto missing = trade(3u, 0u, 1u, 102, 1);
  missing.timestampOrigin = TimestampOrigin::Receive;
  assert(!strict.apply(missing));
  assert(strict.counters().missingTimestamp == 1u);

  Analyzer receive{};
  assert(receive.configure(
      1, ReconstructionPolicy::ReceiveOrderCounterfactual));
  auto bid = side(1u, 0u, 1u, 100, 10, true);
  auto ask = side(2u, 0u, 2u, 101, 10, false);
  bid.timestampOrigin = TimestampOrigin::Receive;
  ask.timestampOrigin = TimestampOrigin::Receive;
  assert(receive.apply(bid));
  assert(receive.apply(ask));
  assert(receive.apply(missing));
  assert(receive.counters().acceptedTrades == 1u);
}

InstrumentCandidate candidate(const char* symbol, const char* base,
                              std::int64_t volume, bool exact) {
  InstrumentCandidate row{};
  assert(row.symbol.copyFrom(symbol));
  std::strncpy(row.baseAsset, base, sizeof(row.baseAsset) - 1u);
  std::strncpy(row.quoteAsset, "USDT", sizeof(row.quoteAsset) - 1u);
  row.tickSizeRaw = 1;
  row.quoteVolumeRaw = volume;
  row.active = true;
  row.exactQuoteVolume = exact;
  return row;
}

void testSelection() {
  const InstrumentCandidate rows[] = {
      candidate("ETHUSDT", "ETH", 100, true),
      candidate("BTCUSDT", "BTC", 1000, true),
      candidate("SOLUSDT", "SOL", 500, true),
      candidate("XRPUSDT", "XRP", 700, true),
      candidate("DOGEUSDT", "DOGE", 900, false),
  };
  const auto selected = selectCampaignInstruments(rows, 5u, true);
  assert(selected.count == 3u);
  assert(std::strcmp(selected.rows[0].symbol.data, "ETHUSDT") == 0);
  assert(std::strcmp(selected.rows[1].symbol.data, "XRPUSDT") == 0);
  assert(std::strcmp(selected.rows[2].symbol.data, "SOLUSDT") == 0);
  const auto fallback = selectCampaignInstruments(rows, 5u, false);
  assert(fallback.count == 1u);
  assert(fallback.volumeFallbackToEthOnly);
}

std::filesystem::path temporaryCapturePath(const char* suffix) {
  const auto nonce = std::chrono::steady_clock::now()
                         .time_since_epoch().count();
  return std::filesystem::temp_directory_path() /
      ("cxet-bbor-test-" + std::to_string(nonce) + suffix);
}

CaptureHeader captureHeader() {
  const ProductCatalog catalog = buildProductCatalog();
  const ProductEligibility* product = nullptr;
  for (std::size_t index = 0u; index < catalog.count; ++index) {
    if (catalog.rows[index].eligible()) {
      product = &catalog.rows[index];
      break;
    }
  }
  assert(product);
  CaptureHeader header{};
  header.recordBytes = static_cast<std::uint16_t>(sizeof(Observation));
  header.exchangeRaw = product->exchange.raw;
  header.marketRaw = product->market.raw;
  header.apiProtocolProfileRaw = product->apiProtocolProfile.raw;
  header.bookTickerWireRaw =
      static_cast<std::uint8_t>(product->bookTickerWire);
  header.tradeWireRaw =
      static_cast<std::uint8_t>(product->tradeWire);
  header.tickSizeRaw = 1;
  header.sessionId = 1u;
  header.startedRealtimeNs = 1u;
  header.bookTickerLaneId = product->bookTickerLaneId;
  header.tradeLaneId = product->tradeLaneId;
  header.bookTickerTransportRaw =
      static_cast<std::uint8_t>(product->bookTickerTransport);
  header.tradeTransportRaw =
      static_cast<std::uint8_t>(product->tradeTransport);
  header.bookTickerParserContractRaw = static_cast<std::uint8_t>(
      CaptureParserContract::BookTickerRuntimeV1);
  header.tradeParserContractRaw = static_cast<std::uint8_t>(
      CaptureParserContract::TradeRuntimeV1);
  assert(header.symbol.copyFrom("ETHUSDT"));
  return header;
}

void testCaptureCompletionContract() {
  const auto completePath = temporaryCapturePath("-complete.bbor");
  {
    std::ofstream output{completePath, std::ios::binary | std::ios::out};
    assert(output);
    const auto header = captureHeader();
    const Observation rows[] = {
        side(1u, 100u, 1u, 100, 10, true),
        side(2u, 100u, 2u, 101, 10, false),
        trade(3u, 101u, 1u, 102, 1),
    };
    CaptureFooter footer{};
    footer.recordCount = 3u;
    footer.completedRealtimeNs = 2u;
    output.write(reinterpret_cast<const char*>(&header), sizeof(header));
    output.write(reinterpret_cast<const char*>(rows), sizeof(rows));
    output.write(reinterpret_cast<const char*>(&footer), sizeof(footer));
    assert(output);
  }
  CaptureAnalysis analysis{};
  std::string error;
  assert(analyzeCapture(completePath, analysis, error));
  assert(analysis.footer.recordCount == 3u);
  assert(analysis.strict.acceptedTrades == 1u);
  assert(std::filesystem::remove(completePath));

  const auto partialPath = temporaryCapturePath("-partial.bbor");
  {
    std::ofstream output{partialPath, std::ios::binary | std::ios::out};
    assert(output);
    const auto header = captureHeader();
    const auto row = side(1u, 100u, 1u, 100, 10, false);
    output.write(reinterpret_cast<const char*>(&header), sizeof(header));
    output.write(reinterpret_cast<const char*>(&row), sizeof(row));
    assert(output);
  }
  error.clear();
  assert(!analyzeCapture(partialPath, analysis, error));
  assert(std::filesystem::remove(partialPath));
}

}  // namespace

int main() {
  testStrictModes();
  testTimestampPolicies();
  testSelection();
  testCaptureCompletionContract();
  return 0;
}
