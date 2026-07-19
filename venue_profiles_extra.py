"""Public, read-only profiles for the registered venues missing from venues.py.

Every REST path and WebSocket subscription below is copied from an
exchange-owned CXET request builder.  This module deliberately contains no
private REST route: the generic Python probe must not approximate a venue's
credential/signing contract.
"""

from __future__ import annotations

import json

from model import ProductSpec, RestCase, WsCase


def _data_with(*keys: str):
    def validate(value: object, opcode: int) -> bool:
        return opcode == 0x1 and isinstance(value, dict) and all(key in value for key in keys)
    return validate


def _market_data(value: object, opcode: int) -> bool:
    return opcode in (0x1, 0x2) and isinstance(value, dict) and (
        "data" in value or ("ch" in value and "tick" in value))


def _ws(name: str, host: str, path: str, payload: dict[str, object], anchor: str,
        *, compression: str = "", selected_wire: str = "json", binary_data: bool = False,
        data_validator=None, application_heartbeat: str = "") -> WsCase:
    if data_validator is None and not binary_data:
        data_validator = _market_data
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


EXTRA_PRODUCTS: tuple[ProductSpec, ...] = (
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
            _ws("trades", "ws-manager-compress.bitmart.com", "/api?protocol=1.1",
                {"op": "subscribe", "args": ["spot/trade:BTC_USDT"]},
                "src/src/exchanges/bitmart/spot/config.cpp:kSubscribeTrades"),
            _ws("orderbook", "ws-manager-compress.bitmart.com", "/api?protocol=1.1",
                {"op": "subscribe", "args": ["spot/depth/increase100:BTC_USDT"]},
                "src/src/exchanges/bitmart/spot/config.cpp:kSubscribeOrderBook"),
            _ws("book_ticker", "ws-manager-compress.bitmart.com", "/api?protocol=1.1",
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
            _ws("trades", "openapi-ws-v2.bitmart.com", "/api?protocol=1.1",
                {"action": "subscribe", "args": ["futures/trade:BTCUSDT"]},
                "src/src/exchanges/bitmart/futures/config.cpp:kSubscribeTrades"),
            _ws("orderbook", "openapi-ws-v2.bitmart.com", "/api?protocol=1.1",
                {"action": "subscribe", "args": ["futures/depthIncrease50:BTCUSDT@100ms"]},
                "src/src/exchanges/bitmart/futures/config.cpp:kSubscribeOrderBook"),
            _ws("book_ticker", "openapi-ws-v2.bitmart.com", "/api?protocol=1.1",
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
            _ws("trades", "api.huobi.pro", "/ws",
                {"sub": "market.btcusdt.trade.detail", "id": "cxet"},
                "src/src/exchanges/htx/spot/config.cpp:kSubscribeTrades", compression="gzip",
                application_heartbeat="htx"),
            _ws("book_ticker", "api.huobi.pro", "/ws",
                {"sub": "market.btcusdt.bbo", "id": "cxet"},
                "src/src/exchanges/htx/spot/config.cpp:kSubscribeBookTicker", compression="gzip",
                application_heartbeat="htx"),
            _ws("orderbook", "api.huobi.pro", "/feed",
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
            _ws("trades", "api.hbdm.com", "/linear-swap-ws",
                {"sub": "market.BTC-USDT.trade.detail", "id": "cxet"},
                "src/src/exchanges/htx/linear_swap/config.cpp:kSubscribeTrades", compression="gzip",
                application_heartbeat="htx"),
            _ws("book_ticker", "api.hbdm.com", "/linear-swap-ws",
                {"sub": "market.BTC-USDT.bbo", "id": "cxet"},
                "src/src/exchanges/htx/linear_swap/config.cpp:kSubscribeBookTicker", compression="gzip",
                application_heartbeat="htx"),
            _ws("orderbook", "api.hbdm.com", "/linear-swap-ws",
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
            _ws("trades", "wbs-api.mexc.com", "/ws",
                {"method": "SUBSCRIPTION", "params": ["spot@public.aggre.deals.v3.api.pb@10ms@BTCUSDT"], "id": 0},
                "src/src/exchanges/mexc/spot/config.cpp:kSubscribeTrades",
                selected_wire="protobuf_binary", binary_data=True),
            _ws("book_ticker", "wbs-api.mexc.com", "/ws",
                {"method": "SUBSCRIPTION", "params": ["spot@public.aggre.bookTicker.v3.api.pb@10ms@BTCUSDT"], "id": 0},
                "src/src/exchanges/mexc/spot/config.cpp:kSubscribeBookTicker",
                selected_wire="protobuf_binary", binary_data=True),
            _ws("orderbook", "wbs-api.mexc.com", "/ws",
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
            _ws("trades", "contract.mexc.com", "/edge",
                {"method": "sub.deal", "param": {"symbol": "BTC_USDT"}, "gzip": False},
                "src/src/exchanges/mexc/futures/config.cpp:kSubscribeTrades"),
            _ws("book_ticker", "contract.mexc.com", "/edge",
                {"method": "sub.ticker", "param": {"symbol": "BTC_USDT"}, "gzip": False},
                "src/src/exchanges/mexc/futures/config.cpp:kSubscribeBookTicker"),
            _ws("orderbook", "contract.mexc.com", "/edge",
                {"method": "sub.depth", "param": {"symbol": "BTC_USDT"}, "gzip": False},
                "src/src/exchanges/mexc/futures/config.cpp:kSubscribeOrderBook"),
            _ws("funding", "contract.mexc.com", "/edge",
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
            _ws("trades", "ws.poloniex.com", "/ws/public",
                {"event": "subscribe", "channel": ["trades"], "symbols": ["BTC_USDT"]},
                "src/src/exchanges/poloniex/spot/config.cpp:kSubscribeTrades"),
            _ws("book_ticker", "ws.poloniex.com", "/ws/public",
                {"event": "subscribe", "channel": ["book"], "symbols": ["BTC_USDT"], "depth": 5},
                "src/src/exchanges/poloniex/spot/config.cpp:kSubscribeBookTicker"),
            _ws("orderbook", "ws.poloniex.com", "/ws/public",
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
            _ws("trades", "ws.poloniex.com", "/ws/v3/public",
                {"event": "subscribe", "channel": ["trades"], "symbols": ["BTC_USDT_PERP"]},
                "src/src/exchanges/poloniex/futures/config.cpp:kSubscribeTrades"),
            _ws("book_ticker", "ws.poloniex.com", "/ws/v3/public",
                {"event": "subscribe", "channel": ["book"], "symbols": ["BTC_USDT_PERP"], "depth": 5},
                "src/src/exchanges/poloniex/futures/config.cpp:kSubscribeBookTicker"),
            _ws("orderbook", "ws.poloniex.com", "/ws/v3/public",
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
            _ws("trades", "stream.toobit.com", "/quote/ws/v1",
                {"symbol": "BTCUSDT", "topic": "trade", "event": "sub", "params": {"binary": False}},
                "src/src/exchanges/toobit/spot/config.cpp:kSubscribeTrades"),
            _ws("orderbook", "stream.toobit.com", "/quote/ws/v1",
                {"symbol": "BTCUSDT", "topic": "diffDepth", "event": "sub", "params": {"binary": False}},
                "src/src/exchanges/toobit/spot/config.cpp:kSubscribeOrderBook"),
            _ws("book_ticker", "stream.toobit.com", "/quote/ws/v1",
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
            _ws("trades", "stream.toobit.com", "/quote/ws/v1",
                {"symbol": "BTC-SWAP-USDT", "topic": "trade", "event": "sub", "params": {"binary": False}},
                "src/src/exchanges/toobit/futures/config.cpp:kSubscribeTrades"),
            _ws("orderbook", "stream.toobit.com", "/quote/ws/v1",
                {"symbol": "BTC-SWAP-USDT", "topic": "diffDepth", "event": "sub", "params": {"binary": False}},
                "src/src/exchanges/toobit/futures/config.cpp:kSubscribeOrderBook"),
            _ws("book_ticker", "stream.toobit.com", "/quote/ws/v1",
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
            _ws("trades", "stream.xt.com", "/public",
                {"method": "subscribe", "params": ["trade@btc_usdt"], "id": "1"},
                "src/src/exchanges/xt/spot/config.cpp:kSubscribeTrades"),
            _ws("orderbook", "stream.xt.com", "/public",
                {"method": "subscribe", "params": ["depth_update@btc_usdt"], "id": "1"},
                "src/src/exchanges/xt/spot/config.cpp:kSubscribeOrderBook"),
            _ws("book_ticker", "stream.xt.com", "/public",
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
            _ws("trades", "fstream.xt.com", "/ws/market",
                {"method": "SUBSCRIBE", "params": ["trade@btc_usdt"], "id": "1"},
                "src/src/exchanges/xt/futures/config.cpp:kSubscribeTrades"),
            _ws("orderbook", "fstream.xt.com", "/ws/market",
                {"method": "SUBSCRIBE", "params": ["depth_update@btc_usdt,100ms"], "id": "1"},
                "src/src/exchanges/xt/futures/config.cpp:kSubscribeOrderBook"),
            _ws("book_ticker", "fstream.xt.com", "/ws/market",
                {"method": "SUBSCRIBE", "params": ["depth@btc_usdt,5,100ms"], "id": "1"},
                "src/src/exchanges/xt/futures/config.cpp:kSubscribeBookTicker"),
            _ws("funding", "fstream.xt.com", "/ws/market",
                {"method": "SUBSCRIBE", "params": ["fund_rate@btc_usdt"], "id": "1"},
                "src/src/exchanges/xt/futures/config.cpp:kSubscribeFunding"),
        ),
        notes=(
            "REST paths: src/src/network/rest/request/xt/futures/.",
            "Core registers funding only as WS; no source-backed REST funding path is declared.",
        ),
    ),
)
