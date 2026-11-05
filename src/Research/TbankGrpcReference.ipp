// Probe-only public reference diagnostics. Included inside tbank_grpc; these
// observations do not implement a production calendar or trading permissions.
namespace {
bool bounded_text(std::string_view value) noexcept {
  if (value.empty() || value.size() > 256u) return false;
  for (char byte : value) {
    const auto c = static_cast<unsigned char>(byte);
    if (c < 32u || c == 127u) return false;
  }
  return true;
}
bool same_native_section(std::string_view returned, std::string_view requested) noexcept {
  if (returned.size() != requested.size()) return false;
  const auto lower_ascii = [](char c) noexcept {
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c + ('a' - 'A')) : c;
  };
  for (std::size_t i = 0; i < returned.size(); ++i)
    if (lower_ascii(returned[i]) != lower_ascii(requested[i])) return false;
  return true;
}
std::vector<std::byte> timestamp_request(std::uint64_t nanoseconds) {
  std::vector<std::byte> out;
  scalar(out, 1, nanoseconds / 1'000'000'000ull);
  scalar(out, 2, nanoseconds % 1'000'000'000ull);
  return out;
}
bool schedule_interval(Bytes body, boost::json::object& output) {
  Cursor cursor{body}; unsigned seen{}; std::string_view type; boost::json::object bounds;
  while (cursor.pos < body.size()) {
    Field f; if (!cursor.next(f)) return false;
    if (f.number > 2u) continue;
    const auto bit = 1u << f.number;
    if ((seen & bit) || f.wire != 2u) return false;
    seen |= bit;
    if (f.number == 1u) {
      type = method_name(f.bytes); if (!bounded_text(type)) return false;
    } else {
      Cursor nested_cursor{f.bytes}; unsigned bound_seen{};
      while (nested_cursor.pos < f.bytes.size()) {
        Field bound; if (!nested_cursor.next(bound)) return false;
        if (bound.number > 2u) continue;
        const auto bound_bit = 1u << bound.number;
        if ((bound_seen & bound_bit) || bound.wire != 2u) return false;
        bound_seen |= bound_bit;
        boost::json::object value;
        if (timestamp(bound.bytes, &value) != MarketDecode::Other) return false;
        bounds[bound.number == 1u ? "start_ts" : "end_ts"] = std::move(value);
      }
      if (bound_seen != 6u) return false;
    }
  }
  if (seen != 6u) return false;
  output = {{"type", type}, {"dealer", type == "dealer_regular_trading_session"},
      {"interval", std::move(bounds)}};
  return true;
}
const char* schedule_stamp_name(std::uint64_t tag) noexcept {
  switch (tag) {
    case 1: return "date";
    case 3: return "start_time";
    case 4: return "end_time";
    case 7: return "opening_auction_start_time";
    case 8: return "closing_auction_end_time";
    case 9: return "evening_opening_auction_start_time";
    case 10: return "evening_start_time";
    case 11: return "evening_end_time";
    case 12: return "clearing_start_time";
    case 13: return "clearing_end_time";
    case 14: return "premarket_start_time";
    case 15: return "premarket_end_time";
    case 16: return "closing_auction_start_time";
    case 17: return "opening_auction_end_time";
    default: return nullptr;
  }
}
bool schedule_day(Bytes body, boost::json::object& output) {
  Cursor cursor{body}; std::uint64_t seen{}; boost::json::array intervals;
  bool trading = false;
  while (cursor.pos < body.size()) {
    Field f; if (!cursor.next(f)) return false;
    if (f.number == 18u) {
      if (f.wire != 2u || intervals.size() == 128u) return false;
      boost::json::object interval;
      if (!schedule_interval(f.bytes, interval)) return false;
      intervals.push_back(std::move(interval)); continue;
    }
    const auto* name = schedule_stamp_name(f.number);
    if (!name && f.number != 2u) continue;
    const auto bit = 1ull << f.number;
    if (seen & bit) return false;
    seen |= bit;
    if (f.number == 2u) {
      if (f.wire != 0u || f.value > 1u) return false;
      trading = f.value != 0;
    } else {
      boost::json::object stamp;
      if (f.wire != 2u || timestamp(f.bytes, &stamp) != MarketDecode::Other) return false;
      output[name] = std::move(stamp);
    }
  }
  if (!(seen & 2u)) return false;
  output["is_trading_day"] = trading;
  output["is_trading_day_present"] = bool(seen & 4u);
  output["intervals"] = std::move(intervals);
  return true;
}
}
std::vector<std::byte> instrument_request(std::string_view uid) {
  if (!valid_uid(uid)) return {};
  std::vector<std::byte> out;
  scalar(out, 1, 3); string_field(out, 3, uid); return out;
}
std::vector<std::byte> schedule_request(std::string_view section, std::uint64_t from,
                                      std::uint64_t to) {
  if (!bounded_text(section) || to <= from) return {};
  std::vector<std::byte> out;
  string_field(out, 1, section);
  nested(out, 2, timestamp_request(from)); nested(out, 3, timestamp_request(to));
  return out;
}
bool inspect_instrument(Bytes body, std::string_view expected_uid,
                        InstrumentObservation& observation, boost::json::object& details) {
  observation = {}; details.clear();
  if (!valid_uid(expected_uid) || body.empty() || body.size() > kMaxBody) return false;
  InstrumentObservation candidate; Cursor cursor{body}; bool instrument_seen = false;
  while (cursor.pos < body.size()) {
    Field f; if (!cursor.next(f)) return false;
    if (f.number != 1u) continue;
    if (instrument_seen || f.wire != 2u) return false;
    instrument_seen = true; Cursor row{f.bytes}; unsigned seen{};
    while (row.pos < row.input.size()) {
      Field item; if (!row.next(item)) return false;
      unsigned bit{}; std::string* target{};
      if (item.number == 2u) { bit = 1u; target = &candidate.ticker; }
      else if (item.number == 3u) { bit = 2u; target = &candidate.class_code; }
      else if (item.number == 15u) { bit = 4u; target = &candidate.exchange; }
      else if (item.number == 25u) { bit = 8u; target = &candidate.uid; }
      else continue;
      if ((seen & bit) || item.wire != 2u || !bounded_text(method_name(item.bytes))) return false;
      seen |= bit; target->assign(method_name(item.bytes));
    }
    if (seen != 15u || candidate.uid != expected_uid) return false;
  }
  if (!instrument_seen) return false;
  details = {{"uid", candidate.uid}, {"ticker", candidate.ticker},
      {"class_code", candidate.class_code}, {"exchange", candidate.exchange}};
  observation = std::move(candidate); return true;
}
bool inspect_trading_schedule(Bytes body, std::string_view expected_section,
                             boost::json::object& details) {
  details.clear();
  if (!bounded_text(expected_section) || body.empty() || body.size() > kMaxBody) return false;
  Cursor cursor{body}; bool section_seen = false; boost::json::array days;
  std::string_view returned_section;
  while (cursor.pos < body.size()) {
    Field f; if (!cursor.next(f)) return false;
    if (f.number != 1u) continue;
    if (section_seen || f.wire != 2u) return false;
    section_seen = true; Cursor section{f.bytes}; bool name_seen = false;
    while (section.pos < section.input.size()) {
      Field item; if (!section.next(item)) return false;
      if (item.number == 1u) {
        if (name_seen || item.wire != 2u || !same_native_section(method_name(item.bytes), expected_section)) return false;
        name_seen = true; returned_section = method_name(item.bytes);
      } else if (item.number == 2u) {
        if (item.wire != 2u || days.size() == 128u) return false;
        boost::json::object day;
        if (!schedule_day(item.bytes, day)) return false;
        days.push_back(std::move(day));
      }
    }
    if (!name_seen) return false;
  }
  if (!section_seen) return false;
  details = {{"exchange", returned_section}, {"requested_exchange", expected_section},
      {"days", std::move(days)}}; return true;
}
bool inspect_trading_status(Bytes body, std::string_view expected_uid, boost::json::object* details) {
  if (details) details->clear();
  if (!valid_uid(expected_uid) || body.empty() || body.size() > kMaxBody) return false;
  Cursor top{body}; bool row_seen = false; boost::json::object result;
  while (top.pos < body.size()) {
    Field f; if (!top.next(f)) return false;
    if (f.number != 1u) continue;
    if (row_seen || f.wire != 2u) return false;
    row_seen = true;
    Cursor row{f.bytes}; bool uid_seen = false, status_seen = false;
    std::array<bool, 3> flag_seen{};
    constexpr std::array<const char*, 3> flag_names{
        "limit_order_available", "market_order_available", "api_trade_available"};
    while (row.pos < row.input.size()) {
      Field item; if (!row.next(item)) return false;
      if (item.number == 6u) {
        if (uid_seen || item.wire != 2u || method_name(item.bytes) != expected_uid) return false;
        uid_seen = true;
      } else if (item.number == 2u) {
        if (status_seen || item.wire != 0u || item.value > 2147483647u) return false;
        status_seen = true;
        if (details) result["trading_status"] = item.value;
      } else if (item.number >= 3u && item.number <= 5u) {
        const auto index = static_cast<std::size_t>(item.number - 3u);
        auto& seen = flag_seen[index];
        if (seen || item.wire != 0u || item.value > 1u) return false;
        seen = true;
        if (details) result[flag_names[index]] = item.value != 0;
      }
    }
    if (!uid_seen) return false;
    if (details) {
      result["uid"] = expected_uid; result["trading_status_present"] = status_seen;
      for (std::size_t i = 0; i < flag_seen.size(); ++i) {
        result[std::string(flag_names[i]) + "_present"] = flag_seen[i];
        if (!flag_seen[i]) result[flag_names[i]] = false;
      }
    }
  }
  if (row_seen && details) *details = std::move(result);
  return row_seen;
}
