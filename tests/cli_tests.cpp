#include "exchange_probe/app.hpp"

#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace {
void require(bool condition, const char* message) {
  if (!condition) {
    std::cerr << "cli test failed: " << message << '\n';
    std::exit(1);
  }
}

exchange_probe::CliParseResult parse(
    std::initializer_list<const char*> arguments) {
  std::vector<std::string> storage;
  for (const auto* argument : arguments) {
    storage.emplace_back(argument);
  }
  std::vector<char*> argv;
  for (auto& argument : storage) {
    argv.push_back(argument.data());
  }
  return exchange_probe::parse_cli(
      static_cast<int>(argv.size()), argv.data());
}
}  // namespace

int main() {
  using namespace exchange_probe;
  const auto matrix =
      parse({"probe", "matrix", "--venue", "binance", "--jsonl"});
  require(matrix.ok && matrix.options.command == Command::Matrix,
          "matrix parse");
  require(matrix.options.venues == std::vector<std::string>{"binance"},
          "venue filter");
  require(matrix.options.jsonl, "jsonl flag");

  const auto latency = parse({
      "probe", "latency", "--surface", "public", "--transport", "ws",
      "--timeout-ms", "2500", "--attempts", "2", "--mode", "standard",
      "--lanes", "4", "--samples", "100", "--route", "both",
  });
  require(latency.ok && latency.options.command == Command::Latency,
          "latency parse");
  require(latency.options.surface == Surface::Public, "public surface");
  require(latency.options.transport == Transport::WebSocket, "ws transport");
  require(latency.options.limits.timeout.count() == 2500, "timeout");
  require(latency.options.limits.attempts == 2, "attempts");
  require(latency.options.lanes == 4U, "lanes");
  require(latency.options.samples == 100U, "samples");
  require(latency.options.connection_mode == ConnectionMode::Cold,
          "cold connection default");
  require(latency.options.route_mode == RouteMode::Both,
          "explicit both routes");

  require(!parse({"probe", "audit"}).ok, "audit source required");
  require(!parse({"probe", "sandbox"}).ok, "sandbox file required");
  require(
      parse({"probe", "latency", "--surface", "private",
             "--transport", "ws", "--confirm-private",
             "--confirm-session-lifecycle"}).ok,
      "private ws confirmation");
  require(
      !parse({"probe", "latency", "--surface", "private",
              "--transport", "ws", "--confirm-private"}).ok,
      "private ws lifecycle confirmation required");
  require(!parse({"probe", "latency", "--attempts", "0"}).ok,
          "zero attempts rejected");
  require(!parse({"probe", "latency", "--lanes", "65"}).ok,
          "lane capacity enforced");
  require(!parse({"probe", "latency", "--path", "invalid"}).ok,
          "invalid path rejected");
  require(!parse({"probe", "run"}).ok, "legacy run rejected");
  const auto placement = parse({
      "probe", "placement", "scan", "--mode", "high", "--path", "direct",
      "--confirm-load", "--geo-provider", "ipinfo", "--geo-cache", "geo.jsonl",
  });
  require(placement.ok && placement.options.command == Command::Placement,
          "placement scan parse");
  require(placement.options.placement_mode == PlacementMode::High,
          "placement high mode");
  require(placement.options.placement_path == PlacementPath::Direct,
          "placement direct path");
  require(
      parse({"probe", "placement", "--surface", "private",
             "--confirm-private"}).ok,
      "private placement confirmation");
  require(
      !parse({"probe", "placement", "--surface", "private",
              "--confirm-private", "--env-file", "secrets.env"}).ok,
      "placement rejects ignored credentials");
  require(!parse({"probe", "placement", "auth"}).ok,
          "legacy placement auth rejected");
  require(
      parse({"probe", "compare", "--input", "one", "--input", "two"}).ok,
      "compare inputs");
  require(!parse({"probe", "unknown"}).ok, "unknown command rejected");

  const auto products = make_profiles();
  CliOptions options;
  options.venues = {"binance"};
  options.products = {"spot"};
  const auto selected = select_products(products, options);
  require(selected.size() == 1, "exact product selection");
  require(selected.front()->venue == "binance", "selected venue");
  require(selected.front()->product == "spot", "selected product");
  return 0;
}
