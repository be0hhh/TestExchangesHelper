#include "Cadence.hpp"
#include <boost/json.hpp>
#include <cctype>
#include <limits>
#include <stdexcept>
#include <time.h>
namespace exchange_probe::cadence {
std::uint64_t nowNs() noexcept {
  timespec ts{};
  if (clock_gettime(CLOCK_MONOTONIC_RAW, &ts) != 0) std::terminate();
  return static_cast<std::uint64_t>(ts.tv_sec) * second +
         static_cast<std::uint64_t>(ts.tv_nsec);
}
std::vector<Feed> feeds(const std::string& symbol) {
  std::vector<Feed> result{
    {"trade", symbol+"@trade", "/ws"},
    {"aggTrade", symbol+"@aggTrade", "/market/ws"},
    {"bookTicker", symbol+"@bookTicker", "/public/ws"}};
  for (unsigned depth : {0u,5u,10u,20u}) {
    for (unsigned interval : {0u,100u,250u,500u}) {
      const auto suffix = std::to_string(interval)+"ms";
      const auto levels = depth == 0 ? "" : std::to_string(depth);
      result.push_back({depth == 0 ? "diff_depth_"+suffix :
          "partial_depth_"+levels+"_"+suffix,
          symbol+"@depth"+levels+"@"+suffix,
          "/public/ws",depth,interval});
    }
  }
  return result;
}
bool append(Lane& lane, const Record& record, const Window& window) {
  const auto start = window.start_ns.load(std::memory_order_acquire);
  if (!start || record.receive_ns < start) {
    ++lane.warmup_events;
    return false;
  }
  if (record.receive_ns >= start + window.duration_seconds*second) return false;
  if (lane.records.size() >= laneRecordLimit) {
    ++lane.overflow;
    lane.status = "incomplete";
    return false;
  }
  lane.records.push_back(record);
  return true;
}
namespace {
std::uint64_t integer(const boost::json::object& obj, const char* key) {
  const auto* value=obj.if_contains(key);
  if (!value) return 0;
  if (value->is_uint64()) return value->as_uint64();
  if (value->is_int64() && value->as_int64()>=0)
    return static_cast<std::uint64_t>(value->as_int64());
  throw std::runtime_error("invalid unsigned field");
}
std::uint64_t timestamp(const boost::json::object& obj, const char* key) {
  const auto ms=integer(obj,key);
  if (ms>std::numeric_limits<std::uint64_t>::max()/1'000'000)
    throw std::runtime_error("timestamp overflow");
  return ms*1'000'000;
}
}
bool decode(const Feed& feed, std::string_view payload, Record& row, bool& control) {
  control=false;
  try {
    auto value=boost::json::parse(payload);
    if (!value.is_object()) return false;
    const auto& outer=value.as_object();
    if (outer.contains("result") && outer.at("result").is_null() &&
        integer(outer,"id")==1) { control=true; return false; }
    const auto* data=outer.if_contains("data");
    const auto& obj=data ? data->as_object() : outer;
    const auto expected=feed.name=="trade" ? "trade" :
      feed.name=="aggTrade" ? "aggTrade" :
      feed.name=="bookTicker" ? "bookTicker" : "depthUpdate";
    const auto* type=obj.if_contains("e");
    const auto* symbol=obj.if_contains("s");
    if (!type || !type->is_string() || type->as_string()!=expected ||
        !symbol || !symbol->is_string()) return false;
    std::string lower{symbol->as_string()};
    for (auto& c:lower) c=static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (lower!=feed.topic.substr(0,feed.topic.find('@'))) return false;
    row.event_ns=timestamp(obj,"E");
    row.transaction_ns=timestamp(obj,"T");
    row.id=integer(obj, feed.name=="trade" ? "t" : feed.name=="aggTrade" ? "a" : "u");
    row.first_id=integer(obj, feed.name=="aggTrade" ? "f" : "U");
    row.last_id=feed.name=="aggTrade" ? integer(obj,"l") : row.id;
    row.previous_id=integer(obj,"pu");
    return row.id!=0 && (feed.name!="aggTrade" ||
      (row.first_id!=0 && row.last_id>=row.first_id));
  } catch (const std::exception&) { return false; }
}
}
