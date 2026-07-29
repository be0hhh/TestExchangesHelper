#include "profile_builders.hpp"
#include "profile_factories.hpp"

namespace exchange_probe {
using namespace profile_factory;

// Legacy call sites retain provenance arguments for review history, but the
// standalone tool neither stores nor resolves paths outside this repository.
#define anchor(...) SourceAnchor{}
#define credential_anchor(...) SourceAnchor{}

void append_major_profiles(std::vector<ProductSpec>& products) {
  products.push_back(ProductSpec{
      .venue = "binance",
      .product = "spot",
      .credential_prefix = "BINANCE_SPOT_API",
      .public_rest = {
          public_rest(
              "instrument_rules",
              "api.binance.com",
              "/api/v3/exchangeInfo",
              {"exchange_info", "instrument_catalog"}),
          public_rest(
              "ticker_24h",
              "api.binance.com",
              "/api/v3/ticker/24hr",
              {"ticker_24h"}),
          public_rest(
              "public_trades",
              "api.binance.com",
              "/api/v3/aggTrades?symbol=BTCUSDT&limit=10",
              {"historical_trades"}),
      },
      .private_rest = {
          private_rest(
              "account",
              "api.binance.com",
              "/api/v3/account",
              {"balances"},
              RestContract::BinancePrivate,
              AuthKind::BinanceHmac),
          private_rest(
              "open_orders",
              "api.binance.com",
              "/api/v3/openOrders?symbol=BTCUSDT",
              {"open_orders"},
              RestContract::BinancePrivate,
              AuthKind::BinanceHmac),
          private_rest(
              "personal_fills",
              "api.binance.com",
              "/api/v3/myTrades?symbol=BTCUSDT&limit=10",
              {"fills_history"},
              RestContract::BinancePrivate,
              AuthKind::BinanceHmac),
      },
      .public_ws = {
          json_ws(
              "trades_json",
              "stream.binance.com",
              "/ws/btcusdt@trade",
              {},
              {"live_trades"},
              {},
              WsAckKind::None,
              "trade",
              "BTCUSDT"),
      },
      .fix_sessions = {
          {
              .name = "trades_fix_sbe",
              .host = "fix-md.binance.com",
              .port = 9002,
              .session_kind = "market_data",
              .capability = "live_trades",
              .core_anchor = anchor(
                  "src/src/exchanges/binance/spot/config.cpp",
                  "kSubscribeTradesFixSbe"),
              .payload_anchor = anchor(
                  "src/src/exchanges/binance/spot/fix/BinanceSpotFixSessionAuth.hpp",
                  "buildMarketDataFixPayload"),
          },
          {
              .name = "bbo_fix_sbe",
              .host = "fix-md.binance.com",
              .port = 9002,
              .session_kind = "market_data",
              .capability = "live_bbo",
              .core_anchor = anchor(
                  "src/src/exchanges/binance/spot/config.cpp",
                  "kSubscribeBookTickerFixSbe"),
              .payload_anchor = anchor(
                  "src/src/exchanges/binance/spot/fix/BinanceSpotFixSessionAuth.hpp",
                  "buildMarketDataFixPayload"),
          },
          {
              .name = "l2_fix_sbe",
              .host = "fix-md.binance.com",
              .port = 9002,
              .session_kind = "market_data",
              .capability = "live_l2",
              .core_anchor = anchor(
                  "src/src/exchanges/binance/spot/config.cpp",
                  "kSubscribeOrderBookFixSbe"),
              .payload_anchor = anchor(
                  "src/src/exchanges/binance/spot/fix/BinanceSpotFixSessionAuth.hpp",
                  "buildMarketDataFixPayload"),
          },
      },
      .notes = {
          "Binance spot selected public market wire is authenticated FIX/SBE.",
          "Private REST is a diagnostic HMAC variant, not the selected FIX session.",
      },
  });

  auto binance_lowercase = public_rest(
      "funding_lowercase",
      "fapi.binance.com",
      "/fapi/v1/premiumIndex?symbol=btcusdt",
      {"funding_current_symbol"},
      RestContract::BinanceFunding,
      "BTCUSDT");
  binance_lowercase = negative_rest(
      std::move(binance_lowercase),
      {400},
      {"-1121"});
  products.push_back(ProductSpec{
      .venue = "binance",
      .product = "futures",
      .credential_prefix = "BINANCE_FUTURES_API",
      .public_rest = {
          public_rest(
              "instrument_rules",
              "fapi.binance.com",
              "/fapi/v1/exchangeInfo",
              {"exchange_info", "instrument_catalog"}),
          public_rest(
              "ticker_24h",
              "fapi.binance.com",
              "/fapi/v1/ticker/24hr",
              {"ticker_24h"},
              RestContract::BinanceTicker24h,
              "BTCUSDT",
              anchor(
                  "src/src/exchanges/binance/fapi/reference/ReferenceCatalogV1.hpp")),
          public_rest(
              "funding_current",
              "fapi.binance.com",
              "/fapi/v1/premiumIndex",
              {"funding_current_all"},
              RestContract::BinanceFunding,
              "BTCUSDT",
              anchor(
                  "src/src/exchanges/binance/fapi/funding/FundingCurrentCatalog.hpp")),
          std::move(binance_lowercase),
          public_rest(
              "funding_history",
              "fapi.binance.com",
              "/fapi/v1/fundingRate?symbol=BTCUSDT&limit=10",
              {"funding_history"}),
          public_rest(
              "public_trades",
              "fapi.binance.com",
              "/fapi/v1/aggTrades?symbol=BTCUSDT&limit=10",
              {"historical_trades"}),
      },
      .private_rest = {
          private_rest(
              "account",
              "fapi.binance.com",
              "/fapi/v2/account",
              {"balances", "positions"},
              RestContract::BinancePrivate,
              AuthKind::BinanceHmac,
              Selection::CoreSelected),
          private_rest(
              "open_orders",
              "fapi.binance.com",
              "/fapi/v1/openOrders?symbol=BTCUSDT",
              {"open_orders"},
              RestContract::BinancePrivate,
              AuthKind::BinanceHmac,
              Selection::CoreSelected),
          private_rest(
              "personal_fills",
              "fapi.binance.com",
              "/fapi/v1/userTrades?symbol=BTCUSDT&limit=10",
              {"fills_history"},
              RestContract::BinancePrivate,
              AuthKind::BinanceHmac,
              Selection::CoreSelected),
      },
      .public_ws = {
          json_ws(
              "funding",
              "fstream.binance.com",
              "/market/ws",
              R"({"method":"SUBSCRIBE","params":["btcusdt@markPrice@1s"],"id":1})",
              {"funding_current_symbol"},
              anchor(
                  "src/src/exchanges/binance/fapi/config.cpp",
                  "kSubscribeFunding"),
              WsAckKind::BinanceSubscription,
              "markPrice",
              "BTCUSDT",
              "1"),
      },
      .credential_anchor = credential_anchor(
          "src/src/exchanges/binance/fapi/config.cpp",
          "BINANCE_FUTURES_API"),
  });

  products.push_back(ProductSpec{
      .venue = "bybit",
      .product = "spot",
      .credential_prefix = "BYBIT_UTA_API",
      .public_rest = {
          public_rest(
              "instrument_rules",
              "api.bybit.com",
              "/v5/market/instruments-info?category=spot&limit=1000",
              {"exchange_info", "instrument_catalog"}),
          public_rest(
              "ticker_24h",
              "api.bybit.com",
              "/v5/market/tickers?category=spot",
              {"ticker_24h"}),
          public_rest(
              "public_trades",
              "api.bybit.com",
              "/v5/market/recent-trade?category=spot&symbol=BTCUSDT&limit=10",
              {"historical_trades"}),
      },
      .private_rest = {
          private_rest(
              "account",
              "api.bybit.com",
              "/v5/account/wallet-balance?accountType=UNIFIED",
              {"balances"},
              RestContract::BybitPrivate,
              AuthKind::BybitHmac,
              Selection::CoreSelected),
          private_rest(
              "open_orders",
              "api.bybit.com",
              "/v5/order/realtime?category=spot&openOnly=0",
              {"open_orders"},
              RestContract::BybitPrivate,
              AuthKind::BybitHmac,
              Selection::CoreSelected),
          private_rest(
              "personal_fills",
              "api.bybit.com",
              "/v5/execution/list?category=spot&limit=10",
              {"fills_history"},
              RestContract::BybitPrivate,
              AuthKind::BybitHmac,
              Selection::CoreSelected),
      },
      .public_ws = {
          json_ws(
              "trades",
              "stream.bybit.com",
              "/v5/public/spot",
              R"({"op":"subscribe","args":["publicTrade.BTCUSDT"]})",
              {"live_trades"},
              anchor(
                  "src/src/exchanges/bybit/spot/config.cpp",
                  "kSubscribeTrades"),
              WsAckKind::BybitSubscription,
              "publicTrade",
              "BTCUSDT"),
      },
      .credential_anchor = credential_anchor(
          "src/src/exchanges/bybit/spot/config.cpp",
          "BYBIT_UTA_API"),
  });

  auto bybit_lowercase = negative_rest(
      public_rest(
          "funding_lowercase",
          "api.bybit.com",
          "/v5/market/tickers?category=linear&symbol=btcusdt",
          {"funding_current_symbol"},
          RestContract::BybitFunding,
          "BTCUSDT"),
      {400},
      {"10001"});
  products.push_back(ProductSpec{
      .venue = "bybit",
      .product = "futures",
      .credential_prefix = "BYBIT_UTA_API",
      .public_rest = {
          public_rest(
              "instrument_rules",
              "api.bybit.com",
              "/v5/market/instruments-info?category=linear&limit=1000",
              {"exchange_info", "instrument_catalog"}),
          public_rest(
              "ticker_24h",
              "api.bybit.com",
              "/v5/market/tickers?category=linear",
              {"ticker_24h"},
              RestContract::BybitTicker24h,
              "BTCUSDT",
              anchor(
                  "src/src/exchanges/bybit/linear/reference/ReferenceCatalogV1.hpp")),
          public_rest(
              "funding_current",
              "api.bybit.com",
              "/v5/market/tickers?category=linear",
              {"funding_current_all"},
              RestContract::BybitFunding,
              "BTCUSDT",
              anchor(
                  "src/src/exchanges/bybit/linear/funding/FundingCurrentCatalogV1.hpp")),
          std::move(bybit_lowercase),
          public_rest(
              "funding_history",
              "api.bybit.com",
              "/v5/market/funding/history?category=linear&symbol=BTCUSDT&limit=10",
              {"funding_history"}),
          public_rest(
              "public_trades",
              "api.bybit.com",
              "/v5/market/recent-trade?category=linear&symbol=BTCUSDT&limit=10",
              {"historical_trades"}),
      },
      .private_rest = {
          private_rest(
              "account",
              "api.bybit.com",
              "/v5/account/wallet-balance?accountType=UNIFIED",
              {"balances", "positions"},
              RestContract::BybitPrivate,
              AuthKind::BybitHmac,
              Selection::CoreSelected),
          private_rest(
              "open_orders",
              "api.bybit.com",
              "/v5/order/realtime?category=linear&openOnly=0",
              {"open_orders"},
              RestContract::BybitPrivate,
              AuthKind::BybitHmac,
              Selection::CoreSelected),
          private_rest(
              "personal_fills",
              "api.bybit.com",
              "/v5/execution/list?category=linear&limit=10",
              {"fills_history"},
              RestContract::BybitPrivate,
              AuthKind::BybitHmac,
              Selection::CoreSelected),
      },
      .public_ws = {
          json_ws(
              "funding",
              "stream.bybit.com",
              "/v5/public/linear",
              R"({"op":"subscribe","args":["tickers.BTCUSDT"]})",
              {"funding_current_symbol"},
              anchor(
                  "src/src/exchanges/bybit/linear/config.cpp",
                  "kSubscribeFunding"),
              WsAckKind::BybitSubscription,
              "tickers",
              "BTCUSDT"),
      },
      .credential_anchor = credential_anchor(
          "src/src/exchanges/bybit/linear/config.cpp",
          "BYBIT_UTA_API"),
  });

  products.push_back(ProductSpec{
      .venue = "okx",
      .product = "spot",
      .credential_prefix = "OKX_SPOT_API",
      .public_rest = {
          public_rest(
              "instrument_rules",
              "www.okx.com",
              "/api/v5/public/instruments?instType=SPOT",
              {"exchange_info", "instrument_catalog"}),
          public_rest(
              "ticker_24h",
              "www.okx.com",
              "/api/v5/market/tickers?instType=SPOT",
              {"ticker_24h"}),
          public_rest(
              "public_trades",
              "www.okx.com",
              "/api/v5/market/trades?instId=BTC-USDT&limit=10",
              {"historical_trades"}),
      },
      .private_rest = {
          private_rest(
              "account",
              "www.okx.com",
              "/api/v5/account/balance",
              {"balances"},
              RestContract::OkxPrivate,
              AuthKind::OkxHmac,
              Selection::CoreSelected),
          private_rest(
              "open_orders",
              "www.okx.com",
              "/api/v5/trade/orders-pending?instType=SPOT",
              {"open_orders"},
              RestContract::OkxPrivate,
              AuthKind::OkxHmac,
              Selection::CoreSelected),
          private_rest(
              "personal_fills",
              "www.okx.com",
              "/api/v5/trade/fills-history?instType=SPOT&limit=10",
              {"fills_history"},
              RestContract::OkxPrivate,
              AuthKind::OkxHmac,
              Selection::CoreSelected),
      },
      .public_ws = {
          json_ws(
              "trades",
              "ws.okx.com",
              "/ws/v5/business",
              R"({"op":"subscribe","args":[{"channel":"trades-all","instId":"BTC-USDT"}]})",
              {"live_trades"},
              anchor(
                  "src/src/exchanges/okx/spot/config.cpp",
                  "kSubscribeTrades"),
              WsAckKind::OkxSubscription,
              "trades-all",
              "BTC-USDT"),
      },
      .credential_anchor = credential_anchor(
          "src/src/exchanges/okx/spot/config.cpp",
          "OKX_SPOT_API"),
  });

  auto okx_lowercase = negative_rest(
      public_rest(
          "funding_lowercase",
          "www.okx.com",
          "/api/v5/public/funding-rate?instId=btc-usdt-swap",
          {"funding_current_symbol"},
          RestContract::OkxFunding,
          "BTC-USDT-SWAP"),
      {400},
      {"51001"});
  products.push_back(ProductSpec{
      .venue = "okx",
      .product = "futures",
      .credential_prefix = "OKX_FUTURES_API",
      .public_rest = {
          public_rest(
              "instrument_rules",
              "www.okx.com",
              "/api/v5/public/instruments?instType=SWAP",
              {"exchange_info", "instrument_catalog"}),
          public_rest(
              "ticker_24h",
              "www.okx.com",
              "/api/v5/market/tickers?instType=SWAP",
              {"ticker_24h"},
              RestContract::OkxTicker24h,
              "BTC-USDT-SWAP",
              anchor(
                  "src/src/exchanges/okx/swap/reference/ReferenceCatalogV1.hpp")),
          public_rest(
              "funding_current",
              "www.okx.com",
              "/api/v5/public/funding-rate?instId=BTC-USDT-SWAP",
              {"funding_current_symbol"},
              RestContract::OkxFunding,
              "BTC-USDT-SWAP",
              anchor(
                  "src/src/exchanges/okx/swap/funding/FundingCurrentCatalogV1.hpp")),
          std::move(okx_lowercase),
          public_rest(
              "funding_history",
              "www.okx.com",
              "/api/v5/public/funding-rate-history?instId=BTC-USDT-SWAP&limit=10",
              {"funding_history"}),
          public_rest(
              "public_trades",
              "www.okx.com",
              "/api/v5/market/trades?instId=BTC-USDT-SWAP&limit=10",
              {"historical_trades"}),
      },
      .private_rest = {
          private_rest(
              "positions",
              "www.okx.com",
              "/api/v5/account/positions?instType=SWAP",
              {"positions"},
              RestContract::OkxPrivate,
              AuthKind::OkxHmac,
              Selection::CoreSelected),
          private_rest(
              "open_orders",
              "www.okx.com",
              "/api/v5/trade/orders-pending?instType=SWAP",
              {"open_orders"},
              RestContract::OkxPrivate,
              AuthKind::OkxHmac,
              Selection::CoreSelected),
          private_rest(
              "personal_fills",
              "www.okx.com",
              "/api/v5/trade/fills-history?instType=SWAP&limit=10",
              {"fills_history"},
              RestContract::OkxPrivate,
              AuthKind::OkxHmac,
              Selection::CoreSelected),
      },
      .public_ws = {
          json_ws(
              "funding",
              "ws.okx.com",
              "/ws/v5/public",
              R"({"op":"subscribe","args":[{"channel":"funding-rate","instId":"BTC-USDT-SWAP"}]})",
              {"funding_current_symbol"},
              anchor(
                  "src/src/exchanges/okx/swap/config.cpp",
                  "kSubscribeFunding"),
              WsAckKind::OkxSubscription,
              "funding-rate",
              "BTC-USDT-SWAP"),
      },
      .credential_anchor = credential_anchor(
          "src/src/exchanges/okx/swap/config.cpp",
          "OKX_FUTURES_API"),
  });

  products.push_back(ProductSpec{
      .venue = "gate",
      .product = "spot",
      .credential_prefix = "GATE_SPOT_API",
      .public_rest = {
          public_rest(
              "instrument_rules",
              "api.gateio.ws",
              "/api/v4/spot/currency_pairs",
              {"exchange_info", "instrument_catalog"}),
          public_rest(
              "ticker_24h",
              "api.gateio.ws",
              "/api/v4/spot/tickers",
              {"ticker_24h"}),
          public_rest(
              "public_trades",
              "api.gateio.ws",
              "/api/v4/spot/trades?currency_pair=BTC_USDT&limit=10",
              {"historical_trades"}),
      },
      .public_ws = {
          json_ws(
              "trades",
              "api.gateio.ws",
              "/ws/v4/",
              R"({"time":1,"channel":"spot.trades","event":"subscribe","payload":["BTC_USDT"]})",
              {"live_trades"},
              {},
              WsAckKind::GateSubscription,
              "spot.trades",
              "BTC_USDT"),
      },
      .notes = {
          "Private Gate spot profile is not registered by this diagnostic.",
      },
  });

  auto gate_lowercase = negative_rest(
      public_rest(
          "funding_lowercase",
          "api.gateio.ws",
          "/api/v4/futures/usdt/contracts/btc_usdt",
          {"funding_current_symbol"},
          RestContract::GateFunding,
          "BTC_USDT"),
      {400},
      {"INVALID_PARAM_VALUE"});
  products.push_back(ProductSpec{
      .venue = "gate",
      .product = "futures",
      .credential_prefix = "GATE_UTA_API",
      .public_rest = {
          public_rest(
              "instrument_rules",
              "api.gateio.ws",
              "/api/v4/futures/usdt/contracts",
              {"exchange_info", "instrument_catalog"}),
          public_rest(
              "ticker_24h",
              "api.gateio.ws",
              "/api/v4/futures/usdt/tickers",
              {"ticker_24h"},
              RestContract::GateTicker24h,
              "BTC_USDT",
              anchor(
                  "src/src/exchanges/gate/usdt/reference/ReferenceCatalogV1.hpp")),
          public_rest(
              "funding_current",
              "api.gateio.ws",
              "/api/v4/futures/usdt/contracts",
              {"funding_current_all"},
              RestContract::GateFunding,
              "BTC_USDT",
              anchor(
                  "src/src/exchanges/gate/usdt/funding/FundingCurrentCatalogV1.hpp")),
          std::move(gate_lowercase),
          public_rest(
              "funding_history",
              "api.gateio.ws",
              "/api/v4/futures/usdt/funding_rate?contract=BTC_USDT&limit=10",
              {"funding_history"}),
          public_rest(
              "public_trades",
              "api.gateio.ws",
              "/api/v4/futures/usdt/trades?contract=BTC_USDT&limit=10",
              {"historical_trades"}),
      },
      .private_rest = {
          private_rest(
              "account",
              "api.gateio.ws",
              "/api/v4/futures/usdt/accounts",
              {"balances", "positions"},
              RestContract::GatePrivate,
              AuthKind::GateHmac,
              Selection::CoreSelected),
          private_rest(
              "open_orders",
              "api.gateio.ws",
              "/api/v4/futures/usdt/orders?contract=BTC_USDT&status=open",
              {"open_orders"},
              RestContract::GatePrivate,
              AuthKind::GateHmac,
              Selection::CoreSelected),
          private_rest(
              "personal_fills",
              "api.gateio.ws",
              "/api/v4/futures/usdt/my_trades?contract=BTC_USDT&limit=10",
              {"fills_history"},
              RestContract::GatePrivate,
              AuthKind::GateHmac,
              Selection::CoreSelected),
      },
      .public_ws = {
          gate_sbe(
              "trades_sbe",
              "futures.trades",
              "BTC_USDT",
              "live_trades",
              2,
              anchor(
                  "src/src/exchanges/gate/usdt/config.cpp",
                  "kSubscribeTradesSbe")),
          gate_sbe(
              "book_ticker_sbe",
              "futures.book_ticker",
              "BTC_USDT",
              "live_bbo",
              1,
              anchor(
                  "src/src/exchanges/gate/usdt/config.cpp",
                  "kSubscribeBookTickerSbe")),
          gate_sbe(
              "orderbook_sbe",
              "futures.obu",
              "ob.BTC_USDT.50",
              "live_l2",
              0,
              anchor(
                  "src/src/exchanges/gate/usdt/config.cpp",
                  "kSubscribeOrderBookSbe")),
      },
      .credential_anchor = credential_anchor(
          "src/src/exchanges/gate/usdt/config.cpp",
          "GATE_UTA_API"),
  });

  products.push_back(ProductSpec{
      .venue = "kucoin",
      .product = "spot",
      .credential_prefix = "KUCOIN_UTA_API",
      .public_rest = {
          public_rest(
              "instrument_rules",
              "api.kucoin.com",
              "/api/v2/symbols",
              {"exchange_info", "instrument_catalog"}),
          public_rest(
              "ticker_24h",
              "api.kucoin.com",
              "/api/v1/market/allTickers",
              {"ticker_24h"}),
          public_rest(
              "public_trades",
              "api.kucoin.com",
              "/api/v1/market/histories?symbol=BTC-USDT",
              {"historical_trades"}),
      },
      .private_rest = {
          private_rest(
              "account",
              "api.kucoin.com",
              "/api/v1/accounts",
              {"balances"},
              RestContract::KucoinPrivate,
              AuthKind::KucoinHmac,
              Selection::CoreSelected),
          private_rest(
              "open_orders",
              "api.kucoin.com",
              "/api/v1/orders?status=active",
              {"open_orders"},
              RestContract::KucoinPrivate,
              AuthKind::KucoinHmac,
              Selection::CoreSelected),
          private_rest(
              "personal_fills",
              "api.kucoin.com",
              "/api/v1/fills?pageSize=10",
              {"fills_history"},
              RestContract::KucoinPrivate,
              AuthKind::KucoinHmac,
              Selection::CoreSelected),
      },
      .public_ws = {
          kucoin_ws(
              "trades",
              "x-push-spot.kucoin.com",
              R"({"id":"probe-kucoin-1","action":"SUBSCRIBE","channel":"trade","tradeType":"SPOT","symbol":"BTC-USDT"})",
              {"live_trades"},
              "trade",
              "BTC-USDT",
              anchor(
                  "src/src/exchanges/kucoin/spot/config.cpp",
                  "kSubscribeTrades")),
          kucoin_ws(
              "book_ticker",
              "x-push-spot.kucoin.com",
              R"({"id":"probe-kucoin-1","action":"SUBSCRIBE","channel":"obu","tradeType":"SPOT","symbol":"BTC-USDT","depth":"1","rpiFilter":0})",
              {"live_bbo"},
              "obu",
              "BTC-USDT",
              anchor(
                  "src/src/exchanges/kucoin/spot/config.cpp",
                  "kSubscribeBookTicker")),
      },
      .notes = {
          "KuCoin UTA Pro public WS is binary JSON and requires welcome before subscribe.",
      },
      .credential_anchor = credential_anchor(
          "src/src/exchanges/kucoin/spot/config.cpp",
          "KUCOIN_UTA_API"),
  });

  auto kucoin_lowercase = negative_rest(
      public_rest(
          "ticker_lowercase",
          "api.kucoin.com",
          "/api/ua/v1/market/ticker?tradeType=FUTURES&symbol=xbtusdtm",
          {"ticker_24h"},
          RestContract::KucoinTicker24h,
          "XBTUSDTM"),
      {400},
      {"400100"});
  products.push_back(ProductSpec{
      .venue = "kucoin",
      .product = "futures",
      .credential_prefix = "KUCOIN_UTA_API",
      .public_rest = {
          public_rest(
              "instrument_rules",
              "api.kucoin.com",
              "/api/ua/v1/market/instrument?tradeType=FUTURES",
              {"exchange_info", "instrument_catalog"}),
          public_rest(
              "ticker_24h",
              "api.kucoin.com",
              "/api/ua/v1/market/ticker?tradeType=FUTURES",
              {"ticker_24h"},
              RestContract::KucoinTicker24h,
              "XBTUSDTM",
              anchor(
                  "src/src/exchanges/kucoin/uta/reference/ReferenceCatalogV1.hpp")),
          std::move(kucoin_lowercase),
          public_rest(
              "public_trades",
              "api.kucoin.com",
              "/api/ua/v1/market/trade?tradeType=FUTURES&symbol=XBTUSDTM",
              {"historical_trades"}),
          public_rest(
              "funding_current",
              "api-futures.kucoin.com",
              "/api/v1/contracts/active",
              {"funding_current_all"},
              RestContract::KucoinFunding,
              "XBTUSDTM",
              anchor(
                  "src/src/exchanges/kucoin/uta/funding/FundingCurrentCatalogV1.hpp")),
          public_rest(
              "funding_history",
              "api-futures.kucoin.com",
              "/api/v1/contract/funding-rates?symbol=XBTUSDTM&from=1700310700000&to=1702310700000",
              {"funding_history"}),
      },
      .private_rest = {
          private_rest(
              "account",
              "api.kucoin.com",
              "/api/ua/v1/unified/account/overview",
              {"balances"},
              RestContract::KucoinPrivate,
              AuthKind::KucoinHmac,
              Selection::CoreSelected),
          private_rest(
              "positions",
              "api.kucoin.com",
              "/api/ua/v1/unified/position/open-list?pageNumber=1&pageSize=10",
              {"positions"},
              RestContract::KucoinPrivate,
              AuthKind::KucoinHmac,
              Selection::CoreSelected),
          private_rest(
              "open_orders",
              "api.kucoin.com",
              "/api/ua/v1/unified/order/open-list?tradeType=FUTURES&pageNumber=1&pageSize=10",
              {"open_orders"},
              RestContract::KucoinPrivate,
              AuthKind::KucoinHmac,
              Selection::CoreSelected),
          private_rest(
              "personal_fills",
              "api.kucoin.com",
              "/api/ua/v1/unified/order/execution?tradeType=FUTURES&pageSize=10&startAt=1700310700000",
              {"fills_history"},
              RestContract::KucoinPrivate,
              AuthKind::KucoinHmac,
              Selection::CoreSelected),
      },
      .public_ws = {
          kucoin_ws(
              "trades",
              "x-push-futures.kucoin.com",
              R"({"id":"probe-kucoin-1","action":"SUBSCRIBE","channel":"trade","tradeType":"FUTURES","symbol":"XBTUSDTM"})",
              {"live_trades"},
              "trade",
              "XBTUSDTM",
              anchor(
                  "src/src/exchanges/kucoin/uta/config.cpp",
                  "kSubscribeTrades")),
          kucoin_ws(
              "funding",
              "x-push-futures.kucoin.com",
              R"({"id":"probe-kucoin-1","action":"SUBSCRIBE","channel":"funding-fee","symbol":"XBTUSDTM"})",
              {"funding_current_symbol"},
              "funding-fee",
              "XBTUSDTM",
              anchor(
                  "src/src/exchanges/kucoin/uta/config.cpp",
                  "kSubscribeFunding")),
      },
      .credential_anchor = credential_anchor(
          "src/src/exchanges/kucoin/uta/config.cpp",
          "KUCOIN_UTA_API"),
  });

  products.push_back(ProductSpec{
      .venue = "bitget",
      .product = "spot",
      .credential_prefix = "BITGET_UTA_API",
      .public_rest = {
          public_rest(
              "instrument_rules",
              "api.bitget.com",
              "/api/v2/spot/public/symbols",
              {"exchange_info", "instrument_catalog"}),
          public_rest(
              "ticker_24h",
              "api.bitget.com",
              "/api/v2/spot/market/tickers",
              {"ticker_24h"}),
          public_rest(
              "public_trades",
              "api.bitget.com",
              "/api/v2/spot/market/fills?symbol=BTCUSDT&limit=10",
              {"historical_trades"}),
      },
      .public_ws = {
          bitget_sbe(
              "trades_sbe",
              "spot",
              "publicTrade",
              "live_trades",
              1003,
              anchor(
                  "src/src/exchanges/bitget/uta/config.cpp",
                  "kSpotSubscribeTradesSbe")),
          bitget_sbe(
              "book_ticker_sbe",
              "spot",
              "books1",
              "live_bbo",
              1002,
              anchor(
                  "src/src/exchanges/bitget/uta/config.cpp",
                  "kSpotSubscribeBookTickerSbe")),
          bitget_sbe(
              "orderbook_sbe",
              "spot",
              "books50",
              "live_l2",
              0,
              anchor(
                  "src/src/exchanges/bitget/uta/config.cpp",
                  "kSpotSubscribeOrderBookSbe")),
      },
      .notes = {
          "Bitget spot selected market wire is SBE.",
          "No private REST profile is declared for spot.",
      },
      .credential_anchor = credential_anchor(
          "src/src/exchanges/bitget/uta/config.cpp",
          "BITGET_UTA_API"),
  });

  products.push_back(ProductSpec{
      .venue = "bitget",
      .product = "futures",
      .credential_prefix = "BITGET_UTA_API",
      .public_rest = {
          public_rest(
              "instrument_rules",
              "api.bitget.com",
              "/api/v3/market/instruments?category=USDT-FUTURES",
              {"exchange_info", "instrument_catalog"}),
          public_rest(
              "ticker_24h",
              "api.bitget.com",
              "/api/v3/market/tickers?category=USDT-FUTURES",
              {"ticker_24h"},
              RestContract::BitgetTicker24h,
              "BTCUSDT",
              anchor(
                  "src/src/exchanges/bitget/uta/reference/ReferenceCatalogV1.hpp")),
          public_rest(
              "funding_current",
              "api.bitget.com",
              "/api/v3/market/current-fund-rate?category=USDT-FUTURES",
              {"funding_current_all"},
              RestContract::BitgetFunding,
              "BTCUSDT",
              anchor(
                  "src/src/exchanges/bitget/uta/funding/FundingCurrentCatalogV1.hpp")),
          public_rest(
              "funding_lowercase",
              "api.bitget.com",
              "/api/v3/market/current-fund-rate?category=USDT-FUTURES&symbol=btcusdt",
              {"funding_current_symbol"},
              RestContract::BitgetFunding,
              "BTCUSDT"),
          public_rest(
              "funding_history",
              "api.bitget.com",
              "/api/v3/market/history-fund-rate?category=USDT-FUTURES&symbol=BTCUSDT&limit=10&pageNo=1",
              {"funding_history"}),
          public_rest(
              "public_trades",
              "api.bitget.com",
              "/api/v2/mix/market/fills?symbol=BTCUSDT&productType=USDT-FUTURES&limit=10",
              {"historical_trades"}),
      },
      .private_rest = {
          private_rest(
              "account",
              "api.bitget.com",
              "/api/v3/account/assets",
              {"balances", "positions"},
              RestContract::BitgetPrivate,
              AuthKind::BitgetHmac,
              Selection::CoreSelected),
          private_rest(
              "open_orders",
              "api.bitget.com",
              "/api/v3/trade/unfilled-orders?category=USDT-FUTURES&symbol=BTCUSDT",
              {"open_orders"},
              RestContract::BitgetPrivate,
              AuthKind::BitgetHmac,
              Selection::CoreSelected),
      },
      .public_ws = {
          bitget_sbe(
              "trades_sbe",
              "usdt-futures",
              "publicTrade",
              "live_trades",
              1003,
              anchor(
                  "src/src/exchanges/bitget/uta/config.cpp",
                  "kSubscribeTradesSbe")),
          bitget_sbe(
              "book_ticker_sbe",
              "usdt-futures",
              "books1",
              "live_bbo",
              1002,
              anchor(
                  "src/src/exchanges/bitget/uta/config.cpp",
                  "kSubscribeBookTickerSbe")),
          bitget_sbe(
              "orderbook_sbe",
              "usdt-futures",
              "books50",
              "live_l2",
              0,
              anchor(
                  "src/src/exchanges/bitget/uta/config.cpp",
                  "kSubscribeOrderBookSbe")),
      },
      .credential_anchor = credential_anchor(
          "src/src/exchanges/bitget/uta/config.cpp",
          "BITGET_UTA_API"),
  });

}

#undef credential_anchor
#undef anchor

}  // namespace exchange_probe
