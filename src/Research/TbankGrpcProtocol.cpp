#include "TbankGrpcProtocol.hpp"

#include <cstdint>
#include <bit>
#include <chrono>
#include <cstdio>
#include <boost/json.hpp>
#include <limits>

namespace exchange_probe::tbank_grpc {
namespace {
using Bytes = std::span<const std::byte>;
constexpr std::size_t kMaxBody = 64u * 1024u;

struct Field {
  std::uint64_t number{};
  unsigned wire{};
  std::uint64_t value{};
  Bytes bytes{};
};
struct Cursor {
  Bytes input;
  std::size_t pos{};
  bool varint(std::uint64_t& value) noexcept {
    value = 0;
    for (unsigned i = 0; i < 10; ++i) {
      if (pos == input.size()) return false;
      const auto byte = std::to_integer<unsigned>(input[pos++]);
      if (i == 9 && byte > 1u) return false;
      value |= std::uint64_t(byte & 0x7fu) << (7u * i);
      if (!(byte & 0x80u)) return true;
    }
    return false;
  }
  bool next(Field& field) noexcept {
    std::uint64_t key{};
    if (!varint(key) || !(key >> 3u) || (key >> 3u) > 0x1fffffffu) return false;
    field = {};
    field.number = key >> 3u;
    field.wire = static_cast<unsigned>(key & 7u);
    if (field.wire == 0u) return varint(field.value);
    std::uint64_t size{};
    if (field.wire == 2u) {
      if (!varint(size) || size > input.size() - pos) return false;
      field.bytes = input.subspan(pos, static_cast<std::size_t>(size));
      pos += static_cast<std::size_t>(size);
      return true;
    }
    if (field.wire == 1u) size = 8;
    else if (field.wire == 5u) size = 4;
    else return false;
    if (size > input.size() - pos) return false;
    pos += static_cast<std::size_t>(size);
    return true;
  }
};
void byte(std::vector<std::byte>& out, unsigned value) { out.push_back(static_cast<std::byte>(value)); }
void text(std::vector<std::byte>& out, std::string_view value) {
  for (char c : value) byte(out, static_cast<unsigned char>(c));
}
AckResult entry(Bytes bytes, std::string_view expected) noexcept {
  Cursor cursor{bytes};
  bool status_seen = false, uid_seen = false, source_seen = false;
  std::uint64_t status{}, source{};
  std::string_view uid;
  while (cursor.pos < bytes.size()) {
    Field f;
    if (!cursor.next(f)) return AckResult::Malformed;
    if (f.number == 3u) {
      if (status_seen || f.wire != 0u) return AckResult::Malformed;
      status_seen = true; status = f.value;
    } else if (f.number == 4u) {
      if (uid_seen || f.wire != 2u) return AckResult::Malformed;
      uid_seen = true;
      if (f.bytes.empty()) return AckResult::Malformed;
      uid = {reinterpret_cast<const char*>(f.bytes.data()), f.bytes.size()};
      if (!valid_uid(uid)) return AckResult::Malformed;
    } else if (f.number == 7u) {
      if (source_seen || f.wire != 0u) return AckResult::Malformed;
      source_seen = true; source = f.value;
    }
  }
  if (!status_seen) return AckResult::Malformed;
  if (source_seen && source != 1u) return AckResult::Malformed;
  if (uid_seen && uid != expected) return AckResult::Rejected;
  if (status != 1u) return AckResult::Rejected;
  if (!uid_seen || !source_seen) return AckResult::Malformed;
  return AckResult::Accepted;
}
AckResult acknowledgement(Bytes bytes, std::string_view expected) noexcept {
  Cursor cursor{bytes};
  bool row_seen = false;
  AckResult result = AckResult::Malformed;
  while (cursor.pos < bytes.size()) {
    Field f;
    if (!cursor.next(f)) return AckResult::Malformed;
    if (f.number != 2u) continue;
    if (row_seen || f.wire != 2u) return AckResult::Malformed;
    row_seen = true;
    result = entry(f.bytes, expected);
  }
  return result;
}

std::string_view method_name(Bytes bytes) noexcept {
  return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}
std::string_view normalized_method(std::string_view value) noexcept {
  if (value.starts_with('/')) value.remove_prefix(1);
  return value;
}
bool valid_method(std::string_view value) noexcept {
  if (value.empty() || value.size() > 255u) return false;
  value = normalized_method(value);
  bool dot = false, slash = false;
  std::size_t component = 0;
  for (char c : value) {
    if (c == '.' || c == '/') {
      if (!component || slash || (c == '/' && !dot)) return false;
      if (c == '/') slash = true; else dot = true;
      component = 0;
    } else {
      const bool alpha = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
      if (!alpha && (!component || c < '0' || c > '9')) return false;
      ++component;
    }
  }
  return slash && component != 0;
}
bool tariff_group(Bytes bytes, TariffGroup& group) noexcept {
  Cursor cursor{bytes};
  std::array<std::string_view, 64> methods{};
  std::size_t count = 0, fields = 0;
  bool minute_seen = false, second_seen = false;
  while (cursor.pos < bytes.size()) {
    if (++fields > methods.size() + 8u) return false;
    Field f;
    if (!cursor.next(f)) return false;
    if (f.number == 1u || f.number == 3u) {
      auto& seen = f.number == 1u ? minute_seen : second_seen;
      if (seen || f.wire != 0u || f.value > 2147483647u) return false;
      seen = true;
      (f.number == 1u ? group.minute : group.second) = static_cast<std::uint32_t>(f.value);
    } else if (f.number == 2u) {
      if (f.wire != 2u || count == methods.size()) return false;
      auto method = method_name(f.bytes);
      if (!valid_method(method)) return false;
      method = normalized_method(method);
      for (std::size_t i = 0; i < count; ++i) if (methods[i] == method) return false;
      methods[count++] = method;
      if (method == normalized_method(kTradingStatusPath)) group.trading_status = true;
      if (method == normalized_method(kUserTariffPath)) group.user_tariff = true;
    }
  }
  return count != 0 && (group.minute != 0 || group.second != 0);
}
} // namespace

bool valid_uid(std::string_view uid) noexcept {
  if (uid.size() != 36u) return false;
  for (std::size_t i = 0; i < uid.size(); ++i) {
    const char c = uid[i];
    if (i == 8u || i == 13u || i == 18u || i == 23u) {
      if (c != '-') return false;
    } else if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
  }
  return true;
}
std::vector<std::byte> trading_status_request(std::string_view uid) {
  if (!valid_uid(uid)) return {};
  std::vector<std::byte> out;
  out.reserve(38u); byte(out, 0x0a); byte(out, 36u); text(out, uid);
  return out;
}
namespace {
void varint(std::vector<std::byte>& out, std::uint64_t value) {
  do { byte(out, static_cast<unsigned>(value & 127u) | (value > 127u ? 128u : 0u)); value >>= 7u; } while (value);
}
void scalar(std::vector<std::byte>& out, unsigned tag, std::uint64_t value) {
  varint(out, tag * 8u); varint(out, value);
}
void nested(std::vector<std::byte>& out, unsigned tag, Bytes value) {
  varint(out, tag * 8u + 2u); varint(out, value.size()); out.insert(out.end(), value.begin(), value.end());
}
void string_field(std::vector<std::byte>& out, unsigned tag, std::string_view value) {
  nested(out, tag, {reinterpret_cast<const std::byte*>(value.data()), value.size()});
}
std::vector<std::byte> book_subscription(std::string_view uid, unsigned source, unsigned depth) {
  std::vector<std::byte> instrument, subscription;
  scalar(instrument, 2, depth); string_field(instrument, 3, uid); scalar(instrument, 4, source);
  scalar(subscription, 1, 1); nested(subscription, 2, instrument);
  return subscription;
}
// Preserve the signed protobuf value without a floating point conversion.
std::int64_t signed64(std::uint64_t value) noexcept { return std::bit_cast<std::int64_t>(value); }
bool signed32(std::uint64_t value, std::int32_t& output) noexcept {
  if (value > 2147483647ull && value < 18446744071562067968ull) return false;
  output = std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(value)); return true;
}
MarketDecode quotation(Bytes body, boost::json::object* output) {
  Cursor cursor{body}; unsigned seen{}; std::int64_t units{}; std::int32_t nano{};
  while (cursor.pos < body.size()) {
    Field f; if (!cursor.next(f)) return MarketDecode::MalformedSyntax;
    if (f.number > 2u) continue;
    const auto bit = 1u << f.number;
    if ((seen & bit) || f.wire != 0u) return MarketDecode::SchemaAnomaly;
    seen |= bit;
    if (f.number == 1u) units = signed64(f.value);
    else if (!signed32(f.value, nano)) return MarketDecode::SchemaAnomaly;
  }
  if (output) *output = {{"units", units}, {"nano", nano}};
  return MarketDecode::Other;
}
std::string render_timestamp(std::int64_t seconds, std::int32_t nanos, bool msk) {
  using namespace std::chrono;
  const sys_seconds instant{std::chrono::seconds{seconds + (msk ? 10800 : 0)}};
  const auto day = floor<days>(instant);
  const year_month_day date{day}; const hh_mm_ss time{instant - day};
  char result[64];
  std::snprintf(result, sizeof(result), "%04d-%02u-%02uT%02lld:%02lld:%02lld.%09d%s",
      static_cast<int>(date.year()), static_cast<unsigned>(date.month()), static_cast<unsigned>(date.day()),
      static_cast<long long>(time.hours().count()), static_cast<long long>(time.minutes().count()),
      static_cast<long long>(time.seconds().count()), nanos, msk ? "+03:00" : "Z");
  return result;
}
MarketDecode timestamp(Bytes body, boost::json::object* output) {
  Cursor cursor{body}; unsigned seen{}; std::int64_t seconds{}; std::int32_t nanos{};
  while (cursor.pos < body.size()) {
    Field f; if (!cursor.next(f)) return MarketDecode::MalformedSyntax;
    if (f.number > 2u) continue;
    const auto bit = 1u << f.number;
    if ((seen & bit) || f.wire != 0u) return MarketDecode::SchemaAnomaly;
    seen |= bit;
    if (f.number == 1u) seconds = signed64(f.value);
    else if (!signed32(f.value, nanos)) return MarketDecode::SchemaAnomaly;
  }
  if (seconds < -62135596800ll || seconds > 253402300799ll || nanos < 0 || nanos >= 1000000000) return MarketDecode::SchemaAnomaly;
  if (output) *output = {{"seconds", seconds}, {"nanos", nanos},
      {"utc", render_timestamp(seconds, nanos, false)}, {"msk", render_timestamp(seconds, nanos, true)}};
  return MarketDecode::Other;
}
MarketDecode order(Bytes body, boost::json::object* output) {
  Cursor cursor{body}; unsigned seen{}; boost::json::object price; std::int64_t quantity{};
  while (cursor.pos < body.size()) {
    Field f; if (!cursor.next(f)) return MarketDecode::MalformedSyntax;
    if (f.number > 2u) continue;
    const auto bit = 1u << f.number;
    if (seen & bit) return MarketDecode::SchemaAnomaly;
    seen |= bit;
    if (f.number == 1u) {
      if (f.wire != 2u) return MarketDecode::SchemaAnomaly;
      const auto result = quotation(f.bytes, output ? &price : nullptr);
      if (result != MarketDecode::Other) return result;
    } else {
      if (f.wire != 0u) return MarketDecode::SchemaAnomaly;
      quantity = signed64(f.value);
    }
  }
  if (output) {
    // Proto3 defaults are values; presence is retained separately for diagnostics.
    if (!(seen & 2u)) price = {{"units", 0}, {"nano", 0}};
    *output = {{"price", std::move(price)}, {"quantity", quantity},
        {"price_present", bool(seen & 2u)}, {"quantity_present", bool(seen & 4u)}};
  }
  return MarketDecode::Other;
}
}
std::vector<std::byte> book_stream_request(std::string_view uid) {
  if (!valid_uid(uid)) return {};
  std::vector<std::byte> out; nested(out, 2, book_subscription(uid, 1, 1)); return out;
}
std::vector<std::byte> combined_market_request(std::string_view uid, unsigned source) {
  if (!valid_uid(uid) || (source != 1u && source != 3u)) return {};
  std::vector<std::byte> out, instrument, trades;
  nested(out, 2, book_subscription(uid, source, 50));
  string_field(instrument, 2, uid); scalar(trades, 1, 1); nested(trades, 2, instrument); scalar(trades, 3, source);
  nested(out, 3, trades); return out;
}
MarketEnvelope market_envelope(Bytes body) noexcept {
  MarketEnvelope envelope;
  if (body.size() > kMaxBody) { envelope.result = MarketDecode::SchemaAnomaly; return envelope; }
  Cursor cursor{body}; std::uint64_t selected{};
  while (cursor.pos < body.size()) {
    Field f;
    if (!cursor.next(f)) { envelope.result = MarketDecode::MalformedSyntax; return envelope; }
    if (f.number > 12u) continue;
    if (selected || f.wire != 2u) { envelope.result = MarketDecode::SchemaAnomaly; return envelope; }
    selected = f.number; envelope.payload = f.bytes;
  }
  if (!selected) { envelope.result = MarketDecode::SchemaAnomaly; return envelope; }
  if (selected == 2u || selected == 7u) envelope.kind = MarketKind::Book;
  else if (selected == 3u || selected == 6u) envelope.kind = MarketKind::Trades;
  if (selected == 2u || selected == 3u) envelope.result = MarketDecode::Ack;
  else if (selected == 6u || selected == 7u) envelope.result = MarketDecode::Data;
  return envelope;
}
MarketObservation inspect_market_source(const MarketEnvelope& envelope, bool include_details) {
  MarketObservation o; o.kind = envelope.kind; o.result = envelope.result;
  auto fail = [&](MarketDecode result) { o.result = result; o.details.clear(); return o; };
  if (o.result != MarketDecode::Ack && o.result != MarketDecode::Data) return o;
  const bool ack = o.result == MarketDecode::Ack, book = o.kind == MarketKind::Book;
  auto parse_row = [&](Bytes row) -> MarketDecode {
    Cursor r{row}; std::uint64_t seen{}; boost::json::array bids, asks;
    while (r.pos < row.size()) {
      Field f; if (!r.next(f)) return MarketDecode::MalformedSyntax;
      const auto uid_tag = ack ? (book ? 4u : 3u) : (book ? 9u : 6u);
      const auto source_tag = ack ? (book ? 7u : 0u) : (book ? 10u : 7u);
      const auto status_tag = book ? 3u : 2u;
      const bool repeated = !ack && book && (f.number == 4u || f.number == 5u);
      if (f.number > (ack ? (book ? 7u : 3u) : (book ? 10u : 7u))) continue;
      if (!repeated) {
        const auto bit = 1ull << f.number;
        if (seen & bit) return MarketDecode::SchemaAnomaly;
        seen |= bit;
      }
      if (f.number == uid_tag) {
        if (f.wire != 2u) return MarketDecode::SchemaAnomaly;
        o.uid = method_name(f.bytes);
        if (!valid_uid(o.uid)) return MarketDecode::SchemaAnomaly;
      } else if (f.number == source_tag) {
        if (f.wire != 0u) return MarketDecode::SchemaAnomaly;
        o.source = f.value; o.source_present = true;
      } else if (ack && f.number == status_tag) {
        if (f.wire != 0u) return MarketDecode::SchemaAnomaly;
        o.status = f.value; o.status_present = true;
      } else if (book && f.number == 2u) {
        if (f.wire != 0u) return MarketDecode::SchemaAnomaly;
        o.depth = f.value; o.depth_present = true;
      } else if (!ack && repeated) {
        if (f.wire != 2u) return MarketDecode::SchemaAnomaly;
        boost::json::object item;
        const auto result = order(f.bytes, include_details ? &item : nullptr);
        if (result != MarketDecode::Other) return result;
        if (include_details) (f.number == 4u ? bids : asks).push_back(std::move(item));
      } else if (!ack && (f.number == (book ? 6u : 5u) || (!book && f.number == 3u))) {
        if (f.wire != 2u) return MarketDecode::SchemaAnomaly;
        boost::json::object value;
        const auto result = f.number == 3u ? quotation(f.bytes, include_details ? &value : nullptr) :
            timestamp(f.bytes, include_details ? &value : nullptr);
        if (result != MarketDecode::Other) return result;
        if (include_details) o.details[f.number == 3u ? "price" : "time"] = std::move(value);
      } else if (!ack && ((!book && (f.number == 2u || f.number == 4u)) || (book && f.number == 3u))) {
        if (f.wire != 0u || (book && f.value > 1u)) return MarketDecode::SchemaAnomaly;
        if (include_details) {
          if (book) o.details["is_consistent"] = bool(f.value);
          else o.details[f.number == 2u ? "direction" : "quantity"] = signed64(f.value);
        }
      }
    }
    if (!ack && book && (!(seen & (1ull << 2u)) || !(seen & (1ull << 6u)))) return MarketDecode::SchemaAnomaly;
    // Message price/time remain required; omitted proto3 direction/quantity
    // and ACK status retain their zero defaults independently of presence.
    if (!ack && !book && (!(seen & (1ull << 3u)) || !(seen & (1ull << 5u)))) return MarketDecode::SchemaAnomaly;
    if ((!ack || o.status == 1u) && o.uid.empty()) return MarketDecode::SchemaAnomaly;
    if (include_details && !ack && book) {
      o.details["bids"] = std::move(bids); o.details["asks"] = std::move(asks);
      o.details["consistency_present"] = bool(seen & (1ull << 3u));
      if (!(seen & (1ull << 3u))) o.details["is_consistent"] = false;
    }
    if (include_details && !ack && !book) {
      o.details["direction_present"] = bool(seen & (1ull << 2u));
      o.details["quantity_present"] = bool(seen & (1ull << 4u));
      if (!(seen & (1ull << 2u))) o.details["direction"] = std::int64_t{0};
      if (!(seen & (1ull << 4u))) o.details["quantity"] = std::int64_t{0};
    }
    return ack ? MarketDecode::Ack : MarketDecode::Data;
  };
  if (!ack) {
    const auto result = parse_row(envelope.payload);
    if (result != MarketDecode::Data) return fail(result);
  } else {
    Cursor a{envelope.payload}; bool row_seen = false, root_source_seen = false;
    while (a.pos < a.input.size()) {
      Field f; if (!a.next(f)) return fail(MarketDecode::MalformedSyntax);
      if (f.number == 2u) {
        if (row_seen || f.wire != 2u) return fail(MarketDecode::SchemaAnomaly);
        row_seen = true;
        const auto result = parse_row(f.bytes);
        if (result != MarketDecode::Ack) return fail(result);
      } else if (!book && f.number == 3u) {
        if (root_source_seen || f.wire != 0u) return fail(MarketDecode::SchemaAnomaly);
        root_source_seen = true; o.source_present = true; o.source = f.value;
      }
    }
    if (!row_seen) return fail(MarketDecode::SchemaAnomaly);
  }
  if (include_details) {
    o.details["source_present"] = o.source_present;
    if (o.source_present) o.details["source"] = o.source;
    if (o.depth_present) o.details["depth"] = o.depth;
    if (ack) {
      o.details["status"] = o.status;
      o.details["status_present"] = o.status_present;
    }
  }
  return o;
}
AckResult inspect_book_ack(Bytes body, std::string_view expected_uid) noexcept {
  if (body.size() > kMaxBody || !valid_uid(expected_uid)) return AckResult::Malformed;
  Cursor cursor{body};
  bool selected = false;
  std::uint64_t kind{};
  Bytes payload;
  while (cursor.pos < body.size()) {
    Field f;
    if (!cursor.next(f)) return AckResult::Malformed;
    if (f.number > 12u) continue;
    if (selected || f.wire != 2u) return AckResult::Malformed;
    selected = true; kind = f.number; payload = f.bytes;
  }
  if (!selected) return AckResult::Malformed;
  return kind == 2u ? acknowledgement(payload, expected_uid) : AckResult::NotAck;
}
bool inspect_user_tariff(Bytes body, TariffSummary& summary) noexcept {
  summary = {};
  if (body.size() > kMaxBody) return false;
  Cursor cursor{body};
  std::size_t fields = 0, groups = 0;
  while (cursor.pos < body.size()) {
    if (++fields > 256u) { summary = {}; return false; }
    Field f;
    if (!cursor.next(f)) { summary = {}; return false; }
    if (f.number == 1u) {
      TariffGroup group{};
      if (f.wire != 2u || ++groups > summary.groups.size() || !tariff_group(f.bytes, group)) {
        summary = {}; return false;
      }
      if (group.trading_status || group.user_tariff) summary.groups[summary.count++] = group;
    } else if (f.number == 2u && f.wire != 2u) {
      summary = {}; return false;
    }
  }
  return true;
}
bool tariff_covers_experiment(const TariffSummary& summary) noexcept {
  if (summary.count > summary.groups.size()) return false;
  bool status = false, tariff = false;
  for (std::size_t i = 0; i < summary.count; ++i) {
    const auto& group = summary.groups[i];
    if (!group.trading_status && !group.user_tariff) return false;
    status |= group.trading_status; tariff |= group.user_tariff;
    if (!group.minute && !group.second) return false;
    if (group.minute && group.minute < (group.trading_status ? 6u : 1u)) return false;
    if (group.second && group.second < 1u) return false;
  }
  return status && tariff;
}
#include "TbankGrpcReference.ipp"
std::uint64_t next_lifetime_slot(std::uint64_t first, std::uint64_t now) noexcept {
  constexpr std::uint64_t period = 20'000'000'000ull;
  if (now < first) return first;
  return now > std::numeric_limits<std::uint64_t>::max() - period
      ? std::numeric_limits<std::uint64_t>::max() : now + period;
}
void poll_lifetime_connections(const std::array<Connection*, 2>& connections,
                               const std::function<bool()>& stopped, std::uint64_t now) {
  for (auto* connection : connections) {
    if (stopped()) return;
    if (connection && connection->open()) connection->poll(now);
  }
}
bool lifetime_horizon_complete(std::uint64_t now, std::uint64_t end,
                               const std::array<std::uint64_t, 2>& next,
                               const std::array<std::uint64_t, 2>& completed) noexcept {
  return end != 0 && now >= end && completed[0] != 0 && completed[1] != 0 &&
         next[0] >= end && next[1] >= end;
}
bool lifetime_slot_expired(std::uint64_t now, std::uint64_t end,
                           const std::array<std::uint64_t, 2>& next) noexcept {
  constexpr std::uint64_t period = 20'000'000'000ull;
  for (auto due : next)
    if (due < end && now >= due && now - due >= period) return true;
  return false;
}
} // namespace exchange_probe::tbank_grpc
