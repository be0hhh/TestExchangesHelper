#include "ResearchAnalysisInternal.hpp"

#include <array>

namespace exchange_probe::research_analysis {

[[nodiscard]] bool emit_relation(
    std::ofstream& output,
    std::uint64_t& relation_id,
    bool& output_full,
    std::string_view mode,
    std::string_view evidence,
    const NormalizedEvent& source,
    const NormalizedEvent& target,
    std::string_view reason) {
  if (relation_id >= kMaximumRelations) output_full = true;
  if (output_full) return false;
  boost::json::object row{
      {"schema", kResearchRelationSchema},
      {"relation_id", relation_id++},
      {"mode", mode},
      {"evidence", evidence},
      {"source_event_id", source.id},
      {"target_event_id", target.id},
      {"reason", reason},
      {"causality", "not_asserted"},
  };
  const auto line = boost::json::serialize(row) + "\n";
  const auto position = output.tellp();
  const auto offset = static_cast<std::streamoff>(position);
  if (offset < 0 ||
      static_cast<std::uint64_t>(offset) +
              static_cast<std::uint64_t>(line.size()) >
          kMaximumDerivedJsonlBytes) {
    output_full = true;
    return false;
  }
  output << line;
  return true;
}

}  // namespace exchange_probe::research_analysis

namespace exchange_probe {
namespace {

// Cold offline storage: at most 100k trade records/identities and 16 MiB of
// retained key text; fields have a separate 256-byte bound. Overflow refuses
// the report rather than silently sampling a prefix.
constexpr std::size_t kMaximumPairedEvents = 100'000U;
constexpr std::size_t kMaximumPairedKeyBytes = 16U * 1024U * 1024U;
constexpr std::size_t kMaximumPairedFieldBytes = 256U;
using PairedKey = std::tuple<unsigned, std::string, ResearchChannelKind, std::string>;
struct PairedIdentity {
  std::optional<std::uint64_t> standard;
  std::optional<std::uint64_t> aws;
  std::uint64_t standard_count{0U};
  std::uint64_t aws_count{0U};
};

// Optional offline lifecycle input. Storage is allocated only during analysis,
// bounded by 8 MiB / 10k rows / 64 KiB per row; overflow refuses the report.
constexpr std::uint64_t kMaximumSessionFileBytes = 8U * 1024U * 1024U;
constexpr std::size_t kMaximumSessionRows = 10'000U;
constexpr std::size_t kMaximumSessionLineBytes = 64U * 1024U;
struct ChannelLifecycle {
  std::uint64_t rows{0};
  std::uint64_t disconnects{0};
  bool legacy{false};
  std::vector<std::uint64_t> reconnect_durations;
};
struct Lifecycle {
  bool file_present{false};
  std::array<ChannelLifecycle, 2> channels;
};

[[nodiscard]] std::optional<std::uint64_t> strict_unsigned(
    const boost::json::value* value) {
  if (value == nullptr) return std::nullopt;
  if (value->is_uint64()) return value->as_uint64();
  if (value->is_int64() && value->as_int64() >= 0)
    return static_cast<std::uint64_t>(value->as_int64());
  return std::nullopt;
}

[[nodiscard]] bool read_lifecycle(
    const std::filesystem::path& path,
    std::string_view standard, std::string_view aws,
    const std::unordered_map<std::string, research_analysis::ChannelDefinition>& definitions,
    Lifecycle& result, std::string& error) {
  using namespace research_analysis;
  std::error_code filesystem_error;
  result.file_present = std::filesystem::exists(path, filesystem_error);
  if (filesystem_error) { error = "sessions_file_status_failed"; return false; }
  if (!result.file_present) return true;
  const auto bytes = std::filesystem::file_size(path, filesystem_error);
  if (filesystem_error || bytes > kMaximumSessionFileBytes) {
    error = "sessions_file_capacity_or_size_invalid";
    return false;
  }
  std::ifstream input{path, std::ios::binary};
  if (!input) { error = "sessions_file_open_failed"; return false; }
  std::set<std::tuple<unsigned, std::string, unsigned>> connected_generations;
  std::size_t rows = 0;
  std::uint64_t bytes_read = 0;
  std::string line;
  line.reserve(kMaximumSessionLineBytes);
  while (true) {
    line.clear();
    char character;
    bool newline = false;
    while (input.get(character)) {
      if (++bytes_read > kMaximumSessionFileBytes) {
        error = "sessions_file_capacity_exceeded";
        return false;
      }
      if (character == '\n') { newline = true; break; }
      if (line.size() == kMaximumSessionLineBytes) {
        error = "sessions_line_capacity_exceeded";
        return false;
      }
      line.push_back(character);
    }
    if (!newline && line.empty() && input.eof()) break;
    if (!input && !input.eof()) { error = "sessions_file_read_failed"; return false; }
    if (++rows > kMaximumSessionRows) { error = "sessions_row_capacity_exceeded"; return false; }
    boost::system::error_code parse_error;
    const auto value = boost::json::parse(line, parse_error);
    if (parse_error || !value.is_object()) { error = "sessions_invalid_jsonl"; return false; }
    const auto& row = value.as_object();
    if (text(row, "schema") != "exchange.api_probe.session.v1") {
      error = "sessions_schema_invalid";
      return false;
    }
    const auto channel = text(row, "channel_id");
    const bool selected = channel == standard || channel == aws;
    if (!selected) {
      if (channel == "pcap" || channel == "supervisor" || definitions.contains(channel)) continue;
      error = "sessions_unknown_channel";
      return false;
    }
    const auto round = strict_unsigned(row.if_contains("round"));
    const auto stage = text(row, "stage");
    const auto* ok = row.if_contains("ok");
    if (!round || *round == 0 || *round > std::numeric_limits<unsigned>::max() ||
        stage.empty() || stage.size() > kMaximumPairedFieldBytes ||
        ok == nullptr || !ok->is_bool() ||
        (row.if_contains("monotonic_ns") && !strict_unsigned(row.if_contains("monotonic_ns")))) {
      error = "sessions_selected_row_invalid";
      return false;
    }
    auto& metrics = result.channels[channel == standard ? 0U : 1U];
    ++metrics.rows;
    const auto* generation_field = row.if_contains("session_generation");
    const auto* attempt_field = row.if_contains("attempt");
    const auto* duration_field = row.if_contains("reconnect_duration_ns");
    const auto* wait_field = row.if_contains("reconnect_wait_ns");
    if (wait_field && !strict_unsigned(wait_field)) {
      error = "sessions_reconnect_wait_invalid";
      return false;
    }
    if (generation_field == nullptr && attempt_field == nullptr && duration_field == nullptr && wait_field == nullptr) {
      metrics.legacy = true;
      continue;
    }
    const auto generation = strict_unsigned(generation_field);
    const auto attempt = strict_unsigned(attempt_field);
    const auto duration = strict_unsigned(duration_field);
    if (!generation || *generation < 1U || *generation > 3U ||
        !attempt || *attempt != *generation || (duration_field && !duration)) {
      error = "sessions_generation_or_duration_invalid";
      return false;
    }
    if (stage == "connected") {
      if (!ok->as_bool() || (*generation == 1U && duration_field) ||
          (*generation > 1U && !duration)) {
        error = "sessions_connected_metadata_invalid";
        return false;
      }
      if (!connected_generations.emplace(static_cast<unsigned>(*round), channel,
                                         static_cast<unsigned>(*generation)).second) {
        error = "sessions_duplicate_connected_generation";
        return false;
      }
      if (duration) metrics.reconnect_durations.push_back(*duration);
    } else if (duration_field) {
      error = "sessions_duration_requires_successful_connected_stage";
      return false;
    }
    if (stage == "disconnected") {
      if (ok->as_bool()) { error = "sessions_disconnected_metadata_invalid"; return false; }
      ++metrics.disconnects;
    }
  }
  return true;
}

[[nodiscard]] boost::json::object lifecycle_json(
    Lifecycle& value, std::string_view standard, std::string_view aws) {
  std::string status = "observed";
  if (!value.file_present) status = "unavailable_missing_sessions";
  else if (value.channels[0].legacy || value.channels[1].legacy) status = "unavailable_legacy_metadata";
  else if (value.channels[0].rows == 0 || value.channels[1].rows == 0) status = "unavailable_selected_channel_missing";
  const bool available = status == "observed";
  boost::json::object result{{"status", status},
      {"definition", "disconnect_to_subscription_write_complete_not_ack_or_first_data"},
      {"quantiles", "nearest_rank"}, {"count_scope", "recorded_selected_channel_session_rows"}};
  for (std::size_t index = 0; index != value.channels.size(); ++index) {
    auto& metrics = value.channels[index];
    boost::json::value statistics;
    if (available && !metrics.reconnect_durations.empty()) {
      std::sort(metrics.reconnect_durations.begin(), metrics.reconnect_durations.end());
      const auto percentile = [&](std::size_t percent) {
        return metrics.reconnect_durations[(metrics.reconnect_durations.size() * percent + 99U) / 100U - 1U];
      };
      statistics = boost::json::object{{"p50", percentile(50)}, {"p95", percentile(95)}, {"p99", percentile(99)}};
    }
    result[index == 0 ? "standard" : "aws"] = boost::json::object{
        {"channel_id", index == 0 ? standard : aws}, {"session_rows", metrics.rows},
        {"disconnect_count", available ? boost::json::value{metrics.disconnects} : boost::json::value{nullptr}},
        {"successful_reconnects", available ? boost::json::value{metrics.reconnect_durations.size()} : boost::json::value{nullptr}},
        {"reconnect_duration_ns", std::move(statistics)}};
  }
  return result;
}

[[nodiscard]] const boost::json::object* manifest_channel(
    const boost::json::object& manifest, std::string_view id) {
  const auto* channels = manifest.if_contains("channels");
  if (channels == nullptr || !channels->is_array()) return nullptr;
  const boost::json::object* found = nullptr;
  for (const auto& channel : channels->as_array()) {
    if (channel.is_object() && research_analysis::text(channel.as_object(), "id") == id) {
      if (found != nullptr) return nullptr;
      found = &channel.as_object();
    }
  }
  return found;
}

[[nodiscard]] bool admitted_pair(
    const boost::json::object& manifest,
    const boost::json::object& standard,
    const boost::json::object& aws,
    const research_analysis::ChannelDefinition& left,
    const research_analysis::ChannelDefinition& right) {
  using namespace research_analysis;
  if (text(manifest, "venue") != "htx" ||
      left.kind != ResearchChannelKind::Trade || right.kind != left.kind ||
      text(standard, "transport") != "ws" || text(aws, "transport") != "ws" ||
      u64(standard, "port") != 443U || u64(aws, "port") != 443U ||
      left.wire != "json" || right.wire != left.wire ||
      left.compression != right.compression ||
      (left.compression != "none" && left.compression != "gzip") ||
      left.depth_semantics != right.depth_semantics ||
      (!left.depth_semantics.empty() && left.depth_semantics != "none") ||
      left.mapping.data_path != "tick.data" ||
      right.mapping.data_path != left.mapping.data_path ||
      right.mapping.event_id_path != left.mapping.event_id_path ||
      right.mapping.symbol_path != left.mapping.symbol_path ||
      !left.mapping.previous_event_id_path.empty() ||
      !right.mapping.previous_event_id_path.empty() ||
      !left.mapping.first_event_id_path.empty() ||
      !right.mapping.first_event_id_path.empty() ||
      !left.mapping.snapshot_path.empty() || !right.mapping.snapshot_path.empty()) {
    return false;
  }
  const auto product = text(manifest, "product");
  const bool spot = product == "spot";
  if (!spot && product != "futures") return false;
  const auto path = spot ? "/ws" : "/linear-swap-ws";
  if (text(standard, "host") != (spot ? "api.huobi.pro" : "api.hbdm.com") ||
      text(aws, "host") != (spot ? "api-aws.huobi.pro" : "api.hbdm.vn") ||
      text(standard, "path") != path || text(aws, "path") != path ||
      left.mapping.event_id_path != (spot ? "tradeId" : "id")) {
    return false;
  }
  const auto subscribe = text(standard, "subscribe");
  if (subscribe.empty() || subscribe != text(aws, "subscribe")) return false;
  boost::system::error_code parse_error;
  const auto subscription = boost::json::parse(subscribe, parse_error);
  return !parse_error && subscription.is_object() &&
         text(subscription.as_object(), "sub") == "market.{symbol}.trade.detail";
}

[[nodiscard]] std::optional<std::int64_t> receipt_delta(
    std::uint64_t standard, std::uint64_t aws) {
  constexpr auto positive_max =
      static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
  if (aws >= standard) {
    const auto magnitude = aws - standard;
    if (magnitude > positive_max) return std::nullopt;
    return static_cast<std::int64_t>(magnitude);
  }
  const auto magnitude = standard - aws;
  if (magnitude > positive_max + 1U) return std::nullopt;
  if (magnitude == positive_max + 1U) {
    return std::numeric_limits<std::int64_t>::min();
  }
  return -static_cast<std::int64_t>(magnitude);
}

}  // namespace

int analyze_paired_research_receipts(
    const std::filesystem::path& directory,
    std::string_view standard_channel,
    std::string_view aws_channel,
    std::ostream& output,
    std::ostream& error_output) {
  using namespace research_analysis;
  const auto refuse = [&](std::string_view reason) {
    error_output << "paired_analysis_error: " << reason << '\n';
    return 2;
  };
  if (standard_channel.empty() || aws_channel.empty() ||
      standard_channel == aws_channel) return refuse("distinct_channels_required");
  std::string error;
  auto manifest = read_object(directory / "manifest.json", error);
  if (!manifest.has_value()) return refuse(error);
  if (text(*manifest, "schema") != kResearchBundleSchema ||
      u64(*manifest, "schema_version") != 3U) {
    return refuse("incompatible_bundle_schema");
  }
  const auto definitions = channel_definitions(*manifest);
  const auto left = definitions.find(std::string{standard_channel});
  const auto right = definitions.find(std::string{aws_channel});
  const auto* left_manifest = manifest_channel(*manifest, standard_channel);
  const auto* right_manifest = manifest_channel(*manifest, aws_channel);
  if (left == definitions.end() || right == definitions.end() ||
      left_manifest == nullptr || right_manifest == nullptr ||
      !admitted_pair(*manifest, *left_manifest, *right_manifest,
                     left->second, right->second)) {
    return refuse("pair_requires_documented_htx_native_trade_scope");
  }
  const auto symbol = text(*manifest, "symbol");
  if (symbol.empty() || symbol.size() > kMaximumPairedFieldBytes) {
    return refuse("manifest_symbol_invalid");
  }
  Lifecycle lifecycle;
  if (!read_lifecycle(directory / "sessions.jsonl", standard_channel, aws_channel,
                      definitions, lifecycle, error)) return refuse(error);
  auto index = read_frame_index(directory / "frames.jsonl", error);
  if (!error.empty()) return refuse(error);
  std::error_code filesystem_error;
  const auto size = std::filesystem::file_size(directory / "frames.bin", filesystem_error);
  if (filesystem_error || size > kMaximumFramesFileBytes ||
      (!index.empty() && index.back().offset + index.back().length != size) ||
      (index.empty() && size != 0U)) return refuse("frames_file_size_mismatch");
  std::ifstream frames{directory / "frames.bin", std::ios::binary};
  if (!frames) return refuse("frames_file_open_failed");
  std::map<PairedKey, PairedIdentity> identities;
  std::size_t key_bytes = 0U;
  std::uint64_t selected_frames = 0U;
  std::uint64_t candidate_records = 0U;
  std::uint64_t invalid_payload_records = 0U;
  std::uint64_t missing_native_id_records = 0U;
  std::uint64_t invalid_id_standard = 0U;
  std::uint64_t invalid_id_aws = 0U;
  std::array<std::array<std::uint64_t, 3>, 2> generation_counts{};
  std::array<std::uint64_t, 2> missing_generations{};
  for (const auto& frame : index) {
    const bool standard = frame.channel == standard_channel;
    if (!standard && frame.channel != aws_channel) continue;
    if (++selected_frames > kMaximumPairedEvents) return refuse("paired_frame_capacity_exceeded");
    const auto endpoint = standard ? 0U : 1U;
    if (frame.session_generation)
      ++generation_counts[endpoint][*frame.session_generation - 1U];
    else ++missing_generations[endpoint];
    // Never substitute the historical persistence-lock timestamp.
    if (!frame.received_monotonic_ns.has_value()) return refuse("read_completion_timestamp_required");
    const auto& channel = standard ? left->second : right->second;
    auto payload = read_payload(frames, frame, error);
    if (!error.empty()) return refuse(error);
    std::string decoded;
    if (channel.compression == "gzip" && frame.binary) {
      if (!decode_gzip_bounded(payload, kMaxWsMessageBytes, decoded, error)) return refuse(error);
      payload = std::move(decoded);
    } else if (frame.binary) {
      return refuse("unexpected_binary_trade_wire");
    }
    boost::system::error_code parse_error;
    const auto value = boost::json::parse(payload, parse_error);
    if (parse_error) {
      ++invalid_payload_records;
      continue;
    }
    const auto* data = json_path(value, "tick.data");
    // Subscription ACK and application heartbeat have no trade-array identity.
    if (data == nullptr) continue;
    if (!value.is_object() ||
        text(value.as_object(), "ch") != "market." + symbol + ".trade.detail") {
      return refuse("trade_channel_symbol_mismatch");
    }
    if (channel.compression == "gzip" && !frame.binary) {
      return refuse("gzip_trade_requires_binary_opcode");
    }
    if (!data->is_array()) {
      ++invalid_payload_records;
      continue;
    }
    for (const auto& trade : data->as_array()) {
      if (++candidate_records > kMaximumPairedEvents) return refuse("paired_event_capacity_exceeded");
      if (!trade.is_object()) {
        ++invalid_payload_records;
        continue;
      }
      // Read only the declared native identity in the raw child object. Spot's
      // obsolete hugeinteger `id` is never a fallback for UniqueTradeId.
      const auto* native_value = json_path(trade, channel.mapping.event_id_path);
      const auto native_id = scalar_text(native_value);
      const bool numeric_id = !native_id.empty() && std::all_of(
          native_id.begin(), native_id.end(), [](char c) { return c >= '0' && c <= '9'; });
      if (!numeric_id) {
        ++missing_native_id_records;
        ++(standard ? invalid_id_standard : invalid_id_aws);
        continue;
      }
      auto event_symbol = channel.mapping.symbol_path.empty()
          ? symbol : scalar_text(json_path(trade, channel.mapping.symbol_path));
      if (event_symbol != symbol) return refuse("trade_symbol_mismatch");
      if (native_id.size() > kMaximumPairedFieldBytes ||
          event_symbol.empty() || event_symbol.size() > kMaximumPairedFieldBytes) return refuse("paired_field_capacity_exceeded");
      // Endpoint generations are independent provenance, never native identity.
      // A repeated native ID across this endpoint's generations is still a
      // duplicate and is excluded by the existing whole-identity policy below.
      PairedKey key{frame.round, event_symbol, ResearchChannelKind::Trade, native_id};
      auto found = identities.find(key);
      if (found == identities.end()) {
        const auto retained_bytes = native_id.size() + event_symbol.size();
        if (identities.size() >= kMaximumPairedEvents ||
            retained_bytes > kMaximumPairedKeyBytes - key_bytes) return refuse("paired_identity_capacity_exceeded");
        key_bytes += retained_bytes;
        found = identities.emplace(std::move(key), PairedIdentity{}).first;
      }
      auto& identity = found->second;
      auto& count = standard ? identity.standard_count : identity.aws_count;
      auto& receipt = standard ? identity.standard : identity.aws;
      if (++count == 1U) {
        receipt = *frame.received_monotonic_ns;
      }
    }
  }
  std::vector<std::int64_t> deltas;
  deltas.reserve(identities.size());
  std::uint64_t unmatched_standard = invalid_id_standard;
  std::uint64_t unmatched_aws = invalid_id_aws;
  std::uint64_t duplicate_records_standard = 0U;
  std::uint64_t duplicate_records_aws = 0U;
  std::uint64_t ambiguous_identities = 0U;
  for (const auto& [key, identity] : identities) {
    duplicate_records_standard += identity.standard_count > 0U ? identity.standard_count - 1U : 0U;
    duplicate_records_aws += identity.aws_count > 0U ? identity.aws_count - 1U : 0U;
    if (identity.standard_count > 1U || identity.aws_count > 1U) {
      ++ambiguous_identities;
      continue;
    }
    if (!identity.aws.has_value()) { ++unmatched_standard; continue; }
    if (!identity.standard.has_value()) { ++unmatched_aws; continue; }
    const auto delta = receipt_delta(*identity.standard, *identity.aws);
    if (!delta.has_value()) return refuse("signed_receipt_delta_overflow");
    deltas.push_back(*delta);
  }
  std::sort(deltas.begin(), deltas.end());
  boost::json::value statistics;
  if (!deltas.empty()) {
    const auto percentile = [&](std::size_t percentage) {
      return deltas[(deltas.size() * percentage + 99U) / 100U - 1U];
    };
    const auto p50 = percentile(50U);
    const auto p95 = percentile(95U);
    // Unsigned subtraction gives the exact nonnegative span across zero too.
    const auto jitter = static_cast<std::uint64_t>(p95) - static_cast<std::uint64_t>(p50);
    statistics = boost::json::object{{"p50", p50}, {"p95", p95},
        {"p99", percentile(99U)}, {"jitter_p95_minus_p50", jitter}};
  }
  const auto* complete = manifest->if_contains("artifact_complete");
  boost::json::object frame_generations;
  for (std::size_t endpoint = 0; endpoint != generation_counts.size(); ++endpoint) {
    const auto& counts = generation_counts[endpoint];
    const auto present = counts[0] + counts[1] + counts[2];
    boost::json::value by_generation;
    if (present != 0)
      by_generation = boost::json::object{{"1", counts[0]}, {"2", counts[1]}, {"3", counts[2]}};
    frame_generations[endpoint == 0 ? "standard" : "aws"] = boost::json::object{
        {"status", present == 0 ? "unavailable" : missing_generations[endpoint] == 0 ? "observed" : "partial"},
        {"frames_with_generation", present}, {"frames_without_generation", missing_generations[endpoint]},
        {"by_generation", std::move(by_generation)}};
  }
  boost::json::object report{
      {"schema", "exchange.api_probe.paired_receipts.v1"}, {"schema_version", 1},
      {"run_id", text(*manifest, "run_id")},
      {"venue", "htx"}, {"product", text(*manifest, "product")}, {"symbol", symbol},
      {"standard_channel", standard_channel}, {"aws_channel", aws_channel},
      {"status", deltas.empty() ? "no_matched_native_trade_ids" : "observed"},
      {"samples", deltas.size()}, {"aws_minus_standard_ns", std::move(statistics)},
      {"selected_frames", selected_frames}, {"candidate_trade_records", candidate_records},
      {"invalid_payload_records", invalid_payload_records},
      {"missing_native_id_records", missing_native_id_records},
      {"invalid_id_standard", invalid_id_standard}, {"invalid_id_aws", invalid_id_aws},
      {"unmatched_standard", unmatched_standard}, {"unmatched_aws", unmatched_aws},
      {"duplicate_records_standard", duplicate_records_standard},
      {"duplicate_records_aws", duplicate_records_aws}, {"ambiguous_identities", ambiguous_identities},
      {"predecessor_gaps_standard", nullptr},
      {"predecessor_gaps_aws", nullptr},
      {"gap_evidence", "unavailable_no_documented_predecessor"},
      {"artifact_complete", complete != nullptr && complete->is_bool() && complete->as_bool()},
      {"identity_scope", "same_manifest_product_round_symbol_trade_native_id"},
      {"receipt_boundary", "steady_clock_complete_ws_read_before_copy_decode_artifact_lock"},
      {"quantiles", "nearest_rank"},
      {"duplicate_policy", "exclude_every_identity_duplicated_on_either_channel"},
      {"lifecycle", lifecycle_json(lifecycle, standard_channel, aws_channel)},
      {"frame_generations", std::move(frame_generations)},
      {"generation_join_policy", "endpoint_generations_may_differ;generation_is_not_native_identity;cross_generation_duplicates_excluded"},
      {"maximum_trade_records", kMaximumPairedEvents},
      {"maximum_retained_key_bytes", kMaximumPairedKeyBytes},
      {"evidence_boundary", "local paired diagnostic; no absolute network latency, matching-engine latency, causal or trading-readiness proof"},
  };
  const auto temporary = directory / "paired_receipts.json.tmp";
  std::ofstream file{temporary, std::ios::binary};
  file << boost::json::serialize(report) << '\n';
  file.flush();
  if (!file) return refuse("paired_report_write_failed");
  file.close();
  if (!file) return refuse("paired_report_close_failed");
  std::error_code rename_error;
  std::filesystem::rename(temporary, directory / "paired_receipts.json", rename_error);
  if (rename_error) return refuse("paired_report_replace_failed");
  output << "paired_receipts: samples=" << deltas.size() << '\n';
  return 0;
}

}  // namespace exchange_probe
