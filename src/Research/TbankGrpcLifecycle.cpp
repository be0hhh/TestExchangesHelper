#include "exchange_probe/App.hpp"
#include "exchange_probe/TbankCredential.hpp"
#include "exchange_probe/TbankGrpcDiagnostic.hpp"
#include "TbankGrpcProtocol.hpp"
#include "TbankPingHandoff.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <limits>
#include <memory>
#include <ostream>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

#include <arpa/inet.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace exchange_probe {
namespace {
using namespace tbank_grpc;
constexpr std::string_view kHost = "invest-public-api.tbank.ru";
constexpr std::uint64_t kSecond = 1'000'000'000ULL;
constexpr std::uint64_t kMaxArtifact = 4U * 1024U * 1024U;

std::uint64_t now_ns() {
  const auto n = std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count();
  return n > 0 ? static_cast<std::uint64_t>(n) : 0;
}
std::uint64_t add(std::uint64_t a, std::uint64_t b) {
  return b > std::numeric_limits<std::uint64_t>::max() - a
      ? std::numeric_limits<std::uint64_t>::max() : a + b;
}
bool numeric_ip(const std::string& value) {
  if (value.find('\0') != std::string::npos) return false;
  std::array<unsigned char, 16> bytes{};
  return ::inet_pton(AF_INET, value.c_str(), bytes.data()) == 1 ||
         ::inet_pton(AF_INET6, value.c_str(), bytes.data()) == 1;
}
const char* event_name(EventKind kind) {
  switch (kind) {
    case EventKind::Ready: return "ready";
    case EventKind::PingSent: return "ping_sent";
    case EventKind::PingAck: return "ping_ack";
    case EventKind::Goaway: return "goaway";
    case EventKind::Headers: return "headers";
    case EventKind::Message: return "message";
    case EventKind::RpcComplete: return "rpc_complete";
    case EventKind::StreamReset: return "stream_reset";
    case EventKind::Eof: return "eof";
    case EventKind::Failure: return "failure";
  }
  return "unknown";
}
std::string_view safe_stage(std::string_view value) {
  static constexpr std::string_view stages[] = {
      "connect_input", "connect_deadline", "resolver_init", "initialize", "connect_prepare",
      "connect_stopped",
      "connect_tls", "connect", "peer_address", "begin_rpc", "poll", "payload",
      "complete_stream", "configuration", "pinned_ip", "tcp_socket", "tcp_connect",
      "tls_context", "tls_ca", "tls_configuration", "tls_deadline", "tls_handshake",
      "tls_verification_or_alpn", "http2_callbacks", "http2_session", "http2_settings",
      "http2_submit_rpc", "rpc_deadline", "ping_deadline", "ping_submit", "http2_send",
      "http2_receive", "tls_write", "tls_read", "http_status", "grpc_status",
      "grpc_message_size_or_encoding", "grpc_truncated"};
  for (auto stage : stages) if (value == stage) return stage;
  return {};
}
const char* status_domain(std::size_t index, std::string_view stage) {
  if (index == 0) return "cxet_grpc_client_status";
  if (stage == "tcp_socket" || stage == "tcp_connect" || stage == "peer_address")
    return "system_errno";
  if (stage == "tls_handshake" || stage == "tls_read" || stage == "tls_write")
    return "openssl_ssl_error";
  if (stage.starts_with("http2_") || stage == "ping_submit") return "nghttp2_status";
  return "local_diagnostic_status";
}

class Artifact {
 public:
  ~Artifact() { if (fd_ >= 0) ::close(fd_); }
  bool create(const std::filesystem::path& root, std::uint64_t limit,
              std::string_view prefix = "tbank-goaway-") {
    std::error_code ec;
    std::filesystem::create_directories(root, ec);
    if (ec) return false;
    auto pattern = (root / (std::string{prefix} + "XXXXXX")).string();
    std::vector<char> writable(pattern.begin(), pattern.end());
    writable.push_back('\0');
    char* directory = ::mkdtemp(writable.data());
    if (!directory) return false;
    path_ = std::filesystem::path(directory) / "events.jsonl";
    fd_ = ::open(path_.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    limit_ = limit;
    return fd_ >= 0;
  }
  bool write(std::string_view line) {
    if (failed_ || line.size() > limit_ - used_) { failed_ = true; return false; }
    std::size_t pos = 0;
    while (pos < line.size()) {
      const auto count = ::write(fd_, line.data() + pos, line.size() - pos);
      if (count < 0 && errno == EINTR) continue;
      if (count <= 0) { failed_ = true; return false; }
      pos += static_cast<std::size_t>(count);
    }
    used_ += line.size();
    return true;
  }
  bool failed() const { return failed_; }
  const std::filesystem::path& path() const { return path_; }
 private:
  int fd_{-1};
  std::uint64_t used_{};
  std::uint64_t limit_{};
  bool failed_{};
  std::filesystem::path path_;
};

struct State {
  std::string peer;
  std::uint64_t ready_ns{};
  std::uint32_t stream{};
  bool streaming{};
  bool goaway{};
  bool message{};
  bool complete{};
  bool reset{};
  bool eof{};
  bool failure{};
  bool http_present{};
  bool grpc_present{};
  std::uint32_t http{};
  std::uint32_t grpc{};
  bool ack{};
  bool bad_ack{};
  bool ack_rejected{};
  bool ack_malformed{};
  std::uint64_t complete_ns{};
  std::uint64_t ack_ns{};
};

struct Phase {
  Artifact& artifact;
  char name;
  std::string_view uid;
  std::array<State, 2> state{};
  void peer_check(std::size_t index, const std::string& expected, const std::string& observed) {
    const bool expected_valid = numeric_ip(expected);
    const bool observed_valid = numeric_ip(observed);
    std::ostringstream line;
    line << "{\"type\":\"peer_check\",\"monotonic_ns\":" << now_ns()
         << ",\"phase\":\"" << name << "\",\"engine\":\""
         << (index == 0 ? "cxet" : "nghttp2") << "\",\"expected_peer\":";
    if (expected_valid) line << '"' << expected << '"'; else line << "null";
    line << ",\"observed_peer\":";
    if (observed_valid) line << '"' << observed << '"'; else line << "null";
    line << ",\"matches\":";
    if (!expected_valid || !observed_valid) line << "null";
    else line << (expected == observed ? "true" : "false");
    line << "}\n";
    artifact.write(line.str());
  }
  void event(std::size_t index, const Event& event) {
    State& s = state[index];
    // Only fixed literals and numbers reach the artifact. Never retain payload.
    std::ostringstream line;
    line << "{\"type\":\"" << event_name(event.kind) << "\",\"monotonic_ns\":"
         << event.monotonic_ns << ",\"phase\":\"" << name << "\",\"engine\":\""
         << (index == 0 ? "cxet" : "nghttp2") << "\",\"peer\":";
    if (s.peer.empty()) line << "null"; else line << '"' << s.peer << '"';
    line << ",\"stream_id\":" << event.stream_id << ",\"last_stream_id\":"
         << event.last_stream_id << ",\"http2_error\":" << event.http2_error
         << ",\"http_status\":";
    if (event.http_status_present) line << event.http_status; else line << "null";
    line << ",\"grpc_status\":";
    if (event.grpc_status_present) line << event.grpc_status; else line << "null";
    line << ",\"ping_ack_matches_probe\":";
    if (event.kind != EventKind::PingAck || !event.ping_ack_match_known) line << "null";
    else line << (event.ping_ack_matched ? "true" : "false");
    const auto stage = safe_stage(event.stage);
    line << ",\"local_stage\":";
    if (stage.empty()) line << "null"; else line << '"' << stage << '"';
    line << ",\"local_status_domain\":";
    if (stage.empty()) line << "null";
    else line << '"' << status_domain(index, stage) << '"';
    line << ",\"local_status\":" << event.local_status << ",\"debug_bytes\":"
         << event.debug_bytes << "}\n";
    artifact.write(line.str());
    if (event.kind == EventKind::Ready) s.ready_ns = event.monotonic_ns;
    if (event.kind == EventKind::Goaway) s.goaway = true;
    if (event.kind == EventKind::Eof) s.eof = true;
    if (event.kind == EventKind::Failure) s.failure = true;
    if (!s.stream || event.stream_id != s.stream) return;
    if (event.http_status_present) { s.http_present = true; s.http = event.http_status; }
    if (event.grpc_status_present) { s.grpc_present = true; s.grpc = event.grpc_status; }
    if (event.kind == EventKind::Message) {
      s.message = true;
      if (s.streaming) {
        const auto ack = inspect_book_ack(event.payload, uid);
        if (ack == AckResult::Accepted) {
          s.ack = true;
          s.ack_ns = event.monotonic_ns;
          std::ostringstream accepted;
          accepted << "{\"type\":\"book_ack_accepted\",\"monotonic_ns\":"
                   << event.monotonic_ns << ",\"phase\":\"" << name
                   << "\",\"engine\":\"" << (index == 0 ? "cxet" : "nghttp2")
                   << "\",\"stream_id\":" << s.stream << "}\n";
          artifact.write(accepted.str());
        }
        else if (ack == AckResult::Rejected || ack == AckResult::Malformed) {
          s.bad_ack = true;
          if (ack == AckResult::Rejected) s.ack_rejected = true;
          else s.ack_malformed = true;
          std::ostringstream rejected;
          rejected << "{\"type\":\"book_ack_"
                   << (ack == AckResult::Rejected ? "rejected" : "malformed")
                   << "\",\"monotonic_ns\":" << event.monotonic_ns
                   << ",\"phase\":\"" << name << "\",\"engine\":\""
                   << (index == 0 ? "cxet" : "nghttp2")
                   << "\",\"stream_id\":" << s.stream << "}\n";
          artifact.write(rejected.str());
        }
      }
    }
    if (event.kind == EventKind::RpcComplete) {
      s.complete = true;
      s.complete_ns = event.monotonic_ns;
    }
    if (event.kind == EventKind::StreamReset) s.reset = true;
  }
};

bool unary_ok(const State& s) {
  return s.message && s.complete && !s.reset && s.http_present && s.http == 200 &&
         s.grpc_present && s.grpc == 0;
}
void clear_rpc(State& s) {
  s.stream = 0; s.streaming = false; s.message = false; s.complete = false;
  s.reset = false; s.http_present = false; s.grpc_present = false;
  s.http = 0; s.grpc = 0; s.ack = false; s.bad_ack = false;
  s.ack_rejected = false; s.ack_malformed = false;
  s.complete_ns = 0; s.ack_ns = 0;
}
struct Result {
  bool hard_error{};
  bool inconclusive{};
  const char* reason{"complete"};
  bool stop{};
};

Result run_phase(char name, const CliOptions& options, const Factories& factories,
                 std::string_view token, std::uint64_t overall, Artifact& artifact,
                 std::string& common_peer) {
  Phase phase{artifact, name, options.symbol};
  std::array<std::unique_ptr<Connection>, 2> conn{factories.cxet(), factories.nghttp2()};
  if (!conn[0] || !conn[1]) return {true, true, "factory_failed"};
  const auto timeout = static_cast<std::uint64_t>(options.limits.timeout.count()) * 1'000'000ULL;
  auto sink = [&](std::size_t i) { return [&, i](const Event& e) { phase.event(i, e); }; };
  const bool ping = name != 'A';
  ConnectOptions config{.host = kHost, .ca_file = options.ca_file->native(), .port = 443,
      .deadline_ns = std::min(overall, add(now_ns(), timeout)),
      .ping_interval_ns = ping ? 10 * kSecond : 0,
      .ping_ack_timeout_ns = ping ? 2 * kSecond : 0};
  if (!conn[0]->connect(config, sink(0), {})) return {true, true, "cxet_connect"};
  phase.state[0].peer = conn[0]->peer_ip();
  phase.peer_check(0, common_peer, phase.state[0].peer);
  if (artifact.failed()) return {true, true, "artifact_write_or_limit"};
  if (!numeric_ip(phase.state[0].peer) ||
      (!common_peer.empty() && phase.state[0].peer != common_peer))
    return {false, true, "peer_changed", true};
  config.pinned_ip = phase.state[0].peer;
  config.deadline_ns = std::min(overall, add(now_ns(), timeout));
  auto service = [&] { conn[0]->poll(now_ns()); };
  if (!conn[1]->connect(config, sink(1), service)) return {true, true, "nghttp2_connect"};
  phase.state[1].peer = conn[1]->peer_ip();
  phase.peer_check(1, phase.state[0].peer, phase.state[1].peer);
  if (artifact.failed()) return {true, true, "artifact_write_or_limit"};
  if (!numeric_ip(phase.state[1].peer) || phase.state[0].peer != phase.state[1].peer)
    return {false, true, "peer_mismatch", true};
  if (common_peer.empty()) common_peer = phase.state[0].peer;
  const auto start = now_ns();
  if (!phase.state[0].ready_ns || !phase.state[1].ready_ns || !start ||
      phase.state[0].ready_ns > start || phase.state[1].ready_ns > start)
    return {true, true, "ready_missing"};
  const auto end = std::min(overall, add(start, static_cast<std::uint64_t>(*options.duration_seconds) * kSecond));
  {
    std::ostringstream line;
    line << "{\"type\":\"phase_start\",\"monotonic_ns\":" << start
         << ",\"phase\":\"" << name << "\",\"peer\":\"" << common_peer
         << "\",\"cxet_ready_age_ns\":" << start - phase.state[0].ready_ns
         << ",\"nghttp2_ready_age_ns\":" << start - phase.state[1].ready_ns
         << ",\"cxet_physical_age_ns\":null,\"nghttp2_physical_age_ns\":null"
         << ",\"ping_interval_ns\":" << config.ping_interval_ns
         << ",\"ping_ack_timeout_ns\":" << config.ping_ack_timeout_ns
         << ",\"ping_send_ns\":null,\"settings_ns\":null}\n";
    artifact.write(line.str());
  }
  if (start >= end) return {false, true, "overall_deadline"};
  auto poll_both = [&] {
    const auto now = now_ns();
    for (std::size_t i = 0; i < 2; ++i)
      if (!phase.state[i].failure && conn[i]->open()) conn[i]->poll(now);
  };
  auto broken = [&] { return phase.state[0].failure || phase.state[1].failure || artifact.failed(); };
  auto early = [&] { return phase.state[0].eof || phase.state[1].eof ||
                            !conn[0]->open() || !conn[1]->open(); };
  auto pause = [] { std::this_thread::sleep_for(std::chrono::milliseconds{1}); };
  auto drain_goaway = [&](std::size_t i) {
    while (phase.state[i].goaway && now_ns() < end && !artifact.failed() &&
           !phase.state[i].failure && !phase.state[i].eof && conn[i]->open() &&
           !phase.state[i].complete && !phase.state[i].reset) {
      poll_both(); pause();
    }
  };
  bool ack_inconclusive = false;
  if (name == 'C' || name == 'D') {
    const auto request = trading_status_request(options.symbol);
    const auto rpc_end = std::min(end, add(now_ns(), timeout));
    for (std::size_t i = 0; i < 2; ++i) {
      if (phase.state[i].goaway || broken() || early()) return {broken(), true, "unary_unavailable"};
      // The runner enforces rpc_end. A longer backend deadline permits an
      // already-open RPC to drain after GOAWAY without making it successful
      // after the unary timeout.
      phase.state[i].stream = conn[i]->begin_rpc(kTradingStatusPath, token, request, false, end);
      if (!phase.state[i].stream) return {true, true, "unary_refused"};
    }
    while (now_ns() < rpc_end && !(unary_ok(phase.state[0]) && unary_ok(phase.state[1])) &&
           !broken() && !early() && !phase.state[0].reset && !phase.state[1].reset &&
           !(phase.state[0].complete && !unary_ok(phase.state[0])) &&
           !(phase.state[1].complete && !unary_ok(phase.state[1]))) {
      poll_both(); pause();
    }
    if (broken()) {
      drain_goaway(0); drain_goaway(1);
      return {true, true, "unary_transport"};
    }
    if (!unary_ok(phase.state[0]) || !unary_ok(phase.state[1]) ||
        !phase.state[0].complete_ns || phase.state[0].complete_ns > rpc_end ||
        !phase.state[1].complete_ns || phase.state[1].complete_ns > rpc_end) {
      drain_goaway(0); drain_goaway(1);
      if (broken()) return {true, true, "unary_transport"};
      return {false, true, "unary_incomplete"};
    }
    const std::size_t streaming = name == 'C' ? 1 : 0;
    clear_rpc(phase.state[0]); clear_rpc(phase.state[1]);
    if (phase.state[0].goaway || phase.state[1].goaway || early())
      return {false, true, "goaway_before_stream"};
    const auto body = book_stream_request(options.symbol);
    phase.state[streaming].streaming = true;
    const auto ack_end = std::min(end, add(now_ns(), timeout));
    phase.state[streaming].stream = conn[streaming]->begin_rpc(kMarketStreamPath, token, body, true, end);
    if (!phase.state[streaming].stream) return {true, true, "stream_refused"};
    while (now_ns() < ack_end && !phase.state[streaming].ack && !phase.state[streaming].bad_ack &&
           !phase.state[streaming].complete && !phase.state[streaming].reset &&
           !phase.state[streaming].failure && !phase.state[streaming].eof &&
           conn[streaming]->open() && !artifact.failed()) {
      poll_both(); pause();
    }
    if (artifact.failed()) return {true, true, "artifact_write_or_limit"};
    if (!phase.state[streaming].ack || !phase.state[streaming].ack_ns ||
        phase.state[streaming].ack_ns > ack_end || phase.state[streaming].bad_ack) {
      // Do not promote a late ACK to success. Continue observing the already
      // open stream and its peer independently through the phase horizon.
      ack_inconclusive = true;
    }
  }
  bool observation_inconclusive = ack_inconclusive;
  bool stream_ended = false;
  while (now_ns() < end && !artifact.failed()) {
    if ((phase.state[0].failure || !conn[0]->open()) &&
        (phase.state[1].failure || !conn[1]->open())) break;
    poll_both();
    for (std::size_t i = 0; i < 2; ++i) {
      if ((phase.state[i].eof || !conn[i]->open()) && !phase.state[i].goaway)
        observation_inconclusive = true;
    }
    if (name == 'C' || name == 'D') {
      const std::size_t i = name == 'C' ? 1 : 0;
      const State& stream = phase.state[i];
      if (!stream_ended && (stream.bad_ack || stream.reset || stream.complete ||
                            stream.failure || stream.eof || !conn[i]->open())) {
        stream_ended = true;
        observation_inconclusive = true;
        drain_goaway(i);
      }
    }
    pause();
  }
  if (broken()) return {true, true, artifact.failed() ? "artifact_write_or_limit" : "transport_failure"};
  for (std::size_t i = 0; i < 2; ++i) {
    if ((phase.state[i].eof || !conn[i]->open()) && !phase.state[i].goaway)
      observation_inconclusive = true;
  }
  if (name == 'C' || name == 'D') {
    const auto i = name == 'C' ? 1U : 0U;
    if (phase.state[i].eof || !conn[i]->open()) {
      stream_ended = true;
      observation_inconclusive = true;
    }
  }
  if (observation_inconclusive) {
    if (name == 'C' || name == 'D') {
      const auto& stream = phase.state[name == 'C' ? 1 : 0];
      if (stream.ack_rejected) return {false, true, "ack_rejected"};
      if (stream.ack_malformed) return {false, true, "ack_malformed"};
    }
    return {false, true, ack_inconclusive ? "ack_incomplete" : stream_ended ? "stream_ended" : "connection_ended"};
  }
  if (end == overall) return {false, true, "overall_deadline"};
  if (!conn[0]->open() && !conn[1]->open()) return {false, false, "connections_closed_after_goaway"};
  return {};
}
}  // namespace

int run_tbank_goaway(const CliOptions& options, const tbank_grpc::Factories& factories,
                     std::ostream& output, std::ostream& error_output) {
  const auto start = now_ns();
  if (!factories.cxet || !factories.nghttp2 || options.command != Command::Research ||
      options.action != "tbank-goaway" || options.engines != "cxet,nghttp2" ||
      !options.env_file || options.env_file->empty() || !options.output_dir || options.output_dir->empty() ||
      !options.ca_file || options.ca_file->empty() || options.ca_file->native().size() > 4096U ||
      options.ca_file->native().find('\0') != std::string::npos ||
      !tbank_grpc::valid_uid(options.symbol) || options.symbol != tbank_grpc::kSberUid || !options.duration_seconds ||
      *options.duration_seconds == 0 || *options.duration_seconds > 120 ||
      options.api_slot == 0 || options.api_slot > 255 || options.overall_timeout_seconds == 0 ||
      options.overall_timeout_seconds > 600 || options.limits.timeout < std::chrono::milliseconds{100} ||
      options.limits.timeout > std::chrono::milliseconds{10000} || options.limits.attempts != 1 ||
      options.max_artifact_bytes < 4096 || options.max_artifact_bytes > kMaxArtifact ||
      options.open_viewer || options.allow_adapter || options.allow_private_adapter ||
      options.capture_pcap || options.capture_tls_keys || options.jsonl ||
      options.limits.raw_public || !start) {
    error_output << "configuration_error: invalid_tbank_goaway_configuration\n";
    return 2;
  }
  const auto overall = add(start, static_cast<std::uint64_t>(options.overall_timeout_seconds) * kSecond);
  try {
    std::string credential_error;
    auto credential = load_tbank_credential(*options.env_file, options.api_slot, credential_error);
    if (!credential || credential->token().empty()) {
      error_output << "configuration_error: credential_unavailable\n";
      return 2;
    }
    Artifact artifact;
    if (!artifact.create(*options.output_dir, options.max_artifact_bytes)) {
      error_output << "artifact_error: create_failed\n";
      return 2;
    }
    std::string peer;
    bool hard_error = false;
    bool inconclusive = false;
    bool stopped = false;
    output << "phase  status  detail\n";
    for (char name : {'A', 'B', 'C', 'D'}) {
      Result result;
      if (hard_error || artifact.failed()) result = {true, true, "not_run_after_error"};
      else if (stopped) result = {false, true, "not_run_after_peer_change"};
      else if (now_ns() >= overall) result = {false, true, "overall_deadline"};
      else result = run_phase(name, options, factories, credential->token(), overall, artifact, peer);
      hard_error = hard_error || result.hard_error || artifact.failed();
      inconclusive = inconclusive || result.inconclusive;
      stopped = stopped || result.stop;
      std::ostringstream line;
      line << "{\"type\":\"phase_result\",\"monotonic_ns\":" << now_ns()
           << ",\"phase\":\"" << name << "\",\"status\":\""
           << (result.inconclusive ? "inconclusive" : "complete")
           << "\",\"reason\":\"" << result.reason << "\"}\n";
      artifact.write(line.str());
      hard_error = hard_error || artifact.failed();
      output << name << "      " << (result.inconclusive ? "inconclusive" : "complete")
             << "  " << result.reason << '\n';
    }
    output << "artifact: " << artifact.path().string() << '\n'
           << "overall: " << (hard_error ? "error" : inconclusive ? "inconclusive" : "complete") << '\n'
           << "Observation only; no causal conclusion.\n";
    if (artifact.failed()) error_output << "artifact_error: write_failed_or_limit_exceeded\n";
    return hard_error ? 2 : 0;
  } catch (...) {
    error_output << "diagnostic_error: local_failure\n";
    return 2;
  }
}

int run_tbank_lifetime(const CliOptions& options, const tbank_grpc::Factories& factories,
                       std::ostream& output, std::ostream& error_output) {
  using namespace tbank_grpc;
  const auto start = now_ns();
  if (!factories.cxet || !factories.nghttp2 || options.command != Command::Research ||
      options.action != "tbank-lifetime" || options.engines != "cxet,nghttp2" ||
      !options.env_file || options.env_file->empty() || !options.output_dir || options.output_dir->empty() ||
      !options.ca_file || options.ca_file->empty() || options.ca_file->native().size() > 4096U ||
      options.ca_file->native().find('\0') != std::string::npos || options.symbol != kSberUid ||
      !options.duration_seconds || (*options.duration_seconds != 600U && *options.duration_seconds != 28800U) ||
      options.api_slot == 0 || options.api_slot > 255 || options.overall_timeout_seconds == 0 ||
      options.overall_timeout_seconds > 600 || options.limits.timeout < std::chrono::milliseconds{100} ||
      options.limits.timeout > std::chrono::milliseconds{5000} || options.limits.attempts != 1 ||
      options.max_artifact_bytes < 4096 || options.max_artifact_bytes > kMaxArtifact ||
      options.open_viewer || options.allow_adapter || options.allow_private_adapter ||
      options.capture_pcap || options.capture_tls_keys || options.jsonl || options.limits.raw_public || !start) {
    error_output << "configuration_error: invalid_tbank_lifetime_configuration\n";
    return 2;
  }
  try {
    std::string credential_error;
    auto credential = load_tbank_credential(*options.env_file, options.api_slot, credential_error);
    if (!credential || credential->token().empty()) {
      error_output << "configuration_error: credential_unavailable\n"; return 2;
    }
    Artifact artifact;
    if (!artifact.create(*options.output_dir, options.max_artifact_bytes)) {
      error_output << "artifact_error: create_failed\n"; return 2;
    }
    Phase phase{artifact, 'L', kSberUid};
    TariffSummary tariff{};
    bool tariff_mode = true, invalid_message = false, stop_latched = false;
    std::array<unsigned, 2> messages{};
    std::array<std::uint64_t, 2> rpc_end{};
    std::array<std::uint64_t, 2> completed{};
    auto sink = [&](std::size_t i) { return [&, i](const Event& e) {
      const bool was_complete = phase.state[i].complete;
      phase.event(i, e);
      if ((e.kind == EventKind::Headers || e.kind == EventKind::Message ||
           e.kind == EventKind::RpcComplete || e.kind == EventKind::StreamReset) &&
          (!phase.state[i].stream || e.stream_id != phase.state[i].stream)) {
        invalid_message = stop_latched = true;
        return;
      }
      if (e.kind == EventKind::Message && e.stream_id == phase.state[i].stream && e.stream_id) {
        if (++messages[i] != 1u) invalid_message = true;
        else if (tariff_mode) {
          if (i != 0 || !inspect_user_tariff(e.payload, tariff)) invalid_message = true;
        } else if (!inspect_trading_status(e.payload)) invalid_message = true;
      }
      const auto& s = phase.state[i];
      if (e.kind == EventKind::RpcComplete &&
          (was_complete || !unary_ok(s) || messages[i] != 1u || !s.complete_ns ||
           !rpc_end[i] || s.complete_ns >= rpc_end[i])) invalid_message = true;
      if (e.kind == EventKind::RpcComplete && !tariff_mode && !invalid_message) ++completed[i];
      if (invalid_message || artifact.failed() || s.goaway || s.failure || s.eof || s.reset ||
          (s.http_present && s.http != 200) || (s.grpc_present && s.grpc != 0)) stop_latched = true;
    }; };
    std::array<std::unique_ptr<Connection>, 2> conn{factories.cxet(), factories.nghttp2()};
    const auto timeout = static_cast<std::uint64_t>(options.limits.timeout.count()) * 1'000'000ULL;
    const auto startup_end = add(start, static_cast<std::uint64_t>(options.overall_timeout_seconds) * kSecond);
    const char* reason = "factory_failed";
    bool complete = false;
    auto finish = [&] {
      for (auto& connection : conn) if (connection) connection->close();
      std::ostringstream line;
      line << "{\"type\":\"lifetime_result\",\"monotonic_ns\":" << now_ns()
           << ",\"status\":\"" << (complete ? "complete" : "incomplete")
           << "\",\"reason\":\"" << reason << "\",\"cxet_status_completed\":" << completed[0]
           << ",\"nghttp2_status_completed\":" << completed[1] << "}\n";
      artifact.write(line.str());
      if (artifact.failed()) { complete = false; reason = "artifact_write_or_limit"; }
      output << "overall: " << (complete ? "complete" : "incomplete") << "  " << reason << '\n'
             << "completed status: cxet=" << completed[0] << " nghttp2=" << completed[1] << '\n'
             << "artifact: " << artifact.path().string() << '\n';
      if (artifact.failed()) error_output << "artifact_error: write_failed_or_limit_exceeded\n";
      return complete ? 0 : 2;
    };
    if (!conn[0] || !conn[1]) return finish();
    artifact.write("{\"type\":\"experiment\",\"usage_known\":false,\"headroom_known\":false,\"unknown_usage_authorized_for_experiment_only\":true,\"reconnect_after_ready\":false,\"rpc_retry\":false,\"initial_dns_address_attempts_may_vary\":true}\n");
    ConnectOptions config{.host = kHost, .ca_file = options.ca_file->native(), .port = 443,
        .deadline_ns = std::min(startup_end, add(now_ns(), timeout)),
        .stop_requested = [&] { return stop_latched; }};
    reason = "cxet_connect";
    if (!conn[0]->connect(config, sink(0), {})) return finish();
    phase.state[0].peer = conn[0]->peer_ip();
    reason = "cxet_peer_invalid";
    if (!numeric_ip(phase.state[0].peer)) return finish();
    config.pinned_ip = phase.state[0].peer;
    config.deadline_ns = std::min(startup_end, add(now_ns(), timeout));
    reason = "nghttp2_connect";
    if (!conn[1]->connect(config, sink(1), [&] { conn[0]->poll(now_ns()); })) return finish();
    phase.state[1].peer = conn[1]->peer_ip();
    phase.peer_check(1, phase.state[0].peer, phase.state[1].peer);
    reason = "peer_mismatch";
    if (!numeric_ip(phase.state[1].peer) || phase.state[0].peer != phase.state[1].peer) return finish();
    auto broken = [&] {
      return stop_latched || invalid_message || artifact.failed() || phase.state[0].goaway || phase.state[1].goaway ||
          phase.state[0].failure || phase.state[1].failure || phase.state[0].eof || phase.state[1].eof ||
          phase.state[0].reset || phase.state[1].reset ||
          (phase.state[0].http_present && phase.state[0].http != 200) ||
          (phase.state[1].http_present && phase.state[1].http != 200) ||
          (phase.state[0].grpc_present && phase.state[0].grpc != 0) ||
          (phase.state[1].grpc_present && phase.state[1].grpc != 0) ||
          !conn[0]->open() || !conn[1]->open();
    };
    auto failure_reason = [&]() -> const char* {
      if (artifact.failed()) return "artifact_write_or_limit";
      for (const auto& s : phase.state) {
        if ((s.http_present && s.http == 429) || (s.grpc_present && s.grpc == 8)) return "quota_exhausted";
        if (s.goaway) return "goaway";
      }
      if (invalid_message) return "invalid_response";
      return "connection_or_rpc_error";
    };
    auto poll = [&] { poll_lifetime_connections({conn[0].get(), conn[1].get()}, broken, now_ns()); };
    auto pause = [] { std::this_thread::sleep_for(std::chrono::milliseconds{1}); };
    reason = "ready_or_connection_unavailable";
    if (broken() || !phase.state[0].ready_ns || !phase.state[1].ready_ns || now_ns() >= startup_end) return finish();
    auto deadline = std::min(startup_end, add(now_ns(), timeout));
    rpc_end[0] = deadline;
    reason = "tariff_refused";
    phase.state[0].stream = conn[0]->begin_rpc(kUserTariffPath, credential->token(), {}, false, deadline);
    if (!phase.state[0].stream) return finish();
    {
      std::ostringstream line;
      line << "{\"type\":\"tariff_start\",\"monotonic_ns\":" << now_ns()
           << ",\"engine\":\"cxet\",\"stream_id\":" << phase.state[0].stream << "}\n";
      artifact.write(line.str());
    }
    while (now_ns() < deadline && !phase.state[0].complete && !phase.state[0].reset && !broken()) { poll(); pause(); }
    reason = "tariff_incomplete_or_error";
    if (broken()) { reason = failure_reason(); return finish(); }
    if (!unary_ok(phase.state[0]) || messages[0] != 1 || !phase.state[0].complete_ns ||
        phase.state[0].complete_ns >= deadline) return finish();
    for (std::size_t i = 0; i < tariff.count; ++i) {
      const auto& g = tariff.groups[i];
      std::ostringstream line;
      line << "{\"type\":\"tariff_group\",\"trading_status\":" << (g.trading_status ? "true" : "false")
           << ",\"user_tariff\":" << (g.user_tariff ? "true" : "false")
           << ",\"per_minute\":" << g.minute << ",\"per_second\":" << g.second << "}\n";
      artifact.write(line.str());
    }
    reason = "tariff_coverage_or_limit";
    if (artifact.failed() || !tariff_covers_experiment(tariff)) return finish();
    const auto base = phase.state[0].complete_ns;
    reason = "invalid_ready_or_completion_time";
    if (phase.state[0].ready_ns > base || phase.state[1].ready_ns > base || base > now_ns()) return finish();
    const auto end = add(base, static_cast<std::uint64_t>(*options.duration_seconds) * kSecond);
    {
      std::ostringstream line;
      line << "{\"type\":\"lifetime_start\",\"monotonic_ns\":" << base
           << ",\"duration_seconds\":" << *options.duration_seconds
           << ",\"cxet_ready_ns\":" << phase.state[0].ready_ns
           << ",\"nghttp2_ready_ns\":" << phase.state[1].ready_ns
           << ",\"physical_age_ns\":null,\"period_seconds\":20,\"offsets_seconds\":[10,20]}\n";
      artifact.write(line.str());
    }
    std::array<std::uint64_t, 2> next{add(base, 10 * kSecond), add(base, 20 * kSecond)};
    std::uint64_t next_allowed = next[0];
    clear_rpc(phase.state[0]); messages = {}; tariff_mode = false;
    const auto request = trading_status_request(kSberUid);
    reason = "observation_incomplete";
    while (now_ns() < end) {
      if (lifetime_slot_expired(now_ns(), end, next)) {
        reason = "status_schedule_incomplete"; return finish();
      }
      poll();
      if (broken()) { reason = failure_reason(); return finish(); }
      const auto now = now_ns();
      if (lifetime_slot_expired(now, end, next)) {
        reason = "status_schedule_incomplete"; return finish();
      }
      for (std::size_t i = 0; i < 2; ++i) {
        auto& s = phase.state[i];
        if (s.stream && (s.reset || (s.complete && (!unary_ok(s) || messages[i] != 1 ||
            !s.complete_ns || s.complete_ns >= rpc_end[i])) || (!s.complete && now >= rpc_end[i]))) {
          reason = "status_incomplete_or_error"; return finish();
        }
        if (s.stream && s.complete) { clear_rpc(s); messages[i] = 0; }
      }
      if (now >= end) break;
      for (std::size_t i = 0; i < 2; ++i) {
        if (broken()) { reason = failure_reason(); return finish(); }
        if (!phase.state[i].stream && now >= next[i] && now >= next_allowed) {
          const auto admission = now_ns();
          if (lifetime_slot_expired(admission, end, next)) {
            reason = "status_schedule_incomplete"; return finish();
          }
          if (admission >= end) break;
          rpc_end[i] = std::min(end, add(admission, timeout));
          const auto stream = conn[i]->begin_rpc(kTradingStatusPath, credential->token(), request, false, rpc_end[i]);
          if (!stream) { reason = "status_refused"; return finish(); }
          // A delayed begin must not pull the next due time toward the old
          // nominal slot; measure both spacing fences after this submission.
          const auto submitted = now_ns();
          phase.state[i].stream = stream;
          if (submitted >= end || lifetime_slot_expired(submitted, end, next)) {
            reason = "status_schedule_incomplete"; return finish();
          }
          std::ostringstream line;
          line << "{\"type\":\"status_start\",\"monotonic_ns\":" << submitted
               << ",\"engine\":\"" << (i == 0 ? "cxet" : "nghttp2")
               << "\",\"stream_id\":" << stream << "}\n";
          artifact.write(line.str());
          next[i] = next_lifetime_slot(next[i], submitted);
          next_allowed = add(submitted, 10 * kSecond);
        }
      }
      pause();
    }
    poll();
    if (broken()) { reason = failure_reason(); return finish(); }
    for (std::size_t i = 0; i < 2; ++i) {
      const auto& s = phase.state[i];
      if (s.stream && (!unary_ok(s) || messages[i] != 1 || !s.complete_ns || s.complete_ns >= rpc_end[i])) {
        reason = "status_inflight_at_deadline"; return finish();
      }
    }
    reason = "status_schedule_incomplete";
    if (!lifetime_horizon_complete(now_ns(), end, next, completed)) return finish();
    complete = true; reason = "duration_elapsed"; return finish();
  } catch (...) {
    error_output << "diagnostic_error: local_failure\n"; return 2;
  }
}

int tbank_grpc::detail::run_ping_handoff(const CliOptions& options, const Factories& factories,
                                        std::ostream& output, std::ostream& error_output,
                                        const HandoffClock& clock) {
  if (!clock.now || !clock.idle || !factories.cxet || !factories.nghttp2 ||
      options.command != Command::Research || options.action != "tbank-ping-handoff" ||
      !options.ca_file || options.ca_file->empty() || options.ca_file->native().size() > 4096U ||
      options.ca_file->native().find('\0') != std::string::npos || !options.output_dir ||
      options.output_dir->empty() || options.env_file || !options.symbol.empty() ||
      options.engines != "cxet,nghttp2" || options.limits.timeout < std::chrono::milliseconds{100} ||
      options.limits.timeout > std::chrono::milliseconds{5000} || options.limits.attempts != 1 ||
      options.max_artifact_bytes < 4096 || options.max_artifact_bytes > kMaxArtifact ||
      options.open_viewer || options.allow_adapter || options.allow_private_adapter ||
      options.capture_pcap || options.capture_tls_keys || options.jsonl || options.limits.raw_public) {
    error_output << "configuration_error: invalid_tbank_ping_handoff_configuration\n"; return 2;
  }
  try {
    Artifact artifact;
    if (!artifact.create(*options.output_dir, options.max_artifact_bytes, "tbank-ping-handoff-")) {
      error_output << "artifact_error: create_failed\n"; return 2;
    }
    struct Channel {
      std::string peer;
      std::uint64_t ready{}, goaway_time{}, normal_sent{}, extra_sent{}, extra_ack{};
      std::uint32_t goaway_error{}, goaway_last{};
      std::size_t goaway_debug{};
      bool goaway{}, failure{}, eof{}, wrong_ack{};
    };
    std::array<Channel, 2> state{};
    bool extra_queued = false, connecting_standby = false;
    std::uint64_t extra_deadline = 0, extra_enqueue = 0;
    auto event_sink = [&](std::size_t i) { return [&, i](const Event& e) {
      auto& s = state[i];
      if (e.kind == EventKind::Ready) {
        if (s.ready || !e.monotonic_ns) s.failure = true;
        else s.ready = e.monotonic_ns;
      }
      if (e.kind == EventKind::Goaway) {
        s.goaway = true; s.goaway_time = e.monotonic_ns;
        s.goaway_error = e.http2_error; s.goaway_last = e.last_stream_id; s.goaway_debug = e.debug_bytes;
      }
      if (e.kind == EventKind::Failure) s.failure = true;
      if (e.kind == EventKind::Eof) s.eof = true;
      if (e.kind == EventKind::Headers || e.kind == EventKind::Message ||
          e.kind == EventKind::RpcComplete || e.kind == EventKind::StreamReset) s.failure = true;
      if (e.kind == EventKind::PingSent) {
        if (i != 1 || !e.monotonic_ns || (e.extra_ping && (!extra_queued || s.extra_sent ||
            e.monotonic_ns < extra_enqueue || e.monotonic_ns >= extra_deadline))) s.failure = true;
        else if (e.extra_ping) s.extra_sent = e.monotonic_ns;
        else s.normal_sent = e.monotonic_ns;
      }
      if (e.kind == EventKind::PingAck && e.extra_ping) {
        if (i != 1 || !extra_queued || !s.extra_sent || s.extra_ack ||
            !e.ping_ack_match_known || !e.ping_ack_matched || !e.monotonic_ns ||
            e.monotonic_ns < s.extra_sent || e.monotonic_ns >= extra_deadline) s.wrong_ack = true;
        else s.extra_ack = e.monotonic_ns;
      } else if (e.kind == EventKind::PingAck && i == 1 && e.ping_ack_match_known && !e.ping_ack_matched) {
        s.wrong_ack = true;
      }
      std::ostringstream line;
      line << "{\"type\":\"" << event_name(e.kind) << "\",\"monotonic_ns\":" << e.monotonic_ns
           << ",\"engine\":\"" << (i == 0 ? "cxet" : "nghttp2") << "\",\"extra_ping\":"
           << (e.extra_ping ? "true" : "false") << ",\"http2_error\":" << e.http2_error
           << ",\"last_stream_id\":" << e.last_stream_id << ",\"ack_matches\":";
      if (e.kind != EventKind::PingAck || !e.ping_ack_match_known) line << "null";
      else line << (e.ping_ack_matched ? "true" : "false");
      const auto stage = safe_stage(e.stage);
      line << ",\"local_stage\":";
      if (stage.empty()) line << "null"; else line << '"' << stage << '"';
      line << ",\"local_status_domain\":";
      if (stage.empty()) line << "null"; else line << '"' << status_domain(i, stage) << '"';
      line << ",\"local_status\":" << e.local_status << ",\"debug_bytes\":" << e.debug_bytes << "}\n";
      artifact.write(line.str());
    }; };
    std::array<std::unique_ptr<Connection>, 2> conn{factories.cxet(), factories.nghttp2()};
    const char* reason = "factory_failed";
    bool complete = false;
    auto finish = [&] {
      for (auto& c : conn) if (c) c->close();
      std::ostringstream line;
      line << "{\"type\":\"handoff_result\",\"status\":\"" << (complete ? "complete" : "inconclusive")
           << "\",\"reason\":\"" << reason << "\",\"cxet_normal_ping_handoff_ns\":null"
           << ",\"nghttp2_last_normal_ping_handoff_ns\":";
      if (state[1].normal_sent) line << state[1].normal_sent; else line << "null";
      line << ",\"extra_queued\":" << (extra_queued ? "true" : "false")
           << ",\"extra_tls_handoff_ns\":";
      if (state[1].extra_sent) line << state[1].extra_sent; else line << "null";
      line << ",\"extra_matching_ack_ns\":";
      if (state[1].extra_ack) line << state[1].extra_ack; else line << "null";
      line << ",\"normal_to_extra_tls_handoff_ns\":";
      if (state[1].normal_sent && state[1].extra_sent >= state[1].normal_sent)
        line << state[1].extra_sent - state[1].normal_sent;
      else line << "null";
      line << ",\"enqueue_to_tls_handoff_ns\":";
      if (extra_enqueue && state[1].extra_sent >= extra_enqueue) line << state[1].extra_sent - extra_enqueue;
      else line << "null";
      line << ",\"tls_handoff_to_ack_ns\":";
      if (state[1].extra_sent && state[1].extra_ack >= state[1].extra_sent)
        line << state[1].extra_ack - state[1].extra_sent;
      else line << "null";
      line << "}\n"; artifact.write(line.str());
      if (artifact.failed()) { complete = false; reason = "artifact_write_or_limit"; }
      output << "overall: " << (complete ? "complete" : "inconclusive") << "  " << reason << '\n'
             << "artifact: " << artifact.path().string() << '\n'
             << "Diagnostic observation; production PING safety is not established.\n";
      if (artifact.failed()) error_output << "artifact_error: write_failed_or_limit_exceeded\n";
      return complete ? 0 : 2;
    };
    if (!conn[0] || !conn[1]) return finish();
    const auto start = clock.now();
    const auto timeout = static_cast<std::uint64_t>(options.limits.timeout.count()) * 1'000'000ULL;
    if (!start) { reason = "clock_unavailable"; return finish(); }
    ConnectOptions config{.host = kHost, .ca_file = options.ca_file->native(), .port = 443,
        .deadline_ns = add(start, timeout), .ping_interval_ns = 10 * kSecond,
        .ping_ack_timeout_ns = 2 * kSecond};
    reason = "primary_connect";
    if (!conn[0]->connect(config, event_sink(0), {})) return finish();
    state[0].peer = conn[0]->peer_ip();
    reason = "primary_peer_or_ready";
    if (!numeric_ip(state[0].peer) || !state[0].ready || state[0].ready > clock.now()) return finish();
    const auto trigger_deadline = add(state[0].ready, 60 * kSecond);
    const auto absolute_end = add(state[0].ready, 67 * kSecond);
    const auto stagger = add(state[0].ready, 10 * kSecond);
    auto primary_unavailable = [&] { return state[0].failure || state[0].eof || !conn[0]->open(); };
    auto invalid_primary_goaway = [&] {
      const auto& s = state[0];
      return s.goaway && (s.goaway_error != 0 || s.goaway_debug != 0 || s.goaway_time < s.ready ||
                           s.goaway_time > clock.now() || s.goaway_time >= trigger_deadline);
    };
    auto backup_unavailable = [&] { return state[1].failure || state[1].eof || state[1].goaway ||
        state[1].wrong_ack || artifact.failed() || !conn[1]->open(); };
    while (clock.now() < stagger && !state[0].goaway && !primary_unavailable() && !artifact.failed()) {
      conn[0]->poll(clock.now()); clock.idle();
    }
    reason = "primary_unavailable_before_standby";
    if (state[0].goaway || primary_unavailable() || artifact.failed() || clock.now() >= trigger_deadline) return finish();
    connecting_standby = true;
    config.pinned_ip = state[0].peer;
    config.deadline_ns = std::min(trigger_deadline, add(clock.now(), timeout));
    config.report_ping_sends = true;
    config.frame_based_ping_idle = true;
    config.stop_requested = [&] { return artifact.failed() || state[1].goaway || state[1].failure || state[1].eof || state[1].wrong_ack ||
        (connecting_standby && (state[0].goaway || primary_unavailable())); };
    reason = "standby_connect";
    if (!conn[1]->connect(config, event_sink(1), [&] { if (!state[0].goaway) conn[0]->poll(clock.now()); })) return finish();
    connecting_standby = false;
    state[1].peer = conn[1]->peer_ip();
    {
      std::ostringstream line;
      line << "{\"type\":\"peer_check\",\"expected_peer\":\"" << state[0].peer << "\",\"observed_peer\":";
      if (numeric_ip(state[1].peer)) line << '"' << state[1].peer << '"'; else line << "null";
      line << ",\"matches\":" << (state[0].peer == state[1].peer ? "true" : "false") << "}\n";
      artifact.write(line.str());
    }
    reason = "peer_mismatch_or_ready";
    if (!numeric_ip(state[1].peer) || state[1].peer != state[0].peer || !state[1].ready ||
        state[1].ready < stagger || state[1].ready > clock.now()) return finish();
    {
      std::ostringstream line;
      line << "{\"type\":\"connections\",\"peer\":\"" << state[0].peer
           << "\",\"primary_ready_ns\":" << state[0].ready << ",\"standby_ready_ns\":" << state[1].ready
           << ",\"physical_age_ns\":null,\"reconnect_after_ready\":false,\"rpc_count\":0}\n";
      artifact.write(line.str());
    }
    while (clock.now() < trigger_deadline && !state[0].goaway && !primary_unavailable() && !backup_unavailable()) {
      conn[0]->poll(clock.now());
      if (primary_unavailable() || invalid_primary_goaway() || backup_unavailable()) break;
      conn[1]->poll(clock.now());
      clock.idle();
    }
    reason = "primary_goaway_not_observed";
    if (!state[0].goaway || state[0].goaway_time < state[0].ready ||
        state[0].goaway_time > clock.now() || state[0].goaway_time >= trigger_deadline) return finish();
    reason = "primary_goaway_error";
    if (invalid_primary_goaway() || primary_unavailable()) return finish();
    reason = "standby_unavailable";
    if (backup_unavailable()) return finish();
    // Drain any already-observable standby GOAWAY/normal ACK before admission.
    conn[1]->poll(clock.now());
    if (backup_unavailable()) return finish();
    const auto queued = clock.now();
    extra_enqueue = queued;
    extra_deadline = std::min(absolute_end, add(queued, 2 * kSecond));
    reason = "extra_ping_refused_or_inflight";
    if (!queued || queued >= trigger_deadline || extra_deadline <= queued ||
        !conn[1]->begin_extra_ping(extra_deadline)) return finish();
    extra_queued = true;
    {
      std::ostringstream line;
      line << "{\"type\":\"extra_ping_queued\",\"monotonic_ns\":" << queued
           << ",\"ack_deadline_ns\":" << extra_deadline << ",\"ack_deadline_exclusive\":true}\n";
      artifact.write(line.str());
    }
    while (clock.now() < extra_deadline && !state[1].extra_ack && !backup_unavailable()) {
      conn[1]->poll(clock.now()); clock.idle();
    }
    reason = "extra_ping_ack_incomplete";
    if (backup_unavailable() || !state[1].extra_sent || !state[1].extra_ack) return finish();
    const auto observe_end = std::min(absolute_end, add(state[1].extra_ack, 5 * kSecond));
    reason = "standby_goaway_or_error_after_ack";
    bool observed_after_ack = false;
    while (clock.now() < observe_end && !backup_unavailable()) {
      conn[1]->poll(clock.now()); observed_after_ack = true; clock.idle();
    }
    if (clock.now() <= absolute_end && !backup_unavailable()) conn[1]->poll(clock.now());
    if (backup_unavailable() || !observed_after_ack || clock.now() > absolute_end ||
        observe_end != add(state[1].extra_ack, 5 * kSecond)) return finish();
    complete = true; reason = "matching_ack_and_observation_complete"; return finish();
  } catch (...) {
    error_output << "diagnostic_error: local_failure\n"; return 2;
  }
}

int run_tbank_ping_handoff(const CliOptions& options, const tbank_grpc::Factories& factories,
                           std::ostream& output, std::ostream& error_output) {
  const tbank_grpc::detail::HandoffClock clock{now_ns,
      [] { std::this_thread::sleep_for(std::chrono::milliseconds{1}); }};
  return tbank_grpc::detail::run_ping_handoff(options, factories, output, error_output, clock);
}
}  // namespace exchange_probe
