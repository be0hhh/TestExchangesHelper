#include "exchange_probe/reconstruction/campaign.hpp"

#include "exchange_probe/reconstruction/report.hpp"
#include "exchange_probe/reconstruction/selection.hpp"

#include "api/builder/UnifiedBuilder.hpp"
#include "api/dispatch/BuildDispatch.hpp"
#include "api/instrument/InstrumentCatalog.hpp"
#include "api/run/RunByConfig.hpp"
#include "api/ticker/Ticker24hCatalog.hpp"
#include "api/ticker/Ticker24hQuality.hpp"
#include "network/rest/FetchRest.hpp"
#include "parse/decimal_to_scaled.hpp"
#include "primitives/buf/BufferView.hpp"
#include "primitives/buf/CanonConstants.hpp"
#include "primitives/buf/CStringPrimitives.hpp"
#include "primitives/buf/MessageBuffer.hpp"
#include "primitives/buf/StringView.hpp"
#include "primitives/composite/InstrumentInfoRow.hpp"

#include <cstdio>
#include <exception>
#include <memory>
#include <string>
#include <vector>

namespace exchange_probe::reconstruction {
namespace {

inline constexpr std::size_t kMaximumUniverseRows = 65'536u;
inline constexpr std::size_t kMaximumUniversePages = 64u;
inline constexpr std::size_t kMaximumReferenceResponseBytes = 32u << 20u;

[[nodiscard]] bool sameSymbol(const Symbol& lhs,
                              const Symbol& rhs) noexcept {
  return cxet::bytescan::hftStrncmp(
             lhs.data, rhs.data, Symbol::capacity) == 0;
}

[[nodiscard]] bool copyText(char* destination, std::size_t capacity,
                            const char* source) noexcept {
  if (!destination || capacity == 0u || !source) return false;
  const auto length = cxet::bytescan::hftStrnlen(source, capacity);
  if (length == capacity) return false;
  cxet::bytescan::hftMemcpy(destination, source, length);
  destination[length] = '\0';
  return true;
}

[[nodiscard]] bool fetchUniverse(
    const ProductEligibility& product,
    std::vector<cxet::api::InstrumentUniverseRow>& output,
    std::string& error) {
  cxet::api::InstrumentUniverseSnapshotProvider provider{};
  if (!cxet::api::stagedInstrumentUniverseSnapshotProvider(
          product.exchange, product.market, &provider) ||
      !cxet::api::validInstrumentUniverseSnapshotProvider(provider)) {
    error = "instrument_universe_provider_unavailable";
    return false;
  }
  if ((provider.mode != cxet::api::InstrumentUniverseSourceMode::RestSnapshot &&
       provider.mode != cxet::api::InstrumentUniverseSourceMode::Hybrid) ||
      provider.restAuth !=
          cxet::api::InstrumentUniverseRestAuth::PublicUnauthenticated) {
    error = "instrument_universe_transport_not_supported_by_probe";
    return false;
  }

  auto pageRows = std::make_unique<cxet::api::InstrumentUniverseRow[]>(
      cxet::api::kInstrumentCatalogMaximumPageRows);
  std::vector<char> response(kMaximumReferenceResponseBytes);
  MessageBuffer requestPayload{};
  cxet::api::InstrumentCatalogPageRequest request{};
  request.exchange = product.exchange;
  request.market = product.market;
  request.apiProtocolProfile = product.apiProtocolProfile;
  for (std::size_t page = 0u; page < kMaximumUniversePages; ++page) {
    requestPayload.setSize(0u);
    if (!provider.buildSnapshotPage(request, requestPayload)) {
      error = "instrument_universe_request_build_failed";
      return false;
    }
    std::size_t responseBytes = 0u;
    bool fetched = false;
    if (provider.restMethod == cxet::api::InstrumentUniverseRestMethod::Get &&
        provider.restPayloadPlacement ==
            cxet::api::InstrumentUniverseRestPayloadPlacement::RequestTarget) {
      fetched = cxet::network::rest::fetchRest(
          provider.restHost, provider.restPort, requestPayload.data(),
          requestPayload.size(), response.data(), response.size(),
          &responseBytes, product.exchange.raw);
    } else if (
        provider.restMethod == cxet::api::InstrumentUniverseRestMethod::Post &&
        provider.restPayloadPlacement ==
            cxet::api::InstrumentUniverseRestPayloadPlacement::JsonBody) {
      int httpStatus = 0;
      fetched = cxet::network::rest::postRestWithContentType(
          provider.restHost, provider.restPort, provider.restFixedPath,
          provider.restFixedPathSize, requestPayload, response.data(),
          response.size(), &responseBytes, nullptr, 0u,
          "application/json", &httpStatus, product.exchange.raw);
    }
    if (!fetched || responseBytes == 0u) {
      error = "instrument_universe_fetch_failed";
      return false;
    }
    cxet::BufferView body{response.data(), responseBytes, response.size()};
    cxet::api::InstrumentCatalogPageResult parsed{};
    if (!provider.parseSnapshotPage(
            body, request, pageRows.get(),
            cxet::api::kInstrumentCatalogMaximumPageRows, parsed) ||
        !cxet::api::validInstrumentCatalogPageResult(provider, parsed) ||
        output.size() + parsed.rowCount > kMaximumUniverseRows) {
      error = "instrument_universe_parse_failed";
      return false;
    }
    output.insert(output.end(), pageRows.get(),
                  pageRows.get() + parsed.rowCount);
    if (!parsed.hasMore) return !output.empty();
    if (!copyText(request.cursor, sizeof(request.cursor), parsed.nextCursor)) {
      error = "instrument_universe_cursor_invalid";
      return false;
    }
  }
  error = "instrument_universe_page_bound";
  return false;
}

struct TickerSnapshot final {
  std::vector<cxet::composite::Ticker24hRow> rows{};
  std::vector<cxet::api::ticker::Ticker24hAcceptedQuality> quality{};
};

[[nodiscard]] bool fetchTickerSnapshot(const ProductEligibility& product,
                                       TickerSnapshot& output,
                                       std::string& error) {
  using namespace cxet::api::ticker;
  Ticker24hDescriptor descriptor{};
  if (!stagedTicker24hDescriptor(
          product.exchange, product.market, Ticker24hScope::Batch,
          &descriptor, product.apiProtocolProfile) ||
      !validTicker24hDescriptor(descriptor) ||
      !stagedTicker24hQualityParser(
          product.exchange, product.market, Ticker24hScope::Batch) ||
      descriptor.restAuth !=
          cxet::api::reference::RestAuth::PublicUnauthenticated ||
      descriptor.maxRows == 0u || descriptor.maxResponseBytes == 0u ||
      descriptor.maxRows > kMaximumUniverseRows ||
      descriptor.maxResponseBytes > kMaximumReferenceResponseBytes) {
    error = "exact_ticker24h_unavailable";
    return false;
  }
  Ticker24hRequest request{};
  request.exchange = product.exchange;
  request.market = product.market;
  request.scope = Ticker24hScope::Batch;
  request.apiProtocolProfile = product.apiProtocolProfile;
  MessageBuffer payload{};
  if (!buildTicker24hRequest(request, payload)) {
    error = "ticker24h_request_build_failed";
    return false;
  }
  std::vector<char> response(descriptor.maxResponseBytes);
  std::size_t responseBytes = 0u;
  bool fetched = false;
  if (descriptor.restMethod == cxet::api::reference::RestMethod::Get &&
      descriptor.restPayloadPlacement ==
          cxet::api::reference::RestPayloadPlacement::RequestTarget) {
    fetched = cxet::network::rest::fetchRest(
        descriptor.host.data, descriptor.port, payload.data(), payload.size(),
        response.data(), response.size(), &responseBytes,
        product.exchange.raw);
  } else if (descriptor.restMethod ==
                 cxet::api::reference::RestMethod::Post &&
             descriptor.restPayloadPlacement ==
                 cxet::api::reference::RestPayloadPlacement::JsonBody) {
    int httpStatus = 0;
    fetched = cxet::network::rest::postRestWithContentType(
        descriptor.host.data, descriptor.port, descriptor.pathTemplate.data,
        descriptor.pathTemplate.size, payload, response.data(),
        response.size(), &responseBytes, nullptr, 0u, "application/json",
        &httpStatus, product.exchange.raw);
  }
  if (!fetched || responseBytes == 0u) {
    error = "ticker24h_fetch_failed";
    return false;
  }
  output.rows.resize(descriptor.maxRows);
  output.quality.resize(descriptor.maxRows);
  std::vector<Ticker24hRejectedSymbol> rejected(descriptor.maxRows);
  cxet::BufferView body{response.data(), responseBytes, response.size()};
  Ticker24hQualityParseResult parsed{};
  if (!parseTicker24hQualityResponse(
          request, body, output.rows.data(), output.quality.data(),
          output.rows.size(), rejected.data(), rejected.size(), parsed)) {
    error = "ticker24h_parse_failed";
    return false;
  }
  output.rows.resize(parsed.rowCount);
  output.quality.resize(parsed.rowCount);
  return true;
}

[[nodiscard]] const cxet::api::InstrumentUniverseRow* findUniverse(
    const std::vector<cxet::api::InstrumentUniverseRow>& universe,
    const cxet::composite::InstrumentInfoRow& metadata) noexcept {
  for (const auto& row : universe) {
    if ((metadata.hasLocalSymbol &&
         sameSymbol(row.localSymbol, metadata.localSymbol)) ||
        (metadata.hasNativeSymbol &&
         sameSymbol(row.nativeSymbol, metadata.nativeSymbol)) ||
        sameSymbol(row.nativeSymbol, metadata.symbol) ||
        sameSymbol(row.localSymbol, metadata.symbol)) {
      return &row;
    }
  }
  return nullptr;
}

void attachTicker(const TickerSnapshot& ticker,
                  const cxet::api::InstrumentUniverseRow& universe,
                  InstrumentCandidate& candidate) noexcept {
  for (std::size_t index = 0u; index < ticker.rows.size(); ++index) {
    if (!sameSymbol(ticker.rows[index].symbol, universe.nativeSymbol) &&
        !sameSymbol(ticker.rows[index].symbol, universe.localSymbol)) {
      continue;
    }
    candidate.quoteVolumeRaw = ticker.rows[index].quoteVolume.raw;
    candidate.exactQuoteVolume =
        ticker.quality[index].quoteVolume.state ==
        cxet::api::ticker::Ticker24hMetricState::Exact;
    return;
  }
}

[[nodiscard]] bool buildCandidates(
    const ProductEligibility& product,
    std::vector<InstrumentCandidate>& candidates,
    bool& exactTickerCapability,
    std::string& error) {
  std::vector<cxet::api::InstrumentUniverseRow> universe;
  if (!fetchUniverse(product, universe, error)) return false;

  constexpr std::size_t kMetadataCapacity = 4096u;
  auto metadata = std::make_unique<cxet::composite::InstrumentInfoRow[]>(
      kMetadataCapacity);
  MessageBuffer request{};
  cxet::UnifiedRequestBuilder builder{};
  builder.protocolProfile(product.apiProtocolProfile);
  std::size_t metadataCount = 0u;
  const char* metadataFailure = nullptr;
  if (!cxet::api::runFetchExchangeInfoByConfig(
          builder, request, product.exchange, product.market, metadata.get(),
          kMetadataCapacity, &metadataCount, &metadataFailure)) {
    error = metadataFailure ? metadataFailure
                            : "instrument_rules_fetch_failed";
    return false;
  }

  TickerSnapshot ticker{};
  exactTickerCapability = product.ticker24hAvailable;
  if (exactTickerCapability &&
      !fetchTickerSnapshot(product, ticker, error)) {
    return false;
  }
  candidates.reserve(metadataCount);
  for (std::size_t index = 0u; index < metadataCount; ++index) {
    const auto* universeRow = findUniverse(universe, metadata[index]);
    if (!universeRow ||
        !cxet::api::instrumentUniverseDefaultEligible(*universeRow) ||
        universeRow->baseMultiplier != 1u ||
        metadata[index].baseMultiplier != 1u) {
      continue;
    }
    std::int64_t tickSizeRaw = 0;
    if (!cxet::parse::parseUnsignedScaledDecimalStrict(
            cxet::StringView::fromCString(metadata[index].tickSize),
            numeric::kPriceScaleDigits, &tickSizeRaw) ||
        tickSizeRaw <= 0) {
      continue;
    }
    InstrumentCandidate candidate{};
    candidate.symbol = universeRow->localSymbol;
    candidate.tickSizeRaw = tickSizeRaw;
    candidate.active = true;
    if (!copyText(candidate.baseAsset, sizeof(candidate.baseAsset),
                  universeRow->baseAsset.data) ||
        !copyText(candidate.quoteAsset, sizeof(candidate.quoteAsset),
                  universeRow->quoteAsset.data)) {
      continue;
    }
    if (exactTickerCapability) attachTicker(ticker, *universeRow, candidate);
    candidates.push_back(candidate);
  }
  return !candidates.empty();
}

[[nodiscard]] std::string productPrefix(const ProductEligibility& product) {
  char buffer[64]{};
  (void)std::snprintf(
      buffer, sizeof(buffer), "e%u_m%u_p%u",
      static_cast<unsigned>(product.exchange.raw),
      static_cast<unsigned>(product.market.raw),
      static_cast<unsigned>(product.apiProtocolProfile.raw));
  return buffer;
}

}  // namespace

bool runCampaign(const CampaignOptions& options,
                 CampaignResult& result) noexcept {
  result = CampaignResult{};
  if (options.outputRoot.empty() || options.sessions == 0u ||
      options.sessions > kMaximumCampaignSessions ||
      options.duration.count() <= 0 ||
      options.duration.count() > kMaximumCaptureSeconds) {
    result.productFailures.emplace_back("campaign_options_invalid");
    return false;
  }
  try {
    result.entries.reserve(
        kMaximumProducts * kMaximumSelectedSymbols * options.sessions);
    result.productFailures.reserve(kMaximumProducts + 1u);
    cxet::initBuildDispatch();
    std::error_code filesystemError;
    if (!std::filesystem::create_directories(options.outputRoot,
                                             filesystemError) &&
        filesystemError) {
      result.productFailures.push_back(
          "campaign_output_create:" + filesystemError.message());
      return false;
    }
    result.catalog = buildProductCatalog();
    if (result.catalog.failure != ProductEligibilityReason::Eligible) {
      result.productFailures.emplace_back("public_market_catalog_unavailable");
      return false;
    }
    for (std::size_t productIndex = 0u;
         productIndex < result.catalog.count; ++productIndex) {
      const auto& product = result.catalog.rows[productIndex];
      if (!product.eligible()) continue;
      if (!product.instrumentRulesAvailable) {
        result.productFailures.push_back(
            productPrefix(product) + ":instrument_rules_unavailable");
        continue;
      }
      std::vector<InstrumentCandidate> candidates;
      bool exactTicker = false;
      std::string productError;
      if (!buildCandidates(product, candidates, exactTicker, productError)) {
        result.productFailures.push_back(
            productPrefix(product) + ":" + productError);
        continue;
      }
      const auto selected = selectCampaignInstruments(
          candidates.data(), candidates.size(), exactTicker);
      if (!selected.ethAvailable) {
        result.productFailures.push_back(
            productPrefix(product) + ":eth_usdt_unavailable");
        continue;
      }
      for (std::size_t symbolIndex = 0u; symbolIndex < selected.count;
           ++symbolIndex) {
        const auto& instrument = selected.rows[symbolIndex];
        for (std::size_t session = 1u; session <= options.sessions; ++session) {
          CampaignEntry entry{};
          entry.product = product;
          entry.symbol = instrument.symbol;
          entry.session = session;
          const std::string stem = productPrefix(product) + "_" +
              instrument.symbol.data + "_s" + std::to_string(session);
          entry.capturePath = options.outputRoot / (stem + ".bbor");
          entry.reportPath = options.outputRoot / (stem + ".md");
          CaptureOptions captureOptions{};
          captureOptions.product = product;
          captureOptions.symbol = instrument.symbol;
          captureOptions.tickSizeRaw = instrument.tickSizeRaw;
          captureOptions.outputPath = entry.capturePath;
          captureOptions.duration = options.duration;
          captureOptions.includeDepth = options.includeDepth;
          CaptureResult captureResult{};
          if (!capture(captureOptions, captureResult)) {
            entry.error = captureResult.error;
            result.entries.push_back(std::move(entry));
            continue;
          }
          CaptureAnalysis analysis{};
          if (!analyzeCapture(entry.capturePath, analysis, entry.error) ||
              !writeMarkdownReport(entry.reportPath, analysis,
                                   entry.error)) {
            result.entries.push_back(std::move(entry));
            continue;
          }
          entry.captured = true;
          result.entries.push_back(std::move(entry));
        }
      }
    }
    return !result.entries.empty();
  } catch (const std::exception& exception) {
    result.productFailures.push_back(exception.what());
  } catch (...) {
    result.productFailures.emplace_back("campaign_unknown_exception");
  }
  return false;
}

}  // namespace exchange_probe::reconstruction
