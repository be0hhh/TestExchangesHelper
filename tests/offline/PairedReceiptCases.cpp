#include "OfflineCase.hpp"
#include "exchange_probe/Research.hpp"
#include "../../src/Network/WsApplication.hpp"

#include <boost/json.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <sstream>
#include <string>

namespace {
using namespace exchange_probe;

struct Fixture {
  std::filesystem::path directory;
  std::string payloads;
  std::string index;
  unsigned count{0U};
  Fixture() {
    directory = std::filesystem::temp_directory_path() /
        ("exchange-probe-paired-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
    CXET_CHECK(std::filesystem::create_directory(directory));
  }
  ~Fixture() {
    std::error_code error;
    std::filesystem::remove_all(directory, error);
  }
  void write(std::string_view name, std::string_view value) {
    std::ofstream file{directory / name, std::ios::binary};
    file << value;
    CXET_CHECK(file.good());
  }
  void manifest(std::string_view standard_semantics = "none",
                std::string_view aws_semantics = "none",
                bool futures = false,
                bool payload_symbol = true) {
    boost::json::array channels;
    for (const auto* channel : {"standard", "aws"}) {
      channels.emplace_back(boost::json::object{
          {"id", channel}, {"kind", "trade"}, {"support", "exact"},
          {"transport", "ws"},
          {"host", std::string_view{channel} == "standard"
                       ? (futures ? "api.hbdm.com" : "api.huobi.pro")
                       : (futures ? "api.hbdm.vn" : "api-aws.huobi.pro")},
          {"port", 443}, {"path", futures ? "/linear-swap-ws" : "/ws"},
          {"subscribe", R"({"sub":"market.{symbol}.trade.detail","id":"probe"})"},
          {"wire", "json"}, {"compression", "none"},
          {"depth_semantics", std::string_view{channel} == "standard"
                                  ? standard_semantics : aws_semantics},
          {"mapping", boost::json::object{
              {"data", "tick.data"}, {"event_id", futures ? "id" : "tradeId"},
              {"symbol", payload_symbol ? "symbol" : ""}}}});
    }
    write("manifest.json", boost::json::serialize(boost::json::object{
        {"schema", kResearchBundleSchema}, {"schema_version", 3},
        {"venue", "htx"}, {"product", futures ? "futures" : "spot"},
        {"artifact_complete", true}, {"symbol", "AAA"},
        {"channels", std::move(channels)}}));
  }
  void add(std::string_view channel, std::string_view id,
           std::optional<std::uint64_t> receipt,
           std::uint64_t round = 1U, std::string_view symbol = "AAA",
           std::string_view previous = "", bool snapshot = false) {
    const auto payload = boost::json::serialize(boost::json::object{
        {"ch", "market." + std::string{symbol} + ".trade.detail"},
        {"tick", boost::json::object{{"data", boost::json::array{
            boost::json::object{{"tradeId", id}, {"id", "obsolete"},
                {"symbol", symbol}, {"prev", previous},
                {"snapshot", snapshot}}}}}}});
    addRaw(channel, payload, receipt, round);
  }
  void addRaw(std::string_view channel, const std::string& payload,
              std::optional<std::uint64_t> receipt, std::uint64_t round = 1U,
              bool binary = false, std::optional<unsigned> generation = std::nullopt) {
    boost::json::object row{
        {"schema", kResearchFrameSchema}, {"frame_id", count},
        {"round", round}, {"channel_id", channel},
        {"opcode", binary ? "binary" : "text"},
        {"monotonic_ns", 1'000'000U + count}, {"utc_ns", 2'000'000U + count},
        {"offset", payloads.size()}, {"length", payload.size()}};
    if (receipt.has_value()) row["received_monotonic_ns"] = *receipt;
    if (generation) {
      row["attempt"] = *generation;
      row["session_generation"] = *generation;
    }
    index += boost::json::serialize(row) + "\n";
    payloads += payload;
    ++count;
  }
  int analyze() {
    write("frames.bin", payloads);
    write("frames.jsonl", index);
    std::ostringstream output, errors;
    return analyze_paired_research_receipts(
        directory, "standard", "aws", output, errors);
  }
  boost::json::object report() {
    std::ifstream file{directory / "paired_receipts.json"};
    CXET_CHECK(file.good());
    return boost::json::parse(std::string{
        std::istreambuf_iterator<char>{file}, {}}).as_object();
  }
};

// Catches reversed sign, using persistence timestamps, and wrong quantiles.
void signedQuantiles() {
  Fixture f;
  f.manifest();
  f.add("standard", "1", 100U); f.add("aws", "1", 80U);
  f.add("standard", "2", 200U); f.add("aws", "2", 205U);
  f.add("standard", "3", 300U); f.add("aws", "3", 320U);
  CXET_CHECK(f.analyze() == 0);
  const auto report = f.report();
  CXET_CHECK(report.at("samples") == 3);
  const auto& delta = report.at("aws_minus_standard_ns").as_object();
  CXET_CHECK(delta.at("p50") == 5 && delta.at("p95") == 20);
  CXET_CHECK(delta.at("p99") == 20 && delta.at("jitter_p95_minus_p50") == 15);
  CXET_CHECK(!std::filesystem::exists(f.directory / "findings.json"));
}

// Catches joining foreign symbols/rounds and sampling duplicate IDs.
void identityScopeAndDuplicates() {
  Fixture f;
  f.manifest();
  f.add("standard", "1", 10U); f.add("aws", "1", 20U, 2U);
  f.add("standard", "2", 30U); f.add("aws", "2", 40U, 2U);
  f.add("standard", "3", 50U); f.add("aws", "3", 60U);
  f.add("aws", "3", 61U);
  f.add("standard", "4", 70U); f.add("aws", "4", 80U);
  CXET_CHECK(f.analyze() == 0);
  const auto report = f.report();
  CXET_CHECK(report.at("samples") == 1);
  CXET_CHECK(report.at("duplicate_records_aws") == 1);
  CXET_CHECK(report.at("ambiguous_identities") == 1);
  CXET_CHECK(report.at("unmatched_standard") == 2);
  CXET_CHECK(report.at("unmatched_aws") == 2);
}

// Catches fallback to legacy lock-time timestamps or invented common IDs.
void requiresReceiptAndNativeId() {
  Fixture f;
  f.manifest();
  f.add("standard", "", 30U); f.add("aws", "", 40U);
  CXET_CHECK(f.analyze() == 0);
  const auto report = f.report();
  CXET_CHECK(report.at("samples") == 0);
  CXET_CHECK(report.at("missing_native_id_records") == 2);
  CXET_CHECK(report.at("invalid_id_standard") == 1 && report.at("invalid_id_aws") == 1);
  CXET_CHECK(report.at("unmatched_standard") == 1 && report.at("unmatched_aws") == 1);
  CXET_CHECK(report.at("aws_minus_standard_ns").is_null());
}

void legacyTimestampRefusal() {
  Fixture f;
  f.manifest();
  f.add("standard", "1", std::nullopt); f.add("aws", "1", 20U);
  CXET_CHECK(f.analyze() == 2);
  CXET_CHECK(!std::filesystem::exists(f.directory / "paired_receipts.json"));
}

// Catches comparing snapshot cadence to an update stream.
void refusesIncompatibleSemantics() {
  Fixture f;
  f.manifest("complete_snapshot", "delta");
  f.add("standard", "1", 10U); f.add("aws", "1", 20U);
  CXET_CHECK(f.analyze() == 2);
  CXET_CHECK(!std::filesystem::exists(f.directory / "paired_receipts.json"));
}

// HTX Trade has no documented predecessor contract: invented gaps must refuse.
void refusesUnsupportedPredecessor() {
  Fixture f;
  f.manifest();
  std::ifstream input{f.directory / "manifest.json"};
  auto manifest = boost::json::parse(std::string{
      std::istreambuf_iterator<char>{input}, {}}).as_object();
  for (auto& channel : manifest.at("channels").as_array()) {
    channel.as_object().at("mapping").as_object()["previous_event_id"] = "prev";
  }
  f.write("manifest.json", boost::json::serialize(manifest));
  f.add("standard", "1", 10U); f.add("aws", "1", 20U);
  CXET_CHECK(f.analyze() == 2);
}

// Catches truncation or overflow when forming signed deltas.
void signedRangeRefusal() {
  Fixture f;
  f.manifest();
  f.add("standard", "1", 0U);
  f.add("aws", "1", UINT64_MAX);
  CXET_CHECK(f.analyze() == 2);
  CXET_CHECK(!std::filesystem::exists(f.directory / "paired_receipts.json"));
}

// Official-origin wire shape, synthetic trade values and local receipts. These
// cases prove offline identity extraction only, never real-provider operation.
// https://huobiapi.github.io/docs/spot/v1/en/#trade-detail
// https://huobiapi.github.io/docs/usdt_swap/v1/en/#general-subscribe-trade-detail-data
void officialOriginBatchIds() {
  for (const bool futures : {false, true}) {
    Fixture f;
    f.manifest("none", "none", futures, false);
    const std::string payload = futures
        ? R"({"ch":"market.AAA.trade.detail","tick":{"data":[{"id":101,"ts":1,"price":10,"amount":2,"direction":"buy"},{"id":102,"ts":2,"price":11,"amount":3,"direction":"sell"}]}})"
        : R"({"ch":"market.AAA.trade.detail","tick":{"data":[{"tradeId":101,"id":999,"ts":1,"price":10,"amount":2,"direction":"buy"},{"tradeId":102,"id":999,"ts":2,"price":11,"amount":3,"direction":"sell"}]}})";
    f.addRaw("standard", payload, 50U);
    f.addRaw("aws", payload, 55U);
    CXET_CHECK(f.analyze() == 0);
    const auto report = f.report();
    CXET_CHECK(report.at("samples") == 2 && report.at("ambiguous_identities") == 0);
    CXET_CHECK(report.at("aws_minus_standard_ns").as_object().at("p50") == 5);
  }
  Fixture f;
  f.manifest();
  std::ifstream input{f.directory / "manifest.json"};
  auto manifest = boost::json::parse(std::string{
      std::istreambuf_iterator<char>{input}, {}}).as_object();
  for (auto& channel : manifest.at("channels").as_array()) {
    channel.as_object()["compression"] = "gzip";
  }
  f.write("manifest.json", boost::json::serialize(manifest));
  // Fixed gzip member of the same official-origin spot trade shape.
  const char bytes[] = "\x1f\x8b\x08\x00\x00\x00\x00\x00\x00\x03\x01\x4f\x00\xb0\xff\x7b\x22\x63\x68\x22\x3a\x22\x6d\x61\x72\x6b\x65\x74\x2e\x41\x41\x41\x2e\x74\x72\x61\x64\x65\x2e\x64\x65\x74\x61\x69\x6c\x22\x2c\x22\x74\x69\x63\x6b\x22\x3a\x7b\x22\x64\x61\x74\x61\x22\x3a\x5b\x7b\x22\x74\x72\x61\x64\x65\x49\x64\x22\x3a\x31\x2c\x22\x73\x79\x6d\x62\x6f\x6c\x22\x3a\x22\x41\x41\x41\x22\x7d\x5d\x7d\x7d\x1a\xf7\xb5\x93\x4f\x00\x00\x00";
  const std::string gzip{bytes, sizeof(bytes) - 1U};
  f.addRaw("standard", R"({"ping":123})", 1U);
  f.addRaw("standard", gzip, 10U, 1U, true);
  f.addRaw("aws", gzip, 20U, 1U, true);
  CXET_CHECK(f.analyze() == 0);
  CXET_CHECK(f.report().at("samples") == 1);
}

void refusesUnknownNativeScope() {
  Fixture f;
  f.manifest();
  // Same numeric ID in an arbitrary venue is insufficient scope evidence.
  std::ifstream input{f.directory / "manifest.json"};
  auto manifest = boost::json::parse(std::string{
      std::istreambuf_iterator<char>{input}, {}}).as_object();
  manifest["venue"] = "other";
  f.write("manifest.json", boost::json::serialize(manifest));
  f.add("standard", "1", 10U); f.add("aws", "1", 20U);
  CXET_CHECK(f.analyze() == 2);
}

void refusesWrongSymbol() {
  for (const bool wrong_channel : {false, true}) {
    Fixture f;
    f.manifest();
    f.add("standard", "1", 10U);
    const std::string payload = wrong_channel
        ? R"({"ch":"market.BBB.trade.detail","tick":{"data":[{"tradeId":1,"symbol":"AAA"}]}})"
        : R"({"ch":"market.AAA.trade.detail","tick":{"data":[{"tradeId":1,"symbol":"BBB"}]}})";
    f.addRaw("aws", payload, 20U);
    CXET_CHECK(f.analyze() == 2);
    CXET_CHECK(!std::filesystem::exists(f.directory / "paired_receipts.json"));
  }
}

void eventCapacityRefusal() {
  Fixture f;
  f.manifest("none", "none", false, false);
  // One complete frame remains below the 8 MiB wire bound but crosses the
  // analyzer's 100k event bound; emitting a sampled prefix would be misleading.
  boost::json::array trades;
  trades.reserve(100'001U);
  for (unsigned id = 1U; id <= 100'001U; ++id) {
    trades.emplace_back(boost::json::object{{"tradeId", id}});
  }
  f.addRaw("standard", boost::json::serialize(boost::json::object{
      {"ch", "market.AAA.trade.detail"},
      {"tick", boost::json::object{{"data", std::move(trades)}}}}), 10U);
  CXET_CHECK(f.analyze() == 2);
  CXET_CHECK(!std::filesystem::exists(f.directory / "paired_receipts.json"));
}

void htxHeartbeat() {
  std::string error;
  // Fixed gzip member (stored deflate) for the numeric-ping fixture.
  const char gzip_bytes[] = "\x1f\x8b\x08\x00\x00\x00\x00\x00\x00\x03\x01\x0c\x00\xf3\xff\x7b\x22\x70\x69\x6e\x67\x22\x3a\x31\x32\x33\x7d\x97\x65\x56\x69\x0c\x00\x00\x00";
  const std::string gzip{gzip_bytes, sizeof(gzip_bytes) - 1U};
  CXET_CHECK(net_detail::htx_application_pong(
      gzip, true, error) == R"({"pong":123})");
  CXET_CHECK(error.empty());
  CXET_CHECK(net_detail::htx_application_pong(
      R"({"ping":123})", false, error) == R"({"pong":123})");
  CXET_CHECK(error.empty());
  CXET_CHECK(net_detail::htx_application_pong(
      R"({"tick":{"id":123}})", false, error).empty());
  CXET_CHECK(error.empty());
  CXET_CHECK(net_detail::htx_application_pong(
      R"({"ping":"123"})", false, error).empty());
  CXET_CHECK(!error.empty());
  error.clear();
  CXET_CHECK(net_detail::htx_application_pong(
      "broken-gzip", true, error).empty());
  CXET_CHECK(!error.empty());
}

void roundNarrowingRefusal() {
  Fixture f;
  f.manifest();
  f.add("standard", "7", 100U, 1U);
  f.add("aws", "7", 200U, 4'294'967'297ULL);
  CXET_CHECK(f.analyze() == 2);
  CXET_CHECK(!std::filesystem::exists(f.directory / "paired_receipts.json"));
}
boost::json::object session(std::string_view channel, std::string_view stage,
                            unsigned generation,
                            std::optional<std::uint64_t> duration = std::nullopt) {
  boost::json::object row{{"schema", "exchange.api_probe.session.v1"},
      {"round", 1}, {"channel_id", channel}, {"stage", stage},
      {"ok", stage != "disconnected"}, {"monotonic_ns", 100},
      {"attempt", generation}, {"session_generation", generation}};
  if (duration) row["reconnect_duration_ns"] = *duration;
  return row;
}
void lifecycleUnavailable() {
  Fixture f;
  f.manifest();
  f.add("standard", "7", 100); f.add("aws", "7", 110);
  CXET_CHECK(f.analyze() == 0);
  auto report = f.report();
  CXET_CHECK(report.at("lifecycle").as_object().at("status") == "unavailable_missing_sessions");
  CXET_CHECK(report.at("lifecycle").as_object().at("standard").as_object().at("disconnect_count").is_null());
  CXET_CHECK(report.at("frame_generations").as_object().at("standard").as_object().at("status") == "unavailable");
  std::string rows;
  for (const auto* channel : {"standard", "aws"}) {
    auto row = session(channel, "connected", 1);
    row.erase("attempt"); row.erase("session_generation");
    rows += boost::json::serialize(row) + '\n';
  }
  f.write("sessions.jsonl", rows);
  CXET_CHECK(f.analyze() == 0);
  report = f.report();
  const auto& lifecycle = report.at("lifecycle").as_object();
  CXET_CHECK(lifecycle.at("status") == "unavailable_legacy_metadata");
  for (const auto* channel : {"standard", "aws"}) {
    const auto& result = lifecycle.at(channel).as_object();
    CXET_CHECK(result.at("disconnect_count").is_null() && result.at("reconnect_duration_ns").is_null());
  }
}
void lifecycleQuantilesAndGenerations() {
  Fixture f;
  f.manifest("none", "none", false, false);
  std::string rows;
  const auto append = [&](boost::json::object row) { rows += boost::json::serialize(row) + '\n'; };
  append(session("standard", "connected", 1));
  append(session("standard", "disconnected", 1));
  append(session("standard", "connected", 2, 30));
  append(session("standard", "disconnected", 2));
  append(session("standard", "connected", 3, 80));
  append(session("aws", "connected", 1));
  append(session("aws", "disconnected", 1));
  append(session("aws", "connected", 2, 20));
  f.write("sessions.jsonl", rows);
  const auto trade = [](unsigned id) {
    return boost::json::serialize(boost::json::object{
        {"ch", "market.AAA.trade.detail"},
        {"tick", boost::json::object{{"data", boost::json::array{
            boost::json::object{{"tradeId", id}}}}}}});
  };
  // Generations may differ across endpoints. Repeated identity across one
  // endpoint's generations remains ambiguous, never a new trade identity.
  f.addRaw("standard", trade(7), 100, 1, false, 2);
  f.addRaw("aws", trade(7), 110, 1, false, 1);
  f.addRaw("standard", trade(8), 200, 1, false, 2);
  f.addRaw("standard", trade(8), 210, 1, false, 3);
  f.addRaw("aws", trade(8), 220, 1, false, 2);
  CXET_CHECK(f.analyze() == 0);
  const auto report = f.report();
  CXET_CHECK(report.at("samples") == 1 && report.at("ambiguous_identities") == 1);
  const auto& lifecycle = report.at("lifecycle").as_object();
  CXET_CHECK(lifecycle.at("status") == "observed");
  const auto& standard = lifecycle.at("standard").as_object();
  CXET_CHECK(standard.at("disconnect_count") == 2 && standard.at("successful_reconnects") == 2);
  const auto& times = standard.at("reconnect_duration_ns").as_object();
  CXET_CHECK(times.at("p50") == 30 && times.at("p95") == 80 && times.at("p99") == 80);
  CXET_CHECK(lifecycle.at("aws").as_object().at("disconnect_count") == 1);
  const auto& generations = report.at("frame_generations").as_object();
  CXET_CHECK(generations.at("standard").as_object().at("by_generation").as_object().at("2") == 2);
  CXET_CHECK(generations.at("standard").as_object().at("by_generation").as_object().at("3") == 1);
  CXET_CHECK(generations.at("aws").as_object().at("by_generation").as_object().at("1") == 1);
}
void malformedLifecycleRefusal() {
  for (unsigned scenario = 0; scenario < 11; ++scenario) {
    Fixture f;
    f.manifest();
    f.add("standard", "7", 100); f.add("aws", "7", 110);
    auto row = session("standard", "connected", 1);
    switch (scenario) {
      case 0: row["schema"] = "wrong"; break;
      case 1: row["round"] = 0; break;
      case 2: row["session_generation"] = 4; break;
      case 3: row["attempt"] = 2; break;
      case 4: row["reconnect_duration_ns"] = -1; break;
      case 5: row["channel_id"] = "unknown"; break;
      case 6: row.erase("session_generation"); break;
      case 7: row["round"] = 4'294'967'297ULL; break;
      case 8: row["reconnect_duration_ns"] = "30"; break;
      default: break;
    }
    std::string rows = boost::json::serialize(row) + '\n';
    if (scenario == 9) rows += rows;  // Duplicate connected generation.
    if (scenario == 10) rows.insert(0, 64U * 1024U + 1U, ' ');
    rows += boost::json::serialize(session("aws", "connected", 1)) + '\n';
    f.write("sessions.jsonl", rows);
    CXET_CHECK(f.analyze() == 2);
    CXET_CHECK(!std::filesystem::exists(f.directory / "paired_receipts.json"));
  }
  Fixture f;
  f.manifest();
  f.addRaw("standard", R"({"ch":"market.AAA.trade.detail","tick":{"data":[{"tradeId":7}]}})", 100, 1, false, 4);
  f.add("aws", "7", 110);
  CXET_CHECK(f.analyze() == 2);
}
}

int main(int argc, char** argv) {
  const cxet::testing::Case cases[]{
      cxet::testing::Case{"paired.signed_receipt_quantiles_use_read_completion", signedQuantiles},
      cxet::testing::Case{"paired.identity_scope_excludes_duplicates", identityScopeAndDuplicates},
      cxet::testing::Case{"paired.requires_native_id_and_receipt_timestamp", requiresReceiptAndNativeId},
      cxet::testing::Case{"paired.refuses_legacy_persistence_timestamp", legacyTimestampRefusal},
      cxet::testing::Case{"paired.refuses_snapshot_delta_semantics", refusesIncompatibleSemantics},
      cxet::testing::Case{"paired.refuses_unsupported_predecessor_mapping", refusesUnsupportedPredecessor},
      cxet::testing::Case{"paired.refuses_signed_delta_overflow", signedRangeRefusal},
      cxet::testing::Case{"paired.official_origin_batch_native_ids", officialOriginBatchIds},
      cxet::testing::Case{"paired.refuses_undocumented_native_scope", refusesUnknownNativeScope},
      cxet::testing::Case{"paired.refuses_wrong_channel_or_trade_symbol", refusesWrongSymbol},
      cxet::testing::Case{"paired.refuses_event_capacity_overflow", eventCapacityRefusal},
      cxet::testing::Case{"paired.htx_heartbeat_validates_ping_and_bounded_gzip", htxHeartbeat},
      cxet::testing::Case{"paired.refuses_round_narrowing_before_identity_join", roundNarrowingRefusal},
      cxet::testing::Case{"paired.lifecycle_missing_or_legacy_metadata_is_unavailable", lifecycleUnavailable},
      cxet::testing::Case{"paired.lifecycle_quantiles_preserve_independent_generations", lifecycleQuantilesAndGenerations},
      cxet::testing::Case{"paired.lifecycle_refuses_malformed_metadata_and_generation", malformedLifecycleRefusal},
  };
  return cxet::testing::runCases(argc, argv, cases);
}
