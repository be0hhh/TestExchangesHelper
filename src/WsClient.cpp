#include "exchange_probe/Net.hpp"

#include "exchange_probe/Contracts.hpp"
#include "NetCommon.hpp"

#include <boost/asio/ip/address.hpp>
#include <boost/asio/ssl/context.hpp>
#include <boost/beast/core/buffers_to_string.hpp>
#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/websocket/ssl.hpp>
#include <boost/json/object.hpp>
#include <boost/json/parse.hpp>
#include <boost/json/serialize.hpp>
#include <boost/system/error_code.hpp>

#include <chrono>
#include <ctime>
#include <string>
#include <utility>

namespace exchange_probe {
namespace {

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace ssl = asio::ssl;
namespace websocket = beast::websocket;

using WebSocket = websocket::stream<net_detail::TlsStream>;

[[nodiscard]] bool timed_out(
    const boost::system::error_code& error) noexcept {
  return error == beast::error::timeout ||
         error == asio::error::timed_out;
}

[[nodiscard]] std::chrono::seconds remaining_seconds(
    std::chrono::steady_clock::time_point deadline) {
  const auto remaining = deadline - std::chrono::steady_clock::now();
  const auto rounded =
      std::chrono::duration_cast<std::chrono::seconds>(remaining);
  return std::max(std::chrono::seconds{1}, rounded);
}

void configure_ws_timeout(
    WebSocket& stream,
    std::chrono::steady_clock::time_point deadline) {
  websocket::stream_base::timeout timeout{
      .handshake_timeout = remaining_seconds(deadline),
      .idle_timeout = remaining_seconds(deadline),
      .keep_alive_pings = false,
  };
  stream.set_option(timeout);
}

[[nodiscard]] std::string subscription_payload(const WsCase& probe_case) {
  if (probe_case.subscribe.empty()) {
    return {};
  }
  boost::system::error_code error;
  auto value = boost::json::parse(probe_case.subscribe, error);
  if (error || !value.is_object()) {
    return probe_case.subscribe;
  }
  auto& object = value.as_object();
  if (object.contains("time")) {
    object["time"] = static_cast<std::int64_t>(std::time(nullptr));
  }
  return boost::json::serialize(value);
}

[[nodiscard]] bool json_message(
    std::string_view payload,
    boost::json::value& value) {
  boost::system::error_code error;
  value = boost::json::parse(payload, error);
  return !error;
}

[[nodiscard]] bool valid_kucoin_welcome(
    const boost::json::value& value,
    bool binary) {
  if (!binary || !value.is_object()) {
    return false;
  }
  const auto& object = value.as_object();
  const auto message = object.find("message");
  const auto interval = object.find("pingInterval");
  return message != object.end() &&
         message->value().is_string() &&
         message->value().as_string() == "welcome" &&
         interval != object.end() &&
         ((interval->value().is_int64() &&
           interval->value().as_int64() > 0) ||
          (interval->value().is_uint64() &&
           interval->value().as_uint64() > 0));
}

[[nodiscard]] std::string application_pong(
    const WsCase& probe_case,
    const boost::json::value& value) {
  if (probe_case.application_heartbeat != "htx" || !value.is_object()) {
    return {};
  }
  const auto ping = value.as_object().find("ping");
  if (ping == value.as_object().end()) {
    return {};
  }
  return boost::json::serialize(
      boost::json::object{{"pong", ping->value()}});
}

[[nodiscard]] bool async_write_message(
    asio::io_context& context,
    WebSocket& stream,
    std::string_view payload,
    bool binary,
    std::chrono::steady_clock::time_point deadline,
    std::string& error) {
  boost::system::error_code operation_error;
  bool complete = false;
  configure_ws_timeout(stream, deadline);
  stream.binary(binary);
  stream.async_write(
      asio::buffer(payload),
      [&](const boost::system::error_code& ec, std::size_t) {
        operation_error = ec;
        complete = true;
      });
  context.run();
  context.restart();
  if (!complete || operation_error) {
    error = timed_out(operation_error)
                ? "ws_write_timeout"
                : operation_error.message();
    return false;
  }
  return true;
}

struct ReadMessage {
  bool ok{false};
  bool binary{false};
  std::string payload;
  std::string error;
};

[[nodiscard]] ReadMessage async_read_message(
    asio::io_context& context,
    WebSocket& stream,
    std::chrono::steady_clock::time_point deadline) {
  ReadMessage result;
  beast::flat_buffer buffer{kMaxWsMessageBytes};
  boost::system::error_code operation_error;
  bool complete = false;
  configure_ws_timeout(stream, deadline);
  stream.async_read(
      buffer,
      [&](const boost::system::error_code& ec, std::size_t) {
        operation_error = ec;
        complete = true;
      });
  context.run();
  context.restart();
  if (!complete || operation_error) {
    result.error =
        operation_error == websocket::error::message_too_big ||
                operation_error == asio::error::no_buffer_space
            ? "ws_message_capacity_exceeded"
            : timed_out(operation_error)
                  ? "ws_read_timeout"
                  : operation_error.message();
    return result;
  }
  result.binary = stream.got_binary();
  result.payload = beast::buffers_to_string(buffer.data());
  result.ok = true;
  return result;
}

void close_socket(WebSocket& stream) noexcept {
  boost::system::error_code ignored;
  beast::get_lowest_layer(stream).socket().shutdown(
      asio::ip::tcp::socket::shutdown_both,
      ignored);
  beast::get_lowest_layer(stream).socket().close(ignored);
}

}  // namespace

WsResult observe_ws(
    const WsCase& probe_case,
    std::chrono::steady_clock::time_point deadline,
    const std::optional<std::string>& pinned_ip,
    const std::optional<bool>& use_proxy) {
  const auto start = std::chrono::steady_clock::now();
  WsResult result{};
  const auto finish = [&]() {
    result.timings.total_us = net_detail::elapsed_us(start);
    result.elapsed_ms = result.timings.total_us / 1000U;
    return std::move(result);
  };
  result.protocol_stage = "configuration";
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
      result.protocol_stage = "proxy_configuration";
      result.error = "proxy_not_configured";
      result.elapsed_ms = net_detail::elapsed_ms(start);
      return finish();
    }
    if (proxy.enabled && !proxy.valid) {
      result.protocol_stage = "proxy_configuration";
      result.error = proxy.error;
      result.elapsed_ms = net_detail::elapsed_ms(start);
      return finish();
    }
    if (proxy.enabled && pinned_ip.has_value()) {
      result.protocol_stage = "route_configuration";
      result.error = "pinned_route_unavailable_through_proxy";
      result.elapsed_ms = net_detail::elapsed_ms(start);
      return finish();
    }
    const auto& connect_host = proxy.enabled ? proxy.host : probe_case.host;
    const auto& connect_port = proxy.enabled ? proxy.port : std::string{"443"};
    result.protocol_stage = "dns";
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

    WebSocket stream{context, tls_context};
    stream.read_message_max(kMaxWsMessageBytes);
    result.protocol_stage =
        proxy.enabled ? "proxy_tcp_connect" : "tcp_connect";
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
      result.protocol_stage = "proxy_connect";
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
        close_socket(stream);
        return finish();
      }
      result.timings.proxy_connect_us = net_detail::elapsed_us(stage_start);
    }

    result.protocol_stage = "tls_configuration";
    if (!net_detail::configure_tls(
            stream.next_layer(),
            probe_case.host,
            result.error)) {
      result.elapsed_ms = net_detail::elapsed_ms(start);
      close_socket(stream);
      return finish();
    }
    result.protocol_stage = "tls_handshake";
    stage_start = std::chrono::steady_clock::now();
    if (!net_detail::tls_handshake(
            context,
            stream.next_layer(),
            deadline,
            result.error)) {
      result.elapsed_ms = net_detail::elapsed_ms(start);
      close_socket(stream);
      return finish();
    }
    result.timings.tls_handshake_us = net_detail::elapsed_us(stage_start);
    result.tls_verified = true;
    net_detail::refresh_transport_metadata(
        result.transport, stream.next_layer());

    unsigned control_pings = 0;
    stream.control_callback(
        [&](websocket::frame_type kind, beast::string_view) {
          if (kind == websocket::frame_type::ping) {
            ++control_pings;
          }
        });
    stream.set_option(websocket::stream_base::decorator(
        [&](websocket::request_type& request) {
          request.set(
              boost::beast::http::field::user_agent,
              "exchange-api-probe/3");
          for (const auto& [name, value] : probe_case.handshake_headers) {
            request.set(name, value);
          }
        }));
    configure_ws_timeout(stream, deadline);
    beast::get_lowest_layer(stream).expires_never();
    boost::system::error_code handshake_error;
    bool handshake_complete = false;
    result.protocol_stage = "ws_handshake";
    stage_start = std::chrono::steady_clock::now();
    stream.async_handshake(
        probe_case.host,
        probe_case.path,
        [&](const boost::system::error_code& error) {
          handshake_error = error;
          handshake_complete = true;
        });
    context.run();
    context.restart();
    if (!handshake_complete || handshake_error) {
      result.error = timed_out(handshake_error)
                         ? "ws_handshake_timeout"
                         : handshake_error.message();
      result.elapsed_ms = net_detail::elapsed_ms(start);
      close_socket(stream);
      return finish();
    }
    result.timings.ws_handshake_us = net_detail::elapsed_us(stage_start);
    result.connected = true;

    if (probe_case.read_welcome) {
      result.protocol_stage = "welcome";
      stage_start = std::chrono::steady_clock::now();
      auto message = async_read_message(context, stream, deadline);
      if (!message.ok) {
        result.error = message.error;
        result.elapsed_ms = net_detail::elapsed_ms(start);
        result.control_pings = control_pings;
        close_socket(stream);
        return finish();
      }
      result.timings.welcome_us = net_detail::elapsed_us(stage_start);
      boost::json::value welcome;
      if (!json_message(message.payload, welcome) ||
          !valid_kucoin_welcome(welcome, message.binary)) {
        result.error = "welcome_contract_mismatch";
        result.elapsed_ms = net_detail::elapsed_ms(start);
        result.control_pings = control_pings;
        close_socket(stream);
        return finish();
      }
    }

    const auto subscribe = subscription_payload(probe_case);
    if (!subscribe.empty()) {
      result.protocol_stage = "subscribe_write";
      stage_start = std::chrono::steady_clock::now();
      if (!async_write_message(
              context,
              stream,
              subscribe,
              probe_case.subscribe_binary,
              deadline,
              result.error)) {
        result.elapsed_ms = net_detail::elapsed_ms(start);
        result.control_pings = control_pings;
        close_socket(stream);
        return finish();
      }
      result.timings.subscribe_write_us = net_detail::elapsed_us(stage_start);
    }

    bool ack_complete = !probe_case.require_ack;
    const auto application_start = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() < deadline) {
      result.protocol_stage = ack_complete ? "data_read" : "ack_read";
      auto message = async_read_message(context, stream, deadline);
      if (!message.ok) {
        result.error = message.error;
        break;
      }
      std::string application_payload = std::move(message.payload);
      if (probe_case.compression == Compression::Gzip) {
        std::string decoded;
        std::string decode_error;
        if (!decode_gzip_bounded(
                application_payload,
                kMaxWsMessageBytes,
                decoded,
                decode_error)) {
          result.error = decode_error;
          result.protocol_stage = "decompression";
          break;
        }
        application_payload = std::move(decoded);
      }

      boost::json::value json;
      const bool json_present = json_message(application_payload, json);
      const auto pong =
          json_present ? application_pong(probe_case, json) : std::string{};
      if (!pong.empty()) {
        result.protocol_stage = "application_pong";
        if (!async_write_message(
                context,
                stream,
                pong,
                false,
                deadline,
                result.error)) {
          break;
        }
        continue;
      }

      const auto ack = validate_ws_ack(
          probe_case,
          json,
          json_present,
          message.binary);
      const auto data = validate_ws_data(
          probe_case,
          json,
          json_present,
          message.binary,
          application_payload);

      if (!ack_complete && ack.matched) {
        ack_complete = true;
        if (result.timings.ack_us == 0U) {
          result.timings.ack_us = net_detail::elapsed_us(application_start);
        }
        result.protocol_stage = "ack";
        if (probe_case.ack_implies_data && data.matched) {
          result.data_complete = true;
          result.timings.first_data_us =
              net_detail::elapsed_us(application_start);
        }
      } else if (!ack_complete && probe_case.data_implies_ack && data.matched) {
        ack_complete = true;
        result.data_complete = true;
        result.timings.ack_us = net_detail::elapsed_us(application_start);
        result.timings.first_data_us = result.timings.ack_us;
        result.protocol_stage = "data_implies_ack";
      } else if (ack_complete && data.matched) {
        result.data_complete = true;
        if (result.timings.first_data_us == 0U) {
          result.timings.first_data_us =
              net_detail::elapsed_us(application_start);
        }
        result.protocol_stage = "data";
      }

      result.ack_complete = ack_complete;
      result.binary = message.binary;
      result.payload = std::move(application_payload);
      result.payload_bytes = result.payload.size();
      result.json = std::move(json);
      result.json_present = json_present;
      if (result.ack_complete &&
          (!probe_case.require_data || result.data_complete)) {
        result.error.clear();
        break;
      }
    }

    result.ack_complete = ack_complete;
    result.control_pings = control_pings;
    if (result.error.empty() &&
        (!result.ack_complete ||
         (probe_case.require_data && !result.data_complete))) {
      result.error =
          result.ack_complete ? "data_after_ack_timeout" : "ack_timeout";
    }
    net_detail::refresh_transport_metadata(
        result.transport, stream.next_layer());
    result.timings.total_us = net_detail::elapsed_us(start);
    result.elapsed_ms = result.timings.total_us / 1000U;
    close_socket(stream);
    return finish();
  } catch (const std::exception& error) {
    result.error = std::string{"exception:"} + error.what();
    result.elapsed_ms = net_detail::elapsed_ms(start);
    return finish();
  }
}

}  // namespace exchange_probe
