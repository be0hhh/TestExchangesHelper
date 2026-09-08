#include "exchange_probe/App.hpp"

#include "exchange_probe/Net.hpp"
#include "NetCommon.hpp"

#include <boost/asio/ssl/context.hpp>
#include <boost/json/parse.hpp>
#include <boost/json/serialize.hpp>
#include <boost/system/error_code.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <exception>
#include <cstdio>
#include <fstream>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

namespace exchange_probe {
namespace {

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace ssl = asio::ssl;

struct Endpoint {
  struct Reference {
    std::string case_name;
    Transport transport{Transport::None};
    Wire wire{Wire::None};
  };

  std::string venue;
  std::string product;
  std::string host;
  std::uint16_t port{443};
  bool auth_required{false};
  std::vector<Reference> references;
};

struct Budget {
  unsigned dns_rounds{30};
  unsigned attempts{3};
};

struct Sample {
  bool tcp_ok{false};
  bool tls_ok{false};
  std::uint64_t tcp_ms{0};
  std::uint64_t tls_ms{0};
  TransportMetadata transport;
  std::string connected_ip;
  std::string ip_family;
  std::string stage{"configuration"};
  std::string error;
};

using GeoCache = std::map<std::string, boost::json::value>;

[[nodiscard]] bool case_selected(const CliOptions& options,
                                 std::string_view name) {
  return options.cases.empty() ||
         std::find(options.cases.begin(), options.cases.end(), name) !=
             options.cases.end();
}

[[nodiscard]] Budget budget_for(PlacementMode mode) noexcept {
  switch (mode) {
    case PlacementMode::Low: return {5u, 1u};
    case PlacementMode::Standard: return {30u, 3u};
    case PlacementMode::High: return {300u, 10u};
  }
  return {};
}

[[nodiscard]] std::string_view mode_name(PlacementMode mode) noexcept {
  switch (mode) {
    case PlacementMode::Low: return "low";
    case PlacementMode::Standard: return "standard";
    case PlacementMode::High: return "high";
  }
  return "unknown";
}

[[nodiscard]] std::string_view path_name(PlacementPath path) noexcept {
  return path == PlacementPath::Direct ? "direct" : "proxy";
}

void emit(std::ostream& output, boost::json::object value) {
  value["schema_version"] = 1;
  output << boost::json::serialize(value) << '\n';
}

[[nodiscard]] boost::json::object transport_metadata_json(
    const TransportMetadata& metadata) {
  return {
      {"remote_ip", metadata.remote_ip},
      {"ip_family", metadata.ip_family},
      {"tls_version", metadata.tls_version},
      {"tls_cipher", metadata.tls_cipher},
      {"alpn", metadata.alpn},
      {"certificate_not_after", metadata.certificate_not_after},
      {"tls_session_reused", metadata.tls_session_reused},
      {"tcp_info_available", metadata.tcp_info_available},
      {"tcp_rtt_us", metadata.tcp_rtt_us},
      {"tcp_rtt_variance_us", metadata.tcp_rtt_variance_us},
      {"tcp_retransmits", metadata.tcp_retransmits},
      {"tcp_congestion_window", metadata.tcp_congestion_window},
      {"tcp_mss", metadata.tcp_mss},
  };
}

[[nodiscard]] std::vector<Endpoint> endpoints_for(
    const std::vector<const ProductSpec*>& products,
    const CliOptions& options) {
  using Key =
      std::tuple<std::string, std::string, std::string, std::uint16_t, bool>;
  std::map<Key, Endpoint> unique;
  const bool private_surface =
      options.surface.value_or(Surface::Public) == Surface::Private;
  const auto append = [&](const ProductSpec& product,
                          std::string_view case_name,
                          std::string_view host,
                          std::uint16_t port,
                          Transport transport,
                          Wire wire,
                          bool auth_required) {
    if (!case_selected(options, case_name)) {
      return;
    }
    Key key{product.venue, product.product, std::string{host}, port,
            auth_required};
    auto [iterator, inserted] = unique.try_emplace(
        key,
        Endpoint{product.venue, product.product, std::string{host}, port,
                 auth_required, {}});
    (void)inserted;
    iterator->second.references.push_back(
        {std::string{case_name}, transport, wire});
  };
  for (const auto* product : products) {
    for (const auto& item : product->public_rest) {
      if (!private_surface) {
        append(*product, item.name, item.host, 443U, Transport::Rest,
               Wire::Json, false);
      }
    }
    for (const auto& item : product->public_ws) {
      if (!private_surface) {
        append(*product, item.name, item.host, 443U, Transport::WebSocket,
               item.wire, false);
      }
    }
    for (const auto& item : product->private_rest) {
      if (private_surface) {
        append(*product, item.name, item.host, 443U, Transport::Rest,
               Wire::Json, true);
      }
    }
    for (const auto& item : product->fix_sessions) {
      if (private_surface) {
        append(*product, item.name, item.host, item.port, Transport::Fix,
               Wire::FixSbe, true);
      }
    }
  }
  std::vector<Endpoint> result;
  result.reserve(unique.size());
  for (auto& [key, endpoint] : unique) {
    (void)key;
    result.push_back(std::move(endpoint));
  }
  return result;
}

[[nodiscard]] boost::json::array endpoint_references(
    const Endpoint& endpoint) {
  boost::json::array references;
  for (const auto& reference : endpoint.references) {
    references.emplace_back(boost::json::object{
        {"case", reference.case_name},
        {"transport", to_string(reference.transport)},
        {"wire", to_string(reference.wire)},
    });
  }
  return references;
}

[[nodiscard]] Sample transport_sample(
    const Endpoint& endpoint,
    const std::optional<asio::ip::tcp::endpoint>& target,
    PlacementPath path,
    std::chrono::milliseconds timeout) {
  const auto start = std::chrono::steady_clock::now();
  Sample result;
  try {
    asio::io_context context;
    ssl::context tls_context{ssl::context::tls_client};
    boost::system::error_code setup_error;
    tls_context.set_default_verify_paths(setup_error);
    if (setup_error) {
      result.error = "default_ca_paths_failed:" + setup_error.message();
      return result;
    }
    tls_context.set_options(ssl::context::default_workarounds |
                            ssl::context::no_sslv2 |
                            ssl::context::no_sslv3);
    net_detail::TlsStream stream{context, tls_context};
    const auto deadline = start + timeout;
    result.stage = "tcp_connect";
    if (path == PlacementPath::Direct) {
      if (target.has_value()) {
        if (!net_detail::connect_endpoint(
                context, beast::get_lowest_layer(stream), *target, deadline,
                result.error)) {
          return result;
        }
      } else {
        const auto resolved = net_detail::resolve(
            context, endpoint.host, std::to_string(endpoint.port), deadline);
        if (!resolved.ok) {
          result.stage = "dns";
          result.error = resolved.error;
          return result;
        }
        if (!net_detail::connect(context, beast::get_lowest_layer(stream),
                                 resolved.endpoints, deadline, result.error)) {
          return result;
        }
      }
    } else {
      const auto proxy = proxy_for_host(endpoint.host);
      if (!proxy.enabled || !proxy.valid) {
        result.stage = "proxy_configuration";
        result.error = proxy.enabled ? proxy.error : "proxy_not_configured";
        return result;
      }
      const auto proxy_targets = net_detail::resolve(context, proxy.host, proxy.port, deadline);
      if (!proxy_targets.ok || !net_detail::connect(context, beast::get_lowest_layer(stream),
                                                     proxy_targets.endpoints, deadline,
                                                     result.error)) {
        if (!proxy_targets.ok) result.error = proxy_targets.error;
        return result;
      }
      if (!net_detail::establish_proxy_tunnel(context, beast::get_lowest_layer(stream),
                                              proxy, endpoint.host,
                                              std::to_string(endpoint.port), deadline,
                                              result.error)) {
        result.stage = "proxy_connect";
        return result;
      }
    }
    boost::system::error_code endpoint_error;
    const auto connected = beast::get_lowest_layer(stream).socket()
                               .remote_endpoint(endpoint_error);
    if (!endpoint_error) {
      result.connected_ip = connected.address().to_string();
      result.ip_family = connected.address().is_v6() ? "ipv6" : "ipv4";
    }
    result.tcp_ok = true;
    result.tcp_ms = net_detail::elapsed_ms(start);
    result.stage = "tls_configuration";
    if (!net_detail::configure_tls(stream, endpoint.host, result.error)) {
      net_detail::close_tls(stream);
      return result;
    }
    result.stage = "tls_handshake";
    if (!net_detail::tls_handshake(context, stream, deadline, result.error)) {
      net_detail::close_tls(stream);
      return result;
    }
    result.tls_ok = true;
    result.tls_ms = net_detail::elapsed_ms(start) - result.tcp_ms;
    result.transport = net_detail::transport_metadata(stream);
    result.stage = "complete";
    net_detail::close_tls(stream);
  } catch (const std::exception& error) {
    result.error = std::string{"exception:"} + error.what();
  }
  return result;
}

[[nodiscard]] bool shell_safe_ip(std::string_view ip) noexcept {
  return !ip.empty() && std::all_of(ip.begin(), ip.end(), [](char value) {
    return (value >= '0' && value <= '9') || (value >= 'a' && value <= 'f') ||
           (value >= 'A' && value <= 'F') || value == '.' || value == ':';
  });
}

[[nodiscard]] std::string command_output(const std::string& command) {
  std::array<char, 512> buffer{};
  std::string result;
  FILE* pipe = ::popen(command.c_str(), "r");
  if (pipe == nullptr) return result;
  while (std::fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) != nullptr) {
    result.append(buffer.data());
  }
  (void)::pclose(pipe);
  return result;
}

[[nodiscard]] std::optional<double> ping_ms(std::string_view ip) {
  if (!shell_safe_ip(ip)) return std::nullopt;
  const auto output = command_output(
      std::string{"/usr/bin/ping "} + (ip.find(':') == std::string_view::npos ? "" : "-6 ") +
      "-n -q -c 1 -W 1 " + std::string{ip} + " 2>/dev/null");
  const auto equals = output.find('=');
  const auto first_slash = equals == std::string::npos ? std::string::npos : output.find('/', equals);
  const auto second_slash = first_slash == std::string::npos ? std::string::npos : output.find('/', first_slash + 1);
  if (first_slash == std::string::npos || second_slash == std::string::npos) return std::nullopt;
  try {
    return std::stod(output.substr(first_slash + 1, second_slash - first_slash - 1));
  } catch (...) {
    return std::nullopt;
  }
}

[[nodiscard]] std::optional<boost::json::value> geo_lookup(
    std::string_view provider, std::string_view ip) {
  if (provider == "none" || !shell_safe_ip(ip)) return std::nullopt;
  const auto url = provider == "ipinfo"
                       ? "https://ipinfo.io/" + std::string{ip} + "/json"
                       : "https://ipapi.co/" + std::string{ip} + "/json";
  const auto raw = command_output(
      "/usr/bin/curl --noproxy '*' --silent --show-error --connect-timeout 3 "
      "--max-time 8 '" + url + "' 2>/dev/null");
  boost::system::error_code error;
  auto value = boost::json::parse(raw, error);
  if (error || !value.is_object()) return std::nullopt;
  const auto* provider_error = value.as_object().if_contains("error");
  if (provider_error != nullptr && provider_error->is_bool() &&
      provider_error->as_bool()) return std::nullopt;
  return value;
}

[[nodiscard]] std::string geo_cache_key(std::string_view provider,
                                        std::string_view ip) {
  return std::string{provider} + "\n" + std::string{ip};
}

[[nodiscard]] GeoCache load_geo_cache(
    const std::optional<std::filesystem::path>& path) {
  GeoCache cache;
  if (!path.has_value()) return cache;
  std::ifstream input{*path};
  std::string line;
  while (std::getline(input, line)) {
    boost::system::error_code error;
    const auto value = boost::json::parse(line, error);
    if (error || !value.is_object()) continue;
    const auto& object = value.as_object();
    const auto* provider = object.if_contains("provider");
    const auto* ip = object.if_contains("ip");
    const auto* payload = object.if_contains("payload");
    if (provider == nullptr || ip == nullptr || payload == nullptr ||
        !provider->is_string() || !ip->is_string()) continue;
    if (!payload->is_object()) continue;
    const auto* provider_error = payload->as_object().if_contains("error");
    if (provider_error != nullptr && provider_error->is_bool() &&
        provider_error->as_bool()) continue;
    cache.emplace(geo_cache_key(provider->as_string(), ip->as_string()), *payload);
  }
  return cache;
}

void append_geo_cache(const std::optional<std::filesystem::path>& path,
                      std::string_view provider,
                      std::string_view ip,
                      const boost::json::value& payload) {
  if (!path.has_value()) return;
  std::ofstream output{*path, std::ios::app};
  if (!output) return;
  output << boost::json::serialize(boost::json::object{
      {"provider", provider}, {"ip", ip}, {"payload", payload}}) << '\n';
}

[[nodiscard]] std::string member(const boost::json::object& object,
                                 std::string_view name) {
  const auto* value = object.if_contains(name);
  return value != nullptr && value->is_string() ? std::string{value->as_string()} : std::string{};
}

[[nodiscard]] std::string coordinate_text(const boost::json::object& object,
                                          std::string_view provider) {
  if (provider == "ipinfo") return member(object, "loc");
  const auto* latitude = object.if_contains("latitude");
  const auto* longitude = object.if_contains("longitude");
  if (latitude == nullptr || longitude == nullptr) return {};
  return boost::json::serialize(*latitude) + "," + boost::json::serialize(*longitude);
}

void emit_geo(std::ostream& output, std::string_view provider,
              std::string_view ip, const boost::json::value& value) {
  const auto& object = value.as_object();
  const auto city = member(object, "city");
  emit(output, {{"kind", "placement_geo"}, {"provider", provider}, {"ip", ip},
                {"country", member(object, provider == "ipinfo" ? "country" : "country_code")},
                {"region", member(object, "region")}, {"city", city},
                {"coordinates", coordinate_text(object, provider)},
                {"frankfurt_candidate", city.find("Frankfurt") != std::string::npos},
                {"payload", value}});
}

}  // namespace

int run_placement(const CliOptions& options,
                  std::ostream& output,
                  std::ostream& error_output) {
  const auto profiles = make_profiles();
  const auto selected = select_products(profiles, options);
  if (selected.empty()) {
    error_output << "configuration_error: filters selected no products\n";
    return 2;
  }
  const auto endpoints = endpoints_for(selected, options);
  if (endpoints.empty()) {
    error_output << "configuration_error: filters selected no placement endpoints\n";
    return 2;
  }
  const auto budget = budget_for(options.placement_mode);
  emit(output, {{"kind", "placement_run"}, {"mode", mode_name(options.placement_mode)},
                {"path", path_name(options.placement_path)}, {"dns_rounds", budget.dns_rounds},
                {"transport_attempts", budget.attempts},
                {"surface", to_string(options.surface.value_or(Surface::Public))}});

  const auto& providers = options.geo_providers;
  auto geo_cache = load_geo_cache(options.geo_cache);
  std::set<std::string> geolocated;
  for (const auto& endpoint : endpoints) {
    emit(output, {{"kind", "placement_endpoint"}, {"venue", endpoint.venue},
                  {"product", endpoint.product},
                  {"host", endpoint.host}, {"port", endpoint.port},
                  {"auth_required", endpoint.auth_required},
                  {"references", endpoint_references(endpoint)}});
    std::map<std::string, asio::ip::tcp::endpoint> ips;
    for (unsigned round = 1; round <= budget.dns_rounds; ++round) {
      asio::io_context context;
      const auto resolved = net_detail::resolve(
          context, endpoint.host, std::to_string(endpoint.port),
          std::chrono::steady_clock::now() + options.limits.timeout);
      if (!resolved.ok) {
        emit(output, {{"kind", "placement_dns"}, {"host", endpoint.host},
                      {"port", endpoint.port}, {"round", round}, {"error", resolved.error}});
        continue;
      }
      for (const auto& item : resolved.endpoints) {
        const auto target = item.endpoint();
        const auto ip = target.address().to_string();
        ips.emplace(ip, target);
        emit(output, {{"kind", "placement_dns"}, {"host", endpoint.host},
                      {"port", endpoint.port}, {"round", round}, {"ip", ip}});
      }
      if (options.placement_mode == PlacementMode::High && round < budget.dns_rounds) {
        std::this_thread::sleep_for(std::chrono::milliseconds{20});
      }
    }
    if (options.route_mode != RouteMode::Pinned) {
      std::uint64_t best_tcp_ms{0};
      std::uint64_t best_tls_ms{0};
      std::string connected_ip;
      std::string ip_family;
      for (unsigned attempt = 1; attempt <= budget.attempts; ++attempt) {
        const auto sample = transport_sample(
            endpoint, std::nullopt, options.placement_path,
            options.limits.timeout);
        if (sample.tcp_ok &&
            (best_tcp_ms == 0U || sample.tcp_ms < best_tcp_ms)) {
          best_tcp_ms = sample.tcp_ms;
        }
        if (sample.tls_ok &&
            (best_tls_ms == 0U || sample.tls_ms < best_tls_ms)) {
          best_tls_ms = sample.tls_ms;
        }
        connected_ip = sample.connected_ip;
        ip_family = sample.ip_family;
        emit(output, {{"kind", "placement_measurement"},
                      {"venue", endpoint.venue},
                      {"product", endpoint.product},
                      {"host", endpoint.host},
                      {"port", endpoint.port},
                      {"connected_ip", sample.connected_ip},
                      {"ip_family", sample.ip_family},
                      {"attempt", attempt},
                      {"path", path_name(options.placement_path)},
                      {"route", "natural"},
                      {"target_ip_applied", false},
                      {"icmp_avg_ms", nullptr},
                      {"tcp_ok", sample.tcp_ok},
                      {"tls_ok", sample.tls_ok},
                      {"tcp_ms", sample.tcp_ms},
                      {"tls_ms", sample.tls_ms},
                      {"transport_metadata",
                       transport_metadata_json(sample.transport)},
                      {"stage", sample.stage},
                      {"error", sample.error},
                      {"protocol_status",
                       endpoint.auth_required
                           ? "transport_only_auth_required"
                           : "transport_only"}});
      }
      emit(output, {{"kind", "placement_summary"},
                    {"venue", endpoint.venue},
                    {"product", endpoint.product},
                    {"host", endpoint.host},
                    {"port", endpoint.port},
                    {"connected_ip", connected_ip},
                    {"ip_family", ip_family},
                    {"path", path_name(options.placement_path)},
                    {"route", "natural"},
                    {"target_ip_applied", false},
                    {"best_icmp_ms", nullptr},
                    {"best_tcp_ms", best_tcp_ms},
                    {"best_tls_ms", best_tls_ms}});
    }
    if (options.route_mode == RouteMode::Natural) {
      continue;
    }
    if (options.placement_path == PlacementPath::Proxy) {
      emit(output, {{"kind", "placement_route_unavailable"},
                    {"venue", endpoint.venue},
                    {"product", endpoint.product},
                    {"host", endpoint.host},
                    {"port", endpoint.port},
                    {"route", "pinned"},
                    {"path", "proxy"},
                    {"reason", "proxy_resolves_destination_host"}});
      continue;
    }
    for (const auto& [ip, target] : ips) {
      std::uint64_t best_tcp_ms{0};
      std::uint64_t best_tls_ms{0};
      std::optional<double> best_ping;
      for (unsigned attempt = 1; attempt <= budget.attempts; ++attempt) {
        const auto sample = transport_sample(
            endpoint, target, options.placement_path, options.limits.timeout);
        const auto ping = ping_ms(ip);
        if (sample.tcp_ok && (best_tcp_ms == 0 || sample.tcp_ms < best_tcp_ms)) {
          best_tcp_ms = sample.tcp_ms;
        }
        if (sample.tls_ok && (best_tls_ms == 0 || sample.tls_ms < best_tls_ms)) {
          best_tls_ms = sample.tls_ms;
        }
        if (ping.has_value() && (!best_ping.has_value() || *ping < *best_ping)) {
          best_ping = ping;
        }
        emit(output, {{"kind", "placement_measurement"}, {"venue", endpoint.venue},
                      {"product", endpoint.product},
                      {"host", endpoint.host}, {"port", endpoint.port}, {"ip", ip},
                      {"ip_family", target.address().is_v6() ? "ipv6" : "ipv4"},
                      {"attempt", attempt}, {"path", path_name(options.placement_path)},
                      {"route", "pinned"},
                      {"target_ip_applied", options.placement_path == PlacementPath::Direct},
                      {"icmp_avg_ms", ping.has_value() ? boost::json::value(*ping) : boost::json::value{}},
                      {"tcp_ok", sample.tcp_ok}, {"tls_ok", sample.tls_ok},
                      {"tcp_ms", sample.tcp_ms}, {"tls_ms", sample.tls_ms},
                      {"transport_metadata",
                       transport_metadata_json(sample.transport)},
                      {"stage", sample.stage}, {"error", sample.error},
                      {"protocol_status", endpoint.auth_required ? "transport_only_auth_required" : "transport_only"}});
        if (options.placement_mode == PlacementMode::High && attempt < budget.attempts) {
          std::this_thread::sleep_for(std::chrono::milliseconds{100});
        }
      }
      emit(output, {{"kind", "placement_summary"}, {"venue", endpoint.venue},
                    {"product", endpoint.product},
                    {"host", endpoint.host}, {"port", endpoint.port}, {"ip", ip},
                    {"ip_family", target.address().is_v6() ? "ipv6" : "ipv4"},
                    {"path", path_name(options.placement_path)},
                    {"route", "pinned"},
                    {"target_ip_applied", options.placement_path == PlacementPath::Direct},
                    {"best_icmp_ms", best_ping.has_value() ? boost::json::value(*best_ping) : boost::json::value{}},
                    {"best_tcp_ms", best_tcp_ms}, {"best_tls_ms", best_tls_ms}});
      if (geolocated.insert(ip).second) {
        for (const auto& provider : providers) {
          const auto key = geo_cache_key(provider, ip);
          const auto cached = geo_cache.find(key);
          if (cached != geo_cache.end()) {
            emit_geo(output, provider, ip, cached->second);
            continue;
          }
          if (const auto geo = geo_lookup(provider, ip); geo.has_value()) {
            geo_cache.emplace(key, *geo);
            append_geo_cache(options.geo_cache, provider, ip, *geo);
            emit_geo(output, provider, ip, *geo);
          }
        }
      }
    }
  }
  return 0;
}

}  // namespace exchange_probe
