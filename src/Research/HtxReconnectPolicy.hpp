#pragma once

#include "exchange_probe/ResearchProfile.hpp"

#include <boost/json/object.hpp>
#include <boost/json/parse.hpp>
#include <boost/system/error_code.hpp>
#include <boost/asio/error.hpp>
#include <boost/asio/ssl/error.hpp>
#include <boost/beast/core/error.hpp>
#include <boost/beast/websocket/error.hpp>
#include <chrono>
#include <optional>
#include <algorithm>

namespace exchange_probe::research_capture {

// Only failures with a typed transient I/O meaning may reopen a public feed.
// Capacity, framing, application and TLS-authority failures are terminal.
[[nodiscard]] inline bool retryable_io_error(
    const boost::system::error_code& error,
    bool normal_websocket_close = false) noexcept {
  namespace asio = boost::asio;
  return error == asio::error::connection_reset ||
         error == asio::error::connection_aborted ||
         error == asio::error::broken_pipe || error == asio::error::eof ||
         error == asio::error::timed_out || error == boost::beast::error::timeout ||
         error == asio::ssl::error::stream_truncated ||
         (normal_websocket_close && error == boost::beast::websocket::error::closed);
}

[[nodiscard]] inline bool reconnect_symbol_supported(
    std::string_view product, std::string_view symbol) noexcept {
  if ((product != "spot" && product != "futures") ||
      symbol.empty() || symbol.size() > 64U) return false;
  return std::all_of(symbol.begin(), symbol.end(), [product](unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || (product == "futures" && c == '-');
  });
}

[[nodiscard]] inline bool reconnect_supported(
    std::string_view venue, std::string_view product,
    const ResearchChannel& channel) {
  if (venue != "htx" || channel.kind != ResearchChannelKind::Trade ||
      channel.transport != "ws" || channel.wire != "json" ||
      (channel.compression != "none" && channel.compression != "gzip") ||
      channel.port != 443U || channel.subscribe_binary ||
      channel.support == ResearchSupport::Candidate ||
      channel.support == ResearchSupport::Unavailable) return false;
  if (product == "spot") {
    if ((channel.host != "api.huobi.pro" && channel.host != "api-aws.huobi.pro") ||
        channel.path != "/ws") return false;
  } else if (product == "futures") {
    if ((channel.host != "api.hbdm.com" && channel.host != "api.hbdm.vn") ||
        channel.path != "/linear-swap-ws") return false;
  } else {
    return false;
  }
  boost::system::error_code error;
  const auto subscription = boost::json::parse(channel.subscribe, error);
  if (error || !subscription.is_object()) return false;
  const auto& object = subscription.as_object();
  const auto* sub = object.if_contains("sub");
  if (sub == nullptr || !sub->is_string() ||
      sub->as_string() != "market.{symbol}.trade.detail") return false;
  // A public subscription may carry only its request ID alongside sub.
  for (const auto& field : object) {
    if (field.key() != "sub" && field.key() != "id") return false;
  }
  return true;
}

// Cold public HTX Trade capture only: the budget includes the initial attempt.
// Caller owns the frozen endpoint and original absolute round deadline. No
// persistent state, random generator, endpoint failover or deadline extension.
[[nodiscard]] inline std::optional<std::chrono::milliseconds> reconnect_wait(
    unsigned completed_attempt,
    unsigned maximum_attempts,
    std::chrono::steady_clock::time_point now,
    std::chrono::steady_clock::time_point deadline,
    unsigned jitter_seed) noexcept {
  if (maximum_attempts < 1U || maximum_attempts > 3U ||
      completed_attempt == 0U || completed_attempt >= maximum_attempts ||
      now >= deadline) return std::nullopt;
  const auto delay = std::chrono::milliseconds{250U + jitter_seed % 101U};
  if (deadline - now <= delay) return std::nullopt;
  return delay;
}

}  // namespace exchange_probe::research_capture
