#include "exchange_probe/reconstruction/Report.hpp"
#include "exchange_probe/reconstruction/Provenance.hpp"

#include <cstring>
#include <filesystem>
#include <exception>
#include <fstream>

namespace exchange_probe::reconstruction {
namespace {

void writeCounters(std::ostream& output, const char* policy,
                   const AnalysisCounters& counters) {
  output << "| " << policy << " | " << counters.trades << " | "
         << counters.acceptedTrades << " | " << counters.rejectedTrades
         << " | " << counters.direct << " | " << counters.reverse << " | "
         << counters.nibbling << " | " << counters.fullNibbling << " | "
         << counters.nextBboConfirmations << " | "
         << counters.nextBboContradictions << " | "
         << counters.depthConfirmations << " | "
         << counters.depthContradictions << " |\n";
}

[[nodiscard]] bool zeroBytes(const std::uint8_t* bytes,
                             std::size_t count) noexcept {
  for (std::size_t index = 0u; index < count; ++index) {
    if (bytes[index] != 0u) return false;
  }
  return true;
}

[[nodiscard]] bool validWire(std::uint8_t raw) noexcept {
  using Wire = cxet::api::market::PublicMarketDataWirePreference;
  return raw == static_cast<std::uint8_t>(Wire::Json) ||
         raw == static_cast<std::uint8_t>(Wire::Sbe) ||
         raw == static_cast<std::uint8_t>(Wire::Protobuf);
}

[[nodiscard]] bool validStreamingTransport(std::uint8_t raw) noexcept {
  return raw == static_cast<std::uint8_t>(cxet::api::RouteTransport::Ws) ||
         raw == static_cast<std::uint8_t>(cxet::api::RouteTransport::Fix) ||
         raw == static_cast<std::uint8_t>(cxet::api::RouteTransport::Grpc);
}

[[nodiscard]] bool validHeader(const CaptureHeader& header) noexcept {
  const bool depth = (header.captureFlags & 1u) != 0u;
  const bool symbolTerminated =
      std::memchr(header.symbol.data, '\0', Symbol::capacity) != nullptr;
  if (header.magic != kCaptureMagic ||
      header.schemaVersion != kCaptureSchemaVersion ||
      header.headerBytes != sizeof(CaptureHeader) ||
      header.recordBytes != sizeof(Observation) ||
      header.tickSizeRaw <= 0 || header.sessionId == 0u ||
      header.startedRealtimeNs == 0u || !symbolTerminated ||
      header.symbol.data[0] == '\0' || (header.captureFlags & ~1u) != 0u ||
      header.reserved0 != 0u || header.reserved1 != 0u ||
      !validWire(header.bookTickerWireRaw) ||
      !validWire(header.tradeWireRaw) ||
      !validStreamingTransport(header.bookTickerTransportRaw) ||
      !validStreamingTransport(header.tradeTransportRaw) ||
      header.bookTickerParserContractRaw != static_cast<std::uint8_t>(
          CaptureParserContract::BookTickerRuntimeV1) ||
      header.tradeParserContractRaw != static_cast<std::uint8_t>(
          CaptureParserContract::TradeRuntimeV1)) {
    return false;
  }
  if (depth) {
    return validWire(header.depthWireRaw) &&
           validStreamingTransport(header.depthTransportRaw) &&
           header.depthParserContractRaw == static_cast<std::uint8_t>(
               CaptureParserContract::DepthRuntimeV1);
  }
  return header.depthWireRaw == 0u && header.depthLaneId == 0u &&
         header.depthTransportRaw == 0u &&
         header.depthParserContractRaw == static_cast<std::uint8_t>(
             CaptureParserContract::None);
}

[[nodiscard]] bool validFooter(const CaptureFooter& footer,
                               std::uint64_t recordCount,
                               const CaptureHeader& header) noexcept {
  return footer.magic == kCaptureCompleteMagic &&
         footer.schemaVersion == kCaptureSchemaVersion &&
         footer.footerBytes == sizeof(CaptureFooter) &&
         footer.recordCount == recordCount &&
         footer.completedRealtimeNs != 0u &&
         footer.completedRealtimeNs >= header.startedRealtimeNs &&
         zeroBytes(reinterpret_cast<const std::uint8_t*>(footer.reserved),
                   sizeof(footer.reserved));
}

[[nodiscard]] bool validTimestampOrigin(TimestampOrigin origin) noexcept {
  return origin == TimestampOrigin::Unknown ||
         origin == TimestampOrigin::Exchange ||
         origin == TimestampOrigin::Receive;
}

[[nodiscard]] bool validObservation(const Observation& row) noexcept {
  if (row.sequence == 0u || !validTimestampOrigin(row.timestampOrigin) ||
      !zeroBytes(row.reserved, sizeof(row.reserved)) ||
      (row.timestampOrigin == TimestampOrigin::Exchange &&
       row.exchangeTimestampNs == 0u)) {
    return false;
  }
  if (row.kind == ObservationKind::BookTickerSide) {
    const bool upsert = row.action == 1u;
    const bool deletion = row.action == 2u;
    return (row.side == 1u || row.side == 2u) &&
           (upsert || deletion) && row.coalesceNext <= 1u &&
           row.bidPriceRaw == 0 && row.bidQtyRaw == 0 &&
           row.askPriceRaw == 0 && row.askQtyRaw == 0 &&
           ((upsert && row.priceRaw > 0 && row.qtyRaw >= 0) ||
            (deletion && row.priceRaw >= 0 && row.qtyRaw == 0));
  }
  if (row.kind == ObservationKind::Trade) {
    return (row.side == 1u || row.side == 2u) && row.action == 0u &&
           row.coalesceNext == 0u && row.priceRaw > 0 && row.qtyRaw > 0 &&
           row.bidPriceRaw == 0 && row.bidQtyRaw == 0 &&
           row.askPriceRaw == 0 && row.askQtyRaw == 0;
  }
  if (row.kind == ObservationKind::DepthBbo) {
    return row.side == 0u && row.action == 0u &&
           row.coalesceNext == 0u && row.priceRaw == 0 &&
           row.qtyRaw == 0 && row.bidPriceRaw > 0 &&
           row.askPriceRaw > row.bidPriceRaw && row.bidQtyRaw >= 0 &&
           row.askQtyRaw >= 0;
  }
  if (row.kind == ObservationKind::Reset) {
    return row.eventId == 0u && row.side == 0u && row.action == 0u &&
           row.coalesceNext == 0u && row.bidPriceRaw == 0 &&
           row.bidQtyRaw == 0 && row.askPriceRaw == 0 &&
           row.askQtyRaw == 0 && row.priceRaw == 0 && row.qtyRaw == 0;
  }
  return false;
}

}  // namespace

bool analyzeCapture(const std::filesystem::path& capturePath,
                    CaptureAnalysis& output,
                    std::string& error) noexcept {
  output = CaptureAnalysis{};
  try {
    std::error_code filesystemError;
    const auto bytes = std::filesystem::file_size(capturePath,
                                                   filesystemError);
    if (filesystemError) {
      error = "capture_stat:" + filesystemError.message();
      return false;
    }
    if (bytes > kMaximumCaptureBytes) {
      error = "capture_size_capacity_exceeded";
      return false;
    }
    if (bytes < sizeof(CaptureHeader) + sizeof(CaptureFooter) ||
        (bytes - sizeof(CaptureHeader) - sizeof(CaptureFooter)) %
                sizeof(Observation) != 0u) {
      error = "capture_size_not_schema_aligned";
      return false;
    }
    const auto recordCount = static_cast<std::uint64_t>(
        (bytes - sizeof(CaptureHeader) - sizeof(CaptureFooter)) /
        sizeof(Observation));
    if (recordCount > kMaximumCaptureRecords) {
      error = "capture_record_capacity_exceeded";
      return false;
    }
    std::ifstream input{capturePath, std::ios::binary};
    if (!input) {
      error = "capture_open_failed";
      return false;
    }
    input.read(reinterpret_cast<char*>(&output.header),
               sizeof(output.header));
    if (!input || !validHeader(output.header) ||
        !validateCaptureHeaderProvenance(output.header)) {
      error = "capture_header_unsupported";
      return false;
    }
    input.seekg(static_cast<std::streamoff>(
                    bytes - sizeof(CaptureFooter)),
                std::ios::beg);
    input.read(reinterpret_cast<char*>(&output.footer),
               sizeof(output.footer));
    if (!input || !validFooter(output.footer, recordCount, output.header)) {
      error = "capture_footer_unsupported";
      return false;
    }
    input.clear();
    input.seekg(static_cast<std::streamoff>(sizeof(CaptureHeader)),
                std::ios::beg);
    if (!input) {
      error = "capture_seek_failed";
      return false;
    }

    Analyzer strict{};
    Analyzer hybrid{};
    Analyzer receive{};
    if (!strict.configure(output.header.tickSizeRaw,
                          ReconstructionPolicy::StrictExchange) ||
        !hybrid.configure(output.header.tickSizeRaw,
                          ReconstructionPolicy::HybridEligibility) ||
        !receive.configure(
            output.header.tickSizeRaw,
            ReconstructionPolicy::ReceiveOrderCounterfactual)) {
      error = "capture_tick_invalid";
      return false;
    }
    Observation observation{};
    std::uint64_t expectedSequence = 1u;
    std::uint64_t lastReceiveTimestamp = 0u;
    for (std::uint64_t index = 0u; index < recordCount; ++index) {
      input.read(reinterpret_cast<char*>(&observation), sizeof(observation));
      if (!input || observation.sequence != expectedSequence ||
          !validObservation(observation) ||
          (lastReceiveTimestamp != 0u &&
           observation.receiveMonotonicNs != 0u &&
           observation.receiveMonotonicNs < lastReceiveTimestamp)) {
        error = "capture_record_invalid";
        return false;
      }
      if (observation.receiveMonotonicNs != 0u) {
        lastReceiveTimestamp = observation.receiveMonotonicNs;
      }
      (void)strict.apply(observation);
      (void)hybrid.apply(observation);
      (void)receive.apply(observation);
      ++expectedSequence;
    }
    output.strict = strict.counters();
    output.hybrid = hybrid.counters();
    output.receiveCounterfactual = receive.counters();
    return true;
  } catch (const std::exception& exception) {
    error = exception.what();
  } catch (...) {
    error = "capture_analysis_unknown_exception";
  }
  return false;
}

bool writeMarkdownReport(const std::filesystem::path& path,
                         const CaptureAnalysis& analysis,
                         std::string& error) noexcept {
  try {
    std::error_code filesystemError;
    if (std::filesystem::exists(path, filesystemError)) {
      error = "report_output_exists";
      return false;
    }
    if (filesystemError) {
      error = "report_output_stat:" + filesystemError.message();
      return false;
    }
    std::ofstream output{path, std::ios::out};
    if (!output) {
      error = "report_output_open_failed";
      return false;
    }
    output << "# BBO reconstruction report\n\n"
           << "- exchange_raw: "
           << static_cast<unsigned>(analysis.header.exchangeRaw) << "\n"
           << "- market_raw: "
           << static_cast<unsigned>(analysis.header.marketRaw) << "\n"
           << "- api_protocol_profile_raw: "
           << static_cast<unsigned>(
                  analysis.header.apiProtocolProfileRaw) << "\n"
           << "- symbol: " << analysis.header.symbol.data << "\n"
           << "- tick_size_raw: " << analysis.header.tickSizeRaw << "\n"
           << "- capture_schema: " << analysis.header.schemaVersion << "\n"
           << "- completed_records: " << analysis.footer.recordCount << "\n"
           << "- book_ticker_wire_raw: "
           << static_cast<unsigned>(analysis.header.bookTickerWireRaw) << "\n"
           << "- trade_wire_raw: "
           << static_cast<unsigned>(analysis.header.tradeWireRaw) << "\n"
           << "- depth_wire_raw: "
           << static_cast<unsigned>(analysis.header.depthWireRaw) << "\n"
           << "- book_ticker_transport_raw: "
           << static_cast<unsigned>(analysis.header.bookTickerTransportRaw)
           << "\n- trade_transport_raw: "
           << static_cast<unsigned>(analysis.header.tradeTransportRaw)
           << "\n- depth_transport_raw: "
           << static_cast<unsigned>(analysis.header.depthTransportRaw)
           << "\n- book_ticker_lane_id: "
           << analysis.header.bookTickerLaneId
           << "\n- trade_lane_id: " << analysis.header.tradeLaneId
           << "\n- depth_lane_id: " << analysis.header.depthLaneId << "\n\n"
           << "The strict row is production-eligible evidence. Hybrid and "
              "receive-order rows are diagnostic counterfactuals only.\n\n"
           << "| policy | trades | accepted | rejected | direct | reverse | "
              "nibbling | full nibbling | next-BBO confirms | next-BBO "
              "contradictions | depth confirms | depth contradictions |\n"
           << "|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|\n";
    writeCounters(output, "strict_exchange", analysis.strict);
    writeCounters(output, "hybrid_eligibility", analysis.hybrid);
    writeCounters(output, "receive_order_counterfactual",
                  analysis.receiveCounterfactual);
    output << "\n## Strict rejection evidence\n\n"
           << "- missing_timestamp: " << analysis.strict.missingTimestamp
           << "\n- equal_book_ticker_timestamp: "
           << analysis.strict.equalBookTickerTimestamp
           << "\n- timestamp_rollback: "
           << analysis.strict.timestampRollback
           << "\n- unknown_aggressor: "
           << analysis.strict.unknownAggressor
           << "\n- terminal_resets: "
           << analysis.strict.terminalResets << "\n";
    output.flush();
    if (!output) {
      error = "report_write_failed";
      return false;
    }
    return true;
  } catch (const std::exception& exception) {
    error = exception.what();
  } catch (...) {
    error = "report_unknown_exception";
  }
  return false;
}

}  // namespace exchange_probe::reconstruction
