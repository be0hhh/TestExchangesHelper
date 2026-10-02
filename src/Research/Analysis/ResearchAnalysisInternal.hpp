#pragma once

#include "exchange_probe/Research.hpp"
#include "exchange_probe/Contracts.hpp"

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


namespace exchange_probe::research_analysis {

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

void enforce_normalized_capacity(NormalizedEvent& event);

[[nodiscard]] boost::json::object distribution_json(
    std::vector<std::uint64_t> values);

[[nodiscard]] bool native_id_less(
    std::string_view lhs,
    std::string_view rhs) noexcept;

[[nodiscard]] std::optional<boost::json::object> read_object(
    const std::filesystem::path& path,
    std::string& error);

[[nodiscard]] std::string text(
    const boost::json::object& object,
    std::string_view key);

[[nodiscard]] std::uint64_t u64(
    const boost::json::object& object,
    std::string_view key) noexcept;

[[nodiscard]] const boost::json::value* json_path(
    const boost::json::value& root,
    std::string_view path);

[[nodiscard]] std::string scalar_text(
    const boost::json::value* value);

[[nodiscard]] std::optional<std::uint64_t> scalar_u64(
    const boost::json::value* value);

[[nodiscard]] std::optional<std::uint64_t> timestamp_ns(
    const boost::json::value* value,
    std::string_view unit);

[[nodiscard]] std::uint64_t fnv1a(std::string_view value) noexcept;

[[nodiscard]] std::uint64_t semantic_event_hash(
    const NormalizedEvent& event);

[[nodiscard]] std::vector<FrameIndex> read_frame_index(
    const std::filesystem::path& path,
    std::string& error);

[[nodiscard]] JsonEventMapping mapping_from_json(
    const boost::json::object& object);

[[nodiscard]] std::unordered_map<std::string, ChannelDefinition>
channel_definitions(const boost::json::object& manifest);

[[nodiscard]] std::string read_payload(
    std::ifstream& frames,
    const FrameIndex& frame,
    std::string& error);

[[nodiscard]] bool is_zero_decimal(std::string_view value);

void apply_levels(
    BookState& state,
    const boost::json::value* value,
    bool bids);

[[nodiscard]] std::pair<std::string, std::string> best_levels(
    const BookState& state);

[[nodiscard]] NormalizedEvent normalize_event(
    const FrameIndex& frame,
    const ChannelDefinition& channel,
    const boost::json::value& value,
    std::uint64_t event_id,
    std::uint64_t semantic_hash);

[[nodiscard]] boost::json::object event_json(
    const NormalizedEvent& event);

[[nodiscard]] bool emit_relation(
    std::ofstream& output,
    std::uint64_t& relation_id,
    bool& output_full,
    std::map<std::string, std::vector<std::uint64_t>>& lag_distributions,
    std::string_view mode,
    std::string_view evidence,
    const NormalizedEvent& source,
    const NormalizedEvent& target,
    std::string_view reason);

}
