#pragma once

// Diagnostic-only capture contract; no production API dependency.
#include <atomic>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace exchange_probe::cadence {
inline constexpr std::uint64_t second = 1'000'000'000ULL;
inline constexpr std::size_t laneCount = 19;
inline constexpr std::size_t laneBytes = (512ULL * 1024 * 1024) / laneCount;
// Eight decimal uint64 fields plus delimiters fit in 168 bytes; leave margin.
inline constexpr std::size_t laneRecordLimit = laneBytes / 192;
struct Feed {
  std::string name;
  std::string topic;
  std::string path;
  unsigned depth{};
  unsigned interval{};
};
struct Record {
  std::uint64_t receive_ns{}, publish_ns{}, event_ns{}, transaction_ns{};
  std::uint64_t id{}, first_id{}, last_id{}, previous_id{};
};
struct Lane {
  Feed feed;
  std::string path_kind; // direct or cxet
  std::string status{"pending"}, error, parser, endpoint, subscription;
  std::uint64_t frames{}, parse_errors{}, disconnects{}, control_pings{};
  std::uint64_t warmup_parse_errors{};
  std::uint64_t overflow{}, warmup_events{};
  std::vector<Record> records;
};
struct Window {
  std::atomic<std::uint64_t> start_ns{0};
  std::atomic<unsigned> prepared{0};
  std::atomic<bool> stop{false};
  unsigned duration_seconds{180};
};
std::uint64_t nowNs() noexcept;
std::vector<Feed> feeds(const std::string& lowerSymbol);
bool append(Lane&, const Record&, const Window&);
bool decode(const Feed&, std::string_view, Record&, bool& control);
// Exactly one lane per feed in catalog order. Called in a dedicated thread.
// Signal prepared once per lane, including unsupported/failure. No fallback.
void captureCxet(const std::string& symbol, Window&, std::vector<Lane>&);
void captureDirect(Lane&, Window&);
}
