#include "ResearchAnalysisInternal.hpp"

namespace exchange_probe::research_analysis {

[[nodiscard]] std::optional<boost::json::object> read_object(
    const std::filesystem::path& path,
    std::string& error) {
  std::error_code filesystem_error;
  const auto size = std::filesystem::file_size(path, filesystem_error);
  if (filesystem_error || size > kMaximumAnalysisJsonBytes) {
    error = filesystem_error
                ? "file_size_failed:" + filesystem_error.message()
                : "file_capacity_exceeded";
    return std::nullopt;
  }
  std::ifstream input{path, std::ios::binary};
  if (!input) {
    error = "file_open_failed";
    return std::nullopt;
  }
  const std::string content{
      std::istreambuf_iterator<char>{input},
      std::istreambuf_iterator<char>{}};
  boost::system::error_code parse_error;
  auto value = boost::json::parse(content, parse_error);
  if (parse_error || !value.is_object()) {
    error = "invalid_json_object";
    return std::nullopt;
  }
  return value.as_object();
}

[[nodiscard]] std::string text(
    const boost::json::object& object,
    std::string_view key) {
  const auto* value = object.if_contains(key);
  return value != nullptr && value->is_string()
             ? std::string{value->as_string()}
             : std::string{};
}

[[nodiscard]] std::uint64_t u64(
    const boost::json::object& object,
    std::string_view key) noexcept {
  const auto* value = object.if_contains(key);
  if (value == nullptr) return 0U;
  if (value->is_uint64()) return value->as_uint64();
  if (value->is_int64() && value->as_int64() >= 0) {
    return static_cast<std::uint64_t>(value->as_int64());
  }
  return 0U;
}

[[nodiscard]] std::vector<FrameIndex> read_frame_index(
    const std::filesystem::path& path,
    std::string& error) {
  std::vector<FrameIndex> result;
  std::error_code filesystem_error;
  const auto index_size = std::filesystem::file_size(path, filesystem_error);
  if (filesystem_error || index_size > kMaximumAnalysisJsonBytes) {
    error = filesystem_error
                ? "frame_index_size_failed:" + filesystem_error.message()
                : "frame_index_capacity_exceeded";
    return result;
  }
  std::ifstream input{path, std::ios::binary};
  if (!input) {
    error = "frame_index_open_failed";
    return result;
  }
  std::string line;
  std::set<std::uint64_t> frame_ids;
  std::uint64_t expected_offset = 0U;
  while (std::getline(input, line)) {
    boost::system::error_code parse_error;
    auto value = boost::json::parse(line, parse_error);
    if (parse_error || !value.is_object()) {
      error = "frame_index_invalid_jsonl";
      result.clear();
      return result;
    }
    const auto& object = value.as_object();
    const auto round = u64(object, "round");
    if (round > std::numeric_limits<unsigned>::max()) {
      error = "frame_index_round_out_of_range";
      return {};
    }
    FrameIndex frame{
        .id = u64(object, "frame_id"),
        .round = static_cast<unsigned>(round),
        .channel = text(object, "channel_id"),
        .binary = text(object, "opcode") == "binary",
        .monotonic_ns = u64(object, "monotonic_ns"),
        .utc_ns = u64(object, "utc_ns"),
        .offset = u64(object, "offset"),
        .length = u64(object, "length"),
    };
    const auto* generation = object.if_contains("session_generation");
    const auto* attempt = object.if_contains("attempt");
    if (generation != nullptr) {
      const auto valid_unsigned = [](const boost::json::value& field) {
        return field.is_uint64() || (field.is_int64() && field.as_int64() >= 0);
      };
      const auto number = u64(object, "session_generation");
      if (!valid_unsigned(*generation) || number < 1U || number > 3U ||
          (attempt != nullptr && (!valid_unsigned(*attempt) || u64(object, "attempt") != number))) {
        error = "frame_session_generation_invalid";
        return {};
      }
      frame.session_generation = static_cast<unsigned>(number);
    } else if (attempt != nullptr) {
      error = "frame_attempt_requires_session_generation";
      return {};
    }
    if (const auto* receipt = object.if_contains("received_monotonic_ns");
        receipt != nullptr) {
      if (receipt->is_uint64()) {
        frame.received_monotonic_ns = receipt->as_uint64();
      } else if (receipt->is_int64() && receipt->as_int64() >= 0) {
        frame.received_monotonic_ns =
            static_cast<std::uint64_t>(receipt->as_int64());
      } else {
        error = "frame_receipt_timestamp_invalid";
        return {};
      }
    }
    if (text(object, "schema") != kResearchFrameSchema ||
        frame.round == 0U || frame.channel.empty() ||
        !frame_ids.insert(frame.id).second ||
        frame.id != static_cast<std::uint64_t>(result.size()) ||
        frame.offset != expected_offset ||
        (!result.empty() &&
         frame.monotonic_ns < result.back().monotonic_ns) ||
        frame.offset >
            std::numeric_limits<std::uint64_t>::max() - frame.length) {
      error = "frame_index_contract_violation";
      result.clear();
      return result;
    }
    expected_offset += frame.length;
    result.push_back(std::move(frame));
  }
  return result;
}

[[nodiscard]] JsonEventMapping mapping_from_json(
    const boost::json::object& object) {
  JsonEventMapping result{
      .data_path = text(object, "data"),
      .symbol_path = text(object, "symbol"),
      .event_time_path = text(object, "event_time"),
      .event_time_unit = text(object, "event_time_unit"),
      .transaction_time_path = text(object, "transaction_time"),
      .transaction_time_unit = text(object, "transaction_time_unit"),
      .event_id_path = text(object, "event_id"),
      .first_event_id_path = text(object, "first_event_id"),
      .previous_event_id_path = text(object, "previous_event_id"),
      .bids_path = text(object, "bids"),
      .asks_path = text(object, "asks"),
      .bid_price_path = text(object, "bid_price"),
      .bid_quantity_path = text(object, "bid_quantity"),
      .ask_price_path = text(object, "ask_price"),
      .ask_quantity_path = text(object, "ask_quantity"),
      .trades_path = text(object, "trades"),
      .price_path = text(object, "price"),
      .quantity_path = text(object, "quantity"),
      .side_path = text(object, "side"),
      .snapshot_path = text(object, "snapshot"),
      .snapshot_value = text(object, "snapshot_value"),
  };
  if (result.event_time_unit.empty()) result.event_time_unit = "ms";
  if (result.transaction_time_unit.empty()) {
    result.transaction_time_unit = "ms";
  }
  return result;
}

[[nodiscard]] std::unordered_map<std::string, ChannelDefinition>
channel_definitions(const boost::json::object& manifest) {
  std::unordered_map<std::string, ChannelDefinition> result;
  const auto* channels = manifest.if_contains("channels");
  if (channels == nullptr || !channels->is_array()) return result;
  for (const auto& item : channels->as_array()) {
    if (!item.is_object()) continue;
    const auto& object = item.as_object();
    ChannelDefinition definition{
        .id = text(object, "id"),
        .kind = research_channel_kind(text(object, "kind")),
        .support = text(object, "support"),
        .wire = text(object, "wire"),
        .compression = text(object, "compression"),
        .depth_semantics = text(object, "depth_semantics"),
    };
    if (const auto* mapping_value = object.if_contains("mapping");
        mapping_value != nullptr && mapping_value->is_object()) {
      definition.mapping = mapping_from_json(mapping_value->as_object());
    }
    if (!definition.id.empty()) {
      result.emplace(definition.id, std::move(definition));
    }
  }
  return result;
}

[[nodiscard]] std::string read_payload(
    std::ifstream& frames,
    const FrameIndex& frame,
    std::string& error) {
  if (frame.length > kMaxWsMessageBytes) {
    error = "frame_payload_capacity_exceeded";
    return {};
  }
  std::string payload(static_cast<std::size_t>(frame.length), '\0');
  frames.clear();
  frames.seekg(static_cast<std::streamoff>(frame.offset));
  frames.read(payload.data(), static_cast<std::streamsize>(payload.size()));
  if (!frames) {
    error = "frame_payload_read_failed";
    return {};
  }
  return payload;
}

}
