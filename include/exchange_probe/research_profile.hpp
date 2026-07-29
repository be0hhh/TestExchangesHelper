#pragma once

#include <boost/json/value.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace exchange_probe {

inline constexpr std::string_view kResearchProfileSchema =
    "exchange.api_probe.profile.v1";
inline constexpr std::size_t kMaximumProfileBytes = 4U * 1024U * 1024U;
inline constexpr std::size_t kMaximumProfileChannels = 256U;
inline constexpr std::size_t kMaximumDiscoveryCandidates = 1024U;

enum class ResearchChannelKind {
  Trade,
  BookTicker,
  Depth,
  Funding,
  Control,
  Unknown,
};

enum class ResearchSupport {
  Exact,
  ObservedOnly,
  Candidate,
  AdapterRequired,
  Unavailable,
};

struct JsonEventMapping {
  std::string data_path;
  std::string symbol_path;
  std::string event_time_path;
  std::string event_time_unit{"ms"};
  std::string transaction_time_path;
  std::string transaction_time_unit{"ms"};
  std::string event_id_path;
  std::string first_event_id_path;
  std::string previous_event_id_path;
  std::string bids_path;
  std::string asks_path;
  std::string bid_price_path;
  std::string bid_quantity_path;
  std::string ask_price_path;
  std::string ask_quantity_path;
  std::string trades_path;
  std::string price_path;
  std::string quantity_path;
  std::string side_path;
  std::string snapshot_path;
  std::string snapshot_value;
};

struct ResearchChannel {
  std::string id;
  std::string name;
  ResearchChannelKind kind{ResearchChannelKind::Unknown};
  ResearchSupport support{ResearchSupport::ObservedOnly};
  std::string transport{"ws"};
  std::string wire{"json"};
  std::string host;
  std::uint16_t port{443U};
  std::string path{"/"};
  std::string subscribe;
  bool subscribe_binary{false};
  std::string compression{"none"};
  std::string depth_semantics{"none"};
  unsigned depth_levels{0U};
  std::string adapter;
  std::vector<std::string> capabilities;
  JsonEventMapping mapping;
};

struct DiscoveryCandidate {
  std::string id;
  std::string channel_id;
  std::string host;
  std::uint16_t port{443U};
  std::string path{"/"};
  std::string subscribe;
  bool subscribe_binary{false};
  std::string provenance{"candidate"};
  std::string note;
};

struct ResearchProductProfile {
  std::string venue;
  std::string product;
  std::string display_name;
  std::string default_symbol;
  std::string symbol_format;
  std::string provenance{"built_in"};
  std::vector<ResearchChannel> channels;
  std::vector<DiscoveryCandidate> discovery;
};

struct ResearchCatalog {
  std::vector<ResearchProductProfile> products;
  std::vector<std::string> source_files;
};

struct ResearchProfileLoadResult {
  ResearchCatalog catalog;
  bool ok{false};
  std::string error;
};

[[nodiscard]] std::string_view to_string(ResearchChannelKind value) noexcept;
[[nodiscard]] std::string_view to_string(ResearchSupport value) noexcept;
[[nodiscard]] ResearchChannelKind research_channel_kind(
    std::string_view value) noexcept;
[[nodiscard]] ResearchSupport research_support(
    std::string_view value) noexcept;

[[nodiscard]] ResearchProfileLoadResult load_research_catalog(
    const std::filesystem::path& root);
[[nodiscard]] boost::json::object validate_research_catalog(
    const ResearchCatalog& catalog);
[[nodiscard]] boost::json::object research_profile_json(
    const ResearchProductProfile& profile);
[[nodiscard]] std::vector<const ResearchProductProfile*> search_research_catalog(
    const ResearchCatalog& catalog,
    std::string_view query,
    std::string_view venue,
    std::string_view product,
    std::string_view capability);
[[nodiscard]] const ResearchProductProfile* find_research_profile(
    const ResearchCatalog& catalog,
    std::string_view venue,
    std::string_view product) noexcept;
[[nodiscard]] const ResearchChannel* find_research_channel(
    const ResearchProductProfile& profile,
    std::string_view channel_id) noexcept;

}  // namespace exchange_probe
