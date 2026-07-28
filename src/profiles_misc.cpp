#include "profile_builders.hpp"
#include "profile_factories.hpp"

#include <utility>

namespace exchange_probe {
using namespace profile_factory;

void append_misc_profiles(std::vector<ProductSpec>& products) {
  auto aster_lowercase = negative_rest(
      public_rest(
          "funding_lowercase",
          "fapi.asterdex.com",
          "/fapi/v3/premiumIndex?symbol=btcusdt",
          {"funding_current_symbol"},
          RestContract::BinanceFunding,
          "BTCUSDT"),
      {400},
      {"-1121"});
  products.push_back(ProductSpec{
      .venue = "aster",
      .product = "futures",
      .credential_prefix = "ASTER_FUTURES_API",
      .public_rest = {
          public_rest(
              "instrument_rules",
              "fapi.asterdex.com",
              "/fapi/v3/exchangeInfo",
              {"exchange_info", "instrument_catalog"}),
          public_rest(
              "ticker_24h",
              "fapi.asterdex.com",
              "/fapi/v1/ticker/24hr",
              {"ticker_24h"},
              RestContract::BinanceTicker24h,
              "BTCUSDT",
              anchor("src/src/exchanges/aster/fapi/reference/ReferenceCatalogV1.hpp")),
          public_rest(
              "funding_current",
              "fapi.asterdex.com",
              "/fapi/v3/premiumIndex",
              {"funding_current_all"},
              RestContract::BinanceFunding,
              "BTCUSDT",
              anchor(
                  "src/src/exchanges/aster/fapi/funding/FundingCurrentCatalog.hpp")),
          std::move(aster_lowercase),
          public_rest(
              "funding_history",
              "fapi.asterdex.com",
              "/fapi/v3/fundingRate?symbol=BTCUSDT&limit=10",
              {"funding_history"}),
          public_rest(
              "public_trades",
              "fapi.asterdex.com",
              "/fapi/v1/aggTrades?symbol=BTCUSDT&limit=10",
              {"historical_trades"}),
      },
      .public_ws = {
          json_ws(
              "funding",
              "fstream.asterdex.com",
              "/ws/btcusdt@markPrice@1s",
              {},
              {"funding_current_symbol"},
              {},
              WsAckKind::None,
              "markPrice",
              "BTCUSDT"),
      },
      .notes = {
          "Aster private V3 signer is intentionally not reproduced.",
      },
  });

  auto bingx = json_ws(
      "trades",
      "open-api-swap.bingx.com",
      "/swap-market",
      R"({"id":"cxet-bingx","reqType":"sub","dataType":"BTC-USDT@trade"})",
      {"live_trades"},
      anchor("src/src/exchanges/bingx/swap/config.cpp", "kSubscribeTrades"),
      WsAckKind::BingxSubscription,
      "BTC-USDT@trade",
      "BTC-USDT",
      "cxet-bingx");
  bingx.data_kind = WsDataKind::BingxTrades;
  bingx.compression = Compression::Gzip;
  bingx.ack_implies_data = true;
  products.push_back(ProductSpec{
      .venue = "bingx",
      .product = "futures",
      .credential_prefix = "BINGX_API",
      .public_ws = {std::move(bingx)},
  });

  auto phemex_trade = json_ws(
      "trades",
      "ws.phemex.com",
      "/",
      R"({"id":0,"method":"trade_p.subscribe","params":["BTCUSDT"]})",
      {"live_trades"},
      anchor("src/src/exchanges/phemex/usdm/config.cpp", "kSubscribeTrades"),
      WsAckKind::PhemexSubscription,
      "trade",
      "BTCUSDT",
      "0");
  auto phemex_l2 = json_ws(
      "orderbook",
      "ws.phemex.com",
      "/",
      R"({"id":0,"method":"orderbook_p.subscribe","params":["BTCUSDT",false,30]})",
      {"live_l2"},
      anchor("src/src/exchanges/phemex/usdm/config.cpp", "kSubscribeOrderBook"),
      WsAckKind::PhemexSubscription,
      "orderbook",
      "BTCUSDT",
      "0");
  auto phemex_bbo = json_ws(
      "book_ticker",
      "ws.phemex.com",
      "/",
      R"({"id":0,"method":"orderbook_p.subscribe","params":["BTCUSDT",false,1]})",
      {"live_bbo"},
      anchor(
          "src/src/exchanges/phemex/usdm/config.cpp",
          "kSubscribeBookTicker"),
      WsAckKind::PhemexSubscription,
      "orderbook",
      "BTCUSDT",
      "0");
  products.push_back(ProductSpec{
      .venue = "phemex",
      .product = "futures",
      .credential_prefix = "PHEMEX_API",
      .public_rest = {
          public_rest(
              "instrument_rules",
              "api.phemex.com",
              "/public/products",
              {"exchange_info", "instrument_catalog"}),
      },
      .public_ws = {
          std::move(phemex_trade),
          std::move(phemex_l2),
          std::move(phemex_bbo),
      },
  });

  products.push_back(ProductSpec{
      .venue = "hyperliquid",
      .product = "futures",
      .credential_prefix = "HYPERLIQUID_API",
      .public_ws = {
          json_ws(
              "trades",
              "api.hyperliquid.xyz",
              "/ws",
              R"({"method":"subscribe","subscription":{"type":"trades","coin":"BTC"}})",
              {"live_trades"},
              anchor(
                  "src/src/exchanges/hyperliquid/futures/config.cpp",
                  "kSubscribeTrades"),
              WsAckKind::HyperliquidSubscription,
              "trades",
              "BTC"),
      },
  });
}

}  // namespace exchange_probe
