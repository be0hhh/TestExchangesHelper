#include "exchange_probe/net.hpp"

#include "net_common.hpp"

#include <boost/asio/ssl/context.hpp>
#include <boost/beast/core/buffers_to_string.hpp>
#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/http.hpp>
#include <boost/json/parse.hpp>
#include <boost/system/error_code.hpp>

#include <chrono>
#include <string>

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
    const std::optional<SignedRequest>& signed_request) {
  namespace asio = boost::asio;
  namespace beast = boost::beast;
  namespace http = beast::http;
  namespace ssl = asio::ssl;

  const auto start = std::chrono::steady_clock::now();
  HttpResult result{};
  result.stage = "configuration";
  try {
    asio::io_context context;
    ssl::context tls_context{ssl::context::tls_client};
    boost::system::error_code setup_error;
    tls_context.set_default_verify_paths(setup_error);
    if (setup_error) {
      result.error = "default_ca_paths_failed:" + setup_error.message();
      result.elapsed_ms = net_detail::elapsed_ms(start);
      return result;
    }
    tls_context.set_options(
        ssl::context::default_workarounds |
        ssl::context::no_sslv2 |
        ssl::context::no_sslv3);

    const auto proxy = proxy_for_host(probe_case.host);
    if (proxy.enabled && !proxy.valid) {
      result.stage = "proxy_configuration";
      result.error = proxy.error;
      result.elapsed_ms = net_detail::elapsed_ms(start);
      return result;
    }
    const auto& connect_host = proxy.enabled ? proxy.host : probe_case.host;
    const auto& connect_port = proxy.enabled ? proxy.port : std::string{"443"};

    result.stage = "dns";
    auto resolved = net_detail::resolve(
        context,
        connect_host,
        connect_port,
        deadline);
    if (!resolved.ok) {
      result.error = resolved.error;
      result.elapsed_ms = net_detail::elapsed_ms(start);
      return result;
    }

    net_detail::TlsStream stream{context, tls_context};
    result.stage = proxy.enabled ? "proxy_tcp_connect" : "tcp_connect";
    if (!net_detail::connect(
            context,
            beast::get_lowest_layer(stream),
            resolved.endpoints,
            deadline,
            result.error)) {
      result.elapsed_ms = net_detail::elapsed_ms(start);
      return result;
    }
    if (proxy.enabled) {
      result.stage = "proxy_connect";
      if (!net_detail::establish_proxy_tunnel(
              context,
              beast::get_lowest_layer(stream),
              proxy,
              probe_case.host,
              "443",
              deadline,
              result.error)) {
        result.elapsed_ms = net_detail::elapsed_ms(start);
        return result;
      }
    }

    result.stage = "tls_configuration";
    if (!net_detail::configure_tls(stream, probe_case.host, result.error)) {
      result.elapsed_ms = net_detail::elapsed_ms(start);
      return result;
    }
    result.stage = "tls_handshake";
    if (!net_detail::tls_handshake(
            context,
            stream,
            deadline,
            result.error)) {
      result.elapsed_ms = net_detail::elapsed_ms(start);
      return result;
    }
    result.tls_verified = true;

    const auto target =
        signed_request.has_value() ? signed_request->path : probe_case.path;
    http::request<http::empty_body> request{
        http::verb::get,
        target,
        11};
    request.set(http::field::host, probe_case.host);
    request.set(http::field::user_agent, "cxet-exchange-api-probe/2");
    request.set(http::field::accept, "application/json");
    if (signed_request.has_value()) {
      for (const auto& [name, value] : signed_request->headers) {
        request.set(name, value);
      }
    }

    boost::system::error_code operation_error;
    bool complete = false;
    result.stage = "http_write";
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
      return result;
    }

    beast::flat_buffer buffer;
    http::response_parser<http::dynamic_body> parser;
    parser.header_limit(kMaxHttpHeadBytes);
    parser.body_limit(kMaxHttpBodyBytes);
    operation_error.clear();
    complete = false;
    result.stage = "http_read";
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
                    ? "http_read_timeout"
                    : operation_error.message();
      result.elapsed_ms = net_detail::elapsed_ms(start);
      net_detail::close_tls(stream);
      return result;
    }

    const auto response = parser.release();
    result.status = response.result_int();
    const auto body = beast::buffers_to_string(response.body().data());
    result.body_bytes = body.size();
    result.stage = "json_parse";
    boost::system::error_code json_error;
    result.json = boost::json::parse(body, json_error);
    if (json_error) {
      result.error = "invalid_json:" + json_error.message();
      result.elapsed_ms = net_detail::elapsed_ms(start);
      net_detail::close_tls(stream);
      return result;
    }
    result.json_present = true;
    result.stage = "complete";
    result.elapsed_ms = net_detail::elapsed_ms(start);
    net_detail::close_tls(stream);
    return result;
  } catch (const std::exception& error) {
    result.error = std::string{"exception:"} + error.what();
    result.elapsed_ms = net_detail::elapsed_ms(start);
    return result;
  }
}

}  // namespace exchange_probe
