#include "exchange_probe/race/Transport.hpp"

#include "exchange_probe/race/Clock.hpp"

#include "NetCommon.hpp"

#include "cxet/Os/ThreadAffinity.hpp"

#include <boost/asio/buffer.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ssl/context.hpp>
#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/http/field.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/websocket/ssl.hpp>
#include <boost/system/error_code.hpp>

#include <algorithm>
#include <chrono>
#include <cstring>

namespace exchange_probe::race {
namespace {

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace ssl = asio::ssl;
namespace websocket = beast::websocket;
using WebSocket = websocket::stream<net_detail::TlsStream>;

enum class GenerationOutcome : std::uint8_t {
  Stopped = 0u,
  RetryableFailure = 1u,
  PermanentTerminal = 2u,
};

GenerationOutcome fail_generation(
    ConnectionRuntime& runtime, ConnectionGenerationObservation& observation,
    FeedStatus status,
    const char* stage, const std::string& reason) noexcept {
  runtime.counters->status.store(
      static_cast<std::uint8_t>(status), std::memory_order_release);
  observation.finalStatus = status;
  observation.failureStage = stage;
  observation.failureReason = reason;
  return GenerationOutcome::RetryableFailure;
}

[[nodiscard]] std::uint64_t elapsed_ns(std::uint64_t start) noexcept {
  const auto now = now_mono_raw_ns();
  return now >= start ? now - start : 0u;
}

[[nodiscard]] bool measured_frame(
    const SessionControl& control, std::uint64_t recvMonoNs) noexcept {
  const auto start =
      control.measuredStartMonoNs.load(std::memory_order_acquire);
  const auto end = control.measuredEndMonoNs.load(std::memory_order_acquire);
  return start != 0u && recvMonoNs >= start && (end == 0u || recvMonoNs < end);
}

}  // namespace

ConnectionGenerationObservation* begin_generation_observation(
    ConnectionObservation& observation, std::uint32_t generation) noexcept {
  if (observation.generationCount >= observation.generations.size()) {
    observation.generationOverflow = true;
    return nullptr;
  }
  auto& output = observation.generations[observation.generationCount++];
  output = ConnectionGenerationObservation{};
  output.generation = generation;
  return &output;
}

void stamp_record_arrival(
    RaceRecord& record, const ConnectionSpec& spec,
    std::uint32_t generation, std::uint64_t recvMonoNs) noexcept {
  const auto origin = record.source.origin;
  record.source = spec.source;
  record.source.origin = origin;
  record.source.connectionGeneration = generation;
  record.timestamps.recvMonoNs = recvMonoNs;
}

bool RawSampleStore::capture(
    const FrameView& frame, bool malformed) noexcept {
  const bool initialSample = count_ < 4u;
  const bool malformedSlot = malformed && count_ < samples_.size();
  if (!initialSample && !malformedSlot) {
    ++omitted_;
    return false;
  }
  auto& output = samples_[count_++];
  output.recvMonoNs = frame.recvMonoNs;
  output.originalSize = static_cast<std::uint32_t>(std::min<std::size_t>(
      frame.size, static_cast<std::size_t>(UINT32_MAX)));
  output.storedSize = static_cast<std::uint32_t>(
      std::min(frame.size, output.bytes.size()));
  output.binary = frame.binary;
  output.malformed = malformed;
  if (output.storedSize != 0u && frame.data != nullptr) {
    std::memcpy(output.bytes.data(), frame.data, output.storedSize);
  }
  return true;
}

GenerationOutcome capture_generation(
    ConnectionRuntime& runtime, ConnectionGenerationObservation& observation,
    std::uint32_t generation) noexcept {
  runtime.counters->generation.store(generation, std::memory_order_release);
  runtime.counters->status.store(
      static_cast<std::uint8_t>(FeedStatus::Connecting),
      std::memory_order_release);
  observation.finalStatus = FeedStatus::Connecting;
  if (runtime.resetNormalizer != nullptr) {
    runtime.resetNormalizer(runtime.normalizerState);
  }
  try {
    asio::io_context context;
    ssl::context tls{ssl::context::tls_client};
    boost::system::error_code error;
    tls.set_default_verify_paths(error);
    if (error) {
      return fail_generation(
          runtime, observation, FeedStatus::Failed, "tls_configuration",
          "default_ca_paths_failed:" + error.message());
    }
    tls.set_options(
        ssl::context::default_workarounds | ssl::context::no_sslv2 |
        ssl::context::no_sslv3);
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds{30};
    const auto dnsStart = now_mono_raw_ns();
    auto resolved = net_detail::resolve(
        context, runtime.spec->host, runtime.spec->port, deadline);
    observation.dnsNs = elapsed_ns(dnsStart);
    if (!resolved.ok) {
      return fail_generation(
          runtime, observation, FeedStatus::Failed, "dns", resolved.error);
    }
    if (!resolved.endpoints.empty()) {
      observation.resolvedIp =
          resolved.endpoints.begin()->endpoint().address().to_string();
    }
    WebSocket stream{context, tls};
    stream.read_message_max(runtime.spec->maximumFrameBytes);
    std::string transportError;
    const auto tcpStart = now_mono_raw_ns();
    if (!net_detail::connect(
            context, beast::get_lowest_layer(stream), resolved.endpoints,
            deadline, transportError)) {
      observation.tcpConnectNs = elapsed_ns(tcpStart);
      return fail_generation(
          runtime, observation, FeedStatus::Failed, "tcp_connect",
          transportError);
    }
    observation.tcpConnectNs = elapsed_ns(tcpStart);
    if (!net_detail::configure_tls(
            stream.next_layer(), runtime.spec->host, transportError)) {
      return fail_generation(
          runtime, observation, FeedStatus::Failed, "tls_configuration",
          transportError);
    }
    const auto tlsStart = now_mono_raw_ns();
    if (!net_detail::tls_handshake(
            context, stream.next_layer(), deadline, transportError)) {
      observation.tlsHandshakeNs = elapsed_ns(tlsStart);
      return fail_generation(
          runtime, observation, FeedStatus::Failed, "tls_handshake",
          transportError);
    }
    observation.tlsHandshakeNs = elapsed_ns(tlsStart);
    const auto metadata = net_detail::transport_metadata(stream.next_layer());
    observation.remoteIp = metadata.remote_ip;
    observation.ipFamily = metadata.ip_family;
    observation.tlsVersion = metadata.tls_version;
    observation.tlsCipher = metadata.tls_cipher;
    boost::system::error_code endpointError;
    const auto localEndpoint =
        beast::get_lowest_layer(stream).socket().local_endpoint(endpointError);
    if (!endpointError) {
      observation.localIp = localEndpoint.address().to_string();
    }

    websocket::stream_base::timeout timeout{
        .handshake_timeout = std::chrono::seconds{30},
        .idle_timeout = std::chrono::seconds{30},
        .keep_alive_pings = true,
    };
    stream.set_option(timeout);
    stream.set_option(websocket::stream_base::decorator(
        [](websocket::request_type& request) {
          request.set(
              boost::beast::http::field::user_agent,
              "exchange-feed-race/1");
        }));
    const auto wsStart = now_mono_raw_ns();
    stream.handshake(runtime.spec->host, runtime.spec->path, error);
    observation.wsHandshakeNs = elapsed_ns(wsStart);
    if (error) {
      return fail_generation(
          runtime, observation, FeedStatus::Failed, "ws_handshake",
          error.message());
    }
    runtime.counters->status.store(
        static_cast<std::uint8_t>(FeedStatus::Subscribing),
        std::memory_order_release);
    if (!runtime.spec->subscribe.empty()) {
      stream.binary(runtime.spec->subscribeBinary);
      stream.write(asio::buffer(runtime.spec->subscribe), error);
      if (error) {
        return fail_generation(
            runtime, observation, FeedStatus::Failed, "subscribe_write",
            error.message());
      }
    }
    beast::flat_buffer buffer{runtime.spec->maximumFrameBytes};
    buffer.reserve(runtime.spec->maximumFrameBytes);
    while (!runtime.control->stop.load(std::memory_order_acquire)) {
      buffer.consume(buffer.size());
      beast::get_lowest_layer(stream).expires_after(std::chrono::seconds{30});
      stream.read(buffer, error);
      const std::uint64_t recvMonoNs = now_mono_raw_ns();
      if (error) {
        if (!runtime.control->stop.load(std::memory_order_acquire)) {
          boost::system::error_code ignored;
          stream.close(websocket::close_code::normal, ignored);
          return fail_generation(
              runtime, observation, FeedStatus::Disconnected, "read",
              error.message());
        }
        break;
      }
      const auto message = buffer.data();
      FrameView frame{
          .data = static_cast<const std::uint8_t*>(message.data()),
          .size = message.size(),
          .recvMonoNs = recvMonoNs,
          .firstReadTsc = 0u,
          .binary = stream.got_binary(),
      };
      runtime.counters->frames.fetch_add(1u, std::memory_order_relaxed);
      runtime.counters->bytes.fetch_add(frame.size, std::memory_order_relaxed);
      NormalizeBatch batch{};
      const bool normalized =
          runtime.normalizer(runtime.normalizerState, frame, batch);
      if (!normalized || batch.malformed ||
          batch.count > batch.records.size()) {
        runtime.counters->malformed.fetch_add(1u, std::memory_order_relaxed);
        static_cast<void>(runtime.samples->capture(frame, true));
        continue;
      }
      static_cast<void>(runtime.samples->capture(frame, false));
      if (terminal_feed_status(batch.status)) {
        runtime.counters->status.store(
            static_cast<std::uint8_t>(batch.status),
            std::memory_order_release);
        static_cast<void>(runtime.ready->set_terminal(
            runtime.readyIndex, batch.status));
        observation.finalStatus = batch.status;
        if (batch.status != FeedStatus::Ready &&
            batch.status != FeedStatus::Degraded) {
          boost::system::error_code ignored;
          stream.close(websocket::close_code::normal, ignored);
          return GenerationOutcome::PermanentTerminal;
        }
      }
      if (!measured_frame(*runtime.control, recvMonoNs) ||
          batch.status != FeedStatus::Ready) {
        continue;
      }
      for (std::size_t index = 0u; index < batch.count; ++index) {
        auto& record = batch.records[index];
        stamp_record_arrival(record, *runtime.spec, generation, recvMonoNs);
        if (!runtime.ingress->publish(record)) {
          runtime.counters->status.store(
              static_cast<std::uint8_t>(FeedStatus::Degraded),
              std::memory_order_release);
          observation.finalStatus = FeedStatus::Degraded;
          break;
        }
        runtime.counters->normalized.fetch_add(1u, std::memory_order_relaxed);
      }
    }
    boost::system::error_code ignored;
    stream.close(websocket::close_code::normal, ignored);
    return GenerationOutcome::Stopped;
  } catch (const std::exception& exception) {
    return fail_generation(
        runtime, observation, FeedStatus::Failed, "exception",
        exception.what());
  } catch (...) {
    return fail_generation(
        runtime, observation, FeedStatus::Failed, "exception",
        "unknown_exception");
  }
}

void capture_connection(ConnectionRuntime& runtime) noexcept {
  if (runtime.spec == nullptr || runtime.ingress == nullptr ||
      runtime.ready == nullptr || runtime.control == nullptr ||
      runtime.samples == nullptr || runtime.counters == nullptr ||
      runtime.observation == nullptr || runtime.normalizer == nullptr) {
    if (runtime.ready != nullptr && runtime.counters != nullptr) {
      runtime.counters->status.store(
          static_cast<std::uint8_t>(FeedStatus::Failed),
          std::memory_order_release);
      static_cast<void>(runtime.ready->set_terminal(
          runtime.readyIndex, FeedStatus::Failed));
    }
    return;
  }
  static_cast<void>(cxet::os::setCurrentThreadAffinity(
      runtime.spec->logicalCpu));
  const std::uint32_t maximumGeneration =
      static_cast<std::uint32_t>(runtime.spec->maximumReconnectAttempts);
  for (std::uint32_t generation = 0u; generation <= maximumGeneration;
       ++generation) {
    if (runtime.control->stop.load(std::memory_order_acquire)) return;
    if (generation != 0u) {
      runtime.counters->reconnects.fetch_add(1u, std::memory_order_relaxed);
    }
    auto* observation = begin_generation_observation(
        *runtime.observation, generation);
    if (observation == nullptr) {
      runtime.counters->status.store(
          static_cast<std::uint8_t>(FeedStatus::Degraded),
          std::memory_order_release);
      static_cast<void>(runtime.ready->set_terminal(
          runtime.readyIndex, FeedStatus::Degraded));
      return;
    }
    const auto outcome = capture_generation(
        runtime, *observation, generation);
    if (outcome != GenerationOutcome::RetryableFailure) return;
  }
  runtime.counters->status.store(
      static_cast<std::uint8_t>(FeedStatus::Failed),
      std::memory_order_release);
  static_cast<void>(runtime.ready->set_terminal(
      runtime.readyIndex, FeedStatus::Failed));
}

}  // namespace exchange_probe::race
