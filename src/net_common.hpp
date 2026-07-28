#pragma once

#include "exchange_probe/model.hpp"

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl/context.hpp>
#include <boost/asio/ssl/stream.hpp>
#include <boost/beast/core/tcp_stream.hpp>
#include <boost/system/error_code.hpp>

#include <chrono>
#include <string>
#include <string_view>

namespace exchange_probe::net_detail {

namespace asio = boost::asio;
namespace ssl = asio::ssl;
namespace beast = boost::beast;
using tcp = asio::ip::tcp;
using TlsStream = ssl::stream<beast::tcp_stream>;

struct ResolveResult {
  tcp::resolver::results_type endpoints;
  bool ok{false};
  bool timed_out{false};
  std::string error;
};

[[nodiscard]] ResolveResult resolve(
    asio::io_context& context,
    std::string_view host,
    std::string_view port,
    std::chrono::steady_clock::time_point deadline);

[[nodiscard]] bool connect(
    asio::io_context& context,
    beast::tcp_stream& stream,
    const tcp::resolver::results_type& endpoints,
    std::chrono::steady_clock::time_point deadline,
    std::string& error);

[[nodiscard]] bool connect_endpoint(
    asio::io_context& context,
    beast::tcp_stream& stream,
    const tcp::endpoint& endpoint,
    std::chrono::steady_clock::time_point deadline,
    std::string& error);

[[nodiscard]] bool establish_proxy_tunnel(
    asio::io_context& context,
    beast::tcp_stream& stream,
    const ProxyConfig& proxy,
    std::string_view destination_host,
    std::string_view destination_port,
    std::chrono::steady_clock::time_point deadline,
    std::string& error);

[[nodiscard]] bool configure_tls(
    TlsStream& stream,
    std::string_view host,
    std::string& error);

[[nodiscard]] bool tls_handshake(
    asio::io_context& context,
    TlsStream& stream,
    std::chrono::steady_clock::time_point deadline,
    std::string& error);

void close_tls(TlsStream& stream) noexcept;

[[nodiscard]] std::uint64_t elapsed_ms(
    std::chrono::steady_clock::time_point start) noexcept;

[[nodiscard]] std::uint64_t elapsed_us(
    std::chrono::steady_clock::time_point start) noexcept;

[[nodiscard]] TransportMetadata transport_metadata(
    TlsStream& stream) noexcept;

void refresh_transport_metadata(
    TransportMetadata& destination,
    TlsStream& stream) noexcept;

}  // namespace exchange_probe::net_detail
