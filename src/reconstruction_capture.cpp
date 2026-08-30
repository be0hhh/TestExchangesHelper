#include "exchange_probe/reconstruction/capture.hpp"
#include "exchange_probe/reconstruction/provenance.hpp"

#include "api/dispatch/BuildDispatch.hpp"
#include "api/market/PublicMarketDataSubscriptionManager.hpp"
#include "primitives/buf/Span.hpp"

#include <array>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <exception>
#include <fstream>
#include <fcntl.h>
#include <linux/fs.h>
#include <sys/syscall.h>
#include <thread>
#include <unistd.h>

namespace exchange_probe::reconstruction {
namespace {

[[nodiscard]] TimestampOrigin timestampOrigin(
    cxet::api::market::PublicMarketDataDirectEvent::TimestampOrigin
        origin) noexcept {
  using DirectOrigin =
      cxet::api::market::PublicMarketDataDirectEvent::TimestampOrigin;
  if (origin == DirectOrigin::Exchange) return TimestampOrigin::Exchange;
  if (origin == DirectOrigin::Receive) return TimestampOrigin::Receive;
  return TimestampOrigin::Unknown;
}

[[nodiscard]] std::uint64_t realtimeNowNs() noexcept {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::system_clock::now().time_since_epoch()).count());
}

class CaptureWriter final {
 public:
  [[nodiscard]] bool open(const CaptureOptions& options,
                          std::string& error) noexcept {
    std::error_code filesystemError;
    if (std::filesystem::exists(options.outputPath, filesystemError)) {
      error = "capture_output_exists";
      return false;
    }
    if (filesystemError) {
      error = "capture_output_stat:" + filesystemError.message();
      return false;
    }
    finalPath_ = options.outputPath;
    temporaryPath_ = options.outputPath;
    temporaryPath_ += ".partial";
    if (std::filesystem::exists(temporaryPath_, filesystemError)) {
      error = "capture_partial_output_exists";
      return false;
    }
    if (filesystemError) {
      error = "capture_partial_output_stat:" + filesystemError.message();
      return false;
    }
    output_.open(temporaryPath_, std::ios::binary | std::ios::out);
    if (!output_) {
      error = "capture_output_open_failed";
      return false;
    }
    CaptureHeader header{};
    header.recordBytes = static_cast<std::uint16_t>(sizeof(Observation));
    header.exchangeRaw = options.product.exchange.raw;
    header.marketRaw = options.product.market.raw;
    header.apiProtocolProfileRaw =
        options.product.apiProtocolProfile.raw;
    header.bookTickerWireRaw =
        static_cast<std::uint8_t>(options.product.bookTickerWire);
    header.tradeWireRaw =
        static_cast<std::uint8_t>(options.product.tradeWire);
    header.depthWireRaw = options.includeDepth
        ? static_cast<std::uint8_t>(options.product.depthWire) : 0u;
    header.tickSizeRaw = options.tickSizeRaw;
    header.startedRealtimeNs = realtimeNowNs();
    header.sessionId = header.startedRealtimeNs;
    header.bookTickerLaneId = options.product.bookTickerLaneId;
    header.tradeLaneId = options.product.tradeLaneId;
    header.depthLaneId = options.includeDepth
        ? options.product.depthLaneId : 0u;
    header.bookTickerTransportRaw =
        static_cast<std::uint8_t>(options.product.bookTickerTransport);
    header.tradeTransportRaw =
        static_cast<std::uint8_t>(options.product.tradeTransport);
    header.depthTransportRaw = options.includeDepth
        ? static_cast<std::uint8_t>(options.product.depthTransport) : 0u;
    header.bookTickerParserContractRaw = static_cast<std::uint8_t>(
        CaptureParserContract::BookTickerRuntimeV1);
    header.tradeParserContractRaw = static_cast<std::uint8_t>(
        CaptureParserContract::TradeRuntimeV1);
    header.depthParserContractRaw = options.includeDepth
        ? static_cast<std::uint8_t>(CaptureParserContract::DepthRuntimeV1)
        : static_cast<std::uint8_t>(CaptureParserContract::None);
    header.captureFlags = options.includeDepth ? 1u : 0u;
    header.symbol = options.symbol;
    output_.write(reinterpret_cast<const char*>(&header), sizeof(header));
    if (!output_) {
      error = "capture_header_write_failed";
      return false;
    }
    return true;
  }

  [[nodiscard]] bool write(const Observation& observation) noexcept {
    if (recordCount_ >= kMaximumCaptureRecords) {
      writeError_ = "capture_record_capacity_exceeded";
      return false;
    }
    output_.write(reinterpret_cast<const char*>(&observation),
                  sizeof(observation));
    if (!output_) {
      writeError_ = "capture_record_write_failed";
      return false;
    }
    ++recordCount_;
    return true;
  }

  [[nodiscard]] const char* writeError() const noexcept {
    return writeError_;
  }

  [[nodiscard]] bool finish(std::string& error) noexcept {
    CaptureFooter footer{};
    footer.recordCount = recordCount_;
    footer.completedRealtimeNs = realtimeNowNs();
    output_.write(reinterpret_cast<const char*>(&footer), sizeof(footer));
    output_.flush();
    if (!output_) {
      error = "capture_footer_write_failed";
      return false;
    }
    output_.close();
    if (output_.fail()) {
      error = "capture_close_failed";
      return false;
    }
#if defined(SYS_renameat2)
    if (::syscall(SYS_renameat2, AT_FDCWD, temporaryPath_.c_str(),
                  AT_FDCWD, finalPath_.c_str(), RENAME_NOREPLACE) != 0) {
      error = errno == EEXIST ? "capture_output_exists"
                             : "capture_atomic_publish_failed";
      return false;
    }
#else
    error = "capture_atomic_publish_unsupported";
    return false;
#endif
    return true;
  }

 private:
  std::ofstream output_{};
  std::filesystem::path finalPath_{};
  std::filesystem::path temporaryPath_{};
  std::uint64_t recordCount_{0u};
  const char* writeError_{"capture_record_write_failed"};
};

[[nodiscard]] Observation baseObservation(
    const cxet::api::market::PublicMarketDataDirectPollEvent& event,
    std::uint64_t sequence) noexcept {
  Observation output{};
  output.sequence = sequence;
  output.exchangeTimestampNs = event.direct.exchangeEventTs.raw;
  output.receiveMonotonicNs = event.latencyEvent.localRecvMonoNs.raw;
  output.timestampOrigin = timestampOrigin(event.direct.timestampOrigin);
  return output;
}

}  // namespace

bool capture(const CaptureOptions& options, CaptureResult& result) noexcept {
  result = CaptureResult{};
  if (!options.product.eligible() || options.symbol.data[0] == '\0' ||
      options.tickSizeRaw <= 0 || options.outputPath.empty() ||
      options.duration.count() <= 0 ||
      options.duration.count() > kMaximumCaptureSeconds) {
    result.error = "capture_options_invalid";
    return false;
  }

  try {
    cxet::initBuildDispatch();
    std::array<cxet::api::market::PublicMarketDataDesiredChannel, 3u>
        desired{};
    std::size_t desiredCount = 2u;
    for (auto& channel : desired) {
      channel.exchange = options.product.exchange;
      channel.market = options.product.market;
      channel.apiProtocolProfile = options.product.apiProtocolProfile;
      channel.symbol = options.symbol;
      channel.numeric.quantityKind =
          options.product.market.raw == canon::kMarketTypeSpot.raw ||
                  options.product.market.raw == canon::kMarketTypeMargin.raw
              ? cxet::api::RuntimeQuantityKind::SpotBase
              : cxet::api::RuntimeQuantityKind::DerivativeContracts;
      channel.captureLatency = true;
      channel.pollTimeoutMs = 1u;
    }
    desired[0].stream =
        cxet::api::market::PublicMarketDataStream::BookTicker;
    desired[0].wirePreference = options.product.bookTickerWire;
    desired[1].stream = cxet::api::market::PublicMarketDataStream::Trades;
    desired[1].wirePreference = options.product.tradeWire;
    if (options.includeDepth && options.product.depthRuntimeAvailable) {
      desired[2].stream =
          cxet::api::market::PublicMarketDataStream::Orderbook;
      desired[2].wirePreference = options.product.depthWire;
      desiredCount = 3u;
    }

    cxet::api::market::PublicMarketDataSubscriptionManager manager{};
    cxet::api::market::PublicMarketDataApplyResult applyResult{};
    char applyError[256]{};
    if (!manager.applyDesired(
            Span<const cxet::api::market::PublicMarketDataDesiredChannel>(
                desired.data(), desiredCount),
            &applyResult, applyError, sizeof(applyError))) {
      result.error = applyError[0] != '\0' ? applyError
                                           : "capture_apply_desired_failed";
      return false;
    }
    if (!validateCaptureProvenance(
            options.product, options.symbol, desiredCount == 3u,
            manager)) {
      result.error = "capture_route_provenance_mismatch";
      manager.closeAll();
      return false;
    }

    CaptureWriter writer{};
    if (!writer.open(options, result.error)) return false;
    const auto deadline = std::chrono::steady_clock::now() + options.duration;
    std::uint64_t sequence = 0u;
    while (std::chrono::steady_clock::now() < deadline) {
      bool drained = false;
      cxet::api::market::PublicMarketDataDirectPollEvent event{};
      while (manager.pollAvailableDirectHot(event)) {
        drained = true;
        if (!event.direct.ready) {
          ++result.terminalEvents;
          Observation row = baseObservation(event, ++sequence);
          row.kind = ObservationKind::Reset;
          if (!writer.write(row)) {
            result.error = writer.writeError();
            return false;
          }
          ++result.observations;
          continue;
        }
        if (event.direct.bookTickerSide) {
          Observation row = baseObservation(event, ++sequence);
          const auto& update = *event.direct.bookTickerSide;
          row.kind = ObservationKind::BookTickerSide;
          row.eventId = update.eventId.raw;
          row.priceRaw = update.level.px.raw;
          row.qtyRaw = update.level.qty.raw;
          row.side = cxet::composite::bookTickerSideIsBid(update.side)
              ? 1u : 2u;
          row.action =
              update.action ==
                      cxet::composite::BookTickerSideAction::Upsert
                  ? 1u : 2u;
          row.coalesceNext = event.direct.coalesceNextBookTickerSide
              ? 1u : 0u;
          if (!writer.write(row)) {
            result.error = writer.writeError();
            return false;
          }
          ++result.observations;
          ++result.bookTickerSides;
        } else if (event.direct.bookTicker) {
          const auto& ticker = *event.direct.bookTicker;
          for (std::uint8_t side = 1u; side <= 2u; ++side) {
            Observation row = baseObservation(event, ++sequence);
            row.kind = ObservationKind::BookTickerSide;
            row.eventId = ticker.eventId.raw;
            row.side = side;
            row.action = 1u;
            row.coalesceNext = side == 1u ? 1u : 0u;
            row.priceRaw = side == 1u ? ticker.bid.px.raw
                                      : ticker.ask.px.raw;
            row.qtyRaw = side == 1u ? ticker.bid.qty.raw
                                    : ticker.ask.qty.raw;
            if (!writer.write(row)) {
              result.error = writer.writeError();
              return false;
            }
            ++result.observations;
            ++result.bookTickerSides;
          }
        }
        if (event.direct.trade) {
          Observation row = baseObservation(event, ++sequence);
          const auto& trade = *event.direct.trade;
          row.kind = ObservationKind::Trade;
          row.eventId = trade.eventId.raw;
          row.priceRaw = trade.price.raw;
          row.qtyRaw = trade.qty.raw;
          row.side = trade.side.raw == Side::Buy().raw ? 1u
              : (trade.side.raw == Side::Sell().raw ? 2u : 0u);
          if (!writer.write(row)) {
            result.error = writer.writeError();
            return false;
          }
          ++result.observations;
          ++result.trades;
        }
        if (event.direct.stream ==
                cxet::api::market::PublicMarketDataStream::Orderbook) {
          const auto* route = manager.routeBySlot(event.routeSlot);
          cxet::api::market::PublicMarketOrderBookView view{};
          if (route && cxet::api::market::publicMarketOrderBookView(
                           *route, &view) && view.bidCount != 0u &&
              view.askCount != 0u) {
            Observation row = baseObservation(event, ++sequence);
            row.kind = ObservationKind::DepthBbo;
            row.eventId = view.lastEventId.raw;
            row.bidPriceRaw = view.bids[0].px.raw;
            row.bidQtyRaw = view.bids[0].qty.raw;
            row.askPriceRaw = view.asks[0].px.raw;
            row.askQtyRaw = view.asks[0].qty.raw;
            if (!writer.write(row)) {
              result.error = writer.writeError();
              return false;
            }
            ++result.observations;
            ++result.depthBbos;
          }
        }
      }
      (void)manager.maintainConnectionsOnce();
      if (!drained) std::this_thread::yield();
    }
    manager.closeAll();
    if (result.bookTickerSides == 0u || result.trades == 0u) {
      result.error = "capture_required_stream_empty";
      return false;
    }
    return writer.finish(result.error);
  } catch (const std::exception& exception) {
    result.error = exception.what();
  } catch (...) {
    result.error = "capture_unknown_exception";
  }
  return false;
}

}  // namespace exchange_probe::reconstruction
