#include "OfflineCase.hpp"
#include "../../../src/Research/TbankGrpcProtocol.hpp"

#include <cstddef>
#include <limits>
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
      {"tbank.protocol.lifetime_schedule_and_response", lifetime_schedule_and_response},
  };
  return cxet::testing::runCases(argc, argv, cases);
}
