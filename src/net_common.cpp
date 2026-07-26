#include "net_common.hpp"

#include "exchange_probe/net.hpp"

#include <boost/asio/bind_executor.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/ssl/host_name_verification.hpp>
#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/http.hpp>
#include <boost/json/string.hpp>

#include <openssl/ssl.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <string>
#include <vector>

namespace exchange_probe {
namespace {

[[nodiscard]] std::string lowercase(std::string_view input) {
  std::string result;
  result.reserve(input.size());
  for (const char value : input) {
    result.push_back(static_cast<char>(
        std::tolower(static_cast<unsigned char>(value))));
  }
  return result;
}

[[nodiscard]] std::string trim(std::string_view input) {
  const auto first = input.find_first_not_of(" \t\r\n");
  if (first == std::string_view::npos) {
    return {};
  }
  const auto last = input.find_last_not_of(" \t\r\n");
  return std::string{input.substr(first, last - first + 1)};
}

[[nodiscard]] bool no_proxy_match(
    std::string_view host,
    std::string_view rules) {
  const auto normalized_host = lowercase(host);
  std::size_t cursor = 0;
  while (cursor <= rules.size()) {
    const auto separator = rules.find(',', cursor);
    auto token = trim(rules.substr(
        cursor,
        separator == std::string_view::npos
            ? rules.size() - cursor
            : separator - cursor));
    token = lowercase(token);
    const auto colon = token.rfind(':');
    if (colon != std::string::npos &&
        token.find(']') == std::string::npos) {
      token.resize(colon);
    }
    if (token == "*" || token == normalized_host) {
      return true;
    }
    if (!token.empty() && token.front() == '.') {
      token.erase(token.begin());
    }
    if (!token.empty() && normalized_host.size() > token.size() &&
        normalized_host.ends_with(token) &&
        normalized_host[normalized_host.size() - token.size() - 1] == '.') {
      return true;
    }
    if (separator == std::string_view::npos) {
      break;
    }
    cursor = separator + 1;
  }
  return false;
}

[[nodiscard]] std::string environment_value(
    const char* primary,
    const char* secondary) {
  if (const char* value = std::getenv(primary); value != nullptr && *value != '\0') {
    return value;
  }
  if (const char* value = std::getenv(secondary); value != nullptr && *value != '\0') {
    return value;
  }
  return {};
}

[[nodiscard]] std::string base64_basic(std::string_view value) {
  static constexpr char table[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string output;
  output.reserve(4U * ((value.size() + 2U) / 3U));
  std::size_t cursor = 0;
  while (cursor < value.size()) {
    const auto a = static_cast<unsigned char>(value[cursor++]);
    const bool have_b = cursor < value.size();
    const auto b =
        have_b ? static_cast<unsigned char>(value[cursor++]) : 0U;
    const bool have_c = cursor < value.size();
    const auto c =
        have_c ? static_cast<unsigned char>(value[cursor++]) : 0U;
    const auto bits =
        static_cast<unsigned>(a) << 16U |
        static_cast<unsigned>(b) << 8U |
        static_cast<unsigned>(c);
    output.push_back(table[(bits >> 18U) & 0x3fU]);
    output.push_back(table[(bits >> 12U) & 0x3fU]);
    output.push_back(have_b ? table[(bits >> 6U) & 0x3fU] : '=');
    output.push_back(have_c ? table[bits & 0x3fU] : '=');
  }
  return output;
}

}  // namespace

ProxyConfig proxy_for_host(std::string_view host) {
  const auto no_proxy =
      environment_value("NO_PROXY", "no_proxy");
  if (!no_proxy.empty() && no_proxy_match(host, no_proxy)) {
    return {};
  }
  const auto raw = environment_value("HTTPS_PROXY", "https_proxy");
  if (raw.empty()) {
    return {};
  }

  ProxyConfig proxy{};
  proxy.enabled = true;
  constexpr std::string_view scheme = "http://";
  if (!std::string_view{raw}.starts_with(scheme)) {
    proxy.valid = false;
    proxy.error = "only_http_connect_proxy_supported";
    return proxy;
  }
  auto authority = std::string_view{raw}.substr(scheme.size());
  if (const auto slash = authority.find('/'); slash != std::string_view::npos) {
    authority = authority.substr(0, slash);
  }
  if (const auto at = authority.rfind('@'); at != std::string_view::npos) {
    const auto credentials = authority.substr(0, at);
    proxy.authorization = "Basic " + base64_basic(credentials);
    authority = authority.substr(at + 1);
  }
  if (authority.empty()) {
    proxy.valid = false;
    proxy.error = "proxy_authority_missing";
    return proxy;
  }
  if (authority.front() == '[') {
    const auto close = authority.find(']');
    if (close == std::string_view::npos) {
      proxy.valid = false;
      proxy.error = "proxy_ipv6_invalid";
      return proxy;
    }
    proxy.host = std::string{authority.substr(1, close - 1)};
    if (close + 1 < authority.size() && authority[close + 1] == ':') {
      proxy.port = std::string{authority.substr(close + 2)};
    }
  } else if (const auto colon = authority.rfind(':');
             colon != std::string_view::npos) {
    proxy.host = std::string{authority.substr(0, colon)};
    proxy.port = std::string{authority.substr(colon + 1)};
  } else {
    proxy.host = std::string{authority};
  }
  if (proxy.port.empty()) {
    proxy.port = "80";
  }
  if (proxy.host.empty() || proxy.port.empty()) {
    proxy.valid = false;
    proxy.error = "proxy_host_or_port_missing";
  }
  return proxy;
}

namespace net_detail {

ResolveResult resolve(
    asio::io_context& context,
    std::string_view host,
    std::string_view port,
    std::chrono::steady_clock::time_point deadline) {
  ResolveResult result;
  tcp::resolver resolver{context};
  asio::steady_timer timer{context};
  bool completed = false;
  timer.expires_at(deadline);
  timer.async_wait([&](const boost::system::error_code& error) {
    if (!error && !completed) {
      result.timed_out = true;
      resolver.cancel();
    }
  });
  resolver.async_resolve(
      std::string{host},
      std::string{port},
      [&](const boost::system::error_code& error,
          tcp::resolver::results_type endpoints) {
        completed = true;
        timer.cancel();
        if (error) {
          result.error = result.timed_out ? "resolve_timeout" : error.message();
          return;
        }
        result.endpoints = std::move(endpoints);
        result.ok = true;
      });
  context.run();
  context.restart();
  return result;
}

bool connect(
    asio::io_context& context,
    beast::tcp_stream& stream,
    const tcp::resolver::results_type& endpoints,
    std::chrono::steady_clock::time_point deadline,
    std::string& error) {
  boost::system::error_code operation_error;
  bool complete = false;
  stream.expires_at(deadline);
  stream.async_connect(
      endpoints,
      [&](const boost::system::error_code& ec, const tcp::endpoint&) {
        operation_error = ec;
        complete = true;
      });
  context.run();
  context.restart();
  if (!complete || operation_error) {
    error = operation_error == beast::error::timeout
                ? "connect_timeout"
                : operation_error.message();
    return false;
  }
  return true;
}

bool establish_proxy_tunnel(
    asio::io_context& context,
    beast::tcp_stream& stream,
    const ProxyConfig& proxy,
    std::string_view destination_host,
    std::string_view destination_port,
    std::chrono::steady_clock::time_point deadline,
    std::string& error) {
  namespace http = beast::http;
  http::request<http::empty_body> request{
      http::verb::connect,
      std::string{destination_host} + ":" + std::string{destination_port},
      11};
  request.set(http::field::host, request.target());
  request.set(http::field::user_agent, "cxet-exchange-api-probe/2");
  request.set(http::field::proxy_connection, "keep-alive");
  if (!proxy.authorization.empty()) {
    request.set(http::field::proxy_authorization, proxy.authorization);
  }

  boost::system::error_code operation_error;
  bool complete = false;
  stream.expires_at(deadline);
  http::async_write(
      stream,
      request,
      [&](const boost::system::error_code& ec, std::size_t) {
        operation_error = ec;
        complete = true;
      });
  context.run();
  context.restart();
  if (!complete || operation_error) {
    error = operation_error == beast::error::timeout
                ? "proxy_connect_write_timeout"
                : operation_error.message();
    return false;
  }

  beast::flat_buffer buffer;
  http::response_parser<http::empty_body> parser;
  parser.header_limit(kMaxHttpHeadBytes);
  complete = false;
  stream.expires_at(deadline);
  http::async_read_header(
      stream,
      buffer,
      parser,
      [&](const boost::system::error_code& ec, std::size_t) {
        operation_error = ec;
        complete = true;
      });
  context.run();
  context.restart();
  if (!complete || operation_error) {
    error = operation_error == beast::error::timeout
                ? "proxy_connect_read_timeout"
                : operation_error.message();
    return false;
  }
  if (parser.get().result() != http::status::ok) {
    error = "proxy_connect_status_" +
            std::to_string(parser.get().result_int());
    return false;
  }
  return true;
}

bool configure_tls(
    TlsStream& stream,
    std::string_view host,
    std::string& error) {
  if (SSL_set_tlsext_host_name(
          stream.native_handle(),
          std::string{host}.c_str()) != 1) {
    error = "tls_sni_failed";
    return false;
  }
  stream.set_verify_mode(ssl::verify_peer);
  stream.set_verify_callback(ssl::host_name_verification(std::string{host}));
  return true;
}

bool tls_handshake(
    asio::io_context& context,
    TlsStream& stream,
    std::chrono::steady_clock::time_point deadline,
    std::string& error) {
  boost::system::error_code operation_error;
  bool complete = false;
  beast::get_lowest_layer(stream).expires_at(deadline);
  stream.async_handshake(
      ssl::stream_base::client,
      [&](const boost::system::error_code& ec) {
        operation_error = ec;
        complete = true;
      });
  context.run();
  context.restart();
  if (!complete || operation_error) {
    error = operation_error == beast::error::timeout
                ? "tls_handshake_timeout"
                : operation_error.message();
    return false;
  }
  return true;
}

void close_tls(TlsStream& stream) noexcept {
  boost::system::error_code ignored;
  stream.shutdown(ignored);
  beast::get_lowest_layer(stream).socket().shutdown(
      tcp::socket::shutdown_both,
      ignored);
  beast::get_lowest_layer(stream).socket().close(ignored);
}

std::uint64_t elapsed_ms(
    std::chrono::steady_clock::time_point start) noexcept {
  const auto elapsed =
      std::chrono::steady_clock::now() - start;
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count());
}

}  // namespace net_detail
}  // namespace exchange_probe
