#include "exchange_probe/Research.hpp"

#include "NetCommon.hpp"

#include <boost/asio/connect.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ssl/context.hpp>
#include <boost/beast/core/buffers_to_string.hpp>
#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/websocket/ssl.hpp>
#include <boost/json/array.hpp>
#include <boost/json/object.hpp>
#include <boost/json/serialize.hpp>
#include <boost/system/error_code.hpp>

#include <algorithm>
#include <atomic>
#include <barrier>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <ostream>
#include <spawn.h>
#include <string>
#include <string_view>
#include <sys/types.h>
#include <sys/wait.h>
#include <thread>
#include <utility>
#include <vector>

#include <csignal>

extern char** environ;

namespace exchange_probe {
namespace {

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace ssl = asio::ssl;
namespace websocket = beast::websocket;
using WebSocket = websocket::stream<net_detail::TlsStream>;

class PcapOwner {
 public:
  PcapOwner() = default;
  PcapOwner(const PcapOwner&) = delete;
  PcapOwner& operator=(const PcapOwner&) = delete;

  ~PcapOwner() { static_cast<void>(stop()); }

  [[nodiscard]] bool start(
      const std::filesystem::path& directory,
      std::chrono::seconds duration,
      std::uint64_t maximum_bytes,
      std::string_view capture_filter,
      std::string& error) {
    const auto output = (directory / "capture.pcapng").string();
    const auto duration_argument =
        "duration:" + std::to_string(std::max<std::int64_t>(
                          1, duration.count()));
    const auto maximum_kib =
        std::max<std::uint64_t>(1U, maximum_bytes / 1024U);
    const auto size_argument = "filesize:" + std::to_string(maximum_kib);
    std::vector<std::string> arguments{
        "dumpcap", "-q", "-i", "any", "-f", std::string{capture_filter},
        "-w", output,
        "-a", duration_argument, "-a", size_argument,
    };
    std::vector<char*> argv;
    argv.reserve(arguments.size() + 1U);
    for (auto& argument : arguments) argv.push_back(argument.data());
    argv.push_back(nullptr);
    const int status =
        posix_spawnp(&pid_, "dumpcap", nullptr, nullptr, argv.data(), environ);
    if (status != 0) {
      pid_ = -1;
      error = "dumpcap_start_failed:" + std::to_string(status);
      return false;
    }
    return true;
  }

  [[nodiscard]] bool stop() noexcept {
    if (pid_ <= 0) return true;
    int status = 0;
    const auto completed = waitpid(pid_, &status, WNOHANG);
    if (completed == 0) {
      static_cast<void>(kill(pid_, SIGINT));
      if (waitpid(pid_, &status, 0) < 0) {
        pid_ = -1;
        return false;
      }
    } else if (completed < 0) {
      pid_ = -1;
      return false;
    }
    pid_ = -1;
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
  }

 private:
  pid_t pid_{-1};
};

[[nodiscard]] std::uint64_t monotonic_ns(
    std::chrono::steady_clock::time_point origin) noexcept {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now() - origin)
          .count());
}

[[nodiscard]] std::uint64_t utc_ns() noexcept {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());
}

[[nodiscard]] std::string replace_symbol(
    std::string value,
    std::string_view symbol) {
  constexpr std::string_view token{"{symbol}"};
  std::size_t position = 0U;
  while ((position = value.find(token, position)) != std::string::npos) {
    value.replace(position, token.size(), symbol);
    position += symbol.size();
  }
  return value;
}

[[nodiscard]] std::string control_kind(
    websocket::frame_type kind) {
  switch (kind) {
    case websocket::frame_type::close: return "close";
    case websocket::frame_type::ping: return "ping";
    case websocket::frame_type::pong: return "pong";
  }
  return "unknown";
}

[[nodiscard]] std::string hex_encode(std::string_view value) {
  static constexpr char digits[] = "0123456789abcdef";
  std::string result;
  result.resize(value.size() * 2U);
  for (std::size_t index = 0U; index < value.size(); ++index) {
    const auto byte = static_cast<unsigned char>(value[index]);
    result[index * 2U] = digits[byte >> 4U];
    result[index * 2U + 1U] = digits[byte & 0x0fU];
  }
  return result;
}

class ResearchBundleWriter {
 public:
  ResearchBundleWriter(
      std::filesystem::path directory,
      const CliOptions& options,
      const ResearchProductProfile& profile,
      std::string symbol,
      std::chrono::steady_clock::time_point origin)
      : directory_(std::move(directory)),
        byte_limit_(options.max_artifact_bytes),
        pcap_requested_(options.capture_pcap),
        tls_keylog_requested_(options.capture_tls_keys),
        profile_(profile),
        symbol_(std::move(symbol)),
        origin_(origin),
        run_id_(
            profile_.venue + "-" + profile_.product + "-" +
            std::to_string(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch())
                    .count())) {
    std::error_code error;
    std::filesystem::create_directories(directory_, error);
    if (error) {
      error_ = "output_directory_create_failed:" + error.message();
      return;
    }
    const auto begin =
        std::filesystem::directory_iterator{directory_, error};
    if (error) {
      error_ = "output_directory_inspection_failed:" + error.message();
      return;
    }
    if (begin != std::filesystem::directory_iterator{}) {
      error_ = "output_directory_not_empty";
      return;
    }
    frames_.open(directory_ / "frames.bin", std::ios::binary);
    frame_index_.open(directory_ / "frames.jsonl", std::ios::binary);
    controls_.open(directory_ / "controls.jsonl", std::ios::binary);
    sessions_.open(directory_ / "sessions.jsonl", std::ios::binary);
    if (!frames_ || !frame_index_ || !controls_ || !sessions_) {
      error_ = "bundle_files_open_failed";
      return;
    }
    write_manifest("running", false);
  }

  [[nodiscard]] bool ok() const noexcept { return error_.empty(); }
  [[nodiscard]] const std::string& error() const noexcept { return error_; }
  [[nodiscard]] bool complete() const noexcept { return finished_successfully_; }
  [[nodiscard]] std::uint64_t frame_count() const noexcept {
    return next_frame_id_.load(std::memory_order_relaxed);
  }

  void session(
      unsigned round,
      std::string_view channel,
      std::string_view stage,
      bool ok,
      std::string_view error,
      const TransportMetadata* transport = nullptr) {
    boost::json::object row{
        {"schema", "exchange.api_probe.session.v1"},
        {"round", round},
        {"channel_id", channel},
        {"stage", stage},
        {"ok", ok},
        {"error", error},
        {"monotonic_ns", monotonic_ns(origin_)},
        {"utc_ns", utc_ns()},
    };
    if (transport != nullptr) {
      row["remote_ip"] = transport->remote_ip;
      row["ip_family"] = transport->ip_family;
      row["tls_version"] = transport->tls_version;
      row["tls_cipher"] = transport->tls_cipher;
      row["alpn"] = transport->alpn;
      row["tcp_rtt_us"] = transport->tcp_rtt_us;
      row["tcp_retransmits"] = transport->tcp_retransmits;
    }
    const auto line = boost::json::serialize(row) + "\n";
    std::scoped_lock lock{mutex_};
    if (!reserve_bytes(line.size())) return;
    sessions_ << line;
    if (!sessions_) mark_storage_failure();
  }

  void control(
      unsigned round,
      std::string_view channel,
      std::string_view direction,
      std::string_view kind,
      std::string_view payload,
      bool send_time_observed) {
    boost::json::object row{
        {"schema", "exchange.api_probe.control_frame.v1"},
        {"round", round},
        {"channel_id", channel},
        {"direction", direction},
        {"control_kind", kind},
        {"payload_encoding", "hex"},
        {"payload_hex", hex_encode(payload)},
        {"monotonic_ns", monotonic_ns(origin_)},
        {"utc_ns", utc_ns()},
        {"send_time_observed", send_time_observed},
    };
    const auto line = boost::json::serialize(row) + "\n";
    std::scoped_lock lock{mutex_};
    if (!reserve_bytes(line.size())) return;
    controls_ << line;
    if (!controls_) mark_storage_failure();
  }

  void frame(
      unsigned round,
      std::string_view channel,
      bool binary,
      std::string_view payload) {
    std::scoped_lock lock{mutex_};
    const auto frame_id =
        next_frame_id_.load(std::memory_order_relaxed);
    const auto offset = frame_bytes_;
    boost::json::object row{
        {"schema", kResearchFrameSchema},
        {"frame_id", frame_id},
        {"round", round},
        {"channel_id", channel},
        {"direction", "inbound"},
        {"opcode", binary ? "binary" : "text"},
        {"monotonic_ns", monotonic_ns(origin_)},
        {"utc_ns", utc_ns()},
        {"offset", offset},
        {"length", payload.size()},
    };
    const auto line = boost::json::serialize(row) + "\n";
    if (!reserve_bytes(payload.size() + line.size())) return;
    next_frame_id_.fetch_add(1U, std::memory_order_relaxed);
    frames_.write(payload.data(), static_cast<std::streamsize>(payload.size()));
    frame_index_ << line;
    if (frames_ && frame_index_) {
      frame_bytes_ += payload.size();
    } else {
      mark_storage_failure();
    }
  }

  [[nodiscard]] bool finish(bool sessions_ok) {
    std::scoped_lock lock{mutex_};
    frames_.flush();
    frame_index_.flush();
    controls_.flush();
    sessions_.flush();
    if (!frames_ || !frame_index_ || !controls_ || !sessions_) {
      mark_storage_failure();
    }
    const bool intended_success =
        sessions_ok && artifact_complete_ && !storage_failed_;
    write_manifest(
        intended_success ? "complete"
                         : sessions_ok ? "incomplete" : "failed",
        intended_success);
    const bool success = intended_success && !storage_failed_;
    if (success) {
      std::ofstream marker{directory_ / "COMPLETE", std::ios::binary};
      marker << "exchange.api_probe.bundle.v3\n";
      marker.flush();
      if (!marker) {
        storage_failed_ = true;
        finished_successfully_ = false;
        return false;
      }
    }
    finished_successfully_ = success;
    return success;
  }

 private:
  [[nodiscard]] bool reserve_bytes(std::size_t amount) {
    if (!artifact_complete_ ||
        bytes_written_ + static_cast<std::uint64_t>(amount) > byte_limit_) {
      artifact_complete_ = false;
      ++records_not_persisted_;
      return false;
    }
    bytes_written_ += static_cast<std::uint64_t>(amount);
    return true;
  }

  void mark_storage_failure() noexcept {
    storage_failed_ = true;
    artifact_complete_ = false;
  }

  void write_manifest(std::string_view status, bool complete) {
    boost::json::array channels;
    for (const auto& channel : profile_.channels) {
      channels.emplace_back(boost::json::object{
          {"id", channel.id},
          {"kind", to_string(channel.kind)},
          {"support", to_string(channel.support)},
          {"transport", channel.transport},
          {"wire", channel.wire},
          {"host", channel.host},
          {"path", channel.path},
          {"depth_semantics", channel.depth_semantics},
          {"depth_levels", channel.depth_levels},
          {"adapter", channel.adapter},
          {"mapping",
           boost::json::object{
               {"data", channel.mapping.data_path},
               {"symbol", channel.mapping.symbol_path},
               {"event_time", channel.mapping.event_time_path},
               {"event_time_unit", channel.mapping.event_time_unit},
               {"transaction_time", channel.mapping.transaction_time_path},
               {"transaction_time_unit",
                channel.mapping.transaction_time_unit},
               {"event_id", channel.mapping.event_id_path},
               {"first_event_id", channel.mapping.first_event_id_path},
               {"previous_event_id", channel.mapping.previous_event_id_path},
               {"bids", channel.mapping.bids_path},
               {"asks", channel.mapping.asks_path},
               {"bid_price", channel.mapping.bid_price_path},
               {"bid_quantity", channel.mapping.bid_quantity_path},
               {"ask_price", channel.mapping.ask_price_path},
               {"ask_quantity", channel.mapping.ask_quantity_path},
               {"trades", channel.mapping.trades_path},
               {"price", channel.mapping.price_path},
               {"quantity", channel.mapping.quantity_path},
               {"side", channel.mapping.side_path},
               {"snapshot", channel.mapping.snapshot_path},
               {"snapshot_value", channel.mapping.snapshot_value},
           }},
      });
    }
    boost::json::object manifest{
        {"schema", kResearchBundleSchema},
        {"schema_version", 3},
        {"run_id", run_id_},
        {"owner", "research"},
        {"status", status},
        {"artifact_complete", complete},
        {"venue", profile_.venue},
        {"product", profile_.product},
        {"symbol", symbol_},
        {"topology", "separate_sessions"},
        {"frames", next_frame_id_.load(std::memory_order_relaxed)},
        {"frame_bytes", frame_bytes_},
        {"record_byte_limit", byte_limit_},
        {"record_bytes_written", bytes_written_},
        {"pcap_requested", pcap_requested_},
        {"tls_keylog_requested", tls_keylog_requested_},
        {"records_not_persisted", records_not_persisted_},
        {"channels", std::move(channels)},
        {"clock_domains",
         boost::json::object{
             {"local_monotonic", "nanoseconds_from_run_start"},
             {"local_utc", "unix_epoch_nanoseconds"},
             {"exchange_event", "profile_declared_or_null"},
             {"exchange_transaction", "profile_declared_or_null"},
         }},
        {"evidence_boundary",
         "diagnostic observation; not exchange backend, matching engine, "
         "trading readiness or causal proof"},
    };
    const auto temporary = directory_ / "manifest.json.tmp";
    std::ofstream file{temporary, std::ios::binary};
    file << boost::json::serialize(manifest) << '\n';
    file.flush();
    if (!file) {
      error_ = "manifest_write_failed";
      mark_storage_failure();
      return;
    }
    file.close();
    std::error_code rename_error;
    std::filesystem::rename(
        temporary, directory_ / "manifest.json", rename_error);
    if (rename_error) {
      error_ = "manifest_replace_failed:" + rename_error.message();
      mark_storage_failure();
    }
  }

  std::filesystem::path directory_;
  std::uint64_t byte_limit_{0U};
  std::uint64_t bytes_written_{0U};
  std::uint64_t frame_bytes_{0U};
  std::uint64_t records_not_persisted_{0U};
  bool pcap_requested_{false};
  bool tls_keylog_requested_{false};
  std::atomic<std::uint64_t> next_frame_id_{0U};
  bool artifact_complete_{true};
  bool storage_failed_{false};
  bool finished_successfully_{false};
  std::ofstream frames_;
  std::ofstream frame_index_;
  std::ofstream controls_;
  std::ofstream sessions_;
  const ResearchProductProfile& profile_;
  std::string symbol_;
  std::chrono::steady_clock::time_point origin_;
  std::string run_id_;
  std::mutex mutex_;
  std::string error_;
};

[[nodiscard]] bool selected_channel(
    const CliOptions& options,
    const ResearchChannel& channel) {
  if (!options.channels.empty()) {
    return std::find(
               options.channels.begin(),
               options.channels.end(),
               channel.id) != options.channels.end();
  }
  return channel.transport == "ws" &&
         channel.support != ResearchSupport::Candidate &&
         channel.support != ResearchSupport::Unavailable;
}

void capture_channel(
    const CliOptions& options,
    const ResearchChannel& channel,
    std::string_view symbol,
    unsigned round,
    std::chrono::seconds capture_duration,
    std::barrier<>& start_barrier,
    ResearchBundleWriter& bundle,
    std::atomic<bool>& any_failure) {
  bool barrier_arrived = false;
  const auto synchronize_start = [&] {
    start_barrier.arrive_and_wait();
    barrier_arrived = true;
  };
  if (channel.transport != "ws" ||
      channel.support == ResearchSupport::Unavailable ||
      channel.support == ResearchSupport::Candidate) {
    bundle.session(
        round,
        channel.id,
        "profile",
        false,
        "channel_not_capturable");
    any_failure.store(true, std::memory_order_relaxed);
    synchronize_start();
    return;
  }
  try {
    asio::io_context context;
    ssl::context tls_context{ssl::context::tls_client};
    boost::system::error_code setup_error;
    tls_context.set_default_verify_paths(setup_error);
    if (setup_error) {
      bundle.session(
          round, channel.id, "tls_configuration", false,
          "default_ca_paths_failed:" + setup_error.message());
      any_failure.store(true, std::memory_order_relaxed);
      synchronize_start();
      return;
    }
    tls_context.set_options(
        ssl::context::default_workarounds |
        ssl::context::no_sslv2 |
        ssl::context::no_sslv3);
    const auto connect_deadline =
        std::chrono::steady_clock::now() + options.limits.timeout;
    auto resolved = net_detail::resolve(
        context,
        channel.host,
        std::to_string(channel.port),
        connect_deadline);
    if (!resolved.ok) {
      bundle.session(
          round, channel.id, "dns", false, resolved.error);
      any_failure.store(true, std::memory_order_relaxed);
      synchronize_start();
      return;
    }
    WebSocket stream{context, tls_context};
    stream.read_message_max(kMaxWsMessageBytes);
    std::string error;
    if (!net_detail::connect(
            context,
            beast::get_lowest_layer(stream),
            resolved.endpoints,
            connect_deadline,
            error) ||
        !net_detail::configure_tls(stream.next_layer(), channel.host, error) ||
        !net_detail::tls_handshake(
            context, stream.next_layer(), connect_deadline, error)) {
      bundle.session(round, channel.id, "transport", false, error);
      any_failure.store(true, std::memory_order_relaxed);
      synchronize_start();
      return;
    }
    const auto metadata = net_detail::transport_metadata(stream.next_layer());
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
              "exchange-api-probe/4");
        }));
    stream.control_callback(
        [&](websocket::frame_type kind, beast::string_view payload) {
          bundle.control(
              round,
              channel.id,
              "inbound",
              control_kind(kind),
              std::string_view{payload.data(), payload.size()},
              true);
          if (kind == websocket::frame_type::ping) {
            bundle.control(
                round,
                channel.id,
                "outbound",
                "pong",
                std::string_view{payload.data(), payload.size()},
                false);
          }
        });
    const auto path = replace_symbol(channel.path, symbol);
    boost::system::error_code operation_error;
    stream.handshake(channel.host, path, operation_error);
    if (operation_error) {
      bundle.session(
          round, channel.id, "ws_handshake", false,
          operation_error.message(), &metadata);
      any_failure.store(true, std::memory_order_relaxed);
      synchronize_start();
      return;
    }
    bundle.session(
        round, channel.id, "connected", true, "", &metadata);
    synchronize_start();
    const auto capture_deadline =
        std::chrono::steady_clock::now() + capture_duration;
    const auto subscription = replace_symbol(channel.subscribe, symbol);
    if (!subscription.empty()) {
      stream.binary(channel.subscribe_binary);
      bundle.control(
          round, channel.id, "outbound", "subscription", subscription, true);
      stream.write(asio::buffer(subscription), operation_error);
      if (operation_error) {
        bundle.session(
            round, channel.id, "subscribe_write", false,
            operation_error.message(), &metadata);
        any_failure.store(true, std::memory_order_relaxed);
        return;
      }
    }
    std::uint64_t frames_read = 0U;
    while (std::chrono::steady_clock::now() < capture_deadline) {
      const auto remaining =
          capture_deadline - std::chrono::steady_clock::now();
      beast::get_lowest_layer(stream).expires_after(
          std::min(
              std::chrono::duration_cast<std::chrono::seconds>(remaining),
              std::chrono::seconds{30}));
      beast::flat_buffer buffer{kMaxWsMessageBytes};
      stream.read(buffer, operation_error);
      if (operation_error) {
        if (operation_error != websocket::error::closed &&
            std::chrono::steady_clock::now() < capture_deadline) {
          bundle.session(
              round, channel.id, "read", false,
              operation_error.message(), &metadata);
          any_failure.store(true, std::memory_order_relaxed);
        }
        break;
      }
      const auto payload = beast::buffers_to_string(buffer.data());
      bundle.frame(round, channel.id, stream.got_binary(), payload);
      ++frames_read;
    }
    boost::system::error_code ignored;
    stream.close(websocket::close_code::normal, ignored);
    const bool received_data = frames_read != 0U;
    bundle.session(
        round, channel.id, "complete", received_data,
        received_data ? std::string_view{} : std::string_view{"no_frames"},
        &metadata);
    if (!received_data) {
      any_failure.store(true, std::memory_order_relaxed);
    }
  } catch (const std::exception& exception) {
    bundle.session(
        round, channel.id, "exception", false, exception.what());
    any_failure.store(true, std::memory_order_relaxed);
    if (!barrier_arrived) synchronize_start();
  }
}

}  // namespace

int capture_research_bundle(
    const CliOptions& options,
    const ResearchProductProfile& profile,
    const std::filesystem::path& directory,
    std::ostream& output,
    std::ostream& error_output) {
  if (options.surface == Surface::Private) {
    error_output
        << "research_error: private session capture requires a complete "
           "read-only lifecycle profile; no private research channel is "
           "declared in profile schema v1\n";
    return 2;
  }
  if (options.capture_tls_keys) {
    error_output
        << "research_error: TLS key logging has no configured key owner; "
           "capture refuses to expose keys implicitly\n";
    return 2;
  }
  std::vector<const ResearchChannel*> channels;
  for (const auto& requested : options.channels) {
    if (find_research_channel(profile, requested) == nullptr) {
      error_output << "research_error: unknown_channel:" << requested
                   << '\n';
      return 2;
    }
  }
  for (const auto& channel : profile.channels) {
    if (selected_channel(options, channel)) channels.push_back(&channel);
  }
  if (channels.empty()) {
    error_output << "research_error: no channels selected\n";
    return 2;
  }
  if (channels.size() > 8U && !options.confirm_load) {
    error_output
        << "research_error: more than 8 simultaneous channels requires "
           "--confirm-load\n";
    return 2;
  }
  const auto symbol =
      options.symbol.empty() ? profile.default_symbol : options.symbol;
  const auto origin = std::chrono::steady_clock::now();
  auto bundle_options = options;
  const auto pcap_budget =
      options.capture_pcap ? options.max_artifact_bytes / 2U : 0U;
  if (options.capture_pcap) {
    bundle_options.max_artifact_bytes -= pcap_budget;
  }
  ResearchBundleWriter bundle{
      directory, bundle_options, profile, symbol, origin};
  if (!bundle.ok()) {
    error_output << "research_error: " << bundle.error() << '\n';
    return 2;
  }
  std::atomic<bool> any_failure{false};
  const auto duration = std::chrono::seconds{
      options.duration_seconds.value_or(300U)};
  PcapOwner pcap;
  if (options.capture_pcap) {
    std::string pcap_filter;
    for (const auto* channel : channels) {
      const bool safe_host =
          !channel->host.empty() &&
          std::all_of(
              channel->host.begin(), channel->host.end(), [](char value) {
                return (value >= 'a' && value <= 'z') ||
                       (value >= 'A' && value <= 'Z') ||
                       (value >= '0' && value <= '9') ||
                       value == '.' || value == '-' || value == ':';
              });
      if (!safe_host) {
        error_output << "research_error: unsafe_pcap_host:"
                     << channel->host << '\n';
        static_cast<void>(bundle.finish(false));
        return 2;
      }
      if (!pcap_filter.empty()) pcap_filter += " or ";
      pcap_filter += "(host " + channel->host + " and tcp port " +
                     std::to_string(channel->port) + ")";
    }
    std::string pcap_error;
    const auto total_duration =
        (duration +
         std::chrono::duration_cast<std::chrono::seconds>(
             options.limits.timeout)) *
        static_cast<std::chrono::seconds::rep>(options.rounds);
    if (!pcap.start(
            directory, total_duration, pcap_budget, pcap_filter,
            pcap_error)) {
      error_output << "research_error: " << pcap_error << '\n';
      static_cast<void>(bundle.finish(false));
      return 2;
    }
    bundle.session(0U, "pcap", "started", true, "");
  }
  for (unsigned round = 1U; round <= options.rounds; ++round) {
    std::barrier start_barrier{
        static_cast<std::ptrdiff_t>(channels.size())};
    std::vector<std::thread> workers;
    workers.reserve(channels.size());
    const bool reverse = round % 2U == 0U;
    bool thread_start_failed = false;
    for (std::size_t index = 0U; index < channels.size(); ++index) {
      const auto selected_index =
          reverse ? channels.size() - 1U - index : index;
      try {
        workers.emplace_back(
            capture_channel,
            std::cref(options),
            std::cref(*channels[selected_index]),
            std::string_view{symbol},
            round,
            duration,
            std::ref(start_barrier),
            std::ref(bundle),
            std::ref(any_failure));
      } catch (const std::exception& exception) {
        const auto missing = channels.size() - index;
        for (std::size_t count = 0U; count < missing; ++count) {
          start_barrier.arrive_and_drop();
        }
        bundle.session(
            round, "supervisor", "thread_start", false, exception.what());
        any_failure.store(true, std::memory_order_relaxed);
        thread_start_failed = true;
        break;
      }
    }
    for (auto& worker : workers) worker.join();
    if (thread_start_failed) break;
  }
  if (options.capture_pcap) {
    const bool pcap_ok = pcap.stop();
    bundle.session(
        0U, "pcap", "complete", pcap_ok,
        pcap_ok ? std::string_view{} : std::string_view{"dumpcap_failed"});
    if (!pcap_ok) any_failure.store(true, std::memory_order_relaxed);
  }
  const bool finished =
      bundle.finish(!any_failure.load(std::memory_order_relaxed));
  output << "research_bundle=" << directory.string()
         << " frames=" << bundle.frame_count()
         << " artifact_complete=" << bundle.complete() << '\n';
  return finished ? 0 : 1;
}

}  // namespace exchange_probe
