#include "exchange_probe/Contracts.hpp"

#include "TestSupport.hpp"

#include <boost/json/parse.hpp>

int main() {
  using namespace exchange_probe;

  const auto binance = boost::json::parse(
      R"({"symbol":"BTCUSDT","priceChangePercent":"1.2","quoteVolume":"42"})");
  const auto ticker = validate_rest_contract(
      RestContract::BinanceTicker24h, binance, "BTCUSDT");
  require_test(ticker.logical_success, "binance envelope");
  require_test(ticker.schema_success, "binance schema");
  require_test(ticker.symbol_success, "binance symbol");

  const auto bybit_error =
      boost::json::parse(R"({"retCode":10001,"retMsg":"bad symbol"})");
  const auto rejected = validate_rest_contract(
      RestContract::BybitTicker24h, bybit_error, "BTCUSDT");
  require_test(!rejected.logical_success, "logical error must not pass");
  require_test(rejected.api_code == "10001", "logical error code");
  return 0;
}
