#include "exchange_probe/race/analysis.hpp"
#include "exchange_probe/race/capture.hpp"
#include "exchange_probe/race/catalog.hpp"
#include "exchange_probe/race/report.hpp"
#include "exchange_probe/race/types.hpp"

#include <boost/json/array.hpp>
#include <boost/json/object.hpp>
#include <boost/json/parse.hpp>
#include <boost/json/value.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <tuple>
#include <vector>

namespace {

using exchange_probe::race::RaceRecord;

void usage(std::ostream& output) {
  output <<
      "usage:\n"
      "  exchange-feed-race plan\n"
      "  exchange-feed-race validate-command [OUTPUT_ROOT]\n"
      "  exchange-feed-race capture-command [OUTPUT_ROOT]\n"
      "  exchange-feed-race capture --output DIR --symbols MAGMA,ETH,BTR "
      "--venues bitget [--sessions N] [--warmup-seconds N] "
      "--measured-seconds N [--validation]\n"
      "  exchange-feed-race analyze CAPTURE_DIR [REPORT_DIR]\n";
}

[[nodiscard]] std::vector<std::string> split_csv(const std::string& value) {
  std::vector<std::string> output;
  std::size_t begin = 0u;
  while (begin <= value.size()) {
    const auto end = value.find(',', begin);
    const auto length = end == std::string::npos ? value.size() - begin
                                                  : end - begin;
    if (length != 0u) output.push_back(value.substr(begin, length));
    if (end == std::string::npos) break;
    begin = end + 1u;
  }
  return output;
}

[[nodiscard]] bool parse_unsigned(
    const char* text, unsigned& output) noexcept {
  if (text == nullptr || *text == '\0') return false;
  const auto* end = text;
  while (*end != '\0') ++end;
  const auto parsed = std::from_chars(text, end, output);
  return parsed.ec == std::errc{} && parsed.ptr == end;
}

[[nodiscard]] std::optional<exchange_probe::race::PublicCaptureOptions>
parse_capture_options(int argc, char** argv, std::string& error) {
  using exchange_probe::race::PublicCaptureOptions;
  PublicCaptureOptions options{};
  options.requestedBases = {"MAGMA", "ETH", "BTR"};
  options.venues = {"bitget"};
  for (int index = 2; index < argc; ++index) {
    const std::string name = argv[index];
    if (name == "--validation") {
      options.validation = true;
      continue;
    }
    if (index + 1 >= argc) {
      error = "capture_option_missing_value:" + name;
      return std::nullopt;
    }
    const char* value = argv[++index];
    unsigned number = 0u;
    if (name == "--output")
      options.outputDirectory = value;
    else if (name == "--symbols")
      options.requestedBases = split_csv(value);
    else if (name == "--venues")
      options.venues = split_csv(value);
    else if (name == "--sessions") {
      if (!parse_unsigned(value, number)) {
        error = "capture_sessions_invalid";
        return std::nullopt;
      }
      options.sessions = number;
    } else if (name == "--warmup-seconds") {
      if (!parse_unsigned(value, number)) {
        error = "capture_warmup_invalid";
        return std::nullopt;
      }
      options.warmup = std::chrono::seconds{number};
    } else if (name == "--measured-seconds") {
      if (!parse_unsigned(value, number)) {
        error = "capture_measured_invalid";
        return std::nullopt;
      }
      options.measured = std::chrono::seconds{number};
    } else {
      error = "capture_option_unknown:" + name;
      return std::nullopt;
    }
  }
  if (options.outputDirectory.empty()) {
    error = "capture_output_required";
    return std::nullopt;
  }
  return options;
}

[[nodiscard]] bool read_records(
    const std::filesystem::path& path, std::vector<RaceRecord>& records,
    std::string& error) {
  std::error_code filesystemError;
  const auto size = std::filesystem::file_size(path, filesystemError);
  if (filesystemError) {
    error = "records_stat:" + filesystemError.message();
    return false;
  }
  if (size % sizeof(RaceRecord) != 0u) {
    error = "records_size_not_schema_aligned";
    return false;
  }
  constexpr std::uintmax_t kMaximumRecords = 500'000'000u;
  const auto count = size / sizeof(RaceRecord);
  if (count > kMaximumRecords) {
    error = "records_count_bound";
    return false;
  }
  records.resize(static_cast<std::size_t>(count));
  std::ifstream input{path, std::ios::binary};
  if (!input) {
    error = "records_open_failed";
    return false;
  }
  input.read(
      reinterpret_cast<char*>(records.data()),
      static_cast<std::streamsize>(size));
  if (!input) {
    error = "records_read_failed";
    return false;
  }
  for (const auto& record : records) {
    if (record.schemaVersion != exchange_probe::race::kRecordSchemaVersion) {
      error = "records_schema_version_unsupported";
      records.clear();
      return false;
    }
  }
  return true;
}

[[nodiscard]] bool json_u64(
    const boost::json::object& object, const char* name,
    std::uint64_t& output) noexcept {
  const auto* value = object.if_contains(name);
  if (value == nullptr) return false;
  if (value->is_uint64()) {
    output = value->as_uint64();
    return true;
  }
  if (value->is_int64() && value->as_int64() >= 0) {
    output = static_cast<std::uint64_t>(value->as_int64());
    return true;
  }
  return false;
}

[[nodiscard]] bool read_json_object(
    const std::filesystem::path& path, boost::json::object& output,
    std::string& error) {
  std::error_code filesystemError;
  const auto size = std::filesystem::file_size(path, filesystemError);
  constexpr std::uintmax_t kMaximumMetadataBytes = 16u * 1024u * 1024u;
  if (filesystemError || size > kMaximumMetadataBytes) {
    error = filesystemError ? "metadata_stat:" + filesystemError.message()
                            : "metadata_size_bound";
    return false;
  }
  std::ifstream input{path, std::ios::binary};
  std::string text(static_cast<std::size_t>(size), '\0');
  input.read(text.data(), static_cast<std::streamsize>(size));
  if (!input) {
    error = "metadata_read_failed:" + path.string();
    return false;
  }
  boost::json::error_code parseError;
  auto value = boost::json::parse(text, parseError);
  if (parseError || !value.is_object()) {
    error = "metadata_parse_failed:" + path.string();
    return false;
  }
  output = std::move(value.as_object());
  return true;
}

[[nodiscard]] std::uint8_t bybit_lane(const std::string& feedId) noexcept {
  if (feedId.ends_with("/orderbook_1")) return 0u;
  if (feedId.ends_with("/orderbook_50")) return 1u;
  if (feedId.ends_with("/orderbook_200")) return 2u;
  if (feedId.ends_with("/orderbook_1000")) return 3u;
  return exchange_probe::race::kNoBboRaceLane;
}

[[nodiscard]] std::string feed_symbol(const std::string& feedId) {
  const auto slash = feedId.find('/');
  return slash == std::string::npos ? std::string{} : feedId.substr(0u, slash);
}

[[nodiscard]] bool source_health(
    const boost::json::object& connections, std::uint32_t sourceId) noexcept {
  const auto* rows = connections.if_contains("connections");
  if (rows == nullptr || !rows->is_array()) return false;
  for (const auto& rowValue : rows->as_array()) {
    if (!rowValue.is_object()) continue;
    const auto& row = rowValue.as_object();
    std::uint64_t rowSource = 0u;
    if (!json_u64(row, "source_id", rowSource) || rowSource != sourceId)
      continue;
    std::uint64_t malformed = 0u;
    std::uint64_t reconnects = 0u;
    std::uint64_t normalized = 0u;
    std::uint64_t finalStatus = 0u;
    const auto* overflow = row.if_contains("generation_overflow");
    return json_u64(row, "malformed", malformed) && malformed == 0u &&
           json_u64(row, "reconnects", reconnects) && reconnects == 0u &&
           json_u64(row, "normalized", normalized) && normalized != 0u &&
           json_u64(row, "final_status", finalStatus) &&
           finalStatus == static_cast<std::uint8_t>(
                              exchange_probe::race::FeedStatus::Ready) &&
           overflow != nullptr && overflow->is_bool() &&
           !overflow->as_bool();
  }
  return false;
}

void add_session_directory(
    const std::filesystem::path& candidate,
    std::vector<std::filesystem::path>& output) {
  std::error_code filesystemError;
  if (std::filesystem::is_regular_file(
          candidate / "records.bin", filesystemError)) {
    output.push_back(candidate);
  }
}

[[nodiscard]] bool discover_session_directories(
    const std::filesystem::path& capture,
    std::vector<std::filesystem::path>& output, std::string& error) {
  add_session_directory(capture, output);
  std::error_code filesystemError;
  if (!std::filesystem::is_directory(capture, filesystemError)) {
    error = filesystemError ? "capture_directory:" + filesystemError.message()
                            : "capture_directory_missing";
    return false;
  }
  constexpr std::size_t kMaximumSessionDirectories = 512u;
  constexpr std::size_t kMaximumVisitedDirectories = 2'048u;
  std::size_t visited = 0u;
  std::filesystem::directory_iterator firstIterator{
      capture, filesystemError};
  const std::filesystem::directory_iterator end;
  for (; firstIterator != end; firstIterator.increment(filesystemError)) {
    if (filesystemError) {
      error = "capture_directory_iterate:" + filesystemError.message();
      return false;
    }
    const auto& first = *firstIterator;
    const bool firstIsDirectory = first.is_directory(filesystemError);
    if (filesystemError) {
      error = "capture_entry_type:" + filesystemError.message();
      return false;
    }
    if (!firstIsDirectory) continue;
    if (++visited > kMaximumVisitedDirectories) {
      error = "capture_directory_visit_bound";
      return false;
    }
    add_session_directory(first.path(), output);
    std::filesystem::directory_iterator secondIterator{
        first.path(), filesystemError};
    for (; secondIterator != end;
         secondIterator.increment(filesystemError)) {
      if (filesystemError) {
        error = "capture_subdirectory_iterate:" + filesystemError.message();
        return false;
      }
      const auto& second = *secondIterator;
      const bool secondIsDirectory = second.is_directory(filesystemError);
      if (filesystemError) {
        error = "capture_subentry_type:" + filesystemError.message();
        return false;
      }
      if (!secondIsDirectory) continue;
      if (++visited > kMaximumVisitedDirectories) {
        error = "capture_directory_visit_bound";
        return false;
      }
      add_session_directory(second.path(), output);
      if (output.size() > kMaximumSessionDirectories) {
        error = "capture_session_directory_bound";
        return false;
      }
    }
    if (filesystemError) {
      error = "capture_subdirectory_iterate:" + filesystemError.message();
      return false;
    }
  }
  if (filesystemError) {
    error = "capture_directory_iterate:" + filesystemError.message();
    return false;
  }
  std::sort(output.begin(), output.end());
  output.erase(std::unique(output.begin(), output.end()), output.end());
  if (output.empty()) {
    error = "capture_records_not_found";
    return false;
  }
  return true;
}

[[nodiscard]] bool append_analysis_session(
    const std::filesystem::path& directory,
    std::vector<RaceRecord>& allRecords,
    exchange_probe::race::AnalysisContext& context, std::string& error) {
  using namespace exchange_probe::race;
  std::vector<RaceRecord> records;
  if (!read_records(directory / "records.bin", records, error)) return false;
  constexpr std::size_t kMaximumCombinedRecords = 500'000'000u;
  if (records.size() > kMaximumCombinedRecords - allRecords.size()) {
    error = "combined_records_count_bound";
    return false;
  }
  boost::json::object sources;
  boost::json::object connections;
  if (!read_json_object(directory / "sources.json", sources, error) ||
      !read_json_object(directory / "connections.json", connections, error)) {
    return false;
  }
  const auto* rows = sources.if_contains("sources");
  if (rows == nullptr || !rows->is_array() || rows->as_array().size() > 512u) {
    error = "sources_metadata_rows";
    return false;
  }
  for (const auto& rowValue : rows->as_array()) {
    if (!rowValue.is_object()) {
      error = "sources_metadata_row";
      return false;
    }
    const auto& row = rowValue.as_object();
    std::uint64_t rawSourceId = 0u;
    const auto* rawFeedId = row.if_contains("feed_id");
    if (!json_u64(row, "source_id", rawSourceId) ||
        rawSourceId > std::numeric_limits<std::uint32_t>::max() ||
        rawFeedId == nullptr || !rawFeedId->is_string() ||
        rawFeedId->as_string().size() > 128u) {
      error = "sources_metadata_identity";
      return false;
    }
    const auto sourceId = static_cast<std::uint32_t>(rawSourceId);
    const std::string feedId{
        rawFeedId->as_string().data(), rawFeedId->as_string().size()};
    const auto record = std::find_if(
        records.begin(), records.end(), [sourceId](const RaceRecord& item) {
          return item.source.sourceId == sourceId;
        });
    if (record == records.end()) continue;
    AnalysisSourceDescriptor descriptor{};
    descriptor.source = record->source;
    descriptor.symbol = feed_symbol(feedId);
    descriptor.feedId = feedId;
    descriptor.bboRaceLane = bybit_lane(feedId);
    descriptor.healthClean = source_health(connections, sourceId);
    context.sources.push_back(std::move(descriptor));
  }
  using WindowKey = std::pair<std::uint16_t, std::uint16_t>;
  std::map<WindowKey, std::pair<std::uint64_t, std::uint64_t>> windows;
  for (const auto& record : records) {
    if (record.timestamps.recvMonoNs == 0u) continue;
    auto& window = windows[{record.source.sessionId,
                            record.source.raceGroupId}];
    if (window.first == 0u || record.timestamps.recvMonoNs < window.first)
      window.first = record.timestamps.recvMonoNs;
    window.second = std::max(window.second, record.timestamps.recvMonoNs);
  }
  for (const auto& [key, window] : windows) {
    context.sessionWindows.push_back(AnalysisSessionWindow{
        key.first, key.second, window.first, window.second});
  }
  allRecords.insert(allRecords.end(), records.begin(), records.end());
  return true;
}

[[nodiscard]] bool load_analysis_capture(
    const std::filesystem::path& capture, std::vector<RaceRecord>& records,
    exchange_probe::race::AnalysisContext& context, std::string& error) {
  std::vector<std::filesystem::path> sessions;
  if (!discover_session_directories(capture, sessions, error)) return false;
  for (const auto& session : sessions) {
    if (!append_analysis_session(session, records, context, error)) {
      error = session.string() + ':' + error;
      return false;
    }
  }
  return true;
}

void print_plan() {
  const auto plans = exchange_probe::race::requested_venue_plans();
  constexpr std::array<const char*, 3u> symbols{"MAGMA", "ETH", "BTR"};
  std::cout << "sessions=3 measured_seconds=600 warmup_seconds=20\n"
            << "venues_sequential=true feeds_dedicated_connections=true\n";
  for (const auto& venue : plans) {
    std::cout << venue.name << " instruments=https://"
              << venue.instruments.host << venue.instruments.path
              << " feeds=" << venue.feeds.size() << " symbols=";
    for (std::size_t index = 0u; index < symbols.size(); ++index) {
      if (index != 0u) std::cout << ',';
      std::cout << symbols[index];
    }
    std::cout << '\n';
    for (const auto& feed : venue.feeds)
      std::cout << "  " << feed.id << " wire="
                << static_cast<unsigned>(feed.wire) << " transport="
                << static_cast<unsigned>(feed.transport)
                << " experimental=" << feed.experimental << '\n';
  }
}

void print_capture_command(
    const std::string& executable, const std::filesystem::path& output,
    bool validation) {
  std::cout << executable << " capture --output " << output.string()
            << " --symbols MAGMA,ETH,BTR --sessions "
            << (validation ? 1 : 3) << " --warmup-seconds 20"
            << " --measured-seconds " << (validation ? 20 : 600)
            << " --venues bitget,bybit,gate,okx,kucoin,binance-usdm,aster"
            << (validation ? " --validation" : "") << '\n';
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    usage(std::cerr);
    return 2;
  }
  const std::string command = argv[1];
  if (command == "plan") {
    print_plan();
    return 0;
  }
  if (command == "validate-command" || command == "capture-command") {
    const std::filesystem::path output = argc >= 3 ? argv[2] : "feed-race-out";
    print_capture_command(
        argv[0], output, command == "validate-command");
    return 0;
  }
  if (command == "capture") {
    std::string error;
    auto options = parse_capture_options(argc, argv, error);
    if (!options.has_value()) {
      std::cerr << error << '\n';
      return 2;
    }
    const auto result = exchange_probe::race::run_public_capture(*options);
    std::cout << "complete=" << result.complete
              << " degraded=" << result.degraded
              << " sessions=" << result.sessionsCompleted
              << " available=" << result.availableInstruments
              << " unavailable=" << result.unavailableInstruments << '\n';
    if (!result.complete) {
      std::cerr << result.error << '\n';
      return 3;
    }
    return result.degraded ? 4 : 0;
  }
  if (command == "analyze") {
    if (argc < 3 || argc > 4) {
      usage(std::cerr);
      return 2;
    }
    const std::filesystem::path capture = argv[2];
    const std::filesystem::path report =
        argc == 4 ? std::filesystem::path{argv[3]} : capture / "report";
    std::vector<RaceRecord> records;
    std::string error;
    exchange_probe::race::AnalysisContext context;
    if (!load_analysis_capture(capture, records, context, error)) {
      std::cerr << error << '\n';
      return 1;
    }
    const auto analysis =
        exchange_probe::race::analyze_records(records, context);
    if (!exchange_probe::race::write_analysis_report(
            analysis, report, error)) {
      std::cerr << error << '\n';
      return 1;
    }
    std::cout << "report=" << (report / "report.md").string()
              << " dashboard=" << (report / "dashboard.html").string()
              << " records=" << analysis.inputRecords
              << " eligible=" << analysis.eligibleRecords << '\n';
    return 0;
  }
  usage(std::cerr);
  return 2;
}
