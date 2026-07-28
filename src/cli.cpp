#include "exchange_probe/app.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <ostream>
#include <string>
#include <string_view>
#include <vector>

namespace exchange_probe {
namespace {

inline constexpr unsigned kMaxLanes = 64U;
inline constexpr unsigned kMaxSamples = 10'000'000U;
inline constexpr unsigned kMaxDurationSeconds = 86'400U;
inline constexpr std::uint64_t kMaxArtifactBytes =
    16ULL * 1024ULL * 1024ULL * 1024ULL;

[[nodiscard]] bool contains(
    const std::vector<std::string>& values,
    std::string_view candidate) {
  return values.empty() ||
         std::find(values.begin(), values.end(), candidate) != values.end();
}

[[nodiscard]] std::optional<unsigned> parse_unsigned(std::string_view text) {
  unsigned result = 0;
  const auto [end, error] =
      std::from_chars(text.data(), text.data() + text.size(), result);
  if (error != std::errc{} || end != text.data() + text.size()) {
    return std::nullopt;
  }
  return result;
}

[[nodiscard]] std::optional<std::uint64_t> parse_u64(std::string_view text) {
  std::uint64_t result = 0;
  const auto [end, error] =
      std::from_chars(text.data(), text.data() + text.size(), result);
  if (error != std::errc{} || end != text.data() + text.size()) {
    return std::nullopt;
  }
  return result;
}

[[nodiscard]] bool option_value(
    int argc,
    char** argv,
    int& index,
    std::string_view option,
    std::string& value,
    std::string& error) {
  if (index + 1 >= argc) {
    error = std::string{option} + " requires a value";
    return false;
  }
  value = argv[++index];
  return true;
}

}  // namespace

CliParseResult parse_cli(int argc, char** argv) {
  CliParseResult result;
  int option_start = 2;
  if (argc < 2) {
    result.options.command = Command::Help;
    result.ok = true;
    return result;
  }
  const std::string_view command = argv[1];
  if (command == "help" || command == "--help" || command == "-h") {
    result.options.command = Command::Help;
  } else if (command == "matrix") {
    result.options.command = Command::Matrix;
  } else if (command == "audit") {
    result.options.command = Command::Audit;
  } else if (command == "latency") {
    result.options.command = Command::Latency;
  } else if (command == "stability") {
    result.options.command = Command::Stability;
  } else if (command == "compare") {
    result.options.command = Command::Compare;
  } else if (command == "run") {
    result.error = "run was replaced by latency";
    return result;
  } else if (command == "sandbox") {
    result.options.command = Command::Sandbox;
  } else if (command == "placement") {
    result.options.command = Command::Placement;
    if (argc > 2 && std::string_view{argv[2]} == "scan") {
      option_start = 3;
    } else if (argc > 2 && std::string_view{argv[2]} == "auth") {
      result.error =
          "placement auth was replaced by --surface private --confirm-private";
      return result;
    }
  } else {
    result.error = "unknown command: " + std::string{command};
    return result;
  }

  for (int index = option_start; index < argc; ++index) {
    const std::string_view option = argv[index];
    std::string value;
    if (option == "--jsonl") {
      result.options.jsonl = true;
    } else if (option == "--confirm-private") {
      result.options.confirm_private = true;
    } else if (option == "--confirm-session-lifecycle") {
      result.options.confirm_session_lifecycle = true;
    } else if (option == "--confirm-load") {
      result.options.confirm_load = true;
    } else if (option == "--raw-public") {
      result.options.limits.raw_public = true;
    } else if (option == "--venue") {
      if (!option_value(argc, argv, index, option, value, result.error)) {
        return result;
      }
      result.options.venues.push_back(std::move(value));
    } else if (option == "--product") {
      if (!option_value(argc, argv, index, option, value, result.error)) {
        return result;
      }
      result.options.products.push_back(std::move(value));
    } else if (option == "--case") {
      if (!option_value(argc, argv, index, option, value, result.error)) {
        return result;
      }
      result.options.cases.push_back(std::move(value));
    } else if (option == "--surface") {
      if (!option_value(argc, argv, index, option, value, result.error)) {
        return result;
      }
      if (value == "public") {
        result.options.surface = Surface::Public;
      } else if (value == "private") {
        result.options.surface = Surface::Private;
      } else {
        result.error = "--surface must be public or private";
        return result;
      }
    } else if (option == "--transport") {
      if (!option_value(argc, argv, index, option, value, result.error)) {
        return result;
      }
      if (value == "rest") {
        result.options.transport = Transport::Rest;
      } else if (value == "ws") {
        result.options.transport = Transport::WebSocket;
      } else if (value == "fix") {
        result.options.transport = Transport::Fix;
      } else {
        result.error = "--transport must be rest, ws or fix";
        return result;
      }
    } else if (option == "--source-root") {
      if (!option_value(argc, argv, index, option, value, result.error)) {
        return result;
      }
      result.options.source_root = value;
    } else if (option == "--file") {
      if (!option_value(argc, argv, index, option, value, result.error)) {
        return result;
      }
      result.options.sandbox_file = value;
    } else if (option == "--env-file") {
      if (!option_value(argc, argv, index, option, value, result.error)) {
        return result;
      }
      result.options.env_file = value;
    } else if (option == "--timeout-ms") {
      if (!option_value(argc, argv, index, option, value, result.error)) {
        return result;
      }
      const auto parsed = parse_unsigned(value);
      if (!parsed.has_value() || *parsed < 100 || *parsed > 120'000) {
        result.error = "--timeout-ms must be in [100,120000]";
        return result;
      }
      result.options.limits.timeout = std::chrono::milliseconds{*parsed};
    } else if (option == "--attempts") {
      if (!option_value(argc, argv, index, option, value, result.error)) {
        return result;
      }
      const auto parsed = parse_unsigned(value);
      if (!parsed.has_value() || *parsed < 1 || *parsed > 10) {
        result.error = "--attempts must be in [1,10]";
        return result;
      }
      result.options.limits.attempts = *parsed;
    } else if (option == "--lanes") {
      if (!option_value(argc, argv, index, option, value, result.error)) {
        return result;
      }
      const auto parsed = parse_unsigned(value);
      if (!parsed.has_value() || *parsed == 0U || *parsed > kMaxLanes) {
        result.error = "--lanes must be in [1,64]";
        return result;
      }
      result.options.lanes = *parsed;
    } else if (option == "--samples") {
      if (!option_value(argc, argv, index, option, value, result.error)) {
        return result;
      }
      const auto parsed = parse_unsigned(value);
      if (!parsed.has_value() || *parsed == 0U ||
          *parsed > kMaxSamples) {
        result.error = "--samples must be in [1,10000000]";
        return result;
      }
      result.options.samples = *parsed;
    } else if (option == "--duration-seconds") {
      if (!option_value(argc, argv, index, option, value, result.error)) {
        return result;
      }
      const auto parsed = parse_unsigned(value);
      if (!parsed.has_value() || *parsed == 0U ||
          *parsed > kMaxDurationSeconds) {
        result.error = "--duration-seconds must be in [1,86400]";
        return result;
      }
      result.options.duration_seconds = *parsed;
    } else if (option == "--max-artifact-bytes") {
      if (!option_value(argc, argv, index, option, value, result.error)) {
        return result;
      }
      const auto parsed = parse_u64(value);
      if (!parsed.has_value() || *parsed < 4096U ||
          *parsed > kMaxArtifactBytes) {
        result.error =
            "--max-artifact-bytes must be in [4096,17179869184]";
        return result;
      }
      result.options.max_artifact_bytes = *parsed;
    } else if (option == "--output-dir") {
      if (!option_value(argc, argv, index, option, value, result.error)) {
        return result;
      }
      result.options.output_dir = value;
    } else if (option == "--input") {
      if (!option_value(argc, argv, index, option, value, result.error)) {
        return result;
      }
      result.options.inputs.emplace_back(value);
    } else if (option == "--mode") {
      if (!option_value(argc, argv, index, option, value, result.error)) {
        return result;
      }
      if (value == "low") {
        result.options.placement_mode = PlacementMode::Low;
      } else if (value == "standard") {
        result.options.placement_mode = PlacementMode::Standard;
      } else if (value == "high") {
        result.options.placement_mode = PlacementMode::High;
      } else {
        result.error = "--mode must be low, standard or high";
        return result;
      }
    } else if (option == "--path") {
      if (!option_value(argc, argv, index, option, value, result.error)) {
        return result;
      }
      if (value == "direct") {
        result.options.placement_path = PlacementPath::Direct;
      } else if (value == "proxy") {
        result.options.placement_path = PlacementPath::Proxy;
      } else {
        result.error = "--path must be direct or proxy";
        return result;
      }
    } else if (option == "--route") {
      if (!option_value(argc, argv, index, option, value, result.error)) {
        return result;
      }
      if (value == "natural") {
        result.options.route_mode = RouteMode::Natural;
      } else if (value == "pinned") {
        result.options.route_mode = RouteMode::Pinned;
      } else if (value == "both") {
        result.options.route_mode = RouteMode::Both;
      } else {
        result.error = "--route must be natural, pinned or both";
        return result;
      }
    } else if (option == "--connection") {
      if (!option_value(argc, argv, index, option, value, result.error)) {
        return result;
      }
      if (value == "cold") {
        result.options.connection_mode = ConnectionMode::Cold;
      } else if (value == "warm") {
        result.options.connection_mode = ConnectionMode::Warm;
      } else if (value == "both") {
        result.options.connection_mode = ConnectionMode::Both;
      } else {
        result.error = "--connection must be cold, warm or both";
        return result;
      }
    } else if (option == "--geo-provider") {
      if (!option_value(argc, argv, index, option, value, result.error)) {
        return result;
      }
      if (value != "ipinfo" && value != "ipapi" && value != "none") {
        result.error = "--geo-provider must be ipinfo, ipapi or none";
        return result;
      }
      result.options.geo_providers.push_back(std::move(value));
    } else if (option == "--geo-cache") {
      if (!option_value(argc, argv, index, option, value, result.error)) {
        return result;
      }
      result.options.geo_cache = value;
    } else {
      result.error = "unknown option: " + std::string{option};
      return result;
    }
  }

  if (result.options.command == Command::Audit &&
      !result.options.source_root.has_value()) {
    result.error = "audit requires --source-root";
    return result;
  }
  if (result.options.command == Command::Sandbox &&
      !result.options.sandbox_file.has_value()) {
    result.error = "sandbox requires --file";
    return result;
  }
  const bool profiler =
      result.options.command == Command::Placement ||
      result.options.command == Command::Latency ||
      result.options.command == Command::Stability;
  const bool session_profiler =
      result.options.command == Command::Latency ||
      result.options.command == Command::Stability;
  if (profiler &&
      result.options.surface == Surface::Private &&
      !result.options.confirm_private) {
    result.error = "private surface requires --confirm-private";
    return result;
  }
  if (profiler &&
      result.options.surface == Surface::Private &&
      result.options.limits.raw_public) {
    result.error = "--raw-public is forbidden for private surface";
    return result;
  }
  if (profiler &&
      result.options.surface.value_or(Surface::Public) == Surface::Public &&
      result.options.env_file.has_value()) {
    result.error = "--env-file requires private surface";
    return result;
  }
  if (result.options.confirm_session_lifecycle &&
      (!session_profiler ||
       result.options.surface != Surface::Private ||
       (result.options.transport != Transport::WebSocket &&
        result.options.transport != Transport::Fix))) {
    result.error =
        "--confirm-session-lifecycle requires private ws or fix";
    return result;
  }
  if (session_profiler &&
      result.options.surface == Surface::Private &&
      (result.options.transport == Transport::WebSocket ||
       result.options.transport == Transport::Fix) &&
      !result.options.confirm_session_lifecycle) {
    result.error =
        "private ws or fix requires --confirm-session-lifecycle";
    return result;
  }
  if (result.options.command != Command::Placement &&
      (!result.options.geo_providers.empty() ||
       result.options.geo_cache.has_value())) {
    result.error = "GeoIP options are valid only for placement";
    return result;
  }
  if (result.options.command == Command::Placement &&
      result.options.geo_cache.has_value() &&
      result.options.geo_providers.empty()) {
    result.error = "--geo-cache requires --geo-provider";
    return result;
  }
  if (result.options.limits.raw_public &&
      result.options.command != Command::Latency &&
      result.options.command != Command::Stability &&
      result.options.command != Command::Sandbox) {
    result.error =
        "--raw-public is valid only for latency, stability or sandbox";
    return result;
  }
  if (!session_profiler &&
      (result.options.lanes.has_value() ||
       result.options.samples.has_value() ||
       result.options.duration_seconds.has_value())) {
    result.error =
        "lanes, samples and duration are valid only for latency or stability";
    return result;
  }
  if (result.options.command == Command::Placement &&
      result.options.env_file.has_value()) {
    result.error = "placement is transport-only and does not load --env-file";
    return result;
  }
  if (result.options.command != Command::Compare &&
      !result.options.inputs.empty()) {
    result.error = "--input is valid only for compare";
    return result;
  }
  if (!profiler &&
      (result.options.confirm_private || result.options.env_file.has_value())) {
    result.error =
        "private credential options are valid only for profiler commands";
    return result;
  }
  if (result.options.command == Command::Compare &&
      result.options.inputs.empty()) {
    result.error = "compare requires at least one --input bundle";
    return result;
  }
  if (result.options.placement_mode == PlacementMode::High &&
      profiler && !result.options.confirm_load) {
    result.error = "high mode requires --confirm-load";
    return result;
  }
  const bool elevated_load =
      result.options.lanes.value_or(4U) > 4U ||
      result.options.samples.value_or(100U) > 100U ||
      result.options.duration_seconds.value_or(300U) > 300U;
  if (profiler && elevated_load && !result.options.confirm_load) {
    result.error =
        "elevated lanes, samples or duration require --confirm-load";
    return result;
  }
  if ((result.options.command == Command::Latency ||
       result.options.command == Command::Stability) &&
      result.options.placement_mode == PlacementMode::High &&
      (!result.options.lanes.has_value() ||
       (!result.options.samples.has_value() &&
        !result.options.duration_seconds.has_value()))) {
    result.error =
        "high latency/stability requires --lanes and --samples or --duration-seconds";
    return result;
  }
  result.ok = true;
  return result;
}

void print_help(std::ostream& output) {
  output
      << "exchange-api-probe 3 (Linux, C++20)\n"
      << "Usage:\n"
      << "  exchange-api-probe matrix [filters] [--jsonl]\n"
      << "  exchange-api-probe audit --source-root PATH [--jsonl]\n"
      << "  exchange-api-probe latency [filters] [profiler options]\n"
      << "  exchange-api-probe stability [filters] [profiler options]\n"
      << "  exchange-api-probe placement [scan] [filters] [profiler options]\n"
      << "  exchange-api-probe compare --input BUNDLE [--input BUNDLE...]\n"
      << "  exchange-api-probe sandbox --file PROFILE.json [probe options]\n"
      << "Profiler options:\n"
      << "      [--surface public|private] [--transport rest|ws|fix]\n"
      << "      [--mode low|standard|high] [--lanes N] [--samples N]\n"
      << "      [--duration-seconds N] [--confirm-load]\n"
      << "      [--path direct|proxy] [--route natural|pinned|both]\n"
      << "      [--connection cold|warm|both] [--output-dir PATH]\n"
      << "      [--max-artifact-bytes N] [--raw-public]\n"
      << "      [--confirm-private] [--confirm-session-lifecycle]\n"
      << "      [--env-file PATH]\n"
      << "      [--geo-provider ipinfo|ipapi|none] [--geo-cache PATH]\n"
      << "Filters (repeatable): --venue NAME --product NAME --case NAME\n"
      << "\nPrivate REST is read-only and requires --confirm-private. Public\n"
      << "commands never load credentials. HTTPS_PROXY/https_proxy and\n"
      << "NO_PROXY/no_proxy are honored without silent direct fallback.\n";
}

std::vector<const ProductSpec*> select_products(
    const std::vector<ProductSpec>& products,
    const CliOptions& options) {
  std::vector<const ProductSpec*> selected;
  for (const auto& product : products) {
    if (contains(options.venues, product.venue) &&
        contains(options.products, product.product)) {
      selected.push_back(&product);
    }
  }
  return selected;
}

}  // namespace exchange_probe
