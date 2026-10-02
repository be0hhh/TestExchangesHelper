#include "ResearchAnalysisInternal.hpp"

namespace exchange_probe::research_analysis {

void enforce_normalized_capacity(NormalizedEvent& event) {
  const std::string* fields[]{
      &event.native_id, &event.first_native_id, &event.previous_native_id,
      &event.symbol, &event.price, &event.quantity, &event.side,
      &event.bid_price, &event.bid_quantity, &event.ask_price,
      &event.ask_quantity,
  };
  std::size_t total = 0U;
  for (const auto* field : fields) {
    if (field->size() > kMaximumNormalizedFieldBytes - total) {
      event.validity = "normalized_field_capacity_exceeded";
      event.native_id.clear();
      event.first_native_id.clear();
      event.previous_native_id.clear();
      event.symbol.clear();
      event.price.clear();
      event.quantity.clear();
      event.side.clear();
      event.bid_price.clear();
      event.bid_quantity.clear();
      event.ask_price.clear();
      event.ask_quantity.clear();
      return;
    }
    total += field->size();
  }
}

[[nodiscard]] const boost::json::value* json_path(
    const boost::json::value& root,
    std::string_view path) {
  if (path.empty() || path == "$") return &root;
  const boost::json::value* current = &root;
  std::size_t begin = path.starts_with("$.") ? 2U : 0U;
  while (begin <= path.size()) {
    const auto end = path.find('.', begin);
    const auto token =
        path.substr(begin, end == std::string_view::npos
                               ? path.size() - begin
                               : end - begin);
    if (token.empty()) return nullptr;
    if (current->is_object()) {
      current = current->as_object().if_contains(token);
    } else if (current->is_array()) {
      std::size_t index = 0U;
      const auto [pointer, error] =
          std::from_chars(token.data(), token.data() + token.size(), index);
      if (error != std::errc{} ||
          pointer != token.data() + token.size() ||
          index >= current->as_array().size()) {
        return nullptr;
      }
      current = &current->as_array()[index];
    } else {
      return nullptr;
    }
    if (current == nullptr || end == std::string_view::npos) return current;
    begin = end + 1U;
  }
  return current;
}

[[nodiscard]] std::string scalar_text(
    const boost::json::value* value) {
  if (value == nullptr) return {};
  if (value->is_string()) return std::string{value->as_string()};
  if (value->is_uint64()) return std::to_string(value->as_uint64());
  if (value->is_int64()) return std::to_string(value->as_int64());
  if (value->is_double()) return boost::json::serialize(*value);
  if (value->is_bool()) return value->as_bool() ? "true" : "false";
  return {};
}

[[nodiscard]] std::optional<std::uint64_t> scalar_u64(
    const boost::json::value* value) {
  if (value == nullptr) return std::nullopt;
  if (value->is_uint64()) return value->as_uint64();
  if (value->is_int64() && value->as_int64() >= 0) {
    return static_cast<std::uint64_t>(value->as_int64());
  }
  if (value->is_string()) {
    std::uint64_t result = 0U;
    const auto string = value->as_string();
    const auto [pointer, error] = std::from_chars(
        string.data(), string.data() + string.size(), result);
    if (error == std::errc{} &&
        pointer == string.data() + string.size()) {
      return result;
    }
  }
  return std::nullopt;
}

[[nodiscard]] std::optional<std::uint64_t> timestamp_ns(
    const boost::json::value* value,
    std::string_view unit) {
  const auto raw = scalar_u64(value);
  if (!raw.has_value()) return std::nullopt;
  std::uint64_t multiplier = 0U;
  if (unit == "ns") multiplier = 1U;
  else if (unit == "us") multiplier = 1'000U;
  else if (unit == "ms") multiplier = 1'000'000U;
  else if (unit == "s") multiplier = 1'000'000'000U;
  else return std::nullopt;
  if (*raw > std::numeric_limits<std::uint64_t>::max() / multiplier) {
    return std::nullopt;
  }
  return *raw * multiplier;
}

[[nodiscard]] std::uint64_t fnv1a(std::string_view value) noexcept {
  std::uint64_t hash = 1469598103934665603ULL;
  for (const unsigned char character : value) {
    hash ^= character;
    hash *= 1099511628211ULL;
  }
  return hash;
}

[[nodiscard]] std::uint64_t semantic_event_hash(
    const NormalizedEvent& event) {
  std::string canonical;
  canonical.reserve(
      event.native_id.size() + event.symbol.size() + event.price.size() +
      event.quantity.size() + event.bid_price.size() +
      event.ask_price.size() + 96U);
  const auto append = [&](std::string_view value) {
    canonical.append(value);
    canonical.push_back('\x1f');
  };
  append(to_string(event.kind));
  append(event.native_id);
  append(event.first_native_id);
  append(event.previous_native_id);
  append(event.symbol);
  append(event.price);
  append(event.quantity);
  append(event.side);
  append(event.bid_price);
  append(event.bid_quantity);
  append(event.ask_price);
  append(event.ask_quantity);
  append(event.snapshot ? "snapshot" : "delta");
  if (event.exchange_event_ns.has_value()) {
    append(std::to_string(*event.exchange_event_ns));
  } else {
    append(std::string_view{});
  }
  if (event.exchange_transaction_ns.has_value()) {
    append(std::to_string(*event.exchange_transaction_ns));
  } else {
    append(std::string_view{});
  }
  return fnv1a(canonical);
}

[[nodiscard]] NormalizedEvent normalize_event(
    const FrameIndex& frame,
    const ChannelDefinition& channel,
    const boost::json::value& value,
    std::uint64_t event_id,
    std::uint64_t semantic_hash) {
  const auto& mapping = channel.mapping;
  const auto* data =
      mapping.data_path.empty() ? &value : json_path(value, mapping.data_path);
  if (data == nullptr) data = &value;
  NormalizedEvent event{
      .id = event_id,
      .frame_id = frame.id,
      .round = frame.round,
      .channel = frame.channel,
      .kind = channel.kind,
      .monotonic_ns = frame.monotonic_ns,
      .utc_ns = frame.utc_ns,
      .exchange_event_ns =
          timestamp_ns(
              json_path(*data, mapping.event_time_path),
              mapping.event_time_unit),
      .exchange_transaction_ns =
          timestamp_ns(
              json_path(*data, mapping.transaction_time_path),
              mapping.transaction_time_unit),
      .native_id = scalar_text(json_path(*data, mapping.event_id_path)),
      .first_native_id =
          scalar_text(json_path(*data, mapping.first_event_id_path)),
      .previous_native_id =
          scalar_text(json_path(*data, mapping.previous_event_id_path)),
      .symbol = scalar_text(json_path(*data, mapping.symbol_path)),
      .price = scalar_text(json_path(*data, mapping.price_path)),
      .quantity = scalar_text(json_path(*data, mapping.quantity_path)),
      .side = scalar_text(json_path(*data, mapping.side_path)),
      .bid_price = scalar_text(json_path(*data, mapping.bid_price_path)),
      .bid_quantity =
          scalar_text(json_path(*data, mapping.bid_quantity_path)),
      .ask_price = scalar_text(json_path(*data, mapping.ask_price_path)),
      .ask_quantity =
          scalar_text(json_path(*data, mapping.ask_quantity_path)),
      .semantic_hash = semantic_hash,
  };
  const auto snapshot =
      scalar_text(json_path(*data, mapping.snapshot_path));
  event.snapshot =
      channel.depth_semantics == "complete_snapshot" ||
      (!mapping.snapshot_value.empty() &&
       snapshot == mapping.snapshot_value);
  if (channel.kind == ResearchChannelKind::Unknown) {
    event.validity = "unknown_channel_kind";
  } else if (
      channel.support != "exact" &&
      mapping.event_time_path.empty() &&
      mapping.transaction_time_path.empty() &&
      mapping.event_id_path.empty() &&
      mapping.price_path.empty() &&
      mapping.bid_price_path.empty() &&
      mapping.bids_path.empty()) {
    event.validity = "observed_only_mapping_unavailable";
  } else if (
      frame.binary && channel.wire != "binary_json" &&
      channel.compression != "gzip") {
    event.validity = "binary_adapter_required";
  }
  return event;
}

[[nodiscard]] boost::json::object event_json(
    const NormalizedEvent& event) {
  return {
      {"schema", kResearchEventSchema},
      {"event_id", event.id},
      {"frame_id", event.frame_id},
      {"round", event.round},
      {"channel_id", event.channel},
      {"event_kind", to_string(event.kind)},
      {"monotonic_ns", event.monotonic_ns},
      {"utc_ns", event.utc_ns},
      {"exchange_event_ns",
       event.exchange_event_ns.has_value()
           ? boost::json::value(*event.exchange_event_ns)
           : boost::json::value{}},
      {"exchange_transaction_ns",
       event.exchange_transaction_ns.has_value()
           ? boost::json::value(*event.exchange_transaction_ns)
           : boost::json::value{}},
      {"native_id", event.native_id},
      {"first_native_id", event.first_native_id},
      {"previous_native_id", event.previous_native_id},
      {"symbol", event.symbol},
      {"price", event.price},
      {"quantity", event.quantity},
      {"side", event.side},
      {"bid_price", event.bid_price},
      {"bid_quantity", event.bid_quantity},
      {"ask_price", event.ask_price},
      {"ask_quantity", event.ask_quantity},
      {"snapshot", event.snapshot},
      {"semantic_hash", event.semantic_hash},
      {"validity", event.validity},
  };
}

}
