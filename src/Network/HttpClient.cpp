#include "exchange_probe/Net.hpp"

#include "NetCommon.hpp"

#include <boost/asio/ip/address.hpp>
#include <boost/beast/core/buffers_to_string.hpp>
#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/http.hpp>
#include <boost/json/parse.hpp>

#include <chrono>
#include <algorithm>
#include <memory>
#include <string>
#include <utility>

namespace exchange_probe {
namespace {
namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
namespace ssl = asio::ssl;
using Clock = std::chrono::steady_clock;

[[nodiscard]] bool timed_out(const boost::system::error_code& error) noexcept {
  return error == beast::error::timeout || error == asio::error::timed_out;
}
[[nodiscard]] std::uint64_t elapsed_ns(Clock::time_point start) {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count());
}
[[nodiscard]] std::uint64_t utc_ns() {
  return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::system_clock::now().time_since_epoch()).count());
}
[[nodiscard]] std::uint64_t monotonic_ns(Clock::time_point value) {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(value.time_since_epoch()).count());
}

// Both existing one-shot calls and diagnostic public keep-alive GETs use this
// implementation. Only the diagnostic path retains raw public response bytes.
class HttpSession {
 public:
  ~HttpSession() { close_socket(); }

  HttpResult request(const RestCase& probe_case, Clock::time_point deadline,
                     const std::optional<SignedRequest>& signed_request,
                     const std::optional<std::string>& pinned_ip,
                     bool persistent) {
    HttpResult result{};
    result.stage = "configuration";
    const auto finish = [&]() {
      if (stream_) net_detail::refresh_transport_metadata(result.transport, *stream_);
      result.connection_available = persistent && connected_ && !invalid_;
      if (!persistent && stream_) {
        // A one-shot response is complete. Teardown must not wait indefinitely
        // for a peer TLS close_notify after the request deadline.
        close_socket();
      }
      return std::move(result);
    };
    try {
      if (invalid_) {
        result.stage = "connection_invalidated";
        result.error = "public_connection_invalidated_no_automatic_reconnect";
        return finish();
      }
      if (Clock::now() >= deadline) {
        result.error = "http_deadline_expired";
        return finish();
      }
      if (!connected_) {
        tls_context_ = std::make_unique<ssl::context>(ssl::context::tls_client);
        boost::system::error_code setup_error;
        tls_context_->set_default_verify_paths(setup_error);
        if (setup_error) {
          result.error = "default_ca_paths_failed:" + setup_error.message();
          invalidate();
          return finish();
        }
        tls_context_->set_options(ssl::context::default_workarounds |
                                  ssl::context::no_sslv2 | ssl::context::no_sslv3);
        result.stage = "dns";
        net_detail::ResolveResult resolved;
        std::optional<asio::ip::tcp::endpoint> pinned_endpoint;
        const auto dns_start = Clock::now();
        if (persistent) result.dns_utc_ns = utc_ns();
        if (pinned_ip) {
          boost::system::error_code address_error;
          const auto address = asio::ip::make_address(*pinned_ip, address_error);
          if (address_error) {
            result.error = "pinned_ip_invalid:" + address_error.message();
            invalidate();
            return finish();
          }
          pinned_endpoint.emplace(address, 443U);
        } else {
          resolved = net_detail::resolve(context_, probe_case.host, "443", deadline);
          if (persistent) {
            result.dns_ns = elapsed_ns(dns_start);
            for (const auto& entry : resolved.endpoints) {
              if (result.dns_answers.size() == 64U) {
                result.error = "dns_answers_capacity_exceeded";
                invalidate();
                return finish();
              }
              result.dns_answers.push_back(entry.endpoint().address().to_string());
            }
          }
          if (!resolved.ok) {
            result.error = resolved.error;
            invalidate();
            return finish();
          }
        }
        stream_ = std::make_unique<net_detail::TlsStream>(context_, *tls_context_);
        result.stage = "tcp_connect";
        const auto connect_start = Clock::now();
        const bool connected = pinned_endpoint
            ? net_detail::connect_endpoint(context_, beast::get_lowest_layer(*stream_),
                                           *pinned_endpoint, deadline, result.error)
            : net_detail::connect(context_, beast::get_lowest_layer(*stream_),
                                  resolved.endpoints, deadline, result.error);
        if (persistent) result.tcp_connect_ns = elapsed_ns(connect_start);
        if (!connected) { invalidate(); return finish(); }
        result.stage = "tls_configuration";
        if (!net_detail::configure_tls(*stream_, probe_case.host, result.error)) {
          invalidate(); return finish();
        }
        result.stage = "tls_handshake";
        const auto handshake_start = Clock::now();
        const bool handshaken = net_detail::tls_handshake(
            context_, *stream_, deadline, result.error);
        if (persistent) result.tls_handshake_ns = elapsed_ns(handshake_start);
        if (!handshaken) { invalidate(); return finish(); }
        connected_ = true;
      }
      result.tls_verified = true;
      result.connection_reused = persistent && completed_requests_ != 0;
      net_detail::refresh_transport_metadata(result.transport, *stream_);
      const auto target = signed_request ? signed_request->path : probe_case.path;
      http::request<http::empty_body> request{http::verb::get, target, 11};
      request.set(http::field::host, probe_case.host);
      request.set(http::field::user_agent, "exchange-api-probe/3");
      request.set(http::field::accept, "application/json");
      if (persistent) request.keep_alive(true);
      if (signed_request) {
        for (const auto& [name, value] : signed_request->headers) request.set(name, value);
      }
      const auto request_start = Clock::now();
      if (persistent) result.request_start_monotonic_ns = monotonic_ns(request_start);
      const auto request_failure = [&]() {
        if (persistent) result.request_failure_ns = elapsed_ns(request_start);
        invalidate();
        return finish();
      };
      boost::system::error_code operation_error;
      bool complete = false;
      result.stage = "http_write";
      beast::get_lowest_layer(*stream_).expires_at(deadline);
      http::async_write(*stream_, request,
          [&](const boost::system::error_code& error, std::size_t) {
            operation_error = error; complete = true;
          });
      context_.run(); context_.restart();
      if (!complete || operation_error) {
        result.error = timed_out(operation_error) ? "http_write_timeout" : operation_error.message();
        return request_failure();
      }
      http::response_parser<http::dynamic_body> parser;
      parser.header_limit(kMaxHttpHeadBytes);
      parser.body_limit(kMaxHttpBodyBytes);
      operation_error.clear(); complete = false;
      result.stage = "http_header_read";
      beast::get_lowest_layer(*stream_).expires_at(deadline);
      http::async_read_header(*stream_, buffer_, parser,
          [&](const boost::system::error_code& error, std::size_t) {
            operation_error = error; complete = true;
          });
      context_.run(); context_.restart();
      if (!complete || operation_error) {
        result.error = operation_error == http::error::body_limit ? "response_capacity_exceeded"
            : timed_out(operation_error) ? "http_header_read_timeout" : operation_error.message();
        return request_failure();
      }
      operation_error.clear(); complete = false;
      result.stage = "http_body_read";
      Clock::time_point receipt{};
      beast::get_lowest_layer(*stream_).expires_at(deadline);
      http::async_read(*stream_, buffer_, parser,
          [&](const boost::system::error_code& error, std::size_t) {
            if (!error) receipt = Clock::now();
            operation_error = error; complete = true;
          });
      context_.run(); context_.restart();
      if (!complete || operation_error) {
        result.error = operation_error == http::error::body_limit ? "response_capacity_exceeded"
            : timed_out(operation_error) ? "http_body_read_timeout" : operation_error.message();
        return request_failure();
      }
      // Complete response receipt: stop the diagnostic timer before body copies
      // and JSON parsing, independently of logical/schema success.
      if (persistent) {
        result.response_receipt_monotonic_ns = monotonic_ns(receipt);
        result.request_response_ns = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(receipt - request_start).count());
      }
      const auto response = parser.release();
      result.status = response.result_int();
      if (persistent) {
        for (const auto field : {http::field::date, http::field::server,
                                 http::field::content_type, http::field::content_encoding,
                                 http::field::connection, http::field::content_length,
                                 http::field::retry_after}) {
          const auto value = response[field];
          if (!value.empty()) {
            const auto name = http::to_string(field);
            result.public_response_headers.emplace(
                std::string{name.data(), name.size()},
                std::string{value.data(), value.size()});
          }
        }
      }
      const auto body = beast::buffers_to_string(response.body().data());
      result.body_bytes = body.size();
      if (persistent) result.raw_body = body;
      ++completed_requests_;
      if (!response.keep_alive()) invalidate();
      result.stage = "json_parse";
      boost::system::error_code json_error;
      result.json = boost::json::parse(body, json_error);
      if (json_error) {
        result.error = "invalid_json:" + json_error.message();
        return finish();
      }
      result.json_present = true;
      result.stage = "complete";
      return finish();
    } catch (const std::exception& error) {
      result.error = std::string{"exception:"} + error.what();
      invalidate();
      return finish();
    }
  }

 private:
  void close_socket() noexcept {
    if (!stream_) return;
    boost::system::error_code ignored;
    auto& socket = beast::get_lowest_layer(*stream_).socket();
    socket.cancel(ignored);
    socket.shutdown(asio::ip::tcp::socket::shutdown_both, ignored);
    socket.close(ignored);
    connected_ = false;
  }
  void invalidate() noexcept { invalid_ = true; close_socket(); }
  asio::io_context context_;
  std::unique_ptr<ssl::context> tls_context_;
  std::unique_ptr<net_detail::TlsStream> stream_;
  beast::flat_buffer buffer_;
  bool connected_{false};
  bool invalid_{false};
  unsigned completed_requests_{0};
};
}  // namespace

struct PublicHttpConnection::Impl {
  explicit Impl(const RestCase& value) : probe_case(value) {}
  RestCase probe_case;
  HttpSession session;
};

PublicHttpConnection::PublicHttpConnection(const RestCase& probe_case)
    : impl_(std::make_unique<Impl>(probe_case)) {}
PublicHttpConnection::~PublicHttpConnection() = default;

HttpResult PublicHttpConnection::get(Clock::time_point deadline) {
  const auto& request = impl_->probe_case;
  if (request.private_case || request.auth != AuthKind::None) {
    HttpResult result{};
    result.stage = "configuration";
    result.error = "public_connection_requires_public_unsigned_case";
    return result;
  }
  if (request.host.empty() || request.host.size() > 253U ||
      !std::all_of(request.host.begin(), request.host.end(), [](unsigned char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
               (c >= '0' && c <= '9') || c == '.' || c == '-';
      }) || request.path.empty() || request.path.front() != '/' ||
      request.path.size() > 8192U || request.path.find_first_of("\r\n") != std::string::npos) {
    HttpResult result{};
    result.stage = "configuration";
    result.error = "public_connection_authority_or_path_invalid";
    return result;
  }
  return impl_->session.request(request, deadline, std::nullopt, std::nullopt, true);
}

HttpResult execute_http(const RestCase& probe_case, Clock::time_point deadline,
                        const std::optional<SignedRequest>& signed_request,
                        const std::optional<std::string>& pinned_ip) {
  HttpSession session;
  return session.request(probe_case, deadline, signed_request, pinned_ip, false);
}
}  // namespace exchange_probe
