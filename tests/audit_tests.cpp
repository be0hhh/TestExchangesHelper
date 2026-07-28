#include "exchange_probe/app.hpp"

#include <boost/json/serialize.hpp>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

namespace {

void require(bool condition, const char* message) {
  if (!condition) {
    std::cerr << "audit test failed: " << message << '\n';
    std::exit(1);
  }
}

void write_file(
    const std::filesystem::path& path,
    std::string_view content) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream output{path, std::ios::binary};
  output << content;
}

}  // namespace

int main() {
  using namespace exchange_probe;
  const auto suffix = std::chrono::steady_clock::now()
                          .time_since_epoch()
                          .count();
  const auto root =
      std::filesystem::temp_directory_path() /
      ("exchange-probe-audit-" + std::to_string(suffix));

  write_file(
      root / "src/src/exchanges/register_all.cpp",
      "#include \"exchanges/register.hpp\"\n"
      "#include \"exchanges/binance/register_binance.hpp\"\n");
  write_file(
      root / "src/src/exchanges/binance/fapi/config.cpp",
      "#include \"exchanges/binance/fapi/market/MarketRoutes.ipp\"\n");
  write_file(
      root / "src/src/exchanges/binance/fapi/market/MarketRoutes.ipp",
      "const int kSubscribeTrades = 1;\n");

  ProductSpec product;
  product.venue = "binance";
  product.product = "futures";
  WsCase trades;
  trades.name = "trades";
  trades.core_anchor = {
      "src/src/exchanges/binance/fapi/config.cpp",
      "kSubscribeTrades",
      {},
  };
  product.public_ws.push_back(std::move(trades));

  const auto audit = audit_profiles({product}, root);
  if (!audit.at("ok").as_bool()) {
    std::cerr << boost::json::serialize(audit) << '\n';
  }
  require(audit.at("ok").as_bool(), "exact registry and ipp closure");
  require(
      audit.at("summary").as_object().at("registered_families").as_uint64() ==
          1U,
      "common register include excluded");

  std::error_code cleanup_error;
  std::filesystem::remove_all(root, cleanup_error);
  return 0;
}
