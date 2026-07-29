#include "exchange_probe/research.hpp"
#include "exchange_probe/contracts.hpp"

#include <boost/json/array.hpp>
#include <boost/json/object.hpp>
#include <boost/json/parse.hpp>
#include <boost/json/serialize.hpp>
#include <boost/system/error_code.hpp>

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <map>
#include <optional>
#include <ostream>
#include <set>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

namespace exchange_probe {
namespace {

inline constexpr std::size_t kMaximumAnalysisJsonBytes =
    64U * 1024U * 1024U;
inline constexpr std::uint64_t kMaximumDerivedJsonlBytes =
    256ULL * 1024ULL * 1024ULL;
inline constexpr std::size_t kMaximumNormalizedFieldBytes = 64U * 1024U;
inline constexpr std::size_t kMaximumBookLevelsPerSide = 100'000U;
inline constexpr std::size_t kMaximumBookScalarBytes = 256U;
inline constexpr std::uint64_t kMaximumFramesFileBytes =
    16ULL * 1024ULL * 1024ULL * 1024ULL;
inline constexpr std::uint64_t kMarketEffectWindowNs =
    250ULL * 1000ULL * 1000ULL;
inline constexpr std::uint64_t kMaximumRelations = 1'000'000ULL;

struct FrameIndex {
  std::uint64_t id{0U};
  unsigned round{0U};
  std::string channel;
  bool binary{false};
  std::uint64_t monotonic_ns{0U};
  std::uint64_t utc_ns{0U};
  std::uint64_t offset{0U};
  std::uint64_t length{0U};
};

struct ChannelDefinition {
  std::string id;
  ResearchChannelKind kind{ResearchChannelKind::Unknown};
  std::string support;
  std::string wire;
  std::string compression;
  std::string depth_semantics;
  JsonEventMapping mapping;
};

struct NormalizedEvent {
  std::uint64_t id{0U};
  std::uint64_t frame_id{0U};
  unsigned round{0U};
  std::string channel;
  ResearchChannelKind kind{ResearchChannelKind::Unknown};
  std::uint64_t monotonic_ns{0U};
  std::uint64_t utc_ns{0U};
  std::optional<std::uint64_t> exchange_event_ns;
  std::optional<std::uint64_t> exchange_transaction_ns;
  std::string native_id;
  std::string first_native_id;
  std::string previous_native_id;
  std::string symbol;
  std::string price;
  std::string quantity;
  std::string side;
  std::string bid_price;
  std::string bid_quantity;
  std::string ask_price;
  std::string ask_quantity;
  bool snapshot{false};
  std::uint64_t semantic_hash{0U};
  std::string validity{"valid"};
};

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

struct DecimalLess {
  [[nodiscard]] bool operator()(
      const std::string& lhs,
      const std::string& rhs) const {
    const auto normalize = [](std::string_view value) {
      const auto point = value.find('.');
      std::string integer{
          value.substr(0U, point == std::string_view::npos
                                ? value.size()
                                : point)};
      std::string fraction{
          point == std::string_view::npos ? std::string{}
                                          : std::string{value.substr(point + 1U)}};
      const auto first = integer.find_first_not_of('0');
      integer = first == std::string::npos ? "0" : integer.substr(first);
      while (!fraction.empty() && fraction.back() == '0') {
        fraction.pop_back();
      }
      return std::pair{std::move(integer), std::move(fraction)};
    };
    const auto left = normalize(lhs);
    const auto right = normalize(rhs);
    if (left.first.size() != right.first.size()) {
      return left.first.size() < right.first.size();
    }
    if (left.first != right.first) return left.first < right.first;
    const auto length = std::max(left.second.size(), right.second.size());
    for (std::size_t index = 0U; index < length; ++index) {
      const char left_digit =
          index < left.second.size() ? left.second[index] : '0';
      const char right_digit =
          index < right.second.size() ? right.second[index] : '0';
      if (left_digit != right_digit) return left_digit < right_digit;
    }
    return false;
  }
};

struct BookState {
  std::map<std::string, std::string, DecimalLess> bids;
  std::map<std::string, std::string, DecimalLess> asks;
  bool valid{false};
  bool invalid_gap{false};
  bool complete{false};
  bool capacity_exceeded{false};
  std::string last_native_id;
  std::string last_bid;
  std::string last_ask;
  std::optional<std::uint64_t> first_event_ns;
  std::optional<std::uint64_t> reconstruction_ns;
  std::optional<std::uint64_t> full_reconstruction_ns;
};

[[nodiscard]] boost::json::object distribution_json(
    std::vector<std::uint64_t> values) {
  static constexpr std::uint64_t boundaries_ns[]{
      50'000U, 100'000U, 250'000U, 500'000U, 1'000'000U,
      2'000'000U, 5'000'000U, 10'000'000U, 25'000'000U,
      50'000'000U, 100'000'000U, 250'000'000U, 1'000'000'000U,
  };
  boost::json::array buckets;
  if (values.empty()) {
    return {
        {"count", 0},
        {"unit", "ns"},
        {"buckets", std::move(buckets)},
    };
  }
  std::sort(values.begin(), values.end());
  const auto percentile = [&](std::size_t percent) {
    return values[((values.size() - 1U) * percent) / 100U];
  };
  std::size_t previous = 0U;
  for (const auto boundary : boundaries_ns) {
    const auto end = static_cast<std::size_t>(
        std::upper_bound(values.begin(), values.end(), boundary) -
        values.begin());
    buckets.emplace_back(boost::json::object{
        {"le_ns", boundary},
        {"count", end - previous},
    });
    previous = end;
  }
  buckets.emplace_back(boost::json::object{
      {"le_ns", nullptr},
      {"count", values.size() - previous},
  });
  return {
      {"count", values.size()},
      {"unit", "ns"},
      {"min", values.front()},
      {"p50", percentile(50U)},
      {"p95", percentile(95U)},
      {"p99", percentile(99U)},
      {"max", values.back()},
      {"buckets", std::move(buckets)},
  };
}

[[nodiscard]] bool native_id_less(
    std::string_view lhs,
    std::string_view rhs) noexcept {
  const bool lhs_digits =
      !lhs.empty() &&
      std::all_of(lhs.begin(), lhs.end(), [](char value) {
        return value >= '0' && value <= '9';
      });
  const bool rhs_digits =
      !rhs.empty() &&
      std::all_of(rhs.begin(), rhs.end(), [](char value) {
        return value >= '0' && value <= '9';
      });
  if (lhs_digits && rhs_digits) {
    const auto lhs_first = lhs.find_first_not_of('0');
    const auto rhs_first = rhs.find_first_not_of('0');
    lhs = lhs_first == std::string_view::npos ? std::string_view{"0"}
                                              : lhs.substr(lhs_first);
    rhs = rhs_first == std::string_view::npos ? std::string_view{"0"}
                                              : rhs.substr(rhs_first);
    if (lhs.size() != rhs.size()) return lhs.size() < rhs.size();
  }
  return lhs < rhs;
}

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
    FrameIndex frame{
        .id = u64(object, "frame_id"),
        .round = static_cast<unsigned>(u64(object, "round")),
        .channel = text(object, "channel_id"),
        .binary = text(object, "opcode") == "binary",
        .monotonic_ns = u64(object, "monotonic_ns"),
        .utc_ns = u64(object, "utc_ns"),
        .offset = u64(object, "offset"),
        .length = u64(object, "length"),
    };
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

[[nodiscard]] bool is_zero_decimal(std::string_view value) {
  if (value.empty()) return false;
  for (const char character : value) {
    if (character != '0' && character != '.') return false;
  }
  return true;
}

void apply_levels(
    BookState& state,
    const boost::json::value* value,
    bool bids) {
  if (value == nullptr || !value->is_array()) return;
  auto& side = bids ? state.bids : state.asks;
  for (const auto& level : value->as_array()) {
    if (!level.is_array() || level.as_array().size() < 2U) continue;
    const auto price = scalar_text(&level.as_array()[0]);
    const auto quantity = scalar_text(&level.as_array()[1]);
    if (price.empty() || quantity.empty()) continue;
    if (price.size() > kMaximumBookScalarBytes ||
        quantity.size() > kMaximumBookScalarBytes) {
      state.capacity_exceeded = true;
      continue;
    }
    if (is_zero_decimal(quantity)) {
      side.erase(price);
    } else {
      if (!side.contains(price) &&
          side.size() >= kMaximumBookLevelsPerSide) {
        state.capacity_exceeded = true;
        continue;
      }
      side[price] = quantity;
    }
  }
}

[[nodiscard]] std::pair<std::string, std::string> best_levels(
    const BookState& state) {
  const auto bid =
      state.bids.empty() ? std::string{} : state.bids.rbegin()->first;
  const auto ask =
      state.asks.empty() ? std::string{} : state.asks.begin()->first;
  return {bid, ask};
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

[[nodiscard]] bool emit_relation(
    std::ofstream& output,
    std::uint64_t& relation_id,
    bool& output_full,
    std::map<std::string, std::vector<std::uint64_t>>& lag_distributions,
    std::string_view mode,
    std::string_view evidence,
    const NormalizedEvent& source,
    const NormalizedEvent& target,
    std::string_view reason) {
  if (relation_id >= kMaximumRelations) output_full = true;
  if (output_full) return false;
  const auto lag =
      target.monotonic_ns >= source.monotonic_ns
          ? static_cast<std::int64_t>(
                target.monotonic_ns - source.monotonic_ns)
          : -static_cast<std::int64_t>(
                source.monotonic_ns - target.monotonic_ns);
  boost::json::object row{
      {"schema", kResearchRelationSchema},
      {"relation_id", relation_id++},
      {"mode", mode},
      {"evidence", evidence},
      {"source_event_id", source.id},
      {"target_event_id", target.id},
      {"receive_lag_ns", lag},
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
  const auto absolute_lag =
      lag >= 0 ? static_cast<std::uint64_t>(lag)
               : static_cast<std::uint64_t>(-(lag + 1)) + 1U;
  lag_distributions[
      std::string{mode} + ":" + source.channel + "->" + target.channel]
      .push_back(absolute_lag);
  return true;
}

}  // namespace

int analyze_research_bundle(
    const std::filesystem::path& directory,
    std::ostream& output,
    std::ostream& error_output) {
  std::string error;
  auto manifest = read_object(directory / "manifest.json", error);
  if (!manifest.has_value()) {
    error_output << "analysis_error: " << error << '\n';
    return 2;
  }
  if (text(*manifest, "schema") != kResearchBundleSchema) {
    error_output << "analysis_error: incompatible_bundle_schema\n";
    return 2;
  }
  if (u64(*manifest, "schema_version") != 3U) {
    error_output << "analysis_error: incompatible_bundle_version\n";
    return 2;
  }
  auto frames_index =
      read_frame_index(directory / "frames.jsonl", error);
  if (!error.empty()) {
    error_output << "analysis_error: " << error << '\n';
    return 2;
  }
  std::error_code frames_size_error;
  const auto frames_size = std::filesystem::file_size(
      directory / "frames.bin", frames_size_error);
  if (frames_size_error || frames_size > kMaximumFramesFileBytes) {
    error_output
        << "analysis_error: "
        << (frames_size_error ? "frames_size_failed"
                              : "frames_capacity_exceeded")
        << '\n';
    return 2;
  }
  for (const auto& frame : frames_index) {
    if (frame.offset + frame.length > frames_size) {
      error_output << "analysis_error: frame_index_out_of_bounds\n";
      return 2;
    }
  }
  std::ifstream frames{directory / "frames.bin", std::ios::binary};
  if (!frames) {
    error_output << "analysis_error: frames_open_failed\n";
    return 2;
  }
  const auto definitions = channel_definitions(*manifest);
  if (definitions.empty()) {
    error_output << "analysis_error: manifest_has_no_channels\n";
    return 2;
  }
  std::ofstream events_file{directory / "events.jsonl", std::ios::binary};
  std::ofstream states_file{
      directory / "state_transitions.jsonl", std::ios::binary};
  std::ofstream relations_file{
      directory / "relations.jsonl", std::ios::binary};
  if (!events_file || !states_file || !relations_file) {
    error_output << "analysis_error: output_open_failed\n";
    return 2;
  }
  std::vector<NormalizedEvent> events;
  events.reserve(frames_index.size());
  std::map<std::pair<unsigned, std::string>, BookState> books;
  std::unordered_map<std::uint64_t, FrameIndex> first_frame_hash;
  std::unordered_map<std::string, std::pair<std::uint64_t, std::uint64_t>>
      native_identity;
  std::uint64_t exact_frame_duplicates = 0U;
  std::uint64_t semantic_duplicates = 0U;
  std::uint64_t identity_collisions = 0U;
  std::uint64_t gaps = 0U;
  std::uint64_t out_of_order = 0U;
  std::uint64_t book_capacity_exceeded = 0U;
  std::uint64_t valid_bbo_transitions = 0U;
  std::uint64_t next_event_id = 0U;
  std::map<std::string, std::uint64_t> previous_arrival;
  std::map<std::string, std::uint64_t> validity_counts;
  std::map<std::string, std::vector<std::uint64_t>> arrival_intervals;
  std::map<std::string, std::vector<std::uint64_t>>
      first_usable_bbo_durations;
  std::map<std::string, std::vector<std::uint64_t>>
      full_reconstruction_durations;
  for (const auto& frame : frames_index) {
    const auto definition = definitions.find(frame.channel);
    if (definition == definitions.end()) continue;
    auto payload = read_payload(frames, frame, error);
    if (!error.empty()) {
      error_output << "analysis_error: " << error << '\n';
      return 2;
    }
    const auto hash = fnv1a(payload);
    const auto [first, inserted] =
        first_frame_hash.emplace(hash, frame);
    if (!inserted && first->second.length == frame.length) {
      auto first_payload = read_payload(frames, first->second, error);
      if (!error.empty()) {
        error_output << "analysis_error: " << error << '\n';
        return 2;
      }
      if (first_payload == payload) ++exact_frame_duplicates;
    }
    std::string decoded_payload;
    std::string decode_error;
    std::string_view semantic_payload{payload};
    if (definition->second.compression == "gzip") {
      if (decode_gzip_bounded(
              payload, kMaxWsMessageBytes, decoded_payload,
              decode_error)) {
        semantic_payload = decoded_payload;
      }
    }
    boost::system::error_code parse_error;
    auto value = boost::json::parse(semantic_payload, parse_error);
    NormalizedEvent event;
    if (!decode_error.empty() || parse_error) {
      event = {
          .id = next_event_id++,
          .frame_id = frame.id,
          .round = frame.round,
          .channel = frame.channel,
          .kind = definition->second.kind,
          .monotonic_ns = frame.monotonic_ns,
          .utc_ns = frame.utc_ns,
          .semantic_hash = hash,
          .validity = !decode_error.empty()
                          ? "decompression_error:" + decode_error
                          : frame.binary ? "binary_adapter_required"
                                         : "json_parse_error",
      };
    } else {
      event = normalize_event(
          frame, definition->second, value, next_event_id++, hash);
      event.semantic_hash = semantic_event_hash(event);
    }
    enforce_normalized_capacity(event);
    const auto previous = previous_arrival.find(event.channel);
    if (previous != previous_arrival.end() &&
        event.monotonic_ns >= previous->second) {
      arrival_intervals[event.channel].push_back(
          event.monotonic_ns - previous->second);
    }
    previous_arrival[event.channel] = event.monotonic_ns;
    if (!event.native_id.empty()) {
      const auto key =
          event.channel + ":" + std::to_string(event.round) + ":" +
          event.native_id;
      const auto [iterator, identity_inserted] =
          native_identity.emplace(key, std::pair{event.id, event.semantic_hash});
      if (!identity_inserted) {
        if (iterator->second.second == event.semantic_hash) {
          ++semantic_duplicates;
        } else {
          ++identity_collisions;
        }
      }
    }
    if (event.kind == ResearchChannelKind::BookTicker) {
      if (!event.bid_price.empty() && !event.ask_price.empty()) {
        ++valid_bbo_transitions;
        states_file << boost::json::serialize(boost::json::object{
            {"schema", "exchange.api_probe.state_transition.v1"},
            {"event_id", event.id},
            {"round", event.round},
            {"channel_id", event.channel},
            {"kind", "bbo"},
            {"bid_price", event.bid_price},
            {"bid_quantity", event.bid_quantity},
            {"ask_price", event.ask_price},
            {"ask_quantity", event.ask_quantity},
            {"monotonic_ns", event.monotonic_ns},
            {"exchange_event_ns",
             event.exchange_event_ns.has_value()
                 ? boost::json::value(*event.exchange_event_ns)
                 : boost::json::value{}},
            {"valid", true},
        }) << '\n';
      }
    } else if (event.kind == ResearchChannelKind::Depth && !parse_error) {
      const auto* data =
          definition->second.mapping.data_path.empty()
              ? &value
              : json_path(value, definition->second.mapping.data_path);
      if (data == nullptr) data = &value;
      auto& state = books[{event.round, event.channel}];
      if (!state.first_event_ns.has_value()) {
        state.first_event_ns = event.monotonic_ns;
      }
      if (event.snapshot) {
        state.bids.clear();
        state.asks.clear();
        state.invalid_gap = false;
        state.complete = true;
        state.capacity_exceeded = false;
      }
      if (!event.previous_native_id.empty() &&
          !state.last_native_id.empty() &&
          event.previous_native_id != state.last_native_id) {
        state.invalid_gap = true;
        ++gaps;
      }
      if (!event.native_id.empty() &&
          !state.last_native_id.empty() &&
          native_id_less(event.native_id, state.last_native_id)) {
        ++out_of_order;
      }
      apply_levels(
          state,
          json_path(*data, definition->second.mapping.bids_path),
          true);
      apply_levels(
          state,
          json_path(*data, definition->second.mapping.asks_path),
          false);
      if (!event.native_id.empty()) state.last_native_id = event.native_id;
      const auto [bid, ask] = best_levels(state);
      event.bid_price = bid;
      event.ask_price = ask;
      const bool valid = !state.invalid_gap && !bid.empty() && !ask.empty() &&
                         !state.capacity_exceeded &&
                         DecimalLess{}(bid, ask);
      if (state.capacity_exceeded) {
        event.validity = "book_capacity_exceeded";
        ++book_capacity_exceeded;
      }
      if (valid && (bid != state.last_bid || ask != state.last_ask)) {
        ++valid_bbo_transitions;
        states_file << boost::json::serialize(boost::json::object{
            {"schema", "exchange.api_probe.state_transition.v1"},
            {"event_id", event.id},
            {"round", event.round},
            {"channel_id", event.channel},
            {"kind", "depth_derived_bbo"},
            {"bid_price", bid},
            {"ask_price", ask},
            {"monotonic_ns", event.monotonic_ns},
            {"exchange_event_ns",
             event.exchange_event_ns.has_value()
                 ? boost::json::value(*event.exchange_event_ns)
                 : boost::json::value{}},
            {"valid", true},
            {"complete", state.complete},
        }) << '\n';
        state.last_bid = bid;
        state.last_ask = ask;
      }
      if (valid && !state.reconstruction_ns.has_value() &&
          state.first_event_ns.has_value()) {
        state.reconstruction_ns =
            event.monotonic_ns - *state.first_event_ns;
        first_usable_bbo_durations[event.channel].push_back(
            *state.reconstruction_ns);
      }
      if (valid && state.complete &&
          !state.full_reconstruction_ns.has_value() &&
          state.first_event_ns.has_value()) {
        state.full_reconstruction_ns =
            event.monotonic_ns - *state.first_event_ns;
        full_reconstruction_durations[event.channel].push_back(
            *state.full_reconstruction_ns);
      }
      state.valid = valid;
    }
    ++validity_counts[event.validity];
    events_file << boost::json::serialize(event_json(event)) << '\n';
    const auto event_position = events_file.tellp();
    const auto state_position = states_file.tellp();
    const auto event_offset =
        static_cast<std::streamoff>(event_position);
    const auto state_offset =
        static_cast<std::streamoff>(state_position);
    if (event_offset < 0 || state_offset < 0 ||
        static_cast<std::uint64_t>(event_offset) >
            kMaximumDerivedJsonlBytes ||
        static_cast<std::uint64_t>(state_offset) >
            kMaximumDerivedJsonlBytes) {
      error_output << "analysis_error: derived_artifact_capacity_exceeded\n";
      return 2;
    }
    events.push_back(std::move(event));
  }

  std::uint64_t relation_id = 0U;
  bool relation_output_full = false;
  std::map<std::string, std::vector<std::uint64_t>> relation_lags;
  std::unordered_map<std::string, std::size_t> identity_index;
  std::unordered_map<std::string, std::size_t> exchange_time_index;
  std::unordered_map<std::string, std::size_t> transaction_time_index;
  std::unordered_map<std::string, std::size_t> state_index;
  std::deque<std::size_t> recent_market_events;
  std::uint64_t relation_candidates_dropped = 0U;
  constexpr std::size_t kMaximumMarketCandidatesPerEvent = 128U;
  for (std::size_t index = 0U; index < events.size(); ++index) {
    const auto& target = events[index];
    if (!target.native_id.empty()) {
      const auto key =
          std::to_string(target.round) + ":" + target.native_id;
      const auto previous = identity_index.find(key);
      if (previous != identity_index.end() &&
          events[previous->second].channel != target.channel) {
        if (!emit_relation(
                relations_file, relation_id, relation_output_full,
                relation_lags,
                "identity", "observed",
                events[previous->second], target,
                "same_native_id_cross_channel_unverified_scope")) {
          ++relation_candidates_dropped;
        }
      }
      identity_index[key] = index;
    }
    if (target.exchange_event_ns.has_value()) {
      const auto key =
          std::to_string(target.round) + ":" +
          std::to_string(*target.exchange_event_ns);
      const auto previous = exchange_time_index.find(key);
      if (previous != exchange_time_index.end() &&
          events[previous->second].channel != target.channel) {
        if (!emit_relation(
                relations_file, relation_id, relation_output_full,
                relation_lags,
                "exchange_time_cohort",
                "consistent", events[previous->second], target,
                "same_exchange_event_time")) {
          ++relation_candidates_dropped;
        }
      }
      exchange_time_index[key] = index;
    }
    if (target.exchange_transaction_ns.has_value()) {
      const auto key =
          std::to_string(target.round) + ":" +
          std::to_string(*target.exchange_transaction_ns);
      const auto previous = transaction_time_index.find(key);
      if (previous != transaction_time_index.end() &&
          events[previous->second].channel != target.channel) {
        if (!emit_relation(
                relations_file, relation_id, relation_output_full,
                relation_lags,
                "exchange_time_cohort",
                "consistent", events[previous->second], target,
                "same_exchange_transaction_time")) {
          ++relation_candidates_dropped;
        }
      }
      transaction_time_index[key] = index;
    }
    const bool target_book =
        target.kind == ResearchChannelKind::BookTicker ||
        target.kind == ResearchChannelKind::Depth;
    if (target_book && !target.bid_price.empty() &&
        !target.ask_price.empty()) {
      const auto key =
          std::to_string(target.round) + ":" + target.bid_price + ":" +
          target.ask_price;
      const auto previous = state_index.find(key);
      if (previous != state_index.end() &&
          events[previous->second].channel != target.channel) {
        if (!emit_relation(
                relations_file, relation_id, relation_output_full,
                relation_lags,
                "state_convergence",
                "consistent", events[previous->second], target,
                "same_resulting_bbo")) {
          ++relation_candidates_dropped;
        }
      }
      state_index[key] = index;
    }
    while (!recent_market_events.empty() &&
           events[recent_market_events.front()].monotonic_ns +
                   kMarketEffectWindowNs <
               target.monotonic_ns) {
      recent_market_events.pop_front();
    }
    std::size_t considered = 0U;
    for (auto iterator = recent_market_events.rbegin();
         iterator != recent_market_events.rend();
         ++iterator) {
      if (considered++ >= kMaximumMarketCandidatesPerEvent) {
        ++relation_candidates_dropped;
        break;
      }
      const auto& source = events[*iterator];
      if (source.round != target.round ||
          source.channel == target.channel) {
        continue;
      }
      const bool source_book =
          source.kind == ResearchChannelKind::BookTicker ||
          source.kind == ResearchChannelKind::Depth;
      const auto& book = source_book ? source : target;
      const auto& trade =
          source.kind == ResearchChannelKind::Trade ? source : target;
      const bool book_trade_pair =
          (source_book && target.kind == ResearchChannelKind::Trade) ||
          (target_book && source.kind == ResearchChannelKind::Trade);
      const bool price_touches_or_crosses =
          !trade.price.empty() && !book.bid_price.empty() &&
          !book.ask_price.empty() &&
          (!DecimalLess{}(book.bid_price, trade.price) ||
           !DecimalLess{}(trade.price, book.ask_price));
      if (book_trade_pair && price_touches_or_crosses) {
        if (!emit_relation(
                relations_file, relation_id, relation_output_full,
                relation_lags,
                "market_effect", "heuristic",
                source, target,
                "trade_touches_or_crosses_bbo_within_receive_window")) {
          ++relation_candidates_dropped;
        }
      }
    }
    if (target_book || target.kind == ResearchChannelKind::Trade) {
      recent_market_events.push_back(index);
    }
  }

  events_file.flush();
  states_file.flush();
  relations_file.flush();
  if (!events_file || !states_file || !relations_file) {
    error_output << "analysis_error: artifact_write_failed\n";
    return 2;
  }
  boost::json::object arrival_distributions;
  for (auto& [channel, values] : arrival_intervals) {
    arrival_distributions[channel] = distribution_json(std::move(values));
  }
  boost::json::object validity_summary;
  for (const auto& [validity, count] : validity_counts) {
    validity_summary[validity] = count;
  }
  boost::json::object usable_bbo_distributions;
  for (auto& [channel, values] : first_usable_bbo_durations) {
    usable_bbo_distributions[channel] =
        distribution_json(std::move(values));
  }
  boost::json::object full_reconstruction_distributions;
  for (auto& [channel, values] : full_reconstruction_durations) {
    full_reconstruction_distributions[channel] =
        distribution_json(std::move(values));
  }
  boost::json::object relation_lag_distributions;
  for (auto& [relation, values] : relation_lags) {
    relation_lag_distributions[relation] =
        distribution_json(std::move(values));
  }
  boost::json::object findings{
      {"schema", kResearchFindingsSchema},
      {"evidence_policy",
       "relations are observations; heuristic edges do not assert causality"},
      {"frames", frames_index.size()},
      {"events", events.size()},
      {"relations", relation_id},
      {"relation_output_full", relation_output_full},
      {"relation_candidates_dropped", relation_candidates_dropped},
      {"valid_bbo_transitions", valid_bbo_transitions},
      {"event_validity", std::move(validity_summary)},
      {"arrival_intervals_by_channel", std::move(arrival_distributions)},
      {"absolute_receive_lag_by_relation",
       std::move(relation_lag_distributions)},
      {"first_usable_bbo_by_channel",
       std::move(usable_bbo_distributions)},
      {"full_bbo_reconstruction",
       boost::json::object{
           {"status",
            full_reconstruction_durations.empty()
                ? "requires_sequence_aligned_snapshot_evidence"
                : "observed"},
           {"durations_by_channel",
            std::move(full_reconstruction_distributions)},
       }},
      {"anomalies",
       boost::json::object{
           {"exact_frame_duplicates", exact_frame_duplicates},
           {"semantic_duplicates", semantic_duplicates},
           {"identity_collisions", identity_collisions},
           {"sequence_gaps", gaps},
           {"out_of_order", out_of_order},
           {"book_capacity_exceeded", book_capacity_exceeded},
       }},
      {"relation_modes",
       boost::json::array{
           "identity",
           "exchange_time_cohort",
           "market_effect",
           "state_convergence",
       }},
  };
  std::ofstream findings_file{
      directory / "findings.json", std::ios::binary};
  findings_file << boost::json::serialize(findings) << '\n';
  std::ofstream report{directory / "REPORT.md", std::ios::binary};
  report
      << "# Exchange channel research\n\n"
      << "- Evidence: diagnostic observation; causality is not asserted.\n"
      << "- Frames/events/relations: " << frames_index.size() << '/'
      << events.size() << '/' << relation_id << "\n"
      << "- Valid BBO transitions: " << valid_bbo_transitions << "\n"
      << "- Exact frame duplicates: " << exact_frame_duplicates << "\n"
      << "- Semantic duplicates: " << semantic_duplicates << "\n"
      << "- Identity collisions: " << identity_collisions << "\n"
      << "- Sequence gaps/out-of-order: " << gaps << '/' << out_of_order
      << "\n"
      << "- First usable BBO timing is distinct from full snapshot-aligned "
         "book reconstruction.\n"
      << "- Exchange and local receive clocks remain separate unless clock "
         "calibration is explicitly present.\n";
  findings_file.flush();
  report.flush();
  if (!findings_file || !report) {
    error_output << "analysis_error: summary_write_failed\n";
    return 2;
  }
  output << "analysis_bundle=" << directory.string()
         << " events=" << events.size()
         << " relations=" << relation_id << '\n';
  return 0;
}

}  // namespace exchange_probe
