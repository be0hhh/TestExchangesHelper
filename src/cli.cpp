#include "exchange_probe/app.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <ostream>
#include <string>
#include <string_view>
#include <vector>

namespace exchange_probe {
namespace {

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
  } else if (command == "run") {
    result.options.command = Command::Run;
  } else if (command == "sandbox") {
    result.options.command = Command::Sandbox;
  } else {
    result.error = "unknown command: " + std::string{command};
    return result;
  }

  for (int index = 2; index < argc; ++index) {
    const std::string_view option = argv[index];
    std::string value;
    if (option == "--jsonl") {
      result.options.jsonl = true;
    } else if (option == "--confirm-private") {
      result.options.confirm_private = true;
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
      } else {
        result.error = "--transport must be rest or ws";
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
  if (result.options.command == Command::Run &&
      result.options.surface == Surface::Private &&
      result.options.transport == Transport::WebSocket) {
    result.error = "private WebSocket probes are not implemented";
    return result;
  }
  if (result.options.command != Command::Run &&
      (result.options.confirm_private || result.options.env_file.has_value())) {
    result.error = "private credential options are valid only for run";
    return result;
  }
  result.ok = true;
  return result;
}

void print_help(std::ostream& output) {
  output
      << "exchange-api-probe 2 (Linux, C++20)\n"
      << "Usage:\n"
      << "  exchange-api-probe matrix [filters] [--jsonl]\n"
      << "  exchange-api-probe audit --source-root PATH [--jsonl]\n"
      << "  exchange-api-probe run [filters] [--surface public|private]\n"
      << "      [--transport rest|ws] [--timeout-ms N] [--attempts N]\n"
      << "      [--raw-public] [--jsonl]\n"
      << "      [--confirm-private] [--env-file PATH]\n"
      << "  exchange-api-probe sandbox --file PROFILE.json [run options]\n"
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
