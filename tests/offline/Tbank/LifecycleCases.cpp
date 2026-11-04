#include "OfflineCase.hpp"
#include "exchange_probe/App.hpp"
#include "exchange_probe/TbankGrpcDiagnostic.hpp"
#include "../../../src/Research/TbankGrpcProtocol.hpp"
#include "../../../src/Research/TbankPingHandoff.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>
#include <unistd.h>

namespace {
using namespace exchange_probe;
using namespace exchange_probe::tbank_grpc;
constexpr std::string_view uid = "e6123145-9665-43e0-8413-cd61b8aa9b13";
constexpr std::string_view secret = "synthetic-token-never-output";

struct TempDir {
  std::filesystem::path path;
  TempDir() {
    auto pattern = (std::filesystem::temp_directory_path() / "tbank-lifecycle-XXXXXX").string();
    std::vector<char> buffer(pattern.begin(), pattern.end());
    buffer.push_back('\0');
    char* created = ::mkdtemp(buffer.data());
    CXET_CHECK(created != nullptr);
    path = created;
  }
  ~TempDir() { std::error_code ec; std::filesystem::remove_all(path, ec); }
};

CliOptions options(const TempDir& temp) {
  CliOptions result;
  result.command = Command::Research;
  result.action = "tbank-goaway";
  result.env_file = temp.path / "synthetic.env";
  result.ca_file = "/synthetic/tbank-ca.pem";
  result.output_dir = temp.path / "output";
  result.symbol = uid;
  result.duration_seconds = 1;
  result.max_artifact_bytes = 4U * 1024U * 1024U;
  result.overall_timeout_seconds = 10;
  result.limits.timeout = std::chrono::milliseconds{500};
  result.open_viewer = false;
  return result;
}

std::uint64_t timestamp() {
  return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count());
}
struct Simulation {
  unsigned factories{};
  unsigned connects{};
  unsigned rpc_count{};
  std::array<bool, 4> stream_started{};
  std::array<bool, 4> ack{};
  std::array<bool, 4> eof_before_ack{};
  std::array<bool, 4> idle_eof{};
  std::array<unsigned, 4> active_polls_after_idle{};
  bool unexpected{};
} simulation;

class FakeConnection final : public Connection {
 public:
  FakeConnection(unsigned phase, unsigned engine) : phase_(phase), engine_(engine) {}
  bool connect(const ConnectOptions& config, EventSink sink,
               const std::function<void()>&) override {
    ++simulation.connects;
    if (config.port != 443 || config.ca_file != "/synthetic/tbank-ca.pem" ||
        (engine_ == 1 && config.pinned_ip != "192.0.2.1"))
      simulation.unexpected = true;
    sink_ = std::move(sink);
    Event event; event.kind = EventKind::Ready; event.monotonic_ns = timestamp();
    sink_(event);
    return true;
  }
  std::string peer_ip() const override { return "192.0.2.1"; }
  std::uint32_t begin_rpc(std::string_view path, std::string_view bearer,
                          std::span<const std::byte> request, bool streaming,
                          std::uint64_t) override {
    ++simulation.rpc_count;
    if (bearer != secret || request.empty() ||
        path != (streaming ? kMarketStreamPath : kTradingStatusPath) ||
        phase_ < 2 || pending_) simulation.unexpected = true;
    streaming_ = streaming;
    if (streaming) simulation.stream_started[phase_] = true;
    pending_ = true;
    return streaming ? 3 : 1;
  }
  void poll(std::uint64_t now) override {
    if (!open_) return;
    if (phase_ < 2) { end(now); return; }
    const unsigned active = phase_ == 2 ? 1 : 0;
    if (engine_ == active && simulation.idle_eof[phase_])
      ++simulation.active_polls_after_idle[phase_];
    if (pending_) {
      pending_ = false;
      Event event; event.kind = EventKind::Message; event.monotonic_ns = now;
      event.stream_id = streaming_ ? 3 : 1;
      if (streaming_) {
        const std::string_view entry = "\x18\x01\x22\x24" "e6123145-9665-43e0-8413-cd61b8aa9b13" "\x38\x01";
        std::vector<std::byte> ack{std::byte{0x12}, static_cast<std::byte>(entry.size() + 2),
                                   std::byte{0x12}, static_cast<std::byte>(entry.size())};
        for (unsigned char c : entry) ack.push_back(static_cast<std::byte>(c));
        event.payload = ack;
        sink_(event);
        simulation.ack[phase_] = true;
        Event ping; ping.kind = EventKind::PingAck; ping.monotonic_ns = now;
        sink_(ping);
      } else {
        sink_(event);
        event.kind = EventKind::RpcComplete;
        event.http_status_present = true; event.http_status = 200;
        event.grpc_status_present = true; event.grpc_status = 0;
        event.stage = "grpc_status"; event.local_status = -17;
        sink_(event);
      }
    }
    if (engine_ != active &&
        (phase_ == 2 ? simulation.stream_started[phase_] : simulation.ack[phase_])) {
      simulation.eof_before_ack[phase_] = !simulation.ack[phase_];
      end(now);
      simulation.idle_eof[phase_] = true;
    }
  }
  bool open() const override { return open_; }
  void close() noexcept override { open_ = false; }
 private:
  void end(std::uint64_t now) {
    Event event; event.kind = EventKind::Goaway; event.monotonic_ns = now;
    sink_(event);
    event.kind = EventKind::Eof; sink_(event);
    open_ = false;
  }
  unsigned phase_;
  unsigned engine_;
  EventSink sink_;
  bool open_{true};
  bool pending_{};
  bool streaming_{};
};
std::unique_ptr<Connection> make_cxet() {
  const auto n = simulation.factories++;
  if (n % 2 != 0) simulation.unexpected = true;
  return std::make_unique<FakeConnection>(n / 2, 0);
}
std::unique_ptr<Connection> make_nghttp2() {
  const auto n = simulation.factories++;
  if (n % 2 != 1) simulation.unexpected = true;
  return std::make_unique<FakeConnection>(n / 2, 1);
}

void missing_factory_refuses_before_io() {
  TempDir temp;
  const auto opts = options(temp); // The env file deliberately does not exist.
  std::ostringstream output, error;
  CXET_CHECK(run_tbank_goaway(opts, Factories{make_nghttp2, nullptr}, output, error) == 2);
  CXET_CHECK(error.str().find("invalid_tbank_goaway_configuration") != std::string::npos);
  CXET_CHECK(error.str().find("credential_unavailable") == std::string::npos);
  CXET_CHECK(!std::filesystem::exists(*opts.output_dir));
  auto no_ca = opts;
  no_ca.ca_file.reset();
  simulation = {};
  std::ostringstream no_ca_output, no_ca_error;
  CXET_CHECK(run_tbank_goaway(no_ca, Factories{make_nghttp2, make_cxet},
                               no_ca_output, no_ca_error) == 2);
  CXET_CHECK(no_ca_error.str().find("invalid_tbank_goaway_configuration") != std::string::npos);
  CXET_CHECK(simulation.factories == 0 && !std::filesystem::exists(*opts.output_dir));
}

void simulated_lifecycle() {
  TempDir temp;
  const auto opts = options(temp);
  { std::ofstream env(*opts.env_file); env << "TBANK_API_1_API=" << secret << '\n'; CXET_CHECK(env.good()); }
  simulation = {};
  std::ostringstream output, error;
  CXET_CHECK(run_tbank_goaway(opts, Factories{make_nghttp2, make_cxet}, output, error) == 0);
  CXET_CHECK(error.str().empty());
  CXET_CHECK(!simulation.unexpected);
  CXET_CHECK(simulation.factories == 8 && simulation.connects == 8 && simulation.rpc_count == 6);
  for (unsigned phase : {2U, 3U}) {
    CXET_CHECK(simulation.ack[phase] && simulation.idle_eof[phase]);
    CXET_CHECK(simulation.active_polls_after_idle[phase] > 0);
  }
  CXET_CHECK(simulation.eof_before_ack[2] && !simulation.eof_before_ack[3]);
  CXET_CHECK(output.str().find("overall: complete") != std::string::npos);
  std::string jsonl;
  unsigned artifacts = 0;
  for (const auto& item : std::filesystem::recursive_directory_iterator(*opts.output_dir)) {
    if (item.path().filename() == "events.jsonl") {
      ++artifacts;
      std::ifstream file(item.path());
      CXET_CHECK(file.good());
      jsonl.assign(std::istreambuf_iterator<char>(file), {});
    }
  }
  CXET_CHECK(artifacts == 1);
  CXET_CHECK(jsonl.find(secret) == std::string::npos && output.str().find(secret) == std::string::npos);
  for (char phase : {'A', 'B', 'C', 'D'}) {
    const auto expected = std::string("\"phase\":\"") + phase + "\",\"status\":\"complete\"";
    CXET_CHECK(jsonl.find(expected) != std::string::npos);
  }
  for (const auto [phase, engine] : {std::pair{'C', "cxet"}, std::pair{'D', "nghttp2"}}) {
    for (const auto* kind : {"goaway", "eof"}) {
      const auto prefix = std::string("\"type\":\"") + kind + "\"";
      auto position = jsonl.find(prefix);
      bool found = false;
      while (position != std::string::npos) {
        const auto end = jsonl.find('\n', position);
        const auto line = jsonl.substr(position, end - position);
        const auto phase_field = std::string("\"phase\":\"") + phase + "\"";
        const auto engine_field = std::string("\"engine\":\"") + engine + "\"";
        if (line.find(phase_field) != std::string::npos && line.find(engine_field) != std::string::npos) {
          found = true;
          break;
        }
        position = end == std::string::npos ? end : jsonl.find(prefix, end + 1);
      }
      CXET_CHECK(found);
    }
  }
  CXET_CHECK(jsonl.find("\"type\":\"book_ack_accepted\"") != std::string::npos);
  CXET_CHECK(jsonl.find("\"grpc_status\":0,\"ping_ack_matches_probe\":null,\"local_stage\":\"grpc_status\",\"local_status_domain\":\"cxet_grpc_client_status\",\"local_status\":-17") != std::string::npos);
  const auto native_ping = jsonl.find("\"type\":\"ping_ack\"", jsonl.find("\"phase\":\"D\""));
  CXET_CHECK(native_ping != std::string::npos);
  const auto native_ping_end = jsonl.find('\n', native_ping);
  CXET_CHECK(native_ping_end != std::string::npos);
  const auto ping_line = jsonl.substr(native_ping, native_ping_end - native_ping);
  CXET_CHECK(ping_line.find("\"engine\":\"cxet\"") != std::string::npos);
  CXET_CHECK(ping_line.find("\"ping_ack_matches_probe\":null") != std::string::npos);
}

struct LifetimeSimulation {
  unsigned calls{}, polls{};
  bool mismatch{}, quota{}, bad_tariff{}, spam{}, late{}, bad_status{}, unexpected{};
  bool success_then_goaway{}, status_success{};
} lifetime;
class LifetimeConnection final : public Connection {
 public:
  explicit LifetimeConnection(bool cxet) : cxet_(cxet) {}
  bool connect(const ConnectOptions& config, EventSink sink, const std::function<void()>&) override {
    sink_ = std::move(sink);
    if (config.ping_interval_ns || config.ping_ack_timeout_ns ||
        (!cxet_ && config.pinned_ip != "192.0.2.1")) lifetime.unexpected = true;
    Event e; e.kind = EventKind::Ready; e.monotonic_ns = timestamp();
    if (lifetime.bad_status || lifetime.success_then_goaway) e.monotonic_ns -= 20'000'000'000ULL;
    sink_(e); return true;
  }
  std::string peer_ip() const override { return !cxet_ && lifetime.mismatch ? "192.0.2.2" : "192.0.2.1"; }
  std::uint32_t begin_rpc(std::string_view path, std::string_view bearer,
                          std::span<const std::byte> body, bool streaming, std::uint64_t deadline) override {
    ++lifetime.calls;
    if (!cxet_ || streaming || bearer != secret || deadline <= timestamp()) lifetime.unexpected = true;
    if (path == kUserTariffPath && body.empty() && lifetime.calls == 1) status_ = false;
    else if ((lifetime.bad_status || lifetime.success_then_goaway) && path == kTradingStatusPath && lifetime.calls == 2 &&
             body.size() == trading_status_request(kSberUid).size()) status_ = true;
    else lifetime.unexpected = true;
    pending_ = true; return 1;
  }
  void poll(std::uint64_t now) override {
    if (!cxet_) {
      if (lifetime.success_then_goaway && lifetime.status_success) {
        Event e; e.kind = EventKind::Goaway; e.monotonic_ns = now; sink_(e);
      }
      return;
    }
    if (pending_) {
      pending_ = false;
      if (status_) {
        const std::string_view row = lifetime.bad_status
            ? "\x32\x24" "e6123145-9665-43e0-8413-cd61b8aa9b13" "\x18\x01\x18\x01"
            : "\x32\x24" "e6123145-9665-43e0-8413-cd61b8aa9b13" "\x18\x01";
        std::vector<std::byte> body{std::byte{10}, static_cast<std::byte>(row.size())};
        for (char c : row) body.push_back(static_cast<std::byte>(c));
        Event e; e.kind = EventKind::Message; e.monotonic_ns = now; e.stream_id = 1; e.payload = body; sink_(e);
        if (lifetime.success_then_goaway) {
          e.kind = EventKind::RpcComplete; e.payload = {};
          e.http_status_present = e.grpc_status_present = true;
          e.http_status = 200; e.grpc_status = 0; sink_(e);
          lifetime.status_success = true;
        }
        return;
      }
      if (lifetime.spam) {
        for (unsigned i = 0; i < 100; ++i) {
          Event e; e.kind = EventKind::PingAck; e.monotonic_ns = now; sink_(e);
        }
      }
      if (!lifetime.quota) {
        std::vector<std::byte> group{std::byte{8}, std::byte{6}};
        auto add_method = [&](std::string_view method) {
          group.push_back(std::byte{18}); group.push_back(static_cast<std::byte>(method.size()));
          for (char c : method) group.push_back(static_cast<std::byte>(c));
        };
        add_method(kTradingStatusPath.substr(1));
        if (!lifetime.bad_tariff) add_method(kUserTariffPath.substr(1));
        // Two method names make the group longer than 127 bytes.
        std::vector<std::byte> body{std::byte{10}, static_cast<std::byte>((group.size() & 127u) | 128u),
                                    static_cast<std::byte>(group.size() >> 7u)};
        body.insert(body.end(), group.begin(), group.end());
        Event e; e.kind = EventKind::Message; e.monotonic_ns = now; e.stream_id = 1; e.payload = body; sink_(e);
      }
      Event e; e.kind = EventKind::RpcComplete;
      e.monotonic_ns = lifetime.late ? now + 1'000'000'000ULL : now; e.stream_id = 1;
      if (lifetime.bad_status || lifetime.success_then_goaway) e.monotonic_ns -= 11'000'000'000ULL;
      e.http_status_present = e.grpc_status_present = true; e.http_status = lifetime.quota ? 429 : 200;
      e.grpc_status = lifetime.quota ? 8 : 0; sink_(e);
    } else if (!lifetime.bad_status && !lifetime.success_then_goaway && ++lifetime.polls == 1) {
      Event e; e.kind = EventKind::Goaway; e.monotonic_ns = now; sink_(e);
    }
  }
  bool open() const override { return true; }
  void close() noexcept override {}
 private:
  bool cxet_{}, pending_{}, status_{};
  EventSink sink_;
};
std::unique_ptr<Connection> lifetime_cxet() { return std::make_unique<LifetimeConnection>(true); }
std::unique_ptr<Connection> lifetime_nghttp2() { return std::make_unique<LifetimeConnection>(false); }
class StopRaceConnection final : public Connection {
 public:
  StopRaceConnection(bool first, bool& stopped, unsigned& flushes)
      : first_(first), stopped_(stopped), flushes_(flushes) {}
  bool connect(const ConnectOptions&, EventSink, const std::function<void()>&) override { return true; }
  std::string peer_ip() const override { return "192.0.2.1"; }
  std::uint32_t begin_rpc(std::string_view, std::string_view, std::span<const std::byte>, bool,
                          std::uint64_t) override { pending_ = true; return 1; }
  void poll(std::uint64_t) override {
    if (first_) stopped_ = true; // GOAWAY/429 on CXET.
    else if (pending_) { ++flushes_; pending_ = false; }
  }
  bool open() const override { return true; }
  void close() noexcept override {}
 private:
  bool first_, &stopped_;
  unsigned& flushes_;
  bool pending_{};
};
void lifetime_stops_and_privacy() {
  {
    bool stopped = false; unsigned flushes = 0;
    StopRaceConnection cxet{true, stopped, flushes}, nghttp2{false, stopped, flushes};
    CXET_CHECK(nghttp2.begin_rpc(kTradingStatusPath, secret, {}, false, 1) == 1);
    poll_lifetime_connections({&cxet, &nghttp2}, [&] { return stopped; }, timestamp());
    CXET_CHECK(stopped && flushes == 0);
  }
  for (unsigned scenario = 0; scenario < 8; ++scenario) {
    TempDir temp;
    auto opts = options(temp); opts.action = "tbank-lifetime"; opts.duration_seconds = 600;
    { std::ofstream env(*opts.env_file); env << "TBANK_API_1_API=" << secret << '\n'; }
    lifetime = {}; lifetime.mismatch = scenario == 1; lifetime.quota = scenario == 2;
    lifetime.bad_tariff = scenario == 3; lifetime.spam = scenario == 4; lifetime.late = scenario == 5;
    lifetime.bad_status = scenario == 6;
    lifetime.success_then_goaway = scenario == 7;
    if (lifetime.spam) opts.max_artifact_bytes = 4096;
    std::ostringstream output, error;
    CXET_CHECK(run_tbank_lifetime(opts, Factories{lifetime_nghttp2, lifetime_cxet}, output, error) == 2);
    CXET_CHECK(!lifetime.unexpected && lifetime.calls == (scenario == 1 ? 0u : scenario >= 6 ? 2u : 1u));
    CXET_CHECK(output.str().find("overall: incomplete") != std::string::npos);
    std::string jsonl;
    for (const auto& item : std::filesystem::recursive_directory_iterator(*opts.output_dir)) {
      if (item.path().filename() == "events.jsonl") {
        std::ifstream file(item.path()); jsonl.assign(std::istreambuf_iterator<char>(file), {});
      }
    }
    CXET_CHECK(jsonl.find(secret) == std::string::npos && output.str().find(secret) == std::string::npos);
    CXET_CHECK(jsonl.find("GetUserTariff") == std::string::npos);
    CXET_CHECK(jsonl.find("\"usage_known\":false") != std::string::npos);
    if (lifetime.bad_status) CXET_CHECK(output.str().find("invalid_response") != std::string::npos);
    if (lifetime.success_then_goaway) {
      CXET_CHECK(output.str().find("overall: incomplete  goaway") != std::string::npos);
      CXET_CHECK(output.str().find("completed status: cxet=1 nghttp2=0") != std::string::npos);
      CXET_CHECK(jsonl.find("\"cxet_status_completed\":1,\"nghttp2_status_completed\":0") != std::string::npos);
    }
    if (lifetime.spam) {
      CXET_CHECK(jsonl.size() <= 4096);
      CXET_CHECK(output.str().find("artifact_write_or_limit") != std::string::npos);
    }
  }
}

struct PingSimulation {
  std::uint64_t now{100'000'000'000ULL}, primary_ready{};
  std::uint64_t extra_deadline{}, ack_time{};
  unsigned mode{}, extra{}, rpc{}, standby_after_primary_error{};
  bool primary_sent{}, standby_sent{}, pending{}, delayed_goaway{}, unexpected{};
} ping_sim;
class PingConnection final : public Connection {
 public:
  explicit PingConnection(bool primary) : primary_(primary) {}
  bool connect(const ConnectOptions& config, EventSink sink, const std::function<void()>& other) override {
    sink_ = std::move(sink);
    if (config.ping_interval_ns != 10'000'000'000ULL || config.ping_ack_timeout_ns != 2'000'000'000ULL ||
        config.ca_file != "/synthetic/tbank-ca.pem" || (!primary_ && config.pinned_ip != "192.0.2.1") ||
        (primary_ && (config.report_ping_sends || config.frame_based_ping_idle)) ||
        (!primary_ && (!config.report_ping_sends || !config.frame_based_ping_idle)))
      ping_sim.unexpected = true;
    if (other) other();
    if (primary_) ping_sim.primary_ready = ping_sim.now;
    else if (ping_sim.now - ping_sim.primary_ready < 10'000'000'000ULL) ping_sim.unexpected = true;
    Event e; e.kind = EventKind::Ready; e.monotonic_ns = ping_sim.now; sink_(e); return true;
  }
  std::string peer_ip() const override { return !primary_ && ping_sim.mode == 6 ? "192.0.2.2" : "192.0.2.1"; }
  std::uint32_t begin_rpc(std::string_view, std::string_view, std::span<const std::byte>, bool,
                          std::uint64_t) override { ++ping_sim.rpc; return 0; }
  bool begin_extra_ping(std::uint64_t deadline) override {
    ++ping_sim.extra;
    if (primary_ || deadline <= ping_sim.now || ping_sim.extra > 1) ping_sim.unexpected = true;
    if (ping_sim.mode == 4) return false; // normal PING still inflight
    ping_sim.extra_deadline = deadline;
    pending_ = true; return true;
  }
  void poll(std::uint64_t now) override {
    if (!primary_ && (ping_sim.mode == 8 || ping_sim.mode == 16) && ping_sim.primary_sent)
      ++ping_sim.standby_after_primary_error;
    const auto elapsed = now - ping_sim.primary_ready;
    const auto primary_due = ping_sim.mode == 13 ? 60'000'000'000ULL
        : ping_sim.mode == 14 ? 61'000'000'000ULL : 15'000'000'000ULL;
    if (primary_ && !ping_sim.primary_sent && elapsed >= primary_due && ping_sim.mode != 5) {
      ping_sim.primary_sent = true;
      Event e; e.kind = ping_sim.mode == 3 ? EventKind::Failure : EventKind::Goaway;
      e.monotonic_ns = now; e.http2_error = ping_sim.mode == 8 ? 11 : 0;
      if (ping_sim.mode == 16) e.debug_bytes = 1;
      sink_(e);
    }
    if (!primary_ && !ping_sim.standby_sent && elapsed >= 12'000'000'000ULL) {
      ping_sim.standby_sent = true;
      Event e; e.kind = ping_sim.mode == 2 ? EventKind::Goaway : EventKind::PingSent;
      e.monotonic_ns = now; sink_(e);
    }
    if (!primary_ && pending_) {
      pending_ = false;
      if (ping_sim.mode != 9) {
        Event sent; sent.kind = EventKind::PingSent; sent.monotonic_ns = now; sent.extra_ping = true; sink_(sent);
      }
      if (ping_sim.mode != 10) {
        Event ack; ack.kind = EventKind::PingAck; ack.monotonic_ns = now; ack.extra_ping = true;
        if (ping_sim.mode == 11) ack.monotonic_ns = ping_sim.extra_deadline;
        if (ping_sim.mode == 12) ack.monotonic_ns = ping_sim.extra_deadline + 1'000'000'000ULL;
        ack.ping_ack_match_known = true; ack.ping_ack_matched = ping_sim.mode != 1; sink_(ack);
        ping_sim.ack_time = now;
      }
      if (ping_sim.mode == 7) {
        Event goaway; goaway.kind = EventKind::Goaway; goaway.monotonic_ns = now;
        goaway.http2_error = 11; sink_(goaway);
      }
    }
    if (!primary_ && ping_sim.mode == 15 && ping_sim.ack_time && !ping_sim.delayed_goaway &&
        now >= ping_sim.ack_time + 3'000'000'000ULL) {
      ping_sim.delayed_goaway = true;
      Event goaway; goaway.kind = EventKind::Goaway; goaway.monotonic_ns = now;
      goaway.http2_error = 11; sink_(goaway);
    }
  }
  bool open() const override { return true; }
  void close() noexcept override {}
 private:
  bool primary_{}, pending_{};
  EventSink sink_;
};
std::unique_ptr<Connection> ping_primary() { return std::make_unique<PingConnection>(true); }
std::unique_ptr<Connection> ping_standby() { return std::make_unique<PingConnection>(false); }
void ping_handoff_scenarios() {
  for (unsigned mode = 0; mode < 17; ++mode) {
    TempDir temp;
    CliOptions opts; opts.command = Command::Research; opts.action = "tbank-ping-handoff";
    opts.ca_file = "/synthetic/tbank-ca.pem"; opts.output_dir = temp.path / "output";
    opts.limits.timeout = std::chrono::milliseconds{500}; opts.max_artifact_bytes = 4096;
    opts.open_viewer = false;
    ping_sim = {}; ping_sim.mode = mode;
    const detail::HandoffClock clock{[] { return ping_sim.now; }, [] { ping_sim.now += 1'000'000'000ULL; }};
    std::ostringstream output, error;
    const auto result = detail::run_ping_handoff(opts, Factories{ping_standby, ping_primary}, output, error, clock);
    CXET_CHECK(result == (mode == 0 ? 0 : 2));
    CXET_CHECK(!ping_sim.unexpected && ping_sim.rpc == 0 && ping_sim.extra <= 1);
    CXET_CHECK(output.str().find(mode == 0 ? "overall: complete" : "overall: inconclusive") != std::string::npos);
    CXET_CHECK(ping_sim.now - ping_sim.primary_ready <= 67'000'000'000ULL);
    std::string jsonl;
    for (const auto& item : std::filesystem::recursive_directory_iterator(*opts.output_dir)) {
      if (item.path().filename() == "events.jsonl") {
        std::ifstream file(item.path()); jsonl.assign(std::istreambuf_iterator<char>(file), {});
      }
    }
    CXET_CHECK(jsonl.size() <= 4096 && jsonl.find(secret) == std::string::npos);
    if (mode == 0) {
      CXET_CHECK(ping_sim.extra == 1 && jsonl.find("\"extra_tls_handoff_ns\":null") == std::string::npos);
      CXET_CHECK(jsonl.find("\"extra_matching_ack_ns\":null") == std::string::npos);
    }
    if (mode == 1 || mode == 9 || mode == 10 || mode == 11 || mode == 12 || mode == 15)
      CXET_CHECK(ping_sim.extra == 1);
    if (mode == 10 || mode == 11 || mode == 12) {
      CXET_CHECK(jsonl.find("\"ack_deadline_exclusive\":true") != std::string::npos);
      CXET_CHECK(jsonl.find("\"extra_matching_ack_ns\":null") != std::string::npos);
    }
    if (mode == 2 || mode == 3 || mode == 5 || mode == 6 || mode == 8 || mode == 13 || mode == 14 || mode == 16)
      CXET_CHECK(ping_sim.extra == 0);
    if (mode == 8 || mode == 16) CXET_CHECK(ping_sim.standby_after_primary_error == 0);
    if (mode == 15) CXET_CHECK(ping_sim.delayed_goaway && ping_sim.now >= ping_sim.ack_time + 3'000'000'000ULL);
  }
}
} // namespace

#include "MarketSourceCases.ipp"

int main(int argc, char** argv) {
  const cxet::testing::Case cases[]{
      {"tbank.lifecycle.market_sources_scenarios", market_sources_scenarios},
      {"tbank.lifecycle.missing_factory_refuses_before_io", missing_factory_refuses_before_io},
      {"tbank.lifecycle.simulated_goaway", simulated_lifecycle},
      {"tbank.lifecycle.lifetime_stops_and_privacy", lifetime_stops_and_privacy},
      {"tbank.lifecycle.ping_handoff_scenarios", ping_handoff_scenarios},
  };
  return cxet::testing::runCases(argc, argv, cases);
}
