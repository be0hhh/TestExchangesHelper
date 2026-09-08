#include "exchange_probe/reconstruction/Campaign.hpp"
#include "exchange_probe/reconstruction/Catalog.hpp"
#include "exchange_probe/reconstruction/Report.hpp"

#include <charconv>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <string_view>

namespace {

using namespace exchange_probe::reconstruction;

void usage() {
  std::cerr
      << "usage:\n"
      << "  exchange-bbo-reconstruction-probe catalog\n"
      << "  exchange-bbo-reconstruction-probe analyze CAPTURE REPORT\n"
      << "  exchange-bbo-reconstruction-probe capture OUTPUT SYMBOL "
         "TICK_RAW EXCHANGE_RAW MARKET_RAW PROFILE_RAW SECONDS [--depth]\n"
      << "  exchange-bbo-reconstruction-probe campaign OUTPUT_ROOT "
         "[SESSIONS] [SECONDS] [--no-depth]\n";
}

template <typename Integer>
[[nodiscard]] bool parseInteger(const char* text, Integer& output) noexcept {
  if (!text || *text == '\0') return false;
  const std::string_view value{text};
  const auto parsed = std::from_chars(
      value.data(), value.data() + value.size(), output);
  return parsed.ec == std::errc{} &&
         parsed.ptr == value.data() + value.size();
}

[[nodiscard]] const ProductEligibility* findProduct(
    const ProductCatalog& catalog, std::uint8_t exchange,
    std::uint8_t market, std::uint8_t profile) noexcept {
  for (std::size_t index = 0u; index < catalog.count; ++index) {
    const auto& row = catalog.rows[index];
    if (row.exchange.raw == exchange && row.market.raw == market &&
        row.apiProtocolProfile.raw == profile) {
      return &row;
    }
  }
  return nullptr;
}

int catalogCommand() {
  const auto catalog = buildProductCatalog();
  if (catalog.failure != ProductEligibilityReason::Eligible) {
    std::cerr << "catalog_error="
              << productEligibilityReasonName(catalog.failure) << '\n';
    return 1;
  }
  for (std::size_t index = 0u; index < catalog.count; ++index) {
    const auto& row = catalog.rows[index];
    std::cout << "exchange_raw=" << static_cast<unsigned>(row.exchange.raw)
              << " market_raw=" << static_cast<unsigned>(row.market.raw)
              << " profile_raw="
              << static_cast<unsigned>(row.apiProtocolProfile.raw)
              << " eligibility="
              << productEligibilityReasonName(row.reason)
              << " instrument_rules=" << row.instrumentRulesAvailable
              << " ticker24h=" << row.ticker24hAvailable
              << " depth=" << row.depthRuntimeAvailable
              << " bbo_ts_required=" << row.bookTickerTimestampRequired
              << " trade_ts_required=" << row.tradeTimestampRequired << '\n';
  }
  return 0;
}

int analyzeCommand(const char* capturePath, const char* reportPath) {
  CaptureAnalysis analysis{};
  std::string error;
  if (!analyzeCapture(capturePath, analysis, error) ||
      !writeMarkdownReport(reportPath, analysis, error)) {
    std::cerr << "analysis_error=" << error << '\n';
    return 1;
  }
  std::cout << "report=" << reportPath
            << " strict_accepted=" << analysis.strict.acceptedTrades
            << " strict_rejected=" << analysis.strict.rejectedTrades << '\n';
  return 0;
}

int captureCommand(int argc, char** argv) {
  if (argc != 9 && argc != 10) {
    usage();
    return 2;
  }
  std::int64_t tick = 0;
  std::uint32_t exchange = 0u;
  std::uint32_t market = 0u;
  std::uint32_t profile = 0u;
  std::uint64_t seconds = 0u;
  if (!parseInteger(argv[4], tick) || !parseInteger(argv[5], exchange) ||
      !parseInteger(argv[6], market) || !parseInteger(argv[7], profile) ||
      !parseInteger(argv[8], seconds) || exchange > 255u || market > 255u ||
      profile > 255u || seconds == 0u ||
      seconds > static_cast<std::uint64_t>(kMaximumCaptureSeconds) ||
      (argc == 10 && std::string_view{argv[9]} != "--depth")) {
    std::cerr << "capture_arguments_invalid\n";
    return 2;
  }
  const auto catalog = buildProductCatalog();
  const auto* product = findProduct(
      catalog, static_cast<std::uint8_t>(exchange),
      static_cast<std::uint8_t>(market),
      static_cast<std::uint8_t>(profile));
  if (!product || !product->eligible()) {
    std::cerr << "capture_product_not_eligible\n";
    return 1;
  }
  CaptureOptions options{};
  options.outputPath = argv[2];
  if (!options.symbol.copyFrom(argv[3])) {
    std::cerr << "capture_symbol_invalid\n";
    return 2;
  }
  options.product = *product;
  options.tickSizeRaw = tick;
  options.duration = std::chrono::seconds{seconds};
  options.includeDepth = argc == 10;
  CaptureResult result{};
  if (!capture(options, result)) {
    std::cerr << "capture_error=" << result.error
              << " observations=" << result.observations << '\n';
    return 1;
  }
  std::cout << "capture=" << options.outputPath.string()
            << " observations=" << result.observations
            << " bbo_sides=" << result.bookTickerSides
            << " trades=" << result.trades
            << " depth_bbos=" << result.depthBbos << '\n';
  return 0;
}

int campaignCommand(int argc, char** argv) {
  if (argc < 3 || argc > 6) {
    usage();
    return 2;
  }
  CampaignOptions options{};
  options.outputRoot = argv[2];
  std::uint64_t sessions = 3u;
  std::uint64_t seconds = 600u;
  int index = 3;
  if (index < argc && std::string_view{argv[index]} != "--no-depth") {
    if (!parseInteger(argv[index++], sessions) || sessions == 0u ||
        sessions > kMaximumCampaignSessions) return 2;
  }
  if (index < argc && std::string_view{argv[index]} != "--no-depth") {
    if (!parseInteger(argv[index++], seconds) || seconds == 0u ||
        seconds > static_cast<std::uint64_t>(kMaximumCaptureSeconds)) {
      return 2;
    }
  }
  if (index < argc) {
    if (std::string_view{argv[index++]} != "--no-depth") return 2;
    options.includeDepth = false;
  }
  if (index != argc) return 2;
  options.sessions = static_cast<std::size_t>(sessions);
  options.duration = std::chrono::seconds{seconds};
  CampaignResult result{};
  const bool completed = runCampaign(options, result);
  std::size_t successful = 0u;
  for (const auto& entry : result.entries) {
    if (entry.captured) ++successful;
    std::cout << "symbol=" << entry.symbol.data
              << " session=" << entry.session
              << " captured=" << entry.captured;
    if (!entry.error.empty()) std::cout << " error=" << entry.error;
    std::cout << '\n';
  }
  for (const auto& failure : result.productFailures) {
    std::cerr << "product_failure=" << failure << '\n';
  }
  std::cout << "campaign_entries=" << result.entries.size()
            << " successful=" << successful << '\n';
  return completed && successful != 0u ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    usage();
    return 2;
  }
  const std::string_view command{argv[1]};
  if (command == "catalog" && argc == 2) return catalogCommand();
  if (command == "analyze" && argc == 4) {
    return analyzeCommand(argv[2], argv[3]);
  }
  if (command == "capture") return captureCommand(argc, argv);
  if (command == "campaign") return campaignCommand(argc, argv);
  usage();
  return 2;
}
