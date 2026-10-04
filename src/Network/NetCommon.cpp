#include "NetCommon.hpp"

#include "exchange_probe/Net.hpp"

#include <boost/asio/bind_executor.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/ssl/host_name_verification.hpp>
#include <boost/json/string.hpp>

#include <openssl/ssl.h>
#include <openssl/x509.h>

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

#if defined(__linux__)
#include <sys/socket.h>
#endif

namespace exchange_probe {
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

bool connect_endpoint(
    asio::io_context& context,
    beast::tcp_stream& stream,
    const tcp::endpoint& endpoint,
    std::chrono::steady_clock::time_point deadline,
    std::string& error) {
  const auto endpoints = tcp::resolver::results_type::create(endpoint, {}, {});
  return connect(context, stream, endpoints, deadline, error);
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

TransportMetadata transport_metadata(TlsStream& stream) noexcept {
  TransportMetadata result;
  try {
    boost::system::error_code endpoint_error;
    const auto endpoint =
        beast::get_lowest_layer(stream).socket().remote_endpoint(endpoint_error);
    if (!endpoint_error) {
      result.remote_ip = endpoint.address().to_string();
      result.ip_family = endpoint.address().is_v6() ? "ipv6" : "ipv4";
    }

    SSL* tls = stream.native_handle();
    if (tls != nullptr) {
      if (const char* version = SSL_get_version(tls); version != nullptr) {
        result.tls_version = version;
      }
      if (const char* cipher = SSL_get_cipher_name(tls); cipher != nullptr) {
        result.tls_cipher = cipher;
      }
      const unsigned char* alpn = nullptr;
      unsigned alpn_size = 0U;
      SSL_get0_alpn_selected(tls, &alpn, &alpn_size);
      if (alpn != nullptr && alpn_size != 0U) {
        result.alpn.assign(
            reinterpret_cast<const char*>(alpn),
            static_cast<std::size_t>(alpn_size));
      }
      result.tls_session_reused = SSL_session_reused(tls) == 1;
      X509* certificate = SSL_get1_peer_certificate(tls);
      if (certificate != nullptr) {
        BIO* memory = BIO_new(BIO_s_mem());
        if (memory != nullptr &&
            ASN1_TIME_print(memory, X509_get0_notAfter(certificate)) == 1) {
          char* data = nullptr;
          const auto size = BIO_get_mem_data(memory, &data);
          if (data != nullptr && size > 0) {
            result.certificate_not_after.assign(
                data, static_cast<std::size_t>(size));
          }
        }
        if (memory != nullptr) {
          BIO_free(memory);
        }
        X509_free(certificate);
      }
    }

  } catch (...) {
    return result;
  }
  return result;
}

void refresh_transport_metadata(
    TransportMetadata& destination,
    TlsStream& stream) noexcept {
  auto latest = transport_metadata(stream);
  if (!latest.remote_ip.empty()) {
    destination.remote_ip = std::move(latest.remote_ip);
    destination.ip_family = std::move(latest.ip_family);
  }
  if (!latest.tls_version.empty()) {
    destination.tls_version = std::move(latest.tls_version);
  }
  if (!latest.tls_cipher.empty()) {
    destination.tls_cipher = std::move(latest.tls_cipher);
  }
  if (!latest.alpn.empty()) {
    destination.alpn = std::move(latest.alpn);
  }
  if (!latest.certificate_not_after.empty()) {
    destination.certificate_not_after =
        std::move(latest.certificate_not_after);
  }
  destination.tls_session_reused =
      destination.tls_session_reused || latest.tls_session_reused;

}

}  // namespace net_detail
}  // namespace exchange_probe
