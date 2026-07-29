#include "exchange_probe/net.hpp"

#include "net_common.hpp"

#include <boost/asio/ssl/context.hpp>
#include <boost/asio/ip/address.hpp>
#include <boost/beast/core/buffers_to_string.hpp>
#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/http.hpp>
#include <boost/json/parse.hpp>
#include <boost/system/error_code.hpp>

#include <chrono>
#include <string>
#include <utility>

namespace exchange_probe {
namespace {

[[nodiscard]] bool timed_out(
    const boost::system::error_code& error) noexcept {
  return error == boost::beast::error::timeout ||
         error == boost::asio::error::timed_out;
}

}  // namespace

HttpResult execute_http(
    const RestCase& probe_case,
    std::chrono::steady_clock::time_point deadline,
    const std::optional<SignedRequest>& signed_request,
    const std::optional<std::string>& pinned_ip,
    const std::optional<bool>& use_proxy) {
  namespace asio = boost::asio;
  namespace beast = boost::beast;
  namespace http = beast::http;
  namespace ssl = asio::ssl;

  const auto start = std::chrono::steady_clock::now();
  HttpResult result{};
  const auto finish = [&]() {
    result.timings.total_us = net_detail::elapsed_us(start);
    result.elapsed_ms = result.timings.total_us / 1000U;
    return std::move(result);
  };
  result.stage = "configuration";
  try {
    asio::io_context context;
    ssl::context tls_context{ssl::context::tls_client};
    boost::system::error_code setup_error;
    tls_context.set_default_verify_paths(setup_error);
    if (setup_error) {
      result.error = "default_ca_paths_failed:" + setup_error.message();
      result.elapsed_ms = net_detail::elapsed_ms(start);
      return finish();
    }
    tls_context.set_options(
        ssl::context::default_workarounds |
        ssl::context::no_sslv2 |
        ssl::context::no_sslv3);

    auto proxy = proxy_for_host(probe_case.host);
    if (use_proxy.has_value() && !*use_proxy) {
      proxy = {};
    }
    if (use_proxy.value_or(false) && !proxy.enabled) {
      result.stage = "proxy_configuration";
      result.error = "proxy_not_configured";
      result.elapsed_ms = net_detail::elapsed_ms(start);
      return finish();
    }
    if (proxy.enabled && !proxy.valid) {
      result.stage = "proxy_configuration";
      result.error = proxy.error;
      result.elapsed_ms = net_detail::elapsed_ms(start);
      return finish();
    }
    if (proxy.enabled && pinned_ip.has_value()) {
      result.stage = "route_configuration";
      result.error = "pinned_route_unavailable_through_proxy";
      result.elapsed_ms = net_detail::elapsed_ms(start);
      return finish();
    }
    const auto& connect_host = proxy.enabled ? proxy.host : probe_case.host;
    const auto& connect_port = proxy.enabled ? proxy.port : std::string{"443"};

    result.stage = "dns";
    auto stage_start = std::chrono::steady_clock::now();
    net_detail::ResolveResult resolved;
    std::optional<asio::ip::tcp::endpoint> pinned_endpoint;
    if (pinned_ip.has_value()) {
      boost::system::error_code address_error;
      const auto address = asio::ip::make_address(*pinned_ip, address_error);
      if (address_error) {
        result.error = "pinned_ip_invalid:" + address_error.message();
        result.elapsed_ms = net_detail::elapsed_ms(start);
        return finish();
      }
      pinned_endpoint.emplace(address, 443U);
    } else {
      resolved = net_detail::resolve(
          context,
          connect_host,
          connect_port,
          deadline);
      result.timings.dns_us = net_detail::elapsed_us(stage_start);
      if (!resolved.ok) {
        result.error = resolved.error;
        result.elapsed_ms = net_detail::elapsed_ms(start);
        return finish();
      }
    }

    net_detail::TlsStream stream{context, tls_context};
    result.stage = proxy.enabled ? "proxy_tcp_connect" : "tcp_connect";
    stage_start = std::chrono::steady_clock::now();
    const bool connected =
        pinned_endpoint.has_value()
            ? net_detail::connect_endpoint(
                  context,
                  beast::get_lowest_layer(stream),
                  *pinned_endpoint,
                  deadline,
                  result.error)
            : net_detail::connect(
                  context,
                  beast::get_lowest_layer(stream),
                  resolved.endpoints,
                  deadline,
                  result.error);
    if (!connected) {
      result.elapsed_ms = net_detail::elapsed_ms(start);
      return finish();
    }
    result.timings.tcp_connect_us = net_detail::elapsed_us(stage_start);
    if (proxy.enabled) {
      result.stage = "proxy_connect";
      stage_start = std::chrono::steady_clock::now();
      if (!net_detail::establish_proxy_tunnel(
              context,
              beast::get_lowest_layer(stream),
              proxy,
              probe_case.host,
              "443",
              deadline,
              result.error)) {
        result.elapsed_ms = net_detail::elapsed_ms(start);
        return finish();
      }
      result.timings.proxy_connect_us = net_detail::elapsed_us(stage_start);
    }

    result.stage = "tls_configuration";
    if (!net_detail::configure_tls(stream, probe_case.host, result.error)) {
      result.elapsed_ms = net_detail::elapsed_ms(start);
      return finish();
    }
    result.stage = "tls_handshake";
    stage_start = std::chrono::steady_clock::now();
    if (!net_detail::tls_handshake(
            context,
            stream,
            deadline,
            result.error)) {
      result.elapsed_ms = net_detail::elapsed_ms(start);
      return finish();
    }
    result.timings.tls_handshake_us = net_detail::elapsed_us(stage_start);
    result.tls_verified = true;
    net_detail::refresh_transport_metadata(result.transport, stream);

    const auto target =
        signed_request.has_value() ? signed_request->path : probe_case.path;
    http::request<http::empty_body> request{
        http::verb::get,
        target,
        11};
    request.set(http::field::host, probe_case.host);
    request.set(http::field::user_agent, "exchange-api-probe/3");
    request.set(http::field::accept, "application/json");
    if (signed_request.has_value()) {
      for (const auto& [name, value] : signed_request->headers) {
        request.set(name, value);
      }
    }

    boost::system::error_code operation_error;
    bool complete = false;
    result.stage = "http_write";
    stage_start = std::chrono::steady_clock::now();
    beast::get_lowest_layer(stream).expires_at(deadline);
    http::async_write(
        stream,
        request,
        [&](const boost::system::error_code& error, std::size_t) {
          operation_error = error;
          complete = true;
        });
    context.run();
    context.restart();
    if (!complete || operation_error) {
      result.error = timed_out(operation_error)
                         ? "http_write_timeout"
                         : operation_error.message();
      result.elapsed_ms = net_detail::elapsed_ms(start);
      net_detail::close_tls(stream);
      return finish();
    }
    result.timings.request_write_us = net_detail::elapsed_us(stage_start);

    beast::flat_buffer buffer;
    http::response_parser<http::dynamic_body> parser;
    parser.header_limit(kMaxHttpHeadBytes);
    parser.body_limit(kMaxHttpBodyBytes);
    operation_error.clear();
    complete = false;
    result.stage = "http_header_read";
    stage_start = std::chrono::steady_clock::now();
    beast::get_lowest_layer(stream).expires_at(deadline);
    http::async_read_header(
        stream,
        buffer,
        parser,
        [&](const boost::system::error_code& error, std::size_t) {
          operation_error = error;
          complete = true;
        });
    context.run();
    context.restart();
    if (!complete || operation_error) {
      result.error =
          operation_error == http::error::body_limit
              ? "response_capacity_exceeded"
              : timed_out(operation_error)
                    ? "http_header_read_timeout"
                    : operation_error.message();
      result.elapsed_ms = net_detail::elapsed_ms(start);
      net_detail::close_tls(stream);
      return finish();
    }
    result.timings.ttfb_us = net_detail::elapsed_us(stage_start);

    operation_error.clear();
    complete = false;
    result.stage = "http_body_read";
    stage_start = std::chrono::steady_clock::now();
    beast::get_lowest_layer(stream).expires_at(deadline);
    http::async_read(
        stream,
        buffer,
        parser,
        [&](const boost::system::error_code& error, std::size_t) {
          operation_error = error;
          complete = true;
        });
    context.run();
    context.restart();
    if (!complete || operation_error) {
      result.error =
          operation_error == http::error::body_limit
              ? "response_capacity_exceeded"
              : timed_out(operation_error)
                    ? "http_body_read_timeout"
                    : operation_error.message();
      net_detail::refresh_transport_metadata(result.transport, stream);
      net_detail::close_tls(stream);
      return finish();
    }
    result.timings.body_read_us = net_detail::elapsed_us(stage_start);

    const auto response = parser.release();
    result.status = response.result_int();
    const auto body = beast::buffers_to_string(response.body().data());
    result.body_bytes = body.size();
    result.stage = "json_parse";
    stage_start = std::chrono::steady_clock::now();
    boost::system::error_code json_error;
    result.json = boost::json::parse(body, json_error);
    result.timings.json_parse_us = net_detail::elapsed_us(stage_start);
    if (json_error) {
      result.error = "invalid_json:" + json_error.message();
      result.elapsed_ms = net_detail::elapsed_ms(start);
      net_detail::close_tls(stream);
      return finish();
    }
    result.json_present = true;
    result.stage = "complete";
    net_detail::refresh_transport_metadata(result.transport, stream);
    result.timings.total_us = net_detail::elapsed_us(start);
    result.elapsed_ms = result.timings.total_us / 1000U;
    net_detail::close_tls(stream);
    return finish();
  } catch (const std::exception& error) {
    result.error = std::string{"exception:"} + error.what();
    result.elapsed_ms = net_detail::elapsed_ms(start);
    return finish();
  }
}

}  // namespace exchange_probe
