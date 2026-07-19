from __future__ import annotations

"""Canonical registry of exchange API probe profiles."""

import json

import auth
from capability_matrix import enrich
from model import FixCase, ProductSpec, RestCase, WsCase


def _ws(name: str, host: str, path: str, payload: dict | None = None,
        ack: tuple[str, ...] = (), **kwargs) -> WsCase:
    kwargs.setdefault("status", "generic_api_only")
    kwargs.setdefault("selection", "core_selected" if kwargs.get("core_anchor") else "diagnostic_variant")
    return WsCase(name, host, path,
                  json.dumps(payload, separators=(",", ":")).encode() if payload else b"", ack,
                  **kwargs)


def _kucoin_welcome(value: object, opcode: int) -> bool:
    return opcode == 0x2 and isinstance(value, dict) and value.get("message") == "welcome" and \
        isinstance(value.get("pingInterval"), int) and value["pingInterval"] > 0


def _kucoin_ack(value: object, opcode: int) -> bool:
    return opcode == 0x2 and value == {"id": "cxet-kucoin-1", "result": True}


def _kucoin_data(value: object, opcode: int) -> bool:
    return opcode == 0x2 and isinstance(value, dict) and value.get("id") != "cxet-kucoin-1"


def _kucoin_public(name: str, host: str, payload: dict, anchor: str) -> WsCase:
    return _ws(name, host, "/", payload,
               subscribe_opcode=0x2, expected_inbound_opcode=0x2,
               read_before_subscribe=True, welcome_validator=_kucoin_welcome,
               ack_validator=_kucoin_ack, data_validator=_kucoin_data,
               require_data_after_ack=True, core_anchor=anchor,
               selected_wire="binary_json", status="core_aligned")


def _gate_sbe_ack(channel: str):
    def validate(value: object, opcode: int) -> bool:
        return opcode == 0x1 and isinstance(value, dict) and value.get("event") == "subscribe" and \
            value.get("channel") == channel and isinstance(value.get("result"), dict) and \
            value["result"].get("status") == "success"
    return validate


def _bitget_sbe_ack(inst_type: str, topic: str, symbol: str):
    def validate(value: object, opcode: int) -> bool:
        return opcode == 0x1 and isinstance(value, dict) and value.get("event") == "subscribe" and \
            value.get("arg") == {"instType": inst_type, "topic": topic, "symbol": symbol}
    return validate


def _gate_sbe(name: str, channel: str, payload_item: str, anchor: str) -> WsCase:
    return _ws(name, "fx-ws.gateio.ws", "/v4/ws/usdt/sbe?sbe_schema_id=1", {
        "time": 1, "channel": channel, "event": "subscribe", "payload": [payload_item]},
        ack_validator=_gate_sbe_ack(channel), expected_inbound_opcode=0x2,
        binary_data=True, require_data_after_ack=True, selected_wire="sbe_binary",
        core_anchor=anchor, parser_anchor=anchor, status="core_aligned")


def _bitget_sbe(name: str, inst_type: str, topic: str, symbol: str, anchor: str) -> WsCase:
    return _ws(name, "ws.bitget.com", "/v3/ws/public/sbe", {
        "op": "subscribe", "args": [{"instType": inst_type, "topic": topic, "symbol": symbol}]},
        ack_validator=_bitget_sbe_ack(inst_type, topic, symbol), expected_inbound_opcode=0x2,
        handshake_headers=(("Origin", "https://www.bitget.com"),
                           ("Referer", "https://www.bitget.com/")),
        binary_data=True, require_data_after_ack=True, selected_wire="sbe_binary",
        core_anchor=anchor, parser_anchor=anchor, status="core_aligned")


def _hyperliquid_ack(value: object, opcode: int) -> bool:
    return opcode == 0x1 and isinstance(value, dict) and value.get("channel") == "subscriptionResponse" and \
        isinstance(value.get("data"), dict) and value["data"].get("method") == "subscribe" and \
        isinstance(value["data"].get("subscription"), dict)


def _phemex_ack(value: object, opcode: int) -> bool:
    return opcode == 0x1 and isinstance(value, dict) and value.get("id") == 0 and \
        value.get("error") in (None, {})


def _bingx_ack(value: object, opcode: int) -> bool:
    return opcode in (0x1, 0x2) and isinstance(value, dict) and value.get("code") == 0


def _bingx_data(value: object, opcode: int) -> bool:
    return opcode in (0x1, 0x2) and isinstance(value, dict) and value.get("dataType") == "BTC-USDT@trade" and \
        isinstance(value.get("data"), list) and bool(value["data"])


def _is_market_data(value: object, opcode: int) -> bool:
    return opcode in (0x1, 0x2) and isinstance(value, dict) and (
        "data" in value or ("ch" in value and "tick" in value))


def _observed_data_ws(name: str, host: str, path: str, payload: dict[str, object], anchor: str,
              *, compression: str = "", selected_wire: str = "json", binary_data: bool = False,
              data_validator=None, application_heartbeat: str = "") -> WsCase:
    if data_validator is None and not binary_data:
        data_validator = _is_market_data
    return WsCase(
        name=name,
        host=host,
        path=path,
        subscribe=json.dumps(payload, separators=(",", ":")).encode(),
        compression=compression,
        selected_wire=selected_wire,
        binary_data=binary_data,
        data_validator=data_validator,
        data_implies_ack=True,
        require_data_after_ack=True,
        application_heartbeat=application_heartbeat,
        core_anchor=anchor,
        parser_anchor=anchor,
        status="core_aligned",
    )


_BASE_PRODUCTS: tuple[ProductSpec, ...] = (
    ProductSpec("binance", "spot", "BINANCE_SPOT_API", (
        RestCase("instrument_rules", "api.binance.com", "/api/v3/exchangeInfo"),
        RestCase("ticker_24h", "api.binance.com", "/api/v3/ticker/24hr"),
        RestCase("public_trades", "api.binance.com", "/api/v3/aggTrades?symbol=BTCUSDT&limit=10"),
    ), (
        RestCase("account", "api.binance.com", "/api/v3/account", True),
        RestCase("open_orders", "api.binance.com", "/api/v3/openOrders?symbol=BTCUSDT", True),
        RestCase("personal_fills", "api.binance.com", "/api/v3/myTrades?symbol=BTCUSDT&limit=10", True),
    ), (
        _ws("trades_json", "stream.binance.com", "/ws/btcusdt@trade",
            capability="live_trades", selected_wire="json", selection="diagnostic_variant",
            require_data_after_ack=True),
    ), auth.binance,
                ("core selected public market wire is authenticated FIX/SBE; legacy JSON WS is intentionally not a core proof",),
                fix_sessions=(
                    FixCase("trades_fix_sbe", "fix-md.binance.com", 9002, "market_data", "fix_sbe_binary",
                            "BINANCE_FIX_API", "src/src/exchanges/binance/spot/config.cpp:kSubscribeTradesFixSbe",
                            "src/src/exchanges/binance/spot/fix/BinanceSpotFixSessionAuth.hpp:buildMarketDataFixPayload",
                            "live_trades"),
                    FixCase("bbo_fix_sbe", "fix-md.binance.com", 9002, "market_data", "fix_sbe_binary",
                            "BINANCE_FIX_API", "src/src/exchanges/binance/spot/config.cpp:kSubscribeBookTickerFixSbe",
                            "src/src/exchanges/binance/spot/fix/BinanceSpotFixSessionAuth.hpp:buildMarketDataFixPayload",
                            "live_bbo"),
                    FixCase("l2_fix_sbe", "fix-md.binance.com", 9002, "market_data", "fix_sbe_binary",
                            "BINANCE_FIX_API", "src/src/exchanges/binance/spot/config.cpp:kSubscribeOrderBookFixSbe",
                            "src/src/exchanges/binance/spot/fix/BinanceSpotFixSessionAuth.hpp:buildMarketDataFixPayload",
                            "live_l2"),
                )),
    ProductSpec("binance", "futures", "BINANCE_FUTURES_API", (
        RestCase("instrument_rules", "fapi.binance.com", "/fapi/v1/exchangeInfo"),
        RestCase("ticker_24h", "fapi.binance.com", "/fapi/v1/ticker/24hr"),
        RestCase("funding_current", "fapi.binance.com", "/fapi/v1/premiumIndex"),
        RestCase("funding_history", "fapi.binance.com", "/fapi/v1/fundingRate?symbol=BTCUSDT&limit=10"),
        RestCase("public_trades", "fapi.binance.com", "/fapi/v1/aggTrades?symbol=BTCUSDT&limit=10"),
    ), (
        RestCase("account", "fapi.binance.com", "/fapi/v2/account", True),
        RestCase("open_orders", "fapi.binance.com", "/fapi/v1/openOrders?symbol=BTCUSDT", True),
        RestCase("personal_fills", "fapi.binance.com", "/fapi/v1/userTrades?symbol=BTCUSDT&limit=10", True),
    ), (_ws("funding", "fstream.binance.com", "/market/ws", {
        "method":"SUBSCRIBE", "params":["btcusdt@markPrice@1s"], "id":1}, ("result", "id"),
        require_data_after_ack=True,
        core_anchor="src/src/exchanges/binance/fapi/config.cpp:kSubscribeFunding",
        status="core_aligned"),), auth.binance),
    ProductSpec("bybit", "spot", "BYBIT_SPOT_API", (
        RestCase("instrument_rules", "api.bybit.com", "/v5/market/instruments-info?category=spot&limit=1000"),
        RestCase("ticker_24h", "api.bybit.com", "/v5/market/tickers?category=spot"),
        RestCase("public_trades", "api.bybit.com", "/v5/market/recent-trade?category=spot&symbol=BTCUSDT&limit=10"),
    ), (
        RestCase("account", "api.bybit.com", "/v5/account/wallet-balance?accountType=UNIFIED", True),
        RestCase("open_orders", "api.bybit.com", "/v5/order/realtime?category=spot&openOnly=0", True),
        RestCase("personal_fills", "api.bybit.com", "/v5/execution/list?category=spot&limit=10", True),
    ), (_ws("trades", "stream.bybit.com", "/v5/public/spot", {"op":"subscribe","args":["publicTrade.BTCUSDT"]}, ("success", "op")),), auth.bybit),
    ProductSpec("bybit", "futures", "BYBIT_FUTURES_API", (
        RestCase("instrument_rules", "api.bybit.com", "/v5/market/instruments-info?category=linear&limit=1000"),
        RestCase("ticker_24h", "api.bybit.com", "/v5/market/tickers?category=linear"),
        RestCase("funding_history", "api.bybit.com", "/v5/market/funding/history?category=linear&symbol=BTCUSDT&limit=10"),
        RestCase("public_trades", "api.bybit.com", "/v5/market/recent-trade?category=linear&symbol=BTCUSDT&limit=10"),
    ), (
        RestCase("account", "api.bybit.com", "/v5/account/wallet-balance?accountType=UNIFIED", True),
        RestCase("open_orders", "api.bybit.com", "/v5/order/realtime?category=linear&openOnly=0", True),
        RestCase("personal_fills", "api.bybit.com", "/v5/execution/list?category=linear&limit=10", True),
    ), (_ws("funding", "stream.bybit.com", "/v5/public/linear", {"op":"subscribe","args":["tickers.BTCUSDT"]}, ("success", "op")),), auth.bybit),
    ProductSpec("okx", "spot", "OKX_SPOT_API", (
        RestCase("instrument_rules", "www.okx.com", "/api/v5/public/instruments?instType=SPOT"),
        RestCase("ticker_24h", "www.okx.com", "/api/v5/market/tickers?instType=SPOT"),
        RestCase("public_trades", "www.okx.com", "/api/v5/market/trades?instId=BTC-USDT&limit=10"),
    ), (
        RestCase("account", "www.okx.com", "/api/v5/account/balance", True),
        RestCase("open_orders", "www.okx.com", "/api/v5/trade/orders-pending?instType=SPOT", True),
        RestCase("personal_fills", "www.okx.com", "/api/v5/trade/fills-history?instType=SPOT&limit=10", True),
    ), (_ws("trades", "ws.okx.com", "/ws/v5/business", {"op":"subscribe","args":[{"channel":"trades-all","instId":"BTC-USDT"}]}, ("event", "arg"),
        require_data_after_ack=True,
        core_anchor="src/src/exchanges/okx/spot/config.cpp:kSubscribeTrades",
        status="core_aligned"),), auth.okx),
    ProductSpec("okx", "futures", "OKX_FUTURES_API", (
        RestCase("instrument_rules", "www.okx.com", "/api/v5/public/instruments?instType=SWAP"),
        RestCase("ticker_24h", "www.okx.com", "/api/v5/market/tickers?instType=SWAP"),
        RestCase("funding_current", "www.okx.com", "/api/v5/public/funding-rate?instId=BTC-USDT-SWAP"),
        RestCase("funding_history", "www.okx.com", "/api/v5/public/funding-rate-history?instId=BTC-USDT-SWAP&limit=10"),
        RestCase("public_trades", "www.okx.com", "/api/v5/market/trades?instId=BTC-USDT-SWAP&limit=10"),
    ), (
        RestCase("positions", "www.okx.com", "/api/v5/account/positions?instType=SWAP", True),
        RestCase("open_orders", "www.okx.com", "/api/v5/trade/orders-pending?instType=SWAP", True),
        RestCase("personal_fills", "www.okx.com", "/api/v5/trade/fills-history?instType=SWAP&limit=10", True),
    ), (_ws("funding", "ws.okx.com", "/ws/v5/public", {"op":"subscribe","args":[{"channel":"funding-rate","instId":"BTC-USDT-SWAP"}]}, ("event", "arg")),), auth.okx),
    ProductSpec("gate", "spot", "GATE_SPOT_API", (
        RestCase("instrument_rules", "api.gateio.ws", "/api/v4/spot/currency_pairs"),
        RestCase("ticker_24h", "api.gateio.ws", "/api/v4/spot/tickers"),
        RestCase("public_trades", "api.gateio.ws", "/api/v4/spot/trades?currency_pair=BTC_USDT&limit=10"),
    ), (), (_ws("trades", "api.gateio.ws", "/ws/v4/", {"time":1,"channel":"spot.trades","event":"subscribe","payload":["BTC_USDT"]}, ("event", "channel")),), auth.gate,
                ("private spot credential profile is not registered in current core",)),
    ProductSpec("gate", "futures", "GATE_FUTURES_API", (
        RestCase("instrument_rules", "api.gateio.ws", "/api/v4/futures/usdt/contracts"),
        RestCase("ticker_24h", "api.gateio.ws", "/api/v4/futures/usdt/tickers"),
        RestCase("funding_current", "api.gateio.ws", "/api/v4/futures/usdt/contracts/BTC_USDT"),
        RestCase("funding_history", "api.gateio.ws", "/api/v4/futures/usdt/funding_rate?contract=BTC_USDT&limit=10"),
        RestCase("public_trades", "api.gateio.ws", "/api/v4/futures/usdt/trades?contract=BTC_USDT&limit=10"),
    ), (
        RestCase("account", "api.gateio.ws", "/api/v4/futures/usdt/accounts", True),
        RestCase("open_orders", "api.gateio.ws", "/api/v4/futures/usdt/orders?contract=BTC_USDT&status=open", True),
        RestCase("personal_fills", "api.gateio.ws", "/api/v4/futures/usdt/my_trades?contract=BTC_USDT&limit=10", True),
    ), (
        _gate_sbe("trades_sbe", "futures.trades", "BTC_USDT",
                  "src/src/exchanges/gate/usdt/config.cpp:kSubscribeTradesSbe"),
        _gate_sbe("book_ticker_sbe", "futures.book_ticker", "BTC_USDT",
                  "src/src/exchanges/gate/usdt/config.cpp:kSubscribeBookTickerSbe"),
        _gate_sbe("orderbook_sbe", "futures.obu", "ob.BTC_USDT.50",
                  "src/src/exchanges/gate/usdt/config.cpp:kSubscribeOrderBookSbe"),
    ), auth.gate),
    ProductSpec("kucoin", "spot", "KUCOIN_UTA_API", (
        RestCase("instrument_rules", "api.kucoin.com", "/api/v2/symbols"),
        RestCase("ticker_24h", "api.kucoin.com", "/api/v1/market/allTickers"),
        RestCase("public_trades", "api.kucoin.com", "/api/v1/market/histories?symbol=BTC-USDT"),
    ), (
        RestCase("account", "api.kucoin.com", "/api/v1/accounts", True),
        RestCase("open_orders", "api.kucoin.com", "/api/v1/orders?status=active", True),
        RestCase("personal_fills", "api.kucoin.com", "/api/v1/fills?pageSize=10", True),
    ), (
        _kucoin_public("trades", "x-push-spot.kucoin.com", {
            "id":"cxet-kucoin-1", "action":"SUBSCRIBE", "channel":"trade",
            "tradeType":"SPOT", "symbol":"BTC-USDT"},
            "src/src/exchanges/kucoin/spot/config.cpp:kSubscribeTrades"),
        _kucoin_public("book_ticker", "x-push-spot.kucoin.com", {
            "id":"cxet-kucoin-1", "action":"SUBSCRIBE", "channel":"obu",
            "tradeType":"SPOT", "symbol":"BTC-USDT", "depth":"1", "rpiFilter":0},
            "src/src/exchanges/kucoin/spot/config.cpp:kSubscribeBookTicker"),
    ), auth.kucoin, ("KuCoin UTA Pro public WS is binary and requires welcome before subscribe",)),
    ProductSpec("kucoin", "futures", "KUCOIN_UTA_API", (
        RestCase("instrument_rules", "api.kucoin.com", "/api/ua/v1/market/instrument?tradeType=FUTURES"),
        RestCase("ticker_24h", "api.kucoin.com", "/api/ua/v1/market/ticker?tradeType=FUTURES"),
        RestCase("public_trades", "api.kucoin.com", "/api/ua/v1/market/trade?tradeType=FUTURES&symbol=XBTUSDTM"),
        RestCase("open_interest", "api.kucoin.com", "/api/ua/v1/market/open-interest?symbol=XBTUSDTM"),
        RestCase("funding_current", "api.kucoin.com", "/api/ua/v1/market/funding-rate?symbol=XBTUSDTM"),
        RestCase("funding_history", "api-futures.kucoin.com", "/api/v1/contract/funding-rates?symbol=XBTUSDTM&from=1700310700000&to=1702310700000"),
    ), (
        RestCase("account", "api.kucoin.com", "/api/ua/v1/unified/account/overview", True),
        RestCase("balances", "api.kucoin.com", "/api/ua/v1/unified/account/balance", True),
        RestCase("positions", "api.kucoin.com", "/api/ua/v1/unified/position/open-list?pageNumber=1&pageSize=10", True),
        RestCase("open_orders", "api.kucoin.com", "/api/ua/v1/unified/order/open-list?tradeType=FUTURES&pageNumber=1&pageSize=10", True),
        RestCase("personal_fills", "api.kucoin.com", "/api/ua/v1/unified/order/execution?tradeType=FUTURES&pageSize=10&startAt=1700310700000", True),
    ), (
        _kucoin_public("trades", "x-push-futures.kucoin.com", {
            "id":"cxet-kucoin-1", "action":"SUBSCRIBE", "channel":"trade",
            "tradeType":"FUTURES", "symbol":"XBTUSDTM"},
            "src/src/exchanges/kucoin/uta/config.cpp:kSubscribeTrades"),
        _kucoin_public("funding", "x-push-futures.kucoin.com", {
            "id":"cxet-kucoin-1", "action":"SUBSCRIBE", "channel":"funding-fee",
            "symbol":"XBTUSDTM"},
            "src/src/exchanges/kucoin/uta/config.cpp:kSubscribeFunding"),
    ), auth.kucoin,
                ("funding current uses UTA; public history uses the documented futures time-range endpoint",)),
    ProductSpec("bitget", "spot", "BITGET_SPOT_API", (
        RestCase("instrument_rules", "api.bitget.com", "/api/v2/spot/public/symbols"),
        RestCase("ticker_24h", "api.bitget.com", "/api/v2/spot/market/tickers"),
        RestCase("public_trades", "api.bitget.com", "/api/v2/spot/market/fills?symbol=BTCUSDT&limit=10"),
    ), (), (
        _bitget_sbe("trades_sbe", "spot", "publicTrade", "BTCUSDT",
                    "src/src/exchanges/bitget/uta/config.cpp:kSpotSubscribeTradesSbe"),
        _bitget_sbe("book_ticker_sbe", "spot", "books1", "BTCUSDT",
                    "src/src/exchanges/bitget/uta/config.cpp:kSpotSubscribeBookTickerSbe"),
        _bitget_sbe("orderbook_sbe", "spot", "books50", "BTCUSDT",
                    "src/src/exchanges/bitget/uta/config.cpp:kSpotSubscribeOrderBookSbe"),
    ), auth.bitget,
                ("core selected spot market wire is SBE; probe validates text ACK followed by binary SBE",)),
    ProductSpec("bitget", "futures", "BITGET_FUTURES_API", (
        RestCase("instrument_rules", "api.bitget.com", "/api/v2/mix/market/contracts?productType=USDT-FUTURES"),
        RestCase("ticker_24h", "api.bitget.com", "/api/v2/mix/market/tickers?productType=USDT-FUTURES"),
        RestCase("funding_history", "api.bitget.com", "/api/v3/market/history-fund-rate?category=USDT-FUTURES&symbol=BTCUSDT&limit=10&pageNo=1"),
        RestCase("public_trades", "api.bitget.com", "/api/v2/mix/market/fills?symbol=BTCUSDT&productType=USDT-FUTURES&limit=10"),
    ), (
        RestCase("account", "api.bitget.com", "/api/v3/account/assets", True,
                 capability="balances", selection="core_selected"),
        RestCase("open_orders", "api.bitget.com",
                 "/api/v3/trade/unfilled-orders?category=USDT-FUTURES&symbol=BTCUSDT", True,
                 capability="open_orders", selection="core_selected"),
    ), (
        _bitget_sbe("trades_sbe", "usdt-futures", "publicTrade", "BTCUSDT",
                    "src/src/exchanges/bitget/uta/config.cpp:kSubscribeTradesSbe"),
        _bitget_sbe("book_ticker_sbe", "usdt-futures", "books1", "BTCUSDT",
                    "src/src/exchanges/bitget/uta/config.cpp:kSubscribeBookTickerSbe"),
        _bitget_sbe("orderbook_sbe", "usdt-futures", "books50", "BTCUSDT",
                    "src/src/exchanges/bitget/uta/config.cpp:kSubscribeOrderBookSbe"),
    ), auth.bitget),
    ProductSpec("aster", "futures", "ASTER_FUTURES_API", (
        RestCase("instrument_rules", "fapi.asterdex.com", "/fapi/v1/exchangeInfo"),
        RestCase("ticker_24h", "fapi.asterdex.com", "/fapi/v1/ticker/24hr"),
        RestCase("funding_current", "fapi.asterdex.com", "/fapi/v1/premiumIndex"),
        RestCase("funding_history", "fapi.asterdex.com", "/fapi/v1/fundingRate?symbol=BTCUSDT&limit=10"),
        RestCase("public_trades", "fapi.asterdex.com", "/fapi/v1/aggTrades?symbol=BTCUSDT&limit=10"),
    ), (), (_ws("funding", "fstream.asterdex.com", "/ws/btcusdt@markPrice@1s"),), None,
                ("Aster private v3 signer/private-key contract is intentionally not reimplemented in Python",)),
    ProductSpec("bingx", "futures", "BINGX_API", (), (), (
        _ws("trades", "open-api-swap.bingx.com", "/swap-market", {
            "id": "cxet-bingx", "reqType": "sub", "dataType": "BTC-USDT@trade"},
            ack_validator=_bingx_ack, data_validator=_bingx_data, ack_implies_data=True,
            require_data_after_ack=True, compression="gzip",
            core_anchor="src/src/exchanges/bingx/swap/config.cpp:kSubscribeTrades", status="core_aligned"),
    ), None, ("BingX payload is copied from the exchange-owned builder; Ping/Pong is not yet modeled by the generic probe",)),
    ProductSpec("phemex", "futures", "PHEMEX_API", (
        RestCase("instrument_rules", "api.phemex.com", "/public/products"),
    ), (), (
        _ws("trades", "ws.phemex.com", "/", {"id": 0, "method": "trade_p.subscribe", "params": ["BTCUSDT"]},
            ack_validator=_phemex_ack, require_data_after_ack=True,
            core_anchor="src/src/exchanges/phemex/usdm/config.cpp:kSubscribeTrades", status="core_aligned"),
        _ws("orderbook", "ws.phemex.com", "/", {
            "id": 0, "method": "orderbook_p.subscribe", "params": ["BTCUSDT", False, 30]},
            capability="live_l2", ack_validator=_phemex_ack, require_data_after_ack=True,
            core_anchor="src/src/exchanges/phemex/usdm/config.cpp:kSubscribeOrderBook", status="core_aligned"),
        _ws("book_ticker", "ws.phemex.com", "/", {
            "id": 0, "method": "orderbook_p.subscribe", "params": ["BTCUSDT", False, 1]},
            capability="live_bbo", ack_validator=_phemex_ack, require_data_after_ack=True,
            core_anchor="src/src/exchanges/phemex/usdm/config.cpp:kSubscribeBookTicker", status="core_aligned"),
    ), None),
    ProductSpec("hyperliquid", "futures", "HYPERLIQUID_API", (), (), (
        _ws("trades", "api.hyperliquid.xyz", "/ws", {
            "method": "subscribe", "subscription": {"type": "trades", "coin": "BTC"}},
            ack_validator=_hyperliquid_ack, require_data_after_ack=True,
            core_anchor="src/src/exchanges/hyperliquid/futures/config.cpp:kSubscribeTrades", status="core_aligned"),
    ), None),
    ProductSpec(
        venue="bitmart",
        product="spot",
        credential_prefix="BITMART_API",
        public_rest=(
            RestCase("instrument_rules", "api-cloud.bitmart.com", "/spot/v1/symbols/details"),
            RestCase("orderbook", "api-cloud.bitmart.com", "/spot/quotation/v3/books?symbol=BTC_USDT&limit=50"),
            RestCase("public_trades", "api-cloud.bitmart.com", "/spot/quotation/v3/trades?symbol=BTC_USDT&limit=10"),
            RestCase("book_ticker", "api-cloud.bitmart.com", "/spot/quotation/v3/ticker?symbol=BTC_USDT"),
        ),
        public_ws=(
            _observed_data_ws("trades", "ws-manager-compress.bitmart.com", "/api?protocol=1.1",
                {"op": "subscribe", "args": ["spot/trade:BTC_USDT"]},
                "src/src/exchanges/bitmart/spot/config.cpp:kSubscribeTrades"),
            _observed_data_ws("orderbook", "ws-manager-compress.bitmart.com", "/api?protocol=1.1",
                {"op": "subscribe", "args": ["spot/depth/increase100:BTC_USDT"]},
                "src/src/exchanges/bitmart/spot/config.cpp:kSubscribeOrderBook"),
            _observed_data_ws("book_ticker", "ws-manager-compress.bitmart.com", "/api?protocol=1.1",
                {"op": "subscribe", "args": ["spot/bookTicker:BTC_USDT"]},
                "src/src/exchanges/bitmart/spot/config.cpp:kSubscribeBookTicker"),
        ),
        notes=(
            "REST paths: src/src/network/rest/request/bitmart/common/BuildMarketDataPath.hpp.",
            "No private REST profile: Python signer is intentionally absent.",
        ),
    ),
    ProductSpec(
        venue="bitmart",
        product="futures",
        credential_prefix="BITMART_API",
        public_rest=(
            RestCase("instrument_rules", "api-cloud-v2.bitmart.com", "/contract/public/details"),
            RestCase("orderbook", "api-cloud-v2.bitmart.com", "/contract/public/depth?symbol=BTCUSDT"),
            RestCase("public_trades", "api-cloud-v2.bitmart.com", "/contract/public/market-trade?symbol=BTCUSDT&limit=10"),
            RestCase("funding_current", "api-cloud-v2.bitmart.com", "/contract/public/funding-rate?symbol=BTCUSDT"),
            RestCase("open_interest", "api-cloud-v2.bitmart.com", "/contract/public/open-interest?symbol=BTCUSDT"),
        ),
        public_ws=(
            _observed_data_ws("trades", "openapi-ws-v2.bitmart.com", "/api?protocol=1.1",
                {"action": "subscribe", "args": ["futures/trade:BTCUSDT"]},
                "src/src/exchanges/bitmart/futures/config.cpp:kSubscribeTrades"),
            _observed_data_ws("orderbook", "openapi-ws-v2.bitmart.com", "/api?protocol=1.1",
                {"action": "subscribe", "args": ["futures/depthIncrease50:BTCUSDT@100ms"]},
                "src/src/exchanges/bitmart/futures/config.cpp:kSubscribeOrderBook"),
            _observed_data_ws("book_ticker", "openapi-ws-v2.bitmart.com", "/api?protocol=1.1",
                {"action": "subscribe", "args": ["futures/bookticker:BTCUSDT"]},
                "src/src/exchanges/bitmart/futures/config.cpp:kSubscribeBookTicker"),
        ),
        notes=("REST paths: src/src/network/rest/request/bitmart/common/BuildMarketDataPath.hpp.",),
    ),
    ProductSpec(
        venue="htx",
        product="spot",
        credential_prefix="HTX_API",
        public_rest=(
            RestCase("instrument_rules", "api.huobi.pro", "/v1/settings/common/market-symbols"),
            RestCase("orderbook", "api.huobi.pro", "/market/depth?symbol=btcusdt&type=step0&depth=20"),
            RestCase("public_trades", "api.huobi.pro", "/market/history/trade?symbol=btcusdt&size=10"),
        ),
        public_ws=(
            _observed_data_ws("trades", "api.huobi.pro", "/ws",
                {"sub": "market.btcusdt.trade.detail", "id": "cxet"},
                "src/src/exchanges/htx/spot/config.cpp:kSubscribeTrades", compression="gzip",
                application_heartbeat="htx"),
            _observed_data_ws("book_ticker", "api.huobi.pro", "/ws",
                {"sub": "market.btcusdt.bbo", "id": "cxet"},
                "src/src/exchanges/htx/spot/config.cpp:kSubscribeBookTicker", compression="gzip",
                application_heartbeat="htx"),
            _observed_data_ws("orderbook", "api.huobi.pro", "/feed",
                {"sub": "market.btcusdt.mbp.20", "id": "cxet"},
                "src/src/exchanges/htx/spot/config.cpp:kSubscribeOrderBook", compression="gzip",
                application_heartbeat="htx"),
        ),
        notes=("REST paths: src/src/network/rest/request/htx/spot/BuildSpotPaths.hpp.",),
    ),
    ProductSpec(
        venue="htx",
        product="futures",
        credential_prefix="HTX_API",
        public_rest=(
            RestCase("instrument_rules", "api.hbdm.com", "/linear-swap-api/v1/swap_contract_info"),
            RestCase("orderbook", "api.hbdm.com", "/linear-swap-ex/market/depth?contract_code=BTC-USDT&type=step0"),
            RestCase("public_trades", "api.hbdm.com", "/linear-swap-ex/market/history/trade?contract_code=BTC-USDT&size=10"),
            RestCase("funding_current", "api.hbdm.com", "/linear-swap-api/v1/swap_funding_rate?contract_code=BTC-USDT"),
            RestCase("open_interest", "api.hbdm.com", "/linear-swap-api/v1/swap_open_interest?contract_code=BTC-USDT"),
        ),
        public_ws=(
            _observed_data_ws("trades", "api.hbdm.com", "/linear-swap-ws",
                {"sub": "market.BTC-USDT.trade.detail", "id": "cxet"},
                "src/src/exchanges/htx/linear_swap/config.cpp:kSubscribeTrades", compression="gzip",
                application_heartbeat="htx"),
            _observed_data_ws("book_ticker", "api.hbdm.com", "/linear-swap-ws",
                {"sub": "market.BTC-USDT.bbo", "id": "cxet"},
                "src/src/exchanges/htx/linear_swap/config.cpp:kSubscribeBookTicker", compression="gzip",
                application_heartbeat="htx"),
            _observed_data_ws("orderbook", "api.hbdm.com", "/linear-swap-ws",
                {"sub": "market.BTC-USDT.depth.size_20.high_freq", "data_type": "incremental", "id": "cxet"},
                "src/src/exchanges/htx/linear_swap/config.cpp:kSubscribeOrderBook", compression="gzip",
                application_heartbeat="htx"),
        ),
        notes=(
            "REST paths: src/src/network/rest/request/htx/linear_swap/BuildLinearPaths.hpp.",
            "Funding WS is private in the registered connector; only public REST funding is declared.",
        ),
    ),
    ProductSpec(
        venue="mexc",
        product="spot",
        credential_prefix="MEXC_API",
        public_rest=(
            RestCase("instrument_rules", "api.mexc.com", "/api/v3/exchangeInfo"),
            RestCase("orderbook", "api.mexc.com", "/api/v3/depth?symbol=BTCUSDT&limit=20"),
            RestCase("public_trades", "api.mexc.com", "/api/v3/trades?symbol=BTCUSDT&limit=10"),
        ),
        public_ws=(
            _observed_data_ws("trades", "wbs-api.mexc.com", "/ws",
                {"method": "SUBSCRIPTION", "params": ["spot@public.aggre.deals.v3.api.pb@10ms@BTCUSDT"], "id": 0},
                "src/src/exchanges/mexc/spot/config.cpp:kSubscribeTrades",
                selected_wire="protobuf_binary", binary_data=True),
            _observed_data_ws("book_ticker", "wbs-api.mexc.com", "/ws",
                {"method": "SUBSCRIPTION", "params": ["spot@public.aggre.bookTicker.v3.api.pb@10ms@BTCUSDT"], "id": 0},
                "src/src/exchanges/mexc/spot/config.cpp:kSubscribeBookTicker",
                selected_wire="protobuf_binary", binary_data=True),
            _observed_data_ws("orderbook", "wbs-api.mexc.com", "/ws",
                {"method": "SUBSCRIPTION", "params": ["spot@public.aggre.depth.v3.api.pb@10ms@BTCUSDT"], "id": 0},
                "src/src/exchanges/mexc/spot/config.cpp:kSubscribeOrderBook",
                selected_wire="protobuf_binary", binary_data=True),
        ),
        notes=(
            "REST paths: src/src/network/rest/request/mexc/spot/.",
            "The registered selected wire is protobuf binary, not JSON market data.",
        ),
    ),
    ProductSpec(
        venue="mexc",
        product="futures",
        credential_prefix="MEXC_API",
        public_rest=(
            RestCase("instrument_rules", "contract.mexc.com", "/api/v1/contract/detail/country"),
            RestCase("orderbook", "contract.mexc.com", "/api/v1/contract/depth/BTC_USDT?limit=20"),
            RestCase("public_trades", "contract.mexc.com", "/api/v1/contract/deals/BTC_USDT"),
        ),
        public_ws=(
            _observed_data_ws("trades", "contract.mexc.com", "/edge",
                {"method": "sub.deal", "param": {"symbol": "BTC_USDT"}, "gzip": False},
                "src/src/exchanges/mexc/futures/config.cpp:kSubscribeTrades"),
            _observed_data_ws("book_ticker", "contract.mexc.com", "/edge",
                {"method": "sub.ticker", "param": {"symbol": "BTC_USDT"}, "gzip": False},
                "src/src/exchanges/mexc/futures/config.cpp:kSubscribeBookTicker"),
            _observed_data_ws("orderbook", "contract.mexc.com", "/edge",
                {"method": "sub.depth", "param": {"symbol": "BTC_USDT"}, "gzip": False},
                "src/src/exchanges/mexc/futures/config.cpp:kSubscribeOrderBook"),
            _observed_data_ws("funding", "contract.mexc.com", "/edge",
                {"method": "sub.funding.rate", "param": {"symbol": "BTC_USDT"}, "gzip": False},
                "src/src/exchanges/mexc/futures/config.cpp:kSubscribeFunding"),
        ),
        notes=("REST paths: src/src/network/rest/request/mexc/futures/.",),
    ),
    ProductSpec(
        venue="poloniex",
        product="spot",
        credential_prefix="POLONIEX_API",
        public_rest=(
            RestCase("instrument_rules", "api.poloniex.com", "/markets"),
            RestCase("orderbook", "api.poloniex.com", "/markets/BTC_USDT/orderBook?limit=20"),
            RestCase("public_trades", "api.poloniex.com", "/markets/BTC_USDT/trades?limit=10"),
        ),
        public_ws=(
            _observed_data_ws("trades", "ws.poloniex.com", "/ws/public",
                {"event": "subscribe", "channel": ["trades"], "symbols": ["BTC_USDT"]},
                "src/src/exchanges/poloniex/spot/config.cpp:kSubscribeTrades"),
            _observed_data_ws("book_ticker", "ws.poloniex.com", "/ws/public",
                {"event": "subscribe", "channel": ["book"], "symbols": ["BTC_USDT"], "depth": 5},
                "src/src/exchanges/poloniex/spot/config.cpp:kSubscribeBookTicker"),
            _observed_data_ws("orderbook", "ws.poloniex.com", "/ws/public",
                {"event": "subscribe", "channel": ["book_lv2"], "symbols": ["BTC_USDT"]},
                "src/src/exchanges/poloniex/spot/config.cpp:kSubscribeOrderBook"),
        ),
        notes=("REST paths: src/src/network/rest/request/poloniex/BuildPaths.hpp.",),
    ),
    ProductSpec(
        venue="poloniex",
        product="futures",
        credential_prefix="POLONIEX_API",
        public_rest=(
            RestCase("instrument_rules", "api.poloniex.com", "/v3/market/allInstruments"),
            RestCase("orderbook", "api.poloniex.com", "/v3/market/orderBook?symbol=BTC_USDT_PERP&limit=20"),
            RestCase("public_trades", "api.poloniex.com", "/v3/market/trades?symbol=BTC_USDT_PERP&limit=10"),
        ),
        public_ws=(
            _observed_data_ws("trades", "ws.poloniex.com", "/ws/v3/public",
                {"event": "subscribe", "channel": ["trades"], "symbols": ["BTC_USDT_PERP"]},
                "src/src/exchanges/poloniex/futures/config.cpp:kSubscribeTrades"),
            _observed_data_ws("book_ticker", "ws.poloniex.com", "/ws/v3/public",
                {"event": "subscribe", "channel": ["book"], "symbols": ["BTC_USDT_PERP"], "depth": 5},
                "src/src/exchanges/poloniex/futures/config.cpp:kSubscribeBookTicker"),
            _observed_data_ws("orderbook", "ws.poloniex.com", "/ws/v3/public",
                {"event": "subscribe", "channel": ["book_lv2"], "symbols": ["BTC_USDT_PERP"]},
                "src/src/exchanges/poloniex/futures/config.cpp:kSubscribeOrderBook"),
        ),
        notes=("REST paths: src/src/network/rest/request/poloniex/BuildPaths.hpp.",),
    ),
    ProductSpec(
        venue="toobit",
        product="spot",
        credential_prefix="TOOBIT_API",
        public_rest=(
            RestCase("instrument_rules", "api.toobit.com", "/api/v1/exchangeInfo"),
            RestCase("orderbook", "api.toobit.com", "/quote/v1/depth?symbol=BTCUSDT&limit=20"),
            RestCase("public_trades", "api.toobit.com", "/quote/v1/trades?symbol=BTCUSDT&limit=10"),
            RestCase("book_ticker", "api.toobit.com", "/quote/v1/ticker/bookTicker?symbol=BTCUSDT"),
        ),
        public_ws=(
            _observed_data_ws("trades", "stream.toobit.com", "/quote/ws/v1",
                {"symbol": "BTCUSDT", "topic": "trade", "event": "sub", "params": {"binary": False}},
                "src/src/exchanges/toobit/spot/config.cpp:kSubscribeTrades"),
            _observed_data_ws("orderbook", "stream.toobit.com", "/quote/ws/v1",
                {"symbol": "BTCUSDT", "topic": "diffDepth", "event": "sub", "params": {"binary": False}},
                "src/src/exchanges/toobit/spot/config.cpp:kSubscribeOrderBook"),
            _observed_data_ws("book_ticker", "stream.toobit.com", "/quote/ws/v1",
                {"symbol": "BTCUSDT", "topic": "depth", "event": "sub", "params": {"binary": False, "limit": 1}},
                "src/src/exchanges/toobit/spot/config.cpp:kSubscribeBookTicker"),
        ),
        notes=("REST paths: src/src/network/rest/request/toobit/common/BuildMarketDataPath.hpp.",),
    ),
    ProductSpec(
        venue="toobit",
        product="futures",
        credential_prefix="TOOBIT_API",
        public_rest=(
            RestCase("instrument_rules", "api.toobit.com", "/api/v1/exchangeInfo"),
            RestCase("orderbook", "api.toobit.com", "/quote/v1/depth?symbol=BTC-SWAP-USDT&limit=20"),
            RestCase("public_trades", "api.toobit.com", "/quote/v1/trades?symbol=BTC-SWAP-USDT&limit=10"),
            RestCase("book_ticker", "api.toobit.com", "/quote/v1/contract/ticker/bookTicker?symbol=BTC-SWAP-USDT"),
            RestCase("funding_current", "api.toobit.com", "/api/v1/futures/fundingRate?symbol=BTC-SWAP-USDT"),
            RestCase("open_interest", "api.toobit.com", "/quote/v1/openInterest?symbol=BTC-SWAP-USDT"),
        ),
        public_ws=(
            _observed_data_ws("trades", "stream.toobit.com", "/quote/ws/v1",
                {"symbol": "BTC-SWAP-USDT", "topic": "trade", "event": "sub", "params": {"binary": False}},
                "src/src/exchanges/toobit/futures/config.cpp:kSubscribeTrades"),
            _observed_data_ws("orderbook", "stream.toobit.com", "/quote/ws/v1",
                {"symbol": "BTC-SWAP-USDT", "topic": "diffDepth", "event": "sub", "params": {"binary": False}},
                "src/src/exchanges/toobit/futures/config.cpp:kSubscribeOrderBook"),
            _observed_data_ws("book_ticker", "stream.toobit.com", "/quote/ws/v1",
                {"symbol": "BTC-SWAP-USDT", "topic": "bookTicker", "event": "sub", "params": {"binary": False}},
                "src/src/exchanges/toobit/futures/config.cpp:kSubscribeBookTicker"),
        ),
        notes=("REST paths: src/src/network/rest/request/toobit/common/BuildMarketDataPath.hpp.",),
    ),
    ProductSpec(
        venue="xt",
        product="spot",
        credential_prefix="XT_API",
        public_rest=(
            RestCase("instrument_rules", "sapi.xt.com", "/v4/public/symbol"),
            RestCase("orderbook", "sapi.xt.com", "/v4/public/depth?symbol=btc_usdt&limit=20"),
            RestCase("public_trades", "sapi.xt.com", "/v4/public/trade/recent?symbol=btc_usdt&limit=10"),
        ),
        public_ws=(
            _observed_data_ws("trades", "stream.xt.com", "/public",
                {"method": "subscribe", "params": ["trade@btc_usdt"], "id": "1"},
                "src/src/exchanges/xt/spot/config.cpp:kSubscribeTrades"),
            _observed_data_ws("orderbook", "stream.xt.com", "/public",
                {"method": "subscribe", "params": ["depth_update@btc_usdt"], "id": "1"},
                "src/src/exchanges/xt/spot/config.cpp:kSubscribeOrderBook"),
            _observed_data_ws("book_ticker", "stream.xt.com", "/public",
                {"method": "subscribe", "params": ["depth@btc_usdt,5"], "id": "1"},
                "src/src/exchanges/xt/spot/config.cpp:kSubscribeBookTicker"),
        ),
        notes=("REST paths: src/src/network/rest/request/xt/spot/.",),
    ),
    ProductSpec(
        venue="xt",
        product="futures",
        credential_prefix="XT_API",
        public_rest=(
            RestCase("instrument_rules", "fapi.xt.com", "/future/market/v3/public/symbol/list"),
            RestCase("orderbook", "fapi.xt.com", "/future/market/v1/public/q/depth?symbol=btc_usdt&level=20"),
            RestCase("public_trades", "fapi.xt.com", "/future/market/v1/public/q/deal?symbol=btc_usdt&num=10"),
        ),
        public_ws=(
            _observed_data_ws("trades", "fstream.xt.com", "/ws/market",
                {"method": "SUBSCRIBE", "params": ["trade@btc_usdt"], "id": "1"},
                "src/src/exchanges/xt/futures/config.cpp:kSubscribeTrades"),
            _observed_data_ws("orderbook", "fstream.xt.com", "/ws/market",
                {"method": "SUBSCRIBE", "params": ["depth_update@btc_usdt,100ms"], "id": "1"},
                "src/src/exchanges/xt/futures/config.cpp:kSubscribeOrderBook"),
            _observed_data_ws("book_ticker", "fstream.xt.com", "/ws/market",
                {"method": "SUBSCRIBE", "params": ["depth@btc_usdt,5,100ms"], "id": "1"},
                "src/src/exchanges/xt/futures/config.cpp:kSubscribeBookTicker"),
            _observed_data_ws("funding", "fstream.xt.com", "/ws/market",
                {"method": "SUBSCRIBE", "params": ["fund_rate@btc_usdt"], "id": "1"},
                "src/src/exchanges/xt/futures/config.cpp:kSubscribeFunding"),
        ),
        notes=(
            "REST paths: src/src/network/rest/request/xt/futures/.",
            "Core registers funding only as WS; no source-backed REST funding path is declared.",
        ),
    ),
)

PRODUCTS = enrich(_BASE_PRODUCTS)
