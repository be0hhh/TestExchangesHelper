#include "OfflineCase.hpp"
#include "../../src/Research/HtxReconnectPolicy.hpp"

#include <chrono>
#include <boost/asio/error.hpp>
#include <boost/asio/ssl/error.hpp>
#include <boost/beast/websocket/error.hpp>

namespace {
using namespace exchange_probe::research_capture;
using Clock = std::chrono::steady_clock;

void optInBudget() {
  const auto now = Clock::time_point{std::chrono::seconds{1}};
  const auto deadline = now + std::chrono::seconds{10};
  CXET_CHECK(!reconnect_wait(1U, 1U, now, deadline, 0U).has_value());
  CXET_CHECK(reconnect_wait(1U, 3U, now, deadline, 0U).has_value());
  CXET_CHECK(reconnect_wait(2U, 3U, now, deadline, 0U).has_value());
  CXET_CHECK(!reconnect_wait(3U, 3U, now, deadline, 0U).has_value());
  CXET_CHECK(!reconnect_wait(1U, 4U, now, deadline, 0U).has_value());
  CXET_CHECK(!reconnect_wait(0U, 3U, now, deadline, 0U).has_value());
}

void absoluteDeadline() {
  const auto now = Clock::time_point{std::chrono::seconds{1}};
  CXET_CHECK(!reconnect_wait(1U, 3U, now, now, 0U).has_value());
  CXET_CHECK(!reconnect_wait(1U, 3U, now, now - std::chrono::milliseconds{1}, 0U).has_value());
  CXET_CHECK(!reconnect_wait(1U, 3U, now, now + std::chrono::milliseconds{250}, 0U).has_value());
  CXET_CHECK(reconnect_wait(1U, 3U, now, now + std::chrono::milliseconds{251}, 0U).has_value());
}

void jitterBounds() {
  const auto now = Clock::time_point{std::chrono::seconds{1}};
  const auto deadline = now + std::chrono::seconds{10};
  const auto low = reconnect_wait(1U, 3U, now, deadline, 0U);
  const auto high = reconnect_wait(1U, 3U, now, deadline, 100U);
  CXET_CHECK(low == std::chrono::milliseconds{250});
  CXET_CHECK(high == std::chrono::milliseconds{350});
  CXET_CHECK(!reconnect_wait(1U, 3U, now, now + std::chrono::milliseconds{350}, 100U).has_value());
  for (unsigned seed = 0U; seed < 1000U; ++seed) {
    const auto delay = reconnect_wait(1U, 3U, now, deadline, seed);
    CXET_CHECK(delay.has_value() && *delay >= std::chrono::milliseconds{250});
    CXET_CHECK(*delay <= std::chrono::milliseconds{350});
  }
}

void supportedPublicTradeOnly() {
  exchange_probe::ResearchChannel channel;
  channel.kind = exchange_probe::ResearchChannelKind::Trade;
  channel.host = "api.huobi.pro";
  channel.path = "/ws";
  channel.subscribe = R"({"sub":"market.{symbol}.trade.detail","id":"probe"})";
  CXET_CHECK(reconnect_supported("htx", "spot", channel));
  CXET_CHECK(!reconnect_supported("other", "spot", channel));
  channel.kind = exchange_probe::ResearchChannelKind::Depth;
  CXET_CHECK(!reconnect_supported("htx", "spot", channel));
  channel.kind = exchange_probe::ResearchChannelKind::Trade;
  channel.host = "unknown.example";
  CXET_CHECK(!reconnect_supported("htx", "spot", channel));
  channel.host = "api.huobi.pro";
  channel.subscribe = R"({"sub":"market.{symbol}.trade.detail","auth":"secret"})";
  CXET_CHECK(!reconnect_supported("htx", "spot", channel));
}

void transientIoOnly() {
  namespace asio = boost::asio;
  namespace ws = boost::beast::websocket;
  CXET_CHECK(retryable_io_error(asio::error::connection_reset));
  CXET_CHECK(retryable_io_error(asio::error::broken_pipe));
  CXET_CHECK(retryable_io_error(asio::error::timed_out));
  CXET_CHECK(retryable_io_error(asio::ssl::error::stream_truncated));
  CXET_CHECK(retryable_io_error(ws::error::closed, true));
  CXET_CHECK(!retryable_io_error(ws::error::closed, false));
  CXET_CHECK(!retryable_io_error(ws::error::message_too_big));
  CXET_CHECK(!retryable_io_error(ws::error::bad_frame_payload));
  CXET_CHECK(!retryable_io_error(asio::error::operation_aborted));
  CXET_CHECK(!retryable_io_error(boost::system::error_code{1, asio::error::get_ssl_category()}));
  CXET_CHECK(!retryable_io_error({}));
}

void frozenSymbolCannotAlterSubscription() {
  CXET_CHECK(reconnect_symbol_supported("spot", "btcusdt"));
  CXET_CHECK(reconnect_symbol_supported("futures", "BTC-USDT"));
  CXET_CHECK(!reconnect_symbol_supported("spot", ""));
  CXET_CHECK(!reconnect_symbol_supported("spot", "btc\"usdt"));
  CXET_CHECK(!reconnect_symbol_supported("futures", "BTC\\USDT"));
  CXET_CHECK(!reconnect_symbol_supported("spot", "btc\nusdt"));
  CXET_CHECK(!reconnect_symbol_supported("spot", std::string(65U, 'a')));
  CXET_CHECK(!reconnect_symbol_supported("other", "btcusdt"));
}
}

int main(int argc, char** argv) {
  const cxet::testing::Case cases[]{
      cxet::testing::Case{"reconnect.default_off_and_three_attempt_budget", optInBudget},
      cxet::testing::Case{"reconnect.original_absolute_deadline_is_not_extended", absoluteDeadline},
      cxet::testing::Case{"reconnect.fixed_backoff_and_jitter_are_bounded", jitterBounds},
      cxet::testing::Case{"reconnect.only_documented_public_htx_trade_is_supported", supportedPublicTradeOnly},
      cxet::testing::Case{"reconnect.only_transient_io_errors_are_retryable", transientIoOnly},
      cxet::testing::Case{"reconnect.frozen_symbol_cannot_alter_subscription", frozenSymbolCannotAlterSubscription},
  };
  return cxet::testing::runCases(argc, argv, cases);
}
