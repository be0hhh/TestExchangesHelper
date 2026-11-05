#include "OfflineCase.hpp"
#include "../../../src/Research/TbankGrpcProtocol.hpp"

#include <cstddef>
#include <boost/json.hpp>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

namespace {
using namespace exchange_probe::tbank_grpc;
using namespace std::string_view_literals;
constexpr std::string_view uid = "01234567-89ab-cdef-0123-456789abcdef";
std::vector<std::byte> bytes(std::string_view value) {
  std::vector<std::byte> result;
  for (unsigned char c : value) result.push_back(static_cast<std::byte>(c));
  return result;
}
std::vector<std::byte> ack(std::string_view entry) {
  std::vector<std::byte> result{std::byte{0x12}, static_cast<std::byte>(entry.size() + 2),
                                std::byte{0x12}, static_cast<std::byte>(entry.size())};
  const auto content = bytes(entry);
  result.insert(result.end(), content.begin(), content.end());
  return result;
}
void requests() {
  CXET_CHECK(valid_uid(uid));
  CXET_CHECK(!valid_uid("01234567-89AB-cdef-0123-456789abcdef"));
  CXET_CHECK(!valid_uid("0123456789ab-cdef-0123-456789abcdef"));
  CXET_CHECK(trading_status_request(uid) == bytes("\x0a\x24" "01234567-89ab-cdef-0123-456789abcdef"));
  CXET_CHECK(book_stream_request(uid) == bytes("\x12\x2e\x08\x01\x12\x2a\x10\x01\x1a\x24"
      "01234567-89ab-cdef-0123-456789abcdef" "\x20\x01"));
  CXET_CHECK(trading_status_request("invalid").empty());
  CXET_CHECK(book_stream_request("invalid").empty());
}
void acknowledgements() {
  constexpr std::string_view good = "\x18\x01\x22\x24" "01234567-89ab-cdef-0123-456789abcdef" "\x38\x01";
  CXET_CHECK(inspect_book_ack(ack(good), uid) == AckResult::Accepted);
  CXET_CHECK(inspect_book_ack(ack("\x18\x02"), uid) == AckResult::Rejected);
  CXET_CHECK(inspect_book_ack(ack("\x18\x63"), uid) == AckResult::Rejected);
  CXET_CHECK(inspect_book_ack(ack(good), "11234567-89ab-cdef-0123-456789abcdef") == AckResult::Rejected);
  CXET_CHECK(inspect_book_ack(ack("\x18\x01\x22\x24" "01234567-89ab-cdef-0123-456789abcdef" "\x38\x02"), uid) == AckResult::Malformed);
  CXET_CHECK(inspect_book_ack(ack("\x18\x01\x18\x01"), uid) == AckResult::Malformed);
  CXET_CHECK(inspect_book_ack(ack("\x18\x01\x22\x01" "x\x38\x01"), uid) == AckResult::Malformed);
  CXET_CHECK(inspect_book_ack(ack("\x18\x01"), uid) == AckResult::Malformed);
  CXET_CHECK(inspect_book_ack(bytes("\x3a\x00"sv), uid) == AckResult::NotAck);
  CXET_CHECK(inspect_book_ack(bytes("\x12\x02\x12"), uid) == AckResult::Malformed);
  CXET_CHECK(inspect_book_ack(bytes("\x12\x00\x12\x00"sv), uid) == AckResult::Malformed);
  CXET_CHECK(inspect_book_ack(bytes("\x12\x04\x12\x00\x12\x00"sv), uid) == AckResult::Malformed);
  CXET_CHECK(inspect_book_ack(std::vector<std::byte>(65537), uid) == AckResult::Malformed);
}
void fixture_varint(std::vector<std::byte>& out, std::uint64_t value) {
  while (value >= 128u) { out.push_back(static_cast<std::byte>((value & 127u) | 128u)); value >>= 7u; }
  out.push_back(static_cast<std::byte>(value));
}
void fixture_scalar(std::vector<std::byte>& out, unsigned tag, std::uint64_t value) {
  fixture_varint(out, tag * 8u); fixture_varint(out, value);
}
void fixture_message(std::vector<std::byte>& out, unsigned tag, const std::vector<std::byte>& value) {
  fixture_varint(out, tag * 8u + 2u); fixture_varint(out, value.size());
  out.insert(out.end(), value.begin(), value.end());
}
std::vector<std::byte> fixture_wrap(unsigned tag, const std::vector<std::byte>& value) {
  std::vector<std::byte> out; fixture_message(out, tag, value); return out;
}
std::vector<std::byte> fixture_time(std::uint64_t seconds, std::uint64_t nanos = 0) {
  std::vector<std::byte> out; fixture_scalar(out, 1, seconds); fixture_scalar(out, 2, nanos); return out;
}
void market_sources_protocol() {
  // Independent complete wire goldens: two subscription fields on one request.
  CXET_CHECK(combined_market_request(uid, 1) == bytes("\x12\x2e\x08\x01\x12\x2a\x10\x32\x1a\x24"
      "01234567-89ab-cdef-0123-456789abcdef" "\x20\x01\x1a\x2c\x08\x01\x12\x26\x12\x24"
      "01234567-89ab-cdef-0123-456789abcdef" "\x18\x01"));
  CXET_CHECK(combined_market_request(uid, 3) == bytes("\x12\x2e\x08\x01\x12\x2a\x10\x32\x1a\x24"
      "01234567-89ab-cdef-0123-456789abcdef" "\x20\x03\x1a\x2c\x08\x01\x12\x26\x12\x24"
      "01234567-89ab-cdef-0123-456789abcdef" "\x18\x03"));
  CXET_CHECK(combined_market_request(uid, 2).empty());
  CXET_CHECK(combined_market_request("invalid", 1).empty());
  std::vector<std::byte> quote;
  fixture_scalar(quote, 1, static_cast<std::uint64_t>(-12ll));
  fixture_scalar(quote, 2, static_cast<std::uint64_t>(-123ll));
  auto order = fixture_wrap(1, quote); fixture_scalar(order, 2, 7);
  auto book = bytes("\x10\x32\x18\x01");
  fixture_message(book, 4, order); fixture_message(book, 5, order);
  fixture_message(book, 6, fixture_time(0, 123)); fixture_message(book, 9, bytes(uid));
  const auto base_book = book;
  for (unsigned source : {0u, 1u, 2u, 3u, 99u}) {
    book = base_book;
    if (source) fixture_scalar(book, 10, source);
    const auto body = fixture_wrap(7, book);
    const auto envelope = market_envelope(body);
    CXET_CHECK(envelope.kind == MarketKind::Book && envelope.result == MarketDecode::Data);
    const auto observation = inspect_market_source(envelope);
    CXET_CHECK(observation.result == MarketDecode::Data && observation.uid == uid &&
        observation.source_present == (source != 0) && observation.source == source);
    const auto& bid = observation.details.at("bids").as_array().at(0).as_object();
    CXET_CHECK(bid.at("price").as_object().at("units").as_int64() == -12);
    CXET_CHECK(bid.at("price").as_object().at("nano").as_int64() == -123);
    CXET_CHECK(bid.at("quantity").as_int64() == 7);
    CXET_CHECK(observation.details.at("is_consistent").as_bool());
    CXET_CHECK(observation.details.at("time").as_object().at("utc").as_string() == "1970-01-01T00:00:00.000000123Z");
    CXET_CHECK(observation.details.at("time").as_object().at("msk").as_string() == "1970-01-01T03:00:00.000000123+03:00");
    CXET_CHECK(inspect_market_source(envelope, false).details.empty());
    if (source) {
      fixture_scalar(book, 10, 1);
      const auto duplicate = fixture_wrap(7, book);
      CXET_CHECK(inspect_market_source(market_envelope(duplicate)).result == MarketDecode::SchemaAnomaly);
    }
  }
  book = base_book; fixture_scalar(book, 10, 0);
  const auto explicit_zero = fixture_wrap(7, book);
  const auto zero_observation = inspect_market_source(market_envelope(explicit_zero));
  CXET_CHECK(zero_observation.source_present && zero_observation.source == 0);
  auto book_ack_row = bytes("\x10\x32\x18\x01");
  fixture_message(book_ack_row, 4, bytes(uid)); fixture_scalar(book_ack_row, 7, 3);
  const auto book_ack = fixture_wrap(2, fixture_wrap(2, book_ack_row));
  auto trade_ack_row = bytes("\x10\x01"); fixture_message(trade_ack_row, 3, bytes(uid));
  auto trade_ack_root = fixture_wrap(2, trade_ack_row); fixture_scalar(trade_ack_root, 3, 3);
  const auto trade_ack = fixture_wrap(3, trade_ack_root);
  auto trade = bytes("\x10\x02"); fixture_message(trade, 3, quote); fixture_scalar(trade, 4, 9);
  fixture_message(trade, 5, fixture_time(1)); fixture_message(trade, 6, bytes(uid)); fixture_scalar(trade, 7, 2);
  const auto trade_body = fixture_wrap(6, trade);
  const auto book_body = fixture_wrap(7, base_book);
  // Interleaving is classified independently of a phase's requested kind/source.
  const std::array<std::vector<std::byte>, 4> interleaved{trade_body, book_ack, book_body, trade_ack};
  const std::array<MarketKind, 4> kinds{MarketKind::Trades, MarketKind::Book, MarketKind::Book, MarketKind::Trades};
  for (std::size_t i = 0; i < interleaved.size(); ++i) {
    const auto observation = inspect_market_source(market_envelope(interleaved[i]));
    CXET_CHECK(observation.kind == kinds[i]);
    CXET_CHECK(observation.result == (i == 1 || i == 3 ? MarketDecode::Ack : MarketDecode::Data));
    if (i == 1 || i == 3) CXET_CHECK(observation.source == 3 && observation.status == 1);
    if (i == 1) CXET_CHECK(observation.depth_present && observation.depth == 50);
  }
  const auto trade_observation = inspect_market_source(market_envelope(trade_body));
  CXET_CHECK(trade_observation.source_present && trade_observation.source == 2);
  CXET_CHECK(trade_observation.details.at("direction").as_int64() == 2);
  CXET_CHECK(trade_observation.details.at("quantity").as_int64() == 9);
  CXET_CHECK(trade_observation.details.at("direction_present").as_bool());
  CXET_CHECK(trade_observation.details.at("quantity_present").as_bool());
  CXET_CHECK(trade_observation.details.at("price").as_object().at("units").as_int64() == -12);
  // Proto3 scalar defaults remain values, with physical presence recorded.
  for (bool explicit_defaults : {false, true}) {
    for (bool book_kind : {false, true}) {
      std::vector<std::byte> ack_row;
      if (explicit_defaults) fixture_scalar(ack_row, book_kind ? 3 : 2, 0);
      fixture_message(ack_row, book_kind ? 4 : 3, bytes(uid));
      const auto ack_body = fixture_wrap(book_kind ? 2 : 3, fixture_wrap(2, ack_row));
      const auto observation = inspect_market_source(market_envelope(ack_body));
      CXET_CHECK(observation.result == MarketDecode::Ack && observation.status == 0);
      CXET_CHECK(observation.status_present == explicit_defaults);
      CXET_CHECK(observation.details.at("status").as_uint64() == 0);
      CXET_CHECK(observation.details.at("status_present").as_bool() == explicit_defaults);
    }
    std::vector<std::byte> zero_trade;
    if (explicit_defaults) fixture_scalar(zero_trade, 2, 0);
    fixture_message(zero_trade, 3, quote);
    if (explicit_defaults) fixture_scalar(zero_trade, 4, 0);
    fixture_message(zero_trade, 5, fixture_time(1)); fixture_message(zero_trade, 6, bytes(uid));
    const auto zero_body = fixture_wrap(6, zero_trade);
    const auto observation = inspect_market_source(market_envelope(zero_body));
    CXET_CHECK(observation.result == MarketDecode::Data && observation.uid == uid);
    CXET_CHECK(observation.details.at("direction").as_int64() == 0);
    CXET_CHECK(observation.details.at("quantity").as_int64() == 0);
    CXET_CHECK(observation.details.at("direction_present").as_bool() == explicit_defaults);
    CXET_CHECK(observation.details.at("quantity_present").as_bool() == explicit_defaults);
  }
  // An omitted source has distinct presence and does not inherit the request.
  trade.resize(trade.size() - 2);
  const auto no_source = fixture_wrap(6, trade);
  CXET_CHECK(!inspect_market_source(market_envelope(no_source)).source_present);
  const auto ping = bytes("\x4a\x00"sv);
  CXET_CHECK(market_envelope(ping).result == MarketDecode::Other && market_envelope(ping).kind == MarketKind::Other);
  auto duplicate_envelope = book_body; duplicate_envelope.insert(duplicate_envelope.end(), trade_body.begin(), trade_body.end());
  CXET_CHECK(market_envelope(duplicate_envelope).result == MarketDecode::SchemaAnomaly);
  auto malformed = trade_body; malformed.pop_back();
  const auto bad_envelope = market_envelope(malformed);
  CXET_CHECK(bad_envelope.result == MarketDecode::MalformedSyntax);
  CXET_CHECK(inspect_market_source(bad_envelope).result == MarketDecode::MalformedSyntax);
  auto bad_quote = quote; bad_quote.push_back(std::byte{8}); bad_quote.push_back(std::byte{1});
  auto bad_trade = bytes("\x10\x02"); fixture_message(bad_trade, 3, bad_quote);
  const auto bad_nested = fixture_wrap(6, bad_trade);
  CXET_CHECK(inspect_market_source(market_envelope(bad_nested)).result == MarketDecode::SchemaAnomaly);
  bad_trade = bytes("\x10\x02"); fixture_message(bad_trade, 3, bytes("\x08"));
  const auto malformed_nested = fixture_wrap(6, bad_trade);
  CXET_CHECK(inspect_market_source(market_envelope(malformed_nested)).result == MarketDecode::MalformedSyntax);
}
void reference_protocol() {
  CXET_CHECK(instrument_request(uid) == bytes("\x08\x03\x1a\x24" "01234567-89ab-cdef-0123-456789abcdef"));
  CXET_CHECK(instrument_request("invalid").empty());
  CXET_CHECK(schedule_request("MOEX", 1'000'000'002ull, 3'000'000'004ull) ==
      bytes("\x0a\x04" "MOEX" "\x12\x04\x08\x01\x10\x02\x1a\x04\x08\x03\x10\x04"));
  CXET_CHECK(schedule_request("", 1, 2).empty());
  CXET_CHECK(schedule_request("MOEX", 2, 1).empty());
  std::vector<std::byte> instrument;
  fixture_message(instrument, 2, bytes("GAZP")); fixture_message(instrument, 3, bytes("TQBR"));
  fixture_message(instrument, 15, bytes("MOEX")); fixture_message(instrument, 25, bytes(uid));
  const auto card = fixture_wrap(1, instrument);
  InstrumentObservation observation; boost::json::object details;
  CXET_CHECK(inspect_instrument(card, uid, observation, details));
  CXET_CHECK(observation.uid == uid && observation.ticker == "GAZP" && observation.class_code == "TQBR" && observation.exchange == "MOEX");
  CXET_CHECK(!inspect_instrument(card, kSberUid, observation, details));
  CXET_CHECK(observation.uid.empty() && details.empty());
  fixture_message(instrument, 25, bytes(uid));
  CXET_CHECK(!inspect_instrument(fixture_wrap(1, instrument), uid, observation, details));
  std::vector<std::byte> day;
  fixture_message(day, 1, fixture_time(86400, 17)); fixture_scalar(day, 2, 0);
  fixture_message(day, 3, fixture_time(86399, 987654321)); fixture_message(day, 4, fixture_time(90000, 123));
  auto bounds = fixture_wrap(1, fixture_time(82800, 1)); fixture_message(bounds, 2, fixture_time(90000, 2));
  auto dealer = fixture_wrap(1, bytes("dealer_regular_trading_session")); fixture_message(dealer, 2, bounds);
  fixture_message(day, 18, dealer);
  auto unknown = fixture_wrap(1, bytes("future_session_type")); fixture_message(unknown, 2, bounds);
  fixture_message(day, 18, unknown);
  auto section = fixture_wrap(1, bytes("MOEX")); fixture_message(section, 2, day);
  const auto schedule = fixture_wrap(1, section);
  CXET_CHECK(inspect_trading_schedule(schedule, "MOEX", details));
  const auto& result_day = details.at("days").as_array().at(0).as_object();
  CXET_CHECK(!result_day.at("is_trading_day").as_bool());
  CXET_CHECK(result_day.at("date").as_object().at("seconds").as_int64() == 86400);
  CXET_CHECK(result_day.at("date").as_object().at("nanos").as_int64() == 17);
  CXET_CHECK(result_day.at("date").as_object().at("utc").as_string() == "1970-01-02T00:00:00.000000017Z");
  CXET_CHECK(result_day.at("start_time").as_object().at("utc").as_string() == "1970-01-01T23:59:59.987654321Z");
  CXET_CHECK(result_day.at("start_time").as_object().at("msk").as_string() == "1970-01-02T02:59:59.987654321+03:00");
  const auto& intervals = result_day.at("intervals").as_array();
  CXET_CHECK(intervals.size() == 2 && intervals.at(0).as_object().at("dealer").as_bool());
  CXET_CHECK(intervals.at(1).as_object().at("type").as_string() == "future_session_type");
  CXET_CHECK(!intervals.at(1).as_object().at("dealer").as_bool());
  CXET_CHECK(!inspect_trading_schedule(schedule, "OTHER", details) && details.empty());
  auto duplicate = section; fixture_message(duplicate, 1, bytes("MOEX"));
  CXET_CHECK(!inspect_trading_schedule(fixture_wrap(1, duplicate), "MOEX", details));
  // The bank returns the same native section in a different ASCII case from
  // the instrument card. Keep both spellings and reject other sections/typos.
  auto native_section = fixture_wrap(1, bytes("MOEX_MRNG_EVNG_E_WKND_DLR"));
  fixture_message(native_section, 2, day);
  const auto native_schedule = fixture_wrap(1, native_section);
  CXET_CHECK(inspect_trading_schedule(native_schedule, "moex_mrng_evng_e_wknd_dlr", details));
  CXET_CHECK(details.at("exchange").as_string() == "MOEX_MRNG_EVNG_E_WKND_DLR");
  CXET_CHECK(details.at("requested_exchange").as_string() == "moex_mrng_evng_e_wknd_dlr");
  CXET_CHECK(!inspect_trading_schedule(native_schedule, "moex_mrng_evng_e_wknd_dlq", details) && details.empty());
  CXET_CHECK(!inspect_trading_schedule(native_schedule, "moex_mrng_evng_e_wknd_dlr_", details) && details.empty());
  CXET_CHECK(!inspect_trading_schedule(native_schedule, "MOEX", details) && details.empty());
  // Complete public TradingSchedules body captured from the bank on 2026-11-05;
  // it contains no authorization metadata or private account information.
  std::ifstream recorded(std::filesystem::path{__FILE__}.parent_path() / "Fixtures/GazpTradingSchedules.pb",
                         std::ios::binary);
  CXET_CHECK(recorded.is_open());
  const std::string recorded_body{std::istreambuf_iterator<char>{recorded}, {}};
  CXET_CHECK(recorded_body.size() == 4112u);
  CXET_CHECK(inspect_trading_schedule(bytes(recorded_body), "moex_mrng_evng_e_wknd_dlr", details));
  CXET_CHECK(details.at("days").as_array().size() == 8u);
  CXET_CHECK(details.at("exchange").as_string() == "MOEX_MRNG_EVNG_E_WKND_DLR");
  auto status = fixture_wrap(6, bytes(uid)); fixture_scalar(status, 2, 5); fixture_scalar(status, 3, 1);
  const auto status_body = fixture_wrap(1, status);
  CXET_CHECK(inspect_trading_status(status_body, uid, &details));
  CXET_CHECK(details.at("trading_status").as_uint64() == 5 && details.at("limit_order_available").as_bool());
  CXET_CHECK(!details.at("market_order_available").as_bool() && !details.at("market_order_available_present").as_bool());
  CXET_CHECK(!inspect_trading_status(status_body, kSberUid, &details) && details.empty());
}
void tariff_limits() {
  auto group = [](std::string_view method, unsigned minute, unsigned second) {
    std::vector<std::byte> row;
    if (minute) { row.push_back(std::byte{0x08}); row.push_back(static_cast<std::byte>(minute)); }
    row.push_back(std::byte{0x12}); row.push_back(static_cast<std::byte>(method.size()));
    auto name = bytes(method); row.insert(row.end(), name.begin(), name.end());
    if (second) { row.push_back(std::byte{0x18}); row.push_back(static_cast<std::byte>(second)); }
    return row;
  };
  auto envelope = [](const std::vector<std::byte>& row) {
    std::vector<std::byte> result{std::byte{0x0a}, static_cast<std::byte>(row.size())};
    result.insert(result.end(), row.begin(), row.end()); return result;
  };
  TariffSummary summary;
  auto body = envelope(group(kTradingStatusPath.substr(1), 0, 6));
  CXET_CHECK(inspect_user_tariff(body, summary));
  CXET_CHECK(summary.count == 1 && summary.groups[0].trading_status &&
             !summary.groups[0].user_tariff && summary.groups[0].minute == 0 && summary.groups[0].second == 6);
  CXET_CHECK(!tariff_covers_experiment(summary));
  summary.groups[0].user_tariff = true;
  CXET_CHECK(tariff_covers_experiment(summary));
  summary.groups[0].minute = 5;
  CXET_CHECK(!tariff_covers_experiment(summary));
  summary.groups[0].minute = 6;
  CXET_CHECK(tariff_covers_experiment(summary));
  body = envelope(group(kUserTariffPath, 20, 0));
  CXET_CHECK(inspect_user_tariff(body, summary));
  CXET_CHECK(summary.count == 1 && summary.groups[0].user_tariff && summary.groups[0].minute == 20);
  body.pop_back();
  CXET_CHECK(!inspect_user_tariff(body, summary) && summary.count == 0);
  body = envelope(group(kTradingStatusPath, 0, 0));
  CXET_CHECK(!inspect_user_tariff(body, summary));
  body = envelope(group("bad method", 10, 0));
  CXET_CHECK(!inspect_user_tariff(body, summary));
  CXET_CHECK(!inspect_user_tariff(std::vector<std::byte>(65537), summary));
}
void lifetime_schedule_and_response() {
  reference_protocol();
  constexpr std::uint64_t second = 1'000'000'000ull;
  CXET_CHECK(next_lifetime_slot(10 * second, 9 * second) == 10 * second);
  CXET_CHECK(next_lifetime_slot(10 * second, 10 * second) == 30 * second);
  CXET_CHECK(next_lifetime_slot(10 * second, 75 * second) == 95 * second);
  // A send delayed from its 70s slot to 89s must not recur at 90s or 99s.
  const auto delayed_next = next_lifetime_slot(70 * second, 89 * second);
  CXET_CHECK(delayed_next == 109 * second && delayed_next > 99 * second);
  // Further delays restart the same channel's full period; there is no burst.
  CXET_CHECK(next_lifetime_slot(delayed_next, 147 * second) == 167 * second);
  CXET_CHECK(next_lifetime_slot(10 * second, std::numeric_limits<std::uint64_t>::max() - 1) ==
             std::numeric_limits<std::uint64_t>::max());
  {
    const std::uint64_t end = 100 * second;
    std::array<std::uint64_t, 2> next{10 * second, 20 * second}, completed{};
    CXET_CHECK(!lifetime_horizon_complete(end, end, next, completed)); // stalled throughout
    completed = {1, 1}; next = {90 * second, 100 * second};
    CXET_CHECK(!lifetime_horizon_complete(end, end, next, completed)); // final slot missed
    next = {30 * second, 40 * second};
    CXET_CHECK(!lifetime_slot_expired(49 * second, end, next)); // jitter under one period
    CXET_CHECK(lifetime_slot_expired(50 * second, end, next)); // one missed period
    CXET_CHECK(lifetime_slot_expired(90 * second, end, next)); // long mid-run pause
    CXET_CHECK(!lifetime_slot_expired(end, end, {100 * second, 110 * second}));
    next = {10 * second, 20 * second}; completed = {};
    std::array<std::uint64_t, 2> previous{};
    std::uint64_t global_previous = 0;
    for (std::uint64_t now = 10 * second; now < end; now += 10 * second) {
      const std::size_t i = next[0] <= next[1] ? 0 : 1;
      CXET_CHECK(now >= next[i]);
      CXET_CHECK(!previous[i] || now - previous[i] >= 20 * second);
      CXET_CHECK(!global_previous || now - global_previous >= 10 * second);
      previous[i] = global_previous = now;
      ++completed[i]; next[i] = next_lifetime_slot(next[i], now);
    }
    CXET_CHECK(lifetime_horizon_complete(end, end, next, completed));
    CXET_CHECK(!lifetime_horizon_complete(end - 1, end, next, completed));
  }
  auto row = bytes("\x32\x24" "e6123145-9665-43e0-8413-cd61b8aa9b13" "\x10\x05"sv);
  std::vector<std::byte> body{std::byte{0x0a}, static_cast<std::byte>(row.size())};
  body.insert(body.end(), row.begin(), row.end());
  CXET_CHECK(inspect_trading_status(body));
  for (unsigned field = 3; field <= 5; ++field) {
    auto duplicate = row;
    duplicate.push_back(static_cast<std::byte>(field << 3u)); duplicate.push_back(std::byte{1});
    duplicate.push_back(static_cast<std::byte>(field << 3u)); duplicate.push_back(std::byte{1});
    std::vector<std::byte> malformed{std::byte{0x0a}, static_cast<std::byte>(duplicate.size())};
    malformed.insert(malformed.end(), duplicate.begin(), duplicate.end());
    CXET_CHECK(!inspect_trading_status(malformed));
  }
  body.pop_back();
  CXET_CHECK(!inspect_trading_status(body));
  CXET_CHECK(!inspect_trading_status({}));
}
} // namespace

int main(int argc, char** argv) {
  const cxet::testing::Case cases[]{
      {"tbank.protocol.request_golden_bytes", requests},
      {"tbank.protocol.book_ack_admission", acknowledgements},
      {"tbank.protocol.tariff_limits", tariff_limits},
      {"tbank.protocol.market_sources", market_sources_protocol},
      {"tbank.protocol.lifetime_schedule_and_response", lifetime_schedule_and_response},
  };
  return cxet::testing::runCases(argc, argv, cases);
}
