#pragma once

#include "exchange_probe/race/ingress.hpp"
#include "exchange_probe/race/session.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>

namespace exchange_probe::race {

inline constexpr std::size_t kMaximumNormalizedFrameRecords = 256u;
inline constexpr std::size_t kRawSampleCapacity = 64u * 1024u;
inline constexpr std::size_t kRawSamplesPerConnection = 8u;
inline constexpr std::size_t kMaximumConnectionGenerations = 8u;

struct FrameView {
  const std::uint8_t* data{nullptr};
  std::size_t size{0u};
  std::uint64_t recvMonoNs{0u};
  std::uint64_t firstReadTsc{0u};
  bool binary{false};
};

struct NormalizeBatch {
  std::array<RaceRecord, kMaximumNormalizedFrameRecords> records{};
  std::size_t count{0u};
  FeedStatus status{FeedStatus::Synchronizing};
  bool malformed{false};
};

using NormalizerFn = bool (*)(
    void* state, const FrameView& frame, NormalizeBatch& output) noexcept;
using NormalizerResetFn = void (*)(void* state) noexcept;

struct ConnectionSpec {
  SourceIdentity source{};
  std::string feedId;
  std::string host;
  std::string port{"443"};
  std::string path{"/"};
  std::string subscribe;
  std::size_t maximumFrameBytes{2u * 1024u * 1024u};
  unsigned logicalCpu{0u};
  unsigned maximumReconnectAttempts{3u};
  bool subscribeBinary{false};
};

struct RawSample {
  std::uint64_t recvMonoNs{0u};
  std::uint32_t originalSize{0u};
  std::uint32_t storedSize{0u};
  bool binary{false};
  bool malformed{false};
  std::array<std::uint8_t, kRawSampleCapacity> bytes{};
};

class RawSampleStore {
 public:
  [[nodiscard]] bool capture(
      const FrameView& frame, bool malformed) noexcept;
  [[nodiscard]] std::size_t count() const noexcept { return count_; }
  [[nodiscard]] const RawSample& sample(std::size_t index) const noexcept {
    return samples_[index];
  }
  [[nodiscard]] std::uint64_t omitted() const noexcept { return omitted_; }

 private:
  std::array<RawSample, kRawSamplesPerConnection> samples_{};
  std::size_t count_{0u};
  std::uint64_t omitted_{0u};
};

struct ConnectionCounters {
  std::atomic<std::uint64_t> frames{0u};
  std::atomic<std::uint64_t> bytes{0u};
  std::atomic<std::uint64_t> malformed{0u};
  std::atomic<std::uint64_t> normalized{0u};
  std::atomic<std::uint64_t> reconnects{0u};
  std::atomic<std::uint32_t> generation{0u};
  std::atomic<std::uint8_t> status{
      static_cast<std::uint8_t>(FeedStatus::Planned)};
};

struct ConnectionGenerationObservation {
  std::uint32_t generation{0u};
  FeedStatus finalStatus{FeedStatus::Planned};
  std::string resolvedIp;
  std::string remoteIp;
  std::string localIp;
  std::string ipFamily;
  std::string tlsVersion;
  std::string tlsCipher;
  std::string failureStage;
  std::string failureReason;
  std::uint64_t dnsNs{0u};
  std::uint64_t tcpConnectNs{0u};
  std::uint64_t tlsHandshakeNs{0u};
  std::uint64_t wsHandshakeNs{0u};
};

struct ConnectionObservation {
  std::array<ConnectionGenerationObservation,
             kMaximumConnectionGenerations> generations{};
  std::size_t generationCount{0u};
  bool generationOverflow{false};
};

struct SessionControl {
  std::atomic<std::uint64_t> measuredStartMonoNs{0u};
  std::atomic<std::uint64_t> measuredEndMonoNs{0u};
  std::atomic<bool> stop{false};
};

struct ConnectionRuntime {
  const ConnectionSpec* spec{nullptr};
  ConnectionIngress* ingress{nullptr};
  ReadyBarrier* ready{nullptr};
  std::size_t readyIndex{0u};
  SessionControl* control{nullptr};
  NormalizerFn normalizer{nullptr};
  NormalizerResetFn resetNormalizer{nullptr};
  void* normalizerState{nullptr};
  RawSampleStore* samples{nullptr};
  ConnectionCounters* counters{nullptr};
  ConnectionObservation* observation{nullptr};
};

[[nodiscard]] ConnectionGenerationObservation* begin_generation_observation(
    ConnectionObservation& observation, std::uint32_t generation) noexcept;

void stamp_record_arrival(
    RaceRecord& record, const ConnectionSpec& spec,
    std::uint32_t generation, std::uint64_t recvMonoNs) noexcept;

void capture_connection(ConnectionRuntime& runtime) noexcept;

}  // namespace exchange_probe::race
