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

  const auto run = parse({
      "probe", "run", "--surface", "public", "--transport", "ws",
      "--timeout-ms", "2500", "--attempts", "2",
  });
  require(run.ok, "run parse");
  require(run.options.surface == Surface::Public, "public surface");
  require(run.options.transport == Transport::WebSocket, "ws transport");
  require(run.options.limits.timeout.count() == 2500, "timeout");
  require(run.options.limits.attempts == 2, "attempts");

  require(!parse({"probe", "audit"}).ok, "audit source required");
  require(!parse({"probe", "sandbox"}).ok, "sandbox file required");
  require(
      !parse({"probe", "run", "--surface", "private",
              "--transport", "ws"}).ok,
      "private ws rejected");
  require(!parse({"probe", "run", "--attempts", "0"}).ok,
          "zero attempts rejected");
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
