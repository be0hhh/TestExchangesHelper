#include "exchange_probe/research_profile.hpp"

#include <boost/json/array.hpp>
#include <boost/json/object.hpp>
#include <boost/json/parse.hpp>
#include <boost/json/serialize.hpp>
#include <boost/system/error_code.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>
#include <iterator>
#include <limits>
#include <set>
#include <tuple>
#include <utility>

namespace exchange_probe {
namespace {

template <std::size_t Size>
[[nodiscard]] bool only_keys(
    const boost::json::object& object,
    const std::array<std::string_view, Size>& allowed,
    std::string& error,
  std::string_view scope) {
  for (const auto& member : object) {
    const std::string_view key{member.key().data(), member.key().size()};
    if (std::find(allowed.begin(), allowed.end(), key) ==
        allowed.end()) {
      error = std::string{scope} + "_unknown_property:" +
              std::string{key};
      return false;
    }
  }
  return true;
}

[[nodiscard]] std::string text(
    const boost::json::object& object,
    std::string_view key) {
  const auto* value = object.if_contains(key);
  return value != nullptr && value->is_string()
             ? std::string{value->as_string()}
             : std::string{};
}

[[nodiscard]] bool boolean(
    const boost::json::object& object,
    std::string_view key,
    bool fallback = false) noexcept {
  const auto* value = object.if_contains(key);
  return value != nullptr && value->is_bool() ? value->as_bool() : fallback;
}

[[nodiscard]] bool optional_string(
    const boost::json::object& object,
    std::string_view key) noexcept {
  const auto* value = object.if_contains(key);
  return value == nullptr || value->is_string();
}

[[nodiscard]] bool optional_boolean(
    const boost::json::object& object,
    std::string_view key) noexcept {
  const auto* value = object.if_contains(key);
  return value == nullptr || value->is_bool();
}

[[nodiscard]] bool unsigned_at_most(
    const boost::json::object& object,
    std::string_view key,
    std::uint64_t maximum) noexcept {
  const auto* value = object.if_contains(key);
  if (value == nullptr) return true;
  if (value->is_uint64()) return value->as_uint64() <= maximum;
  return value->is_int64() && value->as_int64() >= 0 &&
         static_cast<std::uint64_t>(value->as_int64()) <= maximum;
}

[[nodiscard]] bool optional_string_array(
    const boost::json::object& object,
    std::string_view key) noexcept {
  const auto* value = object.if_contains(key);
  if (value == nullptr) return true;
  if (!value->is_array()) return false;
  return std::all_of(
      value->as_array().begin(),
      value->as_array().end(),
      [](const boost::json::value& item) { return item.is_string(); });
}

[[nodiscard]] unsigned unsigned_value(
    const boost::json::object& object,
    std::string_view key,
    unsigned fallback = 0U) noexcept {
  const auto* value = object.if_contains(key);
  if (value == nullptr) return fallback;
  if (value->is_uint64() &&
      value->as_uint64() <= static_cast<std::uint64_t>(
          std::numeric_limits<unsigned>::max())) {
    return static_cast<unsigned>(value->as_uint64());
  }
  if (value->is_int64() && value->as_int64() >= 0 &&
      static_cast<std::uint64_t>(value->as_int64()) <=
          static_cast<std::uint64_t>(std::numeric_limits<unsigned>::max())) {
    return static_cast<unsigned>(value->as_int64());
  }
  return fallback;
}

[[nodiscard]] std::vector<std::string> strings(
    const boost::json::object& object,
    std::string_view key) {
  std::vector<std::string> result;
  const auto* value = object.if_contains(key);
  if (value == nullptr || !value->is_array()) return result;
  result.reserve(value->as_array().size());
  for (const auto& item : value->as_array()) {
    if (item.is_string()) result.emplace_back(item.as_string());
  }
  return result;
}

[[nodiscard]] JsonEventMapping mapping(
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

[[nodiscard]] std::optional<ResearchChannel> parse_channel(
    const boost::json::object& object,
    std::string& error) {
  static constexpr std::array channel_keys{
      std::string_view{"id"}, std::string_view{"name"},
      std::string_view{"kind"}, std::string_view{"support"},
      std::string_view{"transport"}, std::string_view{"wire"},
      std::string_view{"host"}, std::string_view{"port"},
      std::string_view{"path"}, std::string_view{"subscribe"},
      std::string_view{"subscribe_binary"}, std::string_view{"compression"},
      std::string_view{"depth_semantics"}, std::string_view{"depth_levels"},
      std::string_view{"adapter"}, std::string_view{"capabilities"},
      std::string_view{"mapping"},
  };
  if (!only_keys(object, channel_keys, error, "channel")) {
    return std::nullopt;
  }
  static constexpr std::string_view string_keys[]{
      "id", "name", "kind", "support", "transport", "wire", "host",
      "path", "subscribe", "compression", "depth_semantics", "adapter",
  };
  if (std::any_of(
          std::begin(string_keys), std::end(string_keys),
          [&](std::string_view key) {
            return !optional_string(object, key);
          }) ||
      !optional_boolean(object, "subscribe_binary") ||
      !unsigned_at_most(object, "port", 65'535U) ||
      !unsigned_at_most(object, "depth_levels", 100'000U) ||
      !optional_string_array(object, "capabilities")) {
    error = "invalid_channel_property_type";
    return std::nullopt;
  }
  const auto support_text = text(object, "support");
  const auto wire_text = text(object, "wire");
  const auto compression_text = text(object, "compression");
  const auto depth_semantics_text = text(object, "depth_semantics");
  const auto one_of = [](std::string_view value, auto... expected) {
    return ((value == expected) || ...);
  };
  if (!one_of(
          support_text, "exact", "observed_only", "candidate",
          "adapter_required", "unavailable") ||
      !one_of(
          wire_text, "json", "binary_json", "protobuf", "sbe",
          "fix_sbe", "raw") ||
      (!compression_text.empty() &&
       !one_of(
           compression_text, "none", "gzip", "deflate", "zlib",
           "permessage_deflate")) ||
      (!depth_semantics_text.empty() &&
       !one_of(
           depth_semantics_text, "none", "complete_snapshot",
           "rest_rebase", "stream_snapshot_delta"))) {
    error = "invalid_channel_enum";
    return std::nullopt;
  }
  ResearchChannel result{
      .id = text(object, "id"),
      .name = text(object, "name"),
      .kind = research_channel_kind(text(object, "kind")),
      .support = research_support(support_text),
      .transport = text(object, "transport"),
      .wire = wire_text,
      .host = text(object, "host"),
      .port = static_cast<std::uint16_t>(unsigned_value(object, "port", 443U)),
      .path = text(object, "path"),
      .subscribe = text(object, "subscribe"),
      .subscribe_binary = boolean(object, "subscribe_binary"),
      .compression = text(object, "compression"),
      .depth_semantics = text(object, "depth_semantics"),
      .depth_levels = unsigned_value(object, "depth_levels"),
      .adapter = text(object, "adapter"),
      .capabilities = strings(object, "capabilities"),
  };
  if (const auto* value = object.if_contains("mapping");
      value != nullptr && value->is_object()) {
    static constexpr std::array mapping_keys{
        std::string_view{"data"}, std::string_view{"symbol"},
        std::string_view{"event_time"}, std::string_view{"event_time_unit"},
        std::string_view{"transaction_time"},
        std::string_view{"transaction_time_unit"},
        std::string_view{"event_id"}, std::string_view{"first_event_id"},
        std::string_view{"previous_event_id"}, std::string_view{"bids"},
        std::string_view{"asks"}, std::string_view{"bid_price"},
        std::string_view{"bid_quantity"}, std::string_view{"ask_price"},
        std::string_view{"ask_quantity"}, std::string_view{"trades"},
        std::string_view{"price"}, std::string_view{"quantity"},
        std::string_view{"side"}, std::string_view{"snapshot"},
        std::string_view{"snapshot_value"},
    };
    if (!only_keys(
            value->as_object(), mapping_keys, error, "mapping")) {
      return std::nullopt;
    }
    if (std::any_of(
            value->as_object().begin(),
            value->as_object().end(),
            [](const auto& member) { return !member.value().is_string(); })) {
      error = "invalid_mapping_property_type";
      return std::nullopt;
    }
    result.mapping = mapping(value->as_object());
    if (!one_of(
            result.mapping.event_time_unit, "ns", "us", "ms", "s") ||
        !one_of(
            result.mapping.transaction_time_unit, "ns", "us", "ms",
            "s")) {
      error = "invalid_mapping_time_unit";
      return std::nullopt;
    }
  } else if (value != nullptr) {
    error = "mapping_not_object";
    return std::nullopt;
  }
  if (result.name.empty()) result.name = result.id;
  if (result.compression.empty()) result.compression = "none";
  if (result.depth_semantics.empty()) result.depth_semantics = "none";
  if (result.id.empty() || result.host.empty() || result.path.empty() ||
      result.path.front() != '/' || result.port == 0U ||
      result.kind == ResearchChannelKind::Unknown ||
      (result.transport != "rest" && result.transport != "ws" &&
       result.transport != "fix" && result.transport != "grpc")) {
    error = "invalid_channel";
    return std::nullopt;
  }
  return result;
}

[[nodiscard]] std::optional<DiscoveryCandidate> parse_candidate(
    const boost::json::object& object,
    std::string& error) {
  static constexpr std::array candidate_keys{
      std::string_view{"id"}, std::string_view{"channel"},
      std::string_view{"host"}, std::string_view{"port"},
      std::string_view{"path"}, std::string_view{"subscribe"},
      std::string_view{"subscribe_binary"}, std::string_view{"provenance"},
      std::string_view{"note"},
  };
  if (!only_keys(object, candidate_keys, error, "candidate")) {
    return std::nullopt;
  }
  static constexpr std::string_view candidate_string_keys[]{
      "id", "channel", "host", "path", "subscribe", "provenance", "note",
  };
  if (std::any_of(
          std::begin(candidate_string_keys),
          std::end(candidate_string_keys),
          [&](std::string_view key) {
            return !optional_string(object, key);
          }) ||
      !unsigned_at_most(object, "port", 65'535U) ||
      !optional_boolean(object, "subscribe_binary")) {
    error = "invalid_candidate_property_type";
    return std::nullopt;
  }
  DiscoveryCandidate result{
      .id = text(object, "id"),
      .channel_id = text(object, "channel"),
      .host = text(object, "host"),
      .port = static_cast<std::uint16_t>(unsigned_value(object, "port", 443U)),
      .path = text(object, "path"),
      .subscribe = text(object, "subscribe"),
      .subscribe_binary = boolean(object, "subscribe_binary"),
      .provenance = text(object, "provenance"),
      .note = text(object, "note"),
  };
  if (result.provenance.empty()) result.provenance = "candidate";
  if (result.id.empty() || result.host.empty() || result.path.empty() ||
      result.path.front() != '/' || result.port == 0U) {
    error = "invalid_discovery_candidate";
    return std::nullopt;
  }
  return result;
}

[[nodiscard]] bool parse_profile_file(
    const std::filesystem::path& path,
    ResearchCatalog& catalog,
    std::string& error) {
  std::error_code filesystem_error;
  const auto size = std::filesystem::file_size(path, filesystem_error);
  if (filesystem_error || size > kMaximumProfileBytes) {
    error = filesystem_error
                ? "profile_size_failed:" + filesystem_error.message()
                : "profile_capacity_exceeded";
    return false;
  }
  std::ifstream input{path, std::ios::binary};
  if (!input) {
    error = "profile_open_failed";
    return false;
  }
  const std::string content{
      std::istreambuf_iterator<char>{input},
      std::istreambuf_iterator<char>{}};
  boost::system::error_code parse_error;
  auto value = boost::json::parse(content, parse_error);
  if (parse_error || !value.is_object()) {
    error = "profile_json_invalid";
    return false;
  }
  const auto& root = value.as_object();
  static constexpr std::array root_keys{
      std::string_view{"schema"}, std::string_view{"venue"},
      std::string_view{"display_name"}, std::string_view{"provenance"},
      std::string_view{"products"},
  };
  if (!only_keys(root, root_keys, error, "profile")) return false;
  if (!optional_string(root, "schema") ||
      !optional_string(root, "venue") ||
      !optional_string(root, "display_name") ||
      !optional_string(root, "provenance")) {
    error = "invalid_profile_property_type";
    return false;
  }
  if (text(root, "schema") != kResearchProfileSchema) {
    error = "profile_schema_mismatch";
    return false;
  }
  const auto venue = text(root, "venue");
  const auto display_name = text(root, "display_name");
  const auto provenance = text(root, "provenance");
  const auto* products = root.if_contains("products");
  if (venue.empty() || products == nullptr || !products->is_array()) {
    error = "profile_identity_or_products_missing";
    return false;
  }
  for (const auto& product_value : products->as_array()) {
    if (!product_value.is_object()) {
      error = "profile_product_not_object";
      return false;
    }
    const auto& product = product_value.as_object();
    static constexpr std::array product_keys{
        std::string_view{"id"}, std::string_view{"default_symbol"},
        std::string_view{"symbol_format"}, std::string_view{"channels"},
        std::string_view{"discovery"},
    };
    if (!only_keys(product, product_keys, error, "product")) return false;
    if (!optional_string(product, "id") ||
        !optional_string(product, "default_symbol") ||
        !optional_string(product, "symbol_format")) {
      error = "invalid_product_property_type";
      return false;
    }
    ResearchProductProfile parsed{
        .venue = venue,
        .product = text(product, "id"),
        .display_name = display_name,
        .default_symbol = text(product, "default_symbol"),
        .symbol_format = text(product, "symbol_format"),
        .provenance = provenance.empty() ? "built_in" : provenance,
    };
    if (parsed.product.empty() || parsed.default_symbol.empty()) {
      error = "profile_product_identity_missing";
      return false;
    }
    const auto* channels = product.if_contains("channels");
    if (channels == nullptr || !channels->is_array() ||
        channels->as_array().size() > kMaximumProfileChannels) {
      error = "profile_channels_invalid";
      return false;
    }
    for (const auto& channel_value : channels->as_array()) {
      if (!channel_value.is_object()) {
        error = "profile_channel_not_object";
        return false;
      }
      auto channel = parse_channel(channel_value.as_object(), error);
      if (!channel.has_value()) return false;
      parsed.channels.push_back(std::move(*channel));
    }
    if (const auto* discovery = product.if_contains("discovery");
        discovery != nullptr) {
      if (!discovery->is_array() ||
          discovery->as_array().size() > kMaximumDiscoveryCandidates) {
        error = "profile_discovery_invalid";
        return false;
      }
      for (const auto& candidate_value : discovery->as_array()) {
        if (!candidate_value.is_object()) {
          error = "profile_candidate_not_object";
          return false;
        }
        auto candidate = parse_candidate(candidate_value.as_object(), error);
        if (!candidate.has_value()) return false;
        parsed.discovery.push_back(std::move(*candidate));
      }
    }
    catalog.products.push_back(std::move(parsed));
  }
  catalog.source_files.push_back(path.string());
  return true;
}

[[nodiscard]] std::string lowercase(std::string_view value) {
  std::string result{value};
  std::transform(
      result.begin(),
      result.end(),
      result.begin(),
      [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
      });
  return result;
}

[[nodiscard]] bool contains_case_insensitive(
    std::string_view text_value,
    std::string_view query) {
  return query.empty() ||
         lowercase(text_value).find(lowercase(query)) != std::string::npos;
}

[[nodiscard]] boost::json::array strings_json(
    const std::vector<std::string>& values) {
  boost::json::array result;
  result.reserve(values.size());
  for (const auto& value : values) result.emplace_back(value);
  return result;
}

[[nodiscard]] boost::json::object mapping_json(
    const JsonEventMapping& value) {
  return {
      {"data", value.data_path},
      {"symbol", value.symbol_path},
      {"event_time", value.event_time_path},
      {"event_time_unit", value.event_time_unit},
      {"transaction_time", value.transaction_time_path},
      {"transaction_time_unit", value.transaction_time_unit},
      {"event_id", value.event_id_path},
      {"first_event_id", value.first_event_id_path},
      {"previous_event_id", value.previous_event_id_path},
      {"bids", value.bids_path},
      {"asks", value.asks_path},
      {"bid_price", value.bid_price_path},
      {"bid_quantity", value.bid_quantity_path},
      {"ask_price", value.ask_price_path},
      {"ask_quantity", value.ask_quantity_path},
      {"trades", value.trades_path},
      {"price", value.price_path},
      {"quantity", value.quantity_path},
      {"side", value.side_path},
      {"snapshot", value.snapshot_path},
      {"snapshot_value", value.snapshot_value},
  };
}

}  // namespace

std::string_view to_string(ResearchChannelKind value) noexcept {
  switch (value) {
    case ResearchChannelKind::Trade: return "trade";
    case ResearchChannelKind::BookTicker: return "book_ticker";
    case ResearchChannelKind::Depth: return "depth";
    case ResearchChannelKind::Funding: return "funding";
    case ResearchChannelKind::Control: return "control";
    case ResearchChannelKind::Unknown: return "unknown";
  }
  return "unknown";
}

std::string_view to_string(ResearchSupport value) noexcept {
  switch (value) {
    case ResearchSupport::Exact: return "exact";
    case ResearchSupport::ObservedOnly: return "observed_only";
    case ResearchSupport::Candidate: return "candidate";
    case ResearchSupport::AdapterRequired: return "adapter_required";
    case ResearchSupport::Unavailable: return "unavailable";
  }
  return "unavailable";
}

ResearchChannelKind research_channel_kind(std::string_view value) noexcept {
  if (value == "trade") return ResearchChannelKind::Trade;
  if (value == "book_ticker") return ResearchChannelKind::BookTicker;
  if (value == "depth") return ResearchChannelKind::Depth;
  if (value == "funding") return ResearchChannelKind::Funding;
  if (value == "control") return ResearchChannelKind::Control;
  return ResearchChannelKind::Unknown;
}

ResearchSupport research_support(std::string_view value) noexcept {
  if (value == "exact") return ResearchSupport::Exact;
  if (value == "observed_only") return ResearchSupport::ObservedOnly;
  if (value == "candidate") return ResearchSupport::Candidate;
  if (value == "adapter_required") return ResearchSupport::AdapterRequired;
  return ResearchSupport::Unavailable;
}

ResearchProfileLoadResult load_research_catalog(
    const std::filesystem::path& root) {
  ResearchProfileLoadResult result;
  std::error_code filesystem_error;
  auto selected_root = root;
  if (root == std::filesystem::path{"profiles"} &&
      !std::filesystem::is_directory(selected_root, filesystem_error)) {
    filesystem_error.clear();
    const std::filesystem::path installed{
        EXCHANGE_PROBE_INSTALL_PROFILE_ROOT};
    if (std::filesystem::is_directory(installed, filesystem_error)) {
      selected_root = installed;
    } else {
      filesystem_error.clear();
      const std::filesystem::path source{
          EXCHANGE_PROBE_SOURCE_PROFILE_ROOT};
      if (std::filesystem::is_directory(source, filesystem_error)) {
        selected_root = source;
      }
    }
  }
  filesystem_error.clear();
  if (!std::filesystem::is_directory(selected_root, filesystem_error) ||
      filesystem_error) {
    result.error = "profile_root_not_directory";
    return result;
  }
  std::vector<std::filesystem::path> files;
  for (std::filesystem::recursive_directory_iterator iterator{
           selected_root, filesystem_error};
       !filesystem_error &&
       iterator != std::filesystem::recursive_directory_iterator{};
       iterator.increment(filesystem_error)) {
    if (iterator->is_regular_file() &&
        iterator->path().extension() == ".json") {
      files.push_back(iterator->path());
    }
  }
  if (filesystem_error) {
    result.error = "profile_root_scan_failed:" + filesystem_error.message();
    return result;
  }
  std::sort(files.begin(), files.end());
  for (const auto& file : files) {
    if (!parse_profile_file(file, result.catalog, result.error)) {
      result.error = file.string() + ':' + result.error;
      return result;
    }
  }
  const auto validation = validate_research_catalog(result.catalog);
  result.ok = validation.at("ok").as_bool();
  if (!result.ok) {
    result.error = boost::json::serialize(validation.at("issues"));
  }
  return result;
}

boost::json::object validate_research_catalog(
    const ResearchCatalog& catalog) {
  boost::json::array issues;
  if (catalog.products.empty()) {
    issues.emplace_back(boost::json::object{
        {"reason", "catalog_has_no_products"},
    });
  }
  std::set<std::pair<std::string, std::string>> product_ids;
  for (const auto& product : catalog.products) {
    if (!product_ids.emplace(product.venue, product.product).second) {
      issues.emplace_back(boost::json::object{
          {"reason", "duplicate_product"},
          {"venue", product.venue},
          {"product", product.product},
      });
    }
    std::set<std::string> channel_ids;
    if (product.channels.empty()) {
      issues.emplace_back(boost::json::object{
          {"reason", "product_has_no_channels"},
          {"venue", product.venue},
          {"product", product.product},
      });
    }
    for (const auto& channel : product.channels) {
      if (!channel_ids.insert(channel.id).second) {
        issues.emplace_back(boost::json::object{
            {"reason", "duplicate_channel"},
            {"venue", product.venue},
            {"product", product.product},
            {"channel", channel.id},
        });
      }
      if (channel.support == ResearchSupport::Exact &&
          channel.wire != "json" && channel.wire != "binary_json" &&
          channel.adapter.empty()) {
        issues.emplace_back(boost::json::object{
            {"reason", "exact_binary_channel_requires_adapter"},
            {"venue", product.venue},
            {"product", product.product},
            {"channel", channel.id},
        });
      }
      if (channel.support == ResearchSupport::AdapterRequired &&
          channel.adapter.empty()) {
        issues.emplace_back(boost::json::object{
            {"reason", "adapter_required_channel_has_no_adapter"},
            {"venue", product.venue},
            {"product", product.product},
            {"channel", channel.id},
        });
      }
      const bool exact_json =
          channel.support == ResearchSupport::Exact &&
          (channel.wire == "json" || channel.wire == "binary_json");
      if (exact_json &&
          channel.mapping.event_time_path.empty() &&
          channel.mapping.transaction_time_path.empty()) {
        issues.emplace_back(boost::json::object{
            {"reason", "exact_json_channel_has_no_exchange_clock"},
            {"venue", product.venue},
            {"product", product.product},
            {"channel", channel.id},
        });
      }
      if (exact_json && channel.kind == ResearchChannelKind::Trade &&
          (channel.mapping.price_path.empty() ||
           channel.mapping.quantity_path.empty())) {
        issues.emplace_back(boost::json::object{
            {"reason", "exact_trade_mapping_incomplete"},
            {"venue", product.venue},
            {"product", product.product},
            {"channel", channel.id},
        });
      }
      if (exact_json &&
          channel.kind == ResearchChannelKind::BookTicker &&
          (channel.mapping.bid_price_path.empty() ||
           channel.mapping.ask_price_path.empty())) {
        issues.emplace_back(boost::json::object{
            {"reason", "exact_bbo_mapping_incomplete"},
            {"venue", product.venue},
            {"product", product.product},
            {"channel", channel.id},
        });
      }
      if (exact_json && channel.kind == ResearchChannelKind::Depth &&
          (channel.mapping.bids_path.empty() ||
           channel.mapping.asks_path.empty())) {
        issues.emplace_back(boost::json::object{
            {"reason", "exact_depth_mapping_incomplete"},
            {"venue", product.venue},
            {"product", product.product},
            {"channel", channel.id},
        });
      }
    }
    std::set<std::string> candidate_ids;
    for (const auto& candidate : product.discovery) {
      if (!candidate_ids.insert(candidate.id).second) {
        issues.emplace_back(boost::json::object{
            {"reason", "duplicate_discovery_candidate"},
            {"venue", product.venue},
            {"product", product.product},
            {"candidate", candidate.id},
        });
      }
      if (!candidate.channel_id.empty() &&
          !channel_ids.contains(candidate.channel_id)) {
        issues.emplace_back(boost::json::object{
            {"reason", "discovery_references_unknown_channel"},
            {"venue", product.venue},
            {"product", product.product},
            {"candidate", candidate.id},
            {"channel", candidate.channel_id},
        });
      }
    }
  }
  return {
      {"schema", "exchange.api_probe.profile_validation.v1"},
      {"ok", issues.empty()},
      {"products", catalog.products.size()},
      {"files", catalog.source_files.size()},
      {"issues", std::move(issues)},
  };
}

boost::json::object research_profile_json(
    const ResearchProductProfile& profile) {
  boost::json::array channels;
  channels.reserve(profile.channels.size());
  for (const auto& channel : profile.channels) {
    channels.emplace_back(boost::json::object{
        {"id", channel.id},
        {"name", channel.name},
        {"kind", to_string(channel.kind)},
        {"support", to_string(channel.support)},
        {"transport", channel.transport},
        {"wire", channel.wire},
        {"host", channel.host},
        {"port", channel.port},
        {"path", channel.path},
        {"subscribe", channel.subscribe},
        {"subscribe_binary", channel.subscribe_binary},
        {"compression", channel.compression},
        {"depth_semantics", channel.depth_semantics},
        {"depth_levels", channel.depth_levels},
        {"adapter", channel.adapter},
        {"capabilities", strings_json(channel.capabilities)},
        {"mapping", mapping_json(channel.mapping)},
    });
  }
  boost::json::array discovery;
  discovery.reserve(profile.discovery.size());
  for (const auto& candidate : profile.discovery) {
    discovery.emplace_back(boost::json::object{
        {"id", candidate.id},
        {"channel", candidate.channel_id},
        {"host", candidate.host},
        {"port", candidate.port},
        {"path", candidate.path},
        {"subscribe", candidate.subscribe},
        {"subscribe_binary", candidate.subscribe_binary},
        {"provenance", candidate.provenance},
        {"note", candidate.note},
    });
  }
  return {
      {"schema", "exchange.api_probe.profile_view.v1"},
      {"venue", profile.venue},
      {"product", profile.product},
      {"display_name", profile.display_name},
      {"default_symbol", profile.default_symbol},
      {"symbol_format", profile.symbol_format},
      {"provenance", profile.provenance},
      {"channels", std::move(channels)},
      {"discovery", std::move(discovery)},
  };
}

std::vector<const ResearchProductProfile*> search_research_catalog(
    const ResearchCatalog& catalog,
    std::string_view query,
    std::string_view venue,
    std::string_view product,
    std::string_view capability) {
  std::vector<const ResearchProductProfile*> result;
  for (const auto& item : catalog.products) {
    if (!venue.empty() && item.venue != venue) continue;
    if (!product.empty() && item.product != product) continue;
    bool capability_match = capability.empty();
    bool query_match =
        contains_case_insensitive(item.venue, query) ||
        contains_case_insensitive(item.product, query) ||
        contains_case_insensitive(item.display_name, query);
    for (const auto& channel : item.channels) {
      query_match =
          query_match || contains_case_insensitive(channel.id, query) ||
          contains_case_insensitive(channel.name, query) ||
          contains_case_insensitive(channel.host, query);
      capability_match =
          capability_match ||
          std::find(
              channel.capabilities.begin(),
              channel.capabilities.end(),
              capability) != channel.capabilities.end();
    }
    if (query_match && capability_match) result.push_back(&item);
  }
  return result;
}

const ResearchProductProfile* find_research_profile(
    const ResearchCatalog& catalog,
    std::string_view venue,
    std::string_view product) noexcept {
  const auto iterator = std::find_if(
      catalog.products.begin(),
      catalog.products.end(),
      [&](const ResearchProductProfile& candidate) {
        return candidate.venue == venue && candidate.product == product;
      });
  return iterator == catalog.products.end() ? nullptr : &*iterator;
}

const ResearchChannel* find_research_channel(
    const ResearchProductProfile& profile,
    std::string_view channel_id) noexcept {
  const auto iterator = std::find_if(
      profile.channels.begin(),
      profile.channels.end(),
      [&](const ResearchChannel& candidate) {
        return candidate.id == channel_id;
      });
  return iterator == profile.channels.end() ? nullptr : &*iterator;
}

}  // namespace exchange_probe
