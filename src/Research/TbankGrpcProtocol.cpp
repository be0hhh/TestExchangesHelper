#include "TbankGrpcProtocol.hpp"

#include <cstdint>
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
std::vector<std::byte> book_stream_request(std::string_view uid) {
  if (!valid_uid(uid)) return {};
  std::vector<std::byte> out;
  out.reserve(48u);
  byte(out, 0x12); byte(out, 46u); // envelope: book subscription
  byte(out, 0x08); byte(out, 1u); // subscribe
  byte(out, 0x12); byte(out, 42u); // one instrument
  byte(out, 0x10); byte(out, 1u); // depth
  byte(out, 0x1a); byte(out, 36u); text(out, uid);
  byte(out, 0x20); byte(out, 1u); // EXCHANGE
  return out;
}
std::vector<std::byte> market_source_request(MarketKind kind, unsigned source) {
  if (source != 1u && source != 3u) return {};
  if (kind == MarketKind::Book) {
    auto out = book_stream_request(kSberUid);
    out[7] = std::byte{50}; out.back() = static_cast<std::byte>(source);
    return out;
  }
  std::vector<std::byte> out;
  byte(out, 0x1a); byte(out, 44); byte(out, 8); byte(out, 1);
  byte(out, 0x12); byte(out, 38); byte(out, 0x12); byte(out, 36); text(out, kSberUid);
  byte(out, 0x18); byte(out, source);
  return out;
}
namespace {
// Validate known nested messages without interpreting financial values.
MarketDecode nested_syntax(Bytes bytes, unsigned mode) noexcept {
  Cursor c{bytes}; std::uint64_t seen = 0;
  while (c.pos < bytes.size()) {
    Field f; if (!c.next(f)) return MarketDecode::MalformedSyntax;
    if (f.number > 2u) continue;
    const auto bit = 1ull << f.number;
    if (seen & bit) return MarketDecode::SchemaAnomaly;
    seen |= bit;
    if ((mode == 1u && f.number == 1u) ? f.wire != 2u : f.wire != 0u)
      return MarketDecode::SchemaAnomaly;
    if (mode == 1u && f.number == 1u) {
      auto result = nested_syntax(f.bytes, 0u);
      if (result != MarketDecode::Other) return result;
    }
  }
  return MarketDecode::Other;
}
}
MarketObservation inspect_market_source(Bytes body, MarketKind kind) noexcept {
  MarketObservation o;
  auto fail = [&](MarketDecode result) { o.result = result; return o; };
  if (body.size() > kMaxBody) return fail(MarketDecode::SchemaAnomaly);
  Cursor c{body}; Bytes payload; std::uint64_t selected = 0;
  while (c.pos < body.size()) {
    Field f; if (!c.next(f)) return fail(MarketDecode::MalformedSyntax);
    if (f.number > 12u) continue;
    if (selected || f.wire != 2u) return fail(MarketDecode::SchemaAnomaly);
    selected = f.number; payload = f.bytes;
  }
  if (!selected) return fail(MarketDecode::SchemaAnomaly);
  const bool book = kind == MarketKind::Book;
  const bool ack = selected == (book ? 2u : 3u);
  const bool data = selected == (book ? 7u : 6u);
  if (!ack && !data) return o;
  auto parse_row = [&](Bytes row) -> MarketDecode {
    Cursor r{row}; std::uint64_t seen = 0;
    while (r.pos < row.size()) {
      Field f; if (!r.next(f)) return MarketDecode::MalformedSyntax;
      const auto uid_tag = ack ? (book ? 4u : 3u) : (book ? 9u : 6u);
      const auto source_tag = ack ? (book ? 7u : 0u) : (book ? 10u : 7u);
      const auto status_tag = book ? 3u : 2u;
      const bool repeated = !ack && book && (f.number == 4u || f.number == 5u);
      const bool known = f.number <= (ack ? (book ? 7u : 3u) : (book ? 10u : 7u));
      if (!known) continue;
      if (!repeated) {
        const auto bit = 1ull << f.number;
        if (seen & bit) return MarketDecode::SchemaAnomaly;
        seen |= bit;
      }
      if (f.number == uid_tag) {
        if (f.wire != 2u) return MarketDecode::SchemaAnomaly;
        o.uid = {reinterpret_cast<const char*>(f.bytes.data()), f.bytes.size()};
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
      } else if (!ack && (repeated || f.number == (book ? 6u : 5u) || (!book && f.number == 3u))) {
        if (f.wire != 2u) return MarketDecode::SchemaAnomaly;
        auto result = nested_syntax(f.bytes, repeated ? 1u : 0u);
        if (result != MarketDecode::Other) return result;
      } else if (!ack && ((!book && (f.number == 2u || f.number == 4u)) || (book && f.number == 3u))) {
        if (f.wire != 0u) return MarketDecode::SchemaAnomaly;
      }
    }
    if (!ack && book && (!(seen & (1ull<<2u)) || !(seen & (1ull<<6u)))) return MarketDecode::SchemaAnomaly;
    if (!ack && !book && ((seen & 0x3cull) != 0x3cull)) return MarketDecode::SchemaAnomaly;
    if (ack && !o.status_present) return MarketDecode::SchemaAnomaly;
    if ((!ack || o.status == 1u) && o.uid.empty()) return MarketDecode::SchemaAnomaly;
    return ack ? MarketDecode::Ack : MarketDecode::Data;
  };
  if (data) { o.result = parse_row(payload); return o; }
  Cursor a{payload}; bool row_seen = false, root_source_seen = false;
  while (a.pos < payload.size()) {
    Field f; if (!a.next(f)) return fail(MarketDecode::MalformedSyntax);
    if (f.number == 2u) {
      if (row_seen || f.wire != 2u) return fail(MarketDecode::SchemaAnomaly);
      row_seen = true;
      auto result = parse_row(f.bytes);
      if (result != MarketDecode::Ack) return fail(result);
    } else if (!book && f.number == 3u) {
      if (root_source_seen || f.wire != 0u) return fail(MarketDecode::SchemaAnomaly);
      root_source_seen = true; o.source_present = true; o.source = f.value;
    }
  }
  return fail(row_seen ? MarketDecode::Ack : MarketDecode::SchemaAnomaly);
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
bool inspect_trading_status(Bytes body) noexcept {
  if (body.empty() || body.size() > kMaxBody) return false;
  Cursor top{body}; bool row_seen = false;
  while (top.pos < body.size()) {
    Field f; if (!top.next(f)) return false;
    if (f.number != 1u) continue;
    if (row_seen || f.wire != 2u) return false;
    row_seen = true;
    Cursor row{f.bytes}; bool uid_seen = false, status_seen = false;
    std::array<bool, 3> flag_seen{};
    while (row.pos < row.input.size()) {
      Field item; if (!row.next(item)) return false;
      if (item.number == 6u) {
        if (uid_seen || item.wire != 2u || method_name(item.bytes) != kSberUid) return false;
        uid_seen = true;
      } else if (item.number == 2u) {
        if (status_seen || item.wire != 0u || item.value > 2147483647u) return false;
        status_seen = true;
      } else if (item.number >= 3u && item.number <= 5u) {
        auto& seen = flag_seen[static_cast<std::size_t>(item.number - 3u)];
        if (seen || item.wire != 0u || item.value > 1u) return false;
        seen = true;
      }
    }
    if (!uid_seen) return false;
  }
  return row_seen;
}
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
