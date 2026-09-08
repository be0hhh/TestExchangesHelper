#include "exchange_probe/Net.hpp"
#include "NetCommon.hpp"

#include <boost/asio/ssl/context.hpp>
#include <boost/beast/core/buffers_to_string.hpp>
#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/websocket/ssl.hpp>

#include <algorithm>
#include <atomic>
#include <barrier>
#include <charconv>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <latch>
#include <map>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

namespace {

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace ssl = asio::ssl;
namespace websocket = beast::websocket;
using WebSocket = websocket::stream<exchange_probe::net_detail::TlsStream>;
using Clock = std::chrono::steady_clock;

constexpr std::string_view kHost = "fstream.binance.com";
constexpr std::size_t kMaxFrameBytes = 64u * 1024u;
constexpr std::size_t kMaxPendingEvents = 2'000'000u;
constexpr std::uint64_t kRawArtifactLimit = 512ull * 1024ull * 1024ull;

struct Options {
  std::string symbol{"BANKUSDT"};
  unsigned duration_seconds{600u};
  unsigned warmup_seconds{30u};
  std::filesystem::path output_dir{"results/binance-ws-race"};
  bool self_test{false};
};

struct TradeEvent {
  std::uint64_t id{0u};
  std::uint64_t event_ms{0u};
  std::uint64_t trade_ms{0u};
  std::uint64_t receive_ns{0u};
};

struct AggEvent {
  std::uint64_t id{0u};
  std::uint64_t first_id{0u};
  std::uint64_t last_id{0u};
  std::uint64_t event_ms{0u};
  std::uint64_t trade_ms{0u};
  std::uint64_t receive_ns{0u};
};

struct Match {
  unsigned round{0u};
  std::uint64_t agg_id{0u};
  std::uint64_t first_id{0u};
  std::uint64_t last_id{0u};
  std::uint64_t group_size{0u};
  std::int64_t first_advantage_ns{0};
  std::int64_t completion_advantage_ns{0};
  std::uint64_t agg_receive_ns{0u};
  std::uint64_t raw_first_receive_ns{0u};
  std::uint64_t raw_last_receive_ns{0u};
};

struct ChannelStats {
  std::uint64_t frames{0u};
  std::uint64_t events{0u};
  std::uint64_t parse_errors{0u};
  std::uint64_t duplicates{0u};
  std::uint64_t out_of_order{0u};
  std::uint64_t id_gaps{0u};
  std::uint64_t disconnects{0u};
  std::uint64_t control_pings{0u};
  std::uint64_t max_inter_event_ns{0u};
  std::uint64_t previous_id{0u};
  std::uint64_t previous_receive_ns{0u};
};

struct RunStats {
  ChannelStats raw;
  ChannelStats agg;
  std::uint64_t raw_artifact_bytes{0u};
  std::uint64_t raw_artifact_dropped{0u};
  std::uint64_t pending_overflow{0u};
  std::uint64_t unmatched_aggregates{0u};
  std::vector<Match> matches;
};

[[nodiscard]] std::uint64_t monotonic_ns() noexcept {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          Clock::now().time_since_epoch()).count());
}

[[nodiscard]] std::string json_escape(std::string_view input) {
  std::string output;
  output.reserve(input.size() + 16u);
  for (const char c : input) {
    switch (c) {
      case '\\': output += "\\\\"; break;
      case '"': output += "\\\""; break;
      case '\n': output += "\\n"; break;
      case '\r': output += "\\r"; break;
      case '\t': output += "\\t"; break;
      default:
        if (static_cast<unsigned char>(c) < 0x20u) {
          output += '?';
        } else {
          output += c;
        }
    }
  }
  return output;
}

[[nodiscard]] std::optional<std::string_view> field_value(
    std::string_view json,
    std::string_view key) noexcept {
  char needle[32]{};
  if (key.size() + 3u > sizeof(needle)) return std::nullopt;
  needle[0] = '"';
  std::copy(key.begin(), key.end(), needle + 1);
  needle[key.size() + 1u] = '"';
  needle[key.size() + 2u] = ':';
  const std::string_view marker{needle, key.size() + 3u};
  const auto pos = json.find(marker);
  if (pos == std::string_view::npos) return std::nullopt;
  std::size_t begin = pos + marker.size();
  while (begin < json.size() && (json[begin] == ' ' || json[begin] == '\t')) ++begin;
  if (begin >= json.size()) return std::nullopt;
  if (json[begin] == '"') {
    const auto end = json.find('"', begin + 1u);
    if (end == std::string_view::npos) return std::nullopt;
    return json.substr(begin + 1u, end - begin - 1u);
  }
  std::size_t end = begin;
  while (end < json.size() && json[end] != ',' && json[end] != '}' &&
         json[end] != ' ' && json[end] != '\r' && json[end] != '\n') {
    ++end;
  }
  return json.substr(begin, end - begin);
}

[[nodiscard]] bool parse_u64(
    std::string_view json,
    std::string_view key,
    std::uint64_t& output) noexcept {
  const auto value = field_value(json, key);
  if (!value || value->empty()) return false;
  const auto result =
      std::from_chars(value->data(), value->data() + value->size(), output);
  return result.ec == std::errc{} &&
         result.ptr == value->data() + value->size();
}

[[nodiscard]] bool field_equals(
    std::string_view json,
    std::string_view key,
    std::string_view expected) noexcept {
  const auto value = field_value(json, key);
  return value && *value == expected;
}

[[nodiscard]] bool parse_trade(
    std::string_view json,
    std::uint64_t receive_ns,
    TradeEvent& event) noexcept {
  return field_equals(json, "e", "trade") &&
         parse_u64(json, "t", event.id) &&
         parse_u64(json, "E", event.event_ms) &&
         parse_u64(json, "T", event.trade_ms) &&
         ((event.receive_ns = receive_ns), true);
}

[[nodiscard]] bool parse_agg(
    std::string_view json,
    std::uint64_t receive_ns,
    AggEvent& event) noexcept {
  return field_equals(json, "e", "aggTrade") &&
         parse_u64(json, "a", event.id) &&
         parse_u64(json, "f", event.first_id) &&
         parse_u64(json, "l", event.last_id) &&
         parse_u64(json, "E", event.event_ms) &&
         parse_u64(json, "T", event.trade_ms) &&
         event.first_id <= event.last_id &&
         ((event.receive_ns = receive_ns), true);
}

class Collector {
 public:
  Collector(
      std::ofstream& raw_output,
      std::ofstream& matches_output,
      unsigned warmup_seconds)
      : raw_output_(raw_output),
        matches_output_(matches_output),
        warmup_ns_(static_cast<std::uint64_t>(warmup_seconds) * 1'000'000'000ull) {}

  void start_round(unsigned round) {
    std::scoped_lock lock{mutex_};
    stats_.unmatched_aggregates += pending_agg_.size();
    round_ = round;
    round_start_ns_ = monotonic_ns();
    raw_by_id_.clear();
    pending_agg_.clear();
    raw_.previous_id = 0u;
    agg_.previous_id = 0u;
    raw_.previous_receive_ns = 0u;
    agg_.previous_receive_ns = 0u;
  }

  [[nodiscard]] std::uint64_t round_start_ns() const noexcept {
    return round_start_ns_.load(std::memory_order_acquire);
  }

  void record_frame(
      bool raw_channel,
      std::string_view payload,
      std::uint64_t receive_ns) {
    std::scoped_lock lock{mutex_};
    ChannelStats& channel = raw_channel ? raw_ : agg_;
    ++channel.frames;
    if (stats_.raw_artifact_bytes < kRawArtifactLimit) {
      std::ostringstream line;
      line << "{\"round\":" << round_
           << ",\"channel\":\"" << (raw_channel ? "trade" : "aggTrade")
           << "\",\"receive_ns\":" << receive_ns
           << ",\"payload\":\"" << json_escape(payload) << "\"}\n";
      const std::string rendered = line.str();
      if (stats_.raw_artifact_bytes + rendered.size() <= kRawArtifactLimit) {
        raw_output_ << rendered;
        stats_.raw_artifact_bytes += rendered.size();
      } else {
        ++stats_.raw_artifact_dropped;
      }
    } else {
      ++stats_.raw_artifact_dropped;
    }
    const bool measuring = receive_ns >= round_start_ns_ + warmup_ns_;
    if (raw_channel) {
      TradeEvent event{};
      if (!parse_trade(payload, receive_ns, event)) {
        if (!field_value(payload, "result")) ++channel.parse_errors;
        return;
      }
      if (!measuring) return;
      update_sequence(channel, event.id, receive_ns, true);
      if (raw_by_id_.size() >= kMaxPendingEvents) {
        ++stats_.pending_overflow;
        return;
      }
      if (!raw_by_id_.emplace(event.id, event).second) ++channel.duplicates;
      reconcile();
    } else {
      AggEvent event{};
      if (!parse_agg(payload, receive_ns, event)) {
        if (!field_value(payload, "result")) ++channel.parse_errors;
        return;
      }
      if (!measuring) return;
      update_sequence(channel, event.id, receive_ns, false);
      if (pending_agg_.size() >= kMaxPendingEvents) {
        ++stats_.pending_overflow;
        return;
      }
      if (!pending_agg_.emplace(event.id, event).second) ++channel.duplicates;
      reconcile();
    }
  }

  void record_disconnect(bool raw_channel) {
    std::scoped_lock lock{mutex_};
    ++(raw_channel ? raw_.disconnects : agg_.disconnects);
  }

  void record_ping(bool raw_channel) {
    std::scoped_lock lock{mutex_};
    ++(raw_channel ? raw_.control_pings : agg_.control_pings);
  }

  RunStats finish() {
    std::scoped_lock lock{mutex_};
    stats_.unmatched_aggregates += pending_agg_.size();
    stats_.raw = raw_;
    stats_.agg = agg_;
    return stats_;
  }

 private:
  static void update_sequence(
      ChannelStats& stats,
      std::uint64_t id,
      std::uint64_t receive_ns,
      bool exact_contiguous) {
    ++stats.events;
    if (stats.previous_receive_ns != 0u) {
      stats.max_inter_event_ns = std::max(
          stats.max_inter_event_ns, receive_ns - stats.previous_receive_ns);
    }
    if (stats.previous_id != 0u) {
      if (id <= stats.previous_id) {
        ++stats.out_of_order;
      } else if (exact_contiguous && id != stats.previous_id + 1u) {
        stats.id_gaps += id - stats.previous_id - 1u;
      } else if (!exact_contiguous && id != stats.previous_id + 1u) {
        ++stats.id_gaps;
      }
    }
    stats.previous_id = std::max(stats.previous_id, id);
    stats.previous_receive_ns = receive_ns;
  }

  void reconcile() {
    for (auto iterator = pending_agg_.begin(); iterator != pending_agg_.end();) {
      const AggEvent& agg = iterator->second;
      const auto first = raw_by_id_.find(agg.first_id);
      const auto last = raw_by_id_.find(agg.last_id);
      if (first == raw_by_id_.end() || last == raw_by_id_.end()) {
        ++iterator;
        continue;
      }
      Match match{
          .round = round_,
          .agg_id = agg.id,
          .first_id = agg.first_id,
          .last_id = agg.last_id,
          .group_size = agg.last_id - agg.first_id + 1u,
          .first_advantage_ns =
              static_cast<std::int64_t>(agg.receive_ns) -
              static_cast<std::int64_t>(first->second.receive_ns),
          .completion_advantage_ns =
              static_cast<std::int64_t>(agg.receive_ns) -
              static_cast<std::int64_t>(last->second.receive_ns),
          .agg_receive_ns = agg.receive_ns,
          .raw_first_receive_ns = first->second.receive_ns,
          .raw_last_receive_ns = last->second.receive_ns,
      };
      stats_.matches.push_back(match);
      matches_output_
          << match.round << ',' << match.agg_id << ',' << match.first_id << ','
          << match.last_id << ',' << match.group_size << ','
          << match.first_advantage_ns << ',' << match.completion_advantage_ns
          << ',' << match.agg_receive_ns << ',' << match.raw_first_receive_ns
          << ',' << match.raw_last_receive_ns << '\n';
      for (std::uint64_t id = agg.first_id; id <= agg.last_id; ++id) {
        raw_by_id_.erase(id);
        if (id == std::numeric_limits<std::uint64_t>::max()) break;
      }
      iterator = pending_agg_.erase(iterator);
    }
  }

  mutable std::mutex mutex_;
  std::ofstream& raw_output_;
  std::ofstream& matches_output_;
  const std::uint64_t warmup_ns_;
  unsigned round_{0u};
  std::atomic<std::uint64_t> round_start_ns_{0u};
  ChannelStats raw_{};
  ChannelStats agg_{};
  RunStats stats_{};
  std::unordered_map<std::uint64_t, TradeEvent> raw_by_id_;
  std::map<std::uint64_t, AggEvent> pending_agg_;
};

[[nodiscard]] bool connect_and_subscribe(
    asio::io_context& context,
    WebSocket& stream,
    std::string_view path,
    std::string_view topic,
    std::string& error) {
  const auto deadline = Clock::now() + std::chrono::seconds{20};
  const auto proxy = exchange_probe::proxy_for_host(kHost);
  if (proxy.enabled && !proxy.valid) {
    error = proxy.error;
    return false;
  }
  const std::string connect_host =
      proxy.enabled ? proxy.host : std::string{kHost};
  const std::string connect_port =
      proxy.enabled ? proxy.port : std::string{"443"};
  auto resolved = exchange_probe::net_detail::resolve(
      context, connect_host, connect_port, deadline);
  if (!resolved.ok) {
    error = resolved.error;
    return false;
  }
  if (!exchange_probe::net_detail::connect(
          context, beast::get_lowest_layer(stream), resolved.endpoints,
          deadline, error)) {
    return false;
  }
  if (proxy.enabled &&
      !exchange_probe::net_detail::establish_proxy_tunnel(
          context, beast::get_lowest_layer(stream), proxy,
          kHost, "443", deadline, error)) {
    return false;
  }
  if (!exchange_probe::net_detail::configure_tls(
          stream.next_layer(), kHost, error) ||
      !exchange_probe::net_detail::tls_handshake(
          context, stream.next_layer(), deadline, error)) {
    return false;
  }
  websocket::stream_base::timeout timeout{
      .handshake_timeout = std::chrono::seconds{15},
      .idle_timeout = std::chrono::seconds{90},
      .keep_alive_pings = true,
  };
  stream.set_option(timeout);
  stream.read_message_max(kMaxFrameBytes);
  stream.set_option(websocket::stream_base::decorator(
      [](websocket::request_type& request) {
        request.set(boost::beast::http::field::user_agent,
                    "exchange-api-probe-race/1");
      }));
  beast::get_lowest_layer(stream).expires_never();
  boost::system::error_code operation_error;
  stream.handshake(std::string{kHost}, std::string{path}, operation_error);
  if (operation_error) {
    error = "ws_handshake:" + operation_error.message();
    return false;
  }
  const std::string payload =
      "{\"method\":\"SUBSCRIBE\",\"params\":[\"" + std::string{topic} +
      "\"],\"id\":1}";
  stream.write(asio::buffer(payload), operation_error);
  if (operation_error) {
    error = "subscribe_write:" + operation_error.message();
    return false;
  }
  return true;
}

struct ReaderResult {
  bool connected{false};
  std::string error;
};

void run_reader(
    bool raw_channel,
    std::string symbol_lower,
    std::string path,
    unsigned total_seconds,
    std::latch& connected,
    std::barrier<>& ready,
    Collector& collector,
    ReaderResult& result) {
  try {
    asio::io_context context;
    ssl::context tls_context{ssl::context::tls_client};
    boost::system::error_code setup_error;
    tls_context.set_default_verify_paths(setup_error);
    if (setup_error) {
      result.error = "default_ca_paths:" + setup_error.message();
      connected.count_down();
      ready.arrive_and_drop();
      return;
    }
    tls_context.set_options(
        ssl::context::default_workarounds |
        ssl::context::no_sslv2 |
        ssl::context::no_sslv3);
    WebSocket stream{context, tls_context};
    unsigned pings = 0u;
    stream.control_callback(
        [&](websocket::frame_type kind, beast::string_view) {
          if (kind == websocket::frame_type::ping) {
            ++pings;
            collector.record_ping(raw_channel);
          }
        });
    const std::string topic =
        symbol_lower + (raw_channel ? "@trade" : "@aggTrade");
    if (!connect_and_subscribe(
            context, stream, path, topic, result.error)) {
      connected.count_down();
      ready.arrive_and_drop();
      return;
    }
    result.connected = true;
    connected.count_down();
    ready.arrive_and_wait();
    const std::uint64_t deadline_ns =
        collector.round_start_ns() +
        static_cast<std::uint64_t>(total_seconds) * 1'000'000'000ull;
    while (monotonic_ns() < deadline_ns) {
      beast::flat_buffer buffer{kMaxFrameBytes};
      boost::system::error_code read_error;
      stream.read(buffer, read_error);
      if (read_error) {
        if (monotonic_ns() < deadline_ns) {
          result.error = "read:" + read_error.message();
          collector.record_disconnect(raw_channel);
        }
        break;
      }
      const std::uint64_t receive_ns = monotonic_ns();
      const std::string payload = beast::buffers_to_string(buffer.data());
      collector.record_frame(raw_channel, payload, receive_ns);
    }
    boost::system::error_code ignored;
    beast::get_lowest_layer(stream).socket().shutdown(
        asio::ip::tcp::socket::shutdown_both, ignored);
    beast::get_lowest_layer(stream).socket().close(ignored);
    (void)pings;
  } catch (const std::exception& exception) {
    result.error = std::string{"exception:"} + exception.what();
    collector.record_disconnect(raw_channel);
  }
}

[[nodiscard]] std::int64_t percentile(
    std::vector<std::int64_t> values,
    unsigned basis_points) {
  if (values.empty()) return 0;
  std::sort(values.begin(), values.end());
  const std::size_t index =
      static_cast<std::size_t>(
          (static_cast<unsigned long long>(values.size() - 1u) *
           basis_points) / 10000u);
  return values[index];
}

void write_svg(
    const std::filesystem::path& path,
    std::string_view symbol,
    const std::vector<Match>& matches) {
  std::ofstream out{path};
  std::vector<std::int64_t> values;
  values.reserve(matches.size());
  for (const auto& match : matches) values.push_back(match.first_advantage_ns);
  std::sort(values.begin(), values.end());
  const std::int64_t low = values.empty() ? -1 : percentile(values, 100u);
  const std::int64_t high = values.empty() ? 1 : percentile(values, 9900u);
  const double span = high > low ? static_cast<double>(high - low) : 1.0;
  out << "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"1000\" height=\"560\">"
      << "<rect width=\"100%\" height=\"100%\" fill=\"white\"/>"
      << "<text x=\"60\" y=\"35\" font-family=\"sans-serif\" font-size=\"20\">"
      << symbol << " raw trade first-signal advantage CDF</text>"
      << "<text x=\"60\" y=\"55\" font-family=\"sans-serif\" font-size=\"12\">"
      << "positive = trade arrived earlier; clipped to p1..p99</text>"
      << "<line x1=\"60\" y1=\"500\" x2=\"960\" y2=\"500\" stroke=\"black\"/>"
      << "<line x1=\"60\" y1=\"80\" x2=\"60\" y2=\"500\" stroke=\"black\"/>";
  if (low < 0 && high > 0) {
    const double zero_x = 60.0 + 900.0 * (0.0 - static_cast<double>(low)) / span;
    out << "<line x1=\"" << zero_x << "\" y1=\"80\" x2=\"" << zero_x
        << "\" y2=\"500\" stroke=\"#cc0000\" stroke-dasharray=\"4 4\"/>";
  }
  if (values.size() > 1u) {
    out << "<polyline fill=\"none\" stroke=\"#0066cc\" stroke-width=\"2\" points=\"";
    for (std::size_t index = 0u; index < values.size(); ++index) {
      const auto clipped = std::clamp(values[index], low, high);
      const double x = 60.0 + 900.0 *
          static_cast<double>(clipped - low) / span;
      const double y = 500.0 - 420.0 *
          static_cast<double>(index) /
          static_cast<double>(values.size() - 1u);
      out << x << ',' << y << ' ';
    }
    out << "\"/>";
  }
  out << "<text x=\"60\" y=\"530\" font-family=\"monospace\" font-size=\"12\">"
      << "p1=" << static_cast<double>(low) / 1000.0
      << " us; p99=" << static_cast<double>(high) / 1000.0
      << " us; n=" << values.size() << "</text></svg>\n";
}

void write_summary(
    const std::filesystem::path& directory,
    std::string_view symbol,
    const RunStats& stats,
    std::string_view raw_error,
    std::string_view agg_error) {
  std::vector<std::int64_t> first;
  std::vector<std::int64_t> completion;
  std::uint64_t raw_wins = 0u;
  std::uint64_t agg_wins = 0u;
  std::uint64_t ties = 0u;
  first.reserve(stats.matches.size());
  completion.reserve(stats.matches.size());
  for (const auto& match : stats.matches) {
    first.push_back(match.first_advantage_ns);
    completion.push_back(match.completion_advantage_ns);
    if (match.first_advantage_ns > 0) ++raw_wins;
    else if (match.first_advantage_ns < 0) ++agg_wins;
    else ++ties;
  }
  const auto p50 = percentile(first, 5000u);
  const auto p95 = percentile(first, 9500u);
  const auto p99 = percentile(first, 9900u);
  const auto completion_p50 = percentile(completion, 5000u);
  const bool transport_valid =
      stats.pending_overflow == 0u &&
      stats.raw_artifact_dropped == 0u &&
      stats.raw.disconnects == 0u &&
      stats.agg.disconnects == 0u;
  const bool sequence_clean =
      stats.raw.parse_errors == 0U &&
      stats.raw.id_gaps == 0U &&
      stats.raw.duplicates == 0U &&
      stats.raw.out_of_order == 0U &&
      stats.agg.parse_errors == 0U &&
      stats.agg.id_gaps == 0U &&
      stats.agg.duplicates == 0U &&
      stats.agg.out_of_order == 0U;
  const bool comparison_valid =
      transport_valid && sequence_clean && !stats.matches.empty();
  std::ofstream json{directory / "summary.json"};
  json << "{\n"
       << "  \"schema\":\"exchange.api_probe.binance_ws_race.v1\",\n"
       << "  \"symbol\":\"" << json_escape(symbol) << "\",\n"
       << "  \"transport_valid\":"
       << (transport_valid ? "true" : "false") << ",\n"
       << "  \"sequence_clean\":"
       << (sequence_clean ? "true" : "false") << ",\n"
       << "  \"comparison_valid\":"
       << (comparison_valid ? "true" : "false") << ",\n"
       << "  \"matches\":" << stats.matches.size() << ",\n"
       << "  \"raw_wins\":" << raw_wins << ",\"agg_wins\":" << agg_wins
       << ",\"ties\":" << ties << ",\n"
       << "  \"first_advantage_ns\":{\"p50\":" << p50
       << ",\"p95\":" << p95 << ",\"p99\":" << p99 << "},\n"
       << "  \"completion_advantage_ns\":{\"p50\":" << completion_p50 << "},\n"
       << "  \"trade\":{\"frames\":" << stats.raw.frames
       << ",\"events\":" << stats.raw.events
       << ",\"parse_errors\":" << stats.raw.parse_errors
       << ",\"gaps\":" << stats.raw.id_gaps
       << ",\"duplicates\":" << stats.raw.duplicates
       << ",\"out_of_order\":" << stats.raw.out_of_order
       << ",\"disconnects\":" << stats.raw.disconnects << "},\n"
       << "  \"aggTrade\":{\"frames\":" << stats.agg.frames
       << ",\"events\":" << stats.agg.events
       << ",\"parse_errors\":" << stats.agg.parse_errors
       << ",\"gaps\":" << stats.agg.id_gaps
       << ",\"duplicates\":" << stats.agg.duplicates
       << ",\"out_of_order\":" << stats.agg.out_of_order
       << ",\"disconnects\":" << stats.agg.disconnects << "},\n"
       << "  \"unmatched_aggregates\":" << stats.unmatched_aggregates
       << ",\"pending_overflow\":" << stats.pending_overflow
       << ",\"raw_artifact_dropped\":" << stats.raw_artifact_dropped << ",\n"
       << "  \"trade_error\":\"" << json_escape(raw_error)
       << "\",\"aggTrade_error\":\"" << json_escape(agg_error) << "\"\n}\n";

  std::ofstream report{directory / "REPORT.md"};
  report << "# Binance " << symbol << " `trade` vs `aggTrade`\n\n"
         << "- Evidence: public live diagnostic; not production-readiness proof.\n"
         << "- Transport validity: "
         << (transport_valid ? "valid" : "invalid") << "\n"
         << "- Sequence validity: "
         << (sequence_clean ? "clean" : "invalid") << "\n"
         << "- Comparison validity: "
         << (comparison_valid ? "valid" : "invalid") << "\n"
         << "- Matched aggregate groups: " << stats.matches.size() << "\n"
         << "- First-signal wins: raw trade " << raw_wins << ", aggTrade "
         << agg_wins << ", ties " << ties << "\n"
         << "- Raw-first advantage: p50 "
         << static_cast<double>(p50) / 1000.0 << " us, p95 "
         << static_cast<double>(p95) / 1000.0 << " us, p99 "
         << static_cast<double>(p99) / 1000.0 << " us\n"
         << "- Aggregate vs final raw constituent p50: "
         << static_cast<double>(completion_p50) / 1000.0 << " us\n"
         << "- Stability trade: gaps=" << stats.raw.id_gaps
         << ", parse_errors=" << stats.raw.parse_errors
         << ", disconnects=" << stats.raw.disconnects << "\n"
         << "- Stability aggTrade: gaps=" << stats.agg.id_gaps
         << ", parse_errors=" << stats.agg.parse_errors
         << ", disconnects=" << stats.agg.disconnects << "\n"
         << "- Positive advantage means raw `trade` arrived earlier.\n";
}

[[nodiscard]] bool parse_options(int argc, char** argv, Options& options) {
  for (int index = 1; index < argc; ++index) {
    const std::string_view arg{argv[index]};
    if (arg == "--self-test") {
      options.self_test = true;
    } else if ((arg == "--symbol" || arg == "--duration-seconds" ||
                arg == "--warmup-seconds" || arg == "--output-dir") &&
               index + 1 < argc) {
      const std::string value{argv[++index]};
      if (arg == "--symbol") options.symbol = value;
      else if (arg == "--output-dir") options.output_dir = value;
      else {
        unsigned parsed = 0u;
        const auto result = std::from_chars(
            value.data(), value.data() + value.size(), parsed);
        if (result.ec != std::errc{} ||
            result.ptr != value.data() + value.size()) return false;
        if (arg == "--duration-seconds") options.duration_seconds = parsed;
        else options.warmup_seconds = parsed;
      }
    } else {
      return false;
    }
  }
  return options.self_test ||
         (!options.symbol.empty() && options.duration_seconds >= 2u &&
          options.warmup_seconds <= 300u);
}

[[nodiscard]] bool self_test() {
  const std::string raw =
      R"({"e":"trade","E":1000,"T":999,"s":"BANKUSDT","t":42,"p":"1","q":"2","m":false})";
  const std::string agg =
      R"({"e":"aggTrade","E":1001,"T":999,"s":"BANKUSDT","a":7,"p":"1","q":"2","f":42,"l":43,"m":false})";
  TradeEvent trade{};
  AggEvent aggregate{};
  if (!parse_trade(raw, 1234u, trade) || trade.id != 42u ||
      trade.receive_ns != 1234u) return false;
  if (!parse_agg(agg, 2345u, aggregate) || aggregate.id != 7u ||
      aggregate.first_id != 42u || aggregate.last_id != 43u) return false;
  if (parse_trade(agg, 1u, trade) || parse_agg(raw, 1u, aggregate)) return false;
  const std::vector<std::int64_t> values{-10, 20, 30, 40, 50};
  return percentile(values, 5000u) == 30;
}

}  // namespace

int main(int argc, char** argv) {
  Options options;
  if (!parse_options(argc, argv, options)) {
    std::cerr
        << "usage: binance-ws-race [--symbol BANKUSDT] "
           "[--duration-seconds 600] [--warmup-seconds 30] "
           "[--output-dir PATH] | --self-test\n";
    return 2;
  }
  if (options.self_test) {
    if (!self_test()) {
      std::cerr << "self_test_failed\n";
      return 1;
    }
    std::cout << "self_test_passed\n";
    return 0;
  }
  const std::string display_symbol = options.symbol;
  std::transform(
      options.symbol.begin(), options.symbol.end(), options.symbol.begin(),
      [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
  std::filesystem::create_directories(options.output_dir);
  std::ofstream raw_output{options.output_dir / "raw_frames.jsonl"};
  std::ofstream matches_output{options.output_dir / "matches.csv"};
  if (!raw_output || !matches_output) {
    std::cerr << "output_open_failed\n";
    return 3;
  }
  matches_output
      << "round,agg_id,first_id,last_id,group_size,first_advantage_ns,"
         "completion_advantage_ns,agg_receive_ns,raw_first_receive_ns,"
         "raw_last_receive_ns\n";
  Collector collector{raw_output, matches_output, options.warmup_seconds};
  const unsigned round_measurement = options.duration_seconds / 2u;
  const unsigned round_total = round_measurement + options.warmup_seconds;
  ReaderResult last_raw;
  ReaderResult last_agg;
  for (unsigned round = 1u; round <= 2u; ++round) {
    std::latch connected{2};
    std::barrier ready{3};
    ReaderResult raw_result;
    ReaderResult agg_result;
    std::thread first;
    std::thread second;
    if (round == 1u) {
      first = std::thread{
          run_reader, true, options.symbol, "/ws", round_total,
          std::ref(connected), std::ref(ready),
          std::ref(collector), std::ref(raw_result)};
      std::this_thread::sleep_for(std::chrono::milliseconds{100});
      second = std::thread{
          run_reader, false, options.symbol, "/market/ws", round_total,
          std::ref(connected), std::ref(ready),
          std::ref(collector), std::ref(agg_result)};
    } else {
      first = std::thread{
          run_reader, false, options.symbol, "/market/ws", round_total,
          std::ref(connected), std::ref(ready),
          std::ref(collector), std::ref(agg_result)};
      std::this_thread::sleep_for(std::chrono::milliseconds{100});
      second = std::thread{
          run_reader, true, options.symbol, "/ws", round_total,
          std::ref(connected), std::ref(ready),
          std::ref(collector), std::ref(raw_result)};
    }
    connected.wait();
    collector.start_round(round);
    ready.arrive_and_wait();
    first.join();
    second.join();
    last_raw = raw_result;
    last_agg = agg_result;
    std::cout << "round=" << round
              << " trade_connected=" << raw_result.connected
              << " agg_connected=" << agg_result.connected
              << " trade_error=" << raw_result.error
              << " agg_error=" << agg_result.error << '\n';
    if (!raw_result.connected || !agg_result.connected) break;
  }
  RunStats stats = collector.finish();
  write_summary(
      options.output_dir, display_symbol, stats, last_raw.error, last_agg.error);
  write_svg(
      options.output_dir / "latency_cdf.svg", display_symbol, stats.matches);
  std::cout << "matches=" << stats.matches.size()
            << " output=" << options.output_dir.string() << '\n';
  return stats.matches.empty() ? 5 : 0;
}
