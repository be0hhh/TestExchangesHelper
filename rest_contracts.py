"""Bounded semantic checks for public reference-data REST responses."""
from __future__ import annotations

from typing import Any


_CONTRACTS: dict[str, tuple[str, str, tuple[str, ...], str, str]] = {
    # envelope, symbol field, required fields, volume semantics, change semantics
    "binance_ticker24h": ("binance", "symbol", ("quoteVolume", "priceChangePercent"),
                            "quoteVolume:quote", "priceChangePercent:percentage_points"),
    "binance_funding": ("binance", "symbol", ("lastFundingRate", "nextFundingTime"),
                         "", "lastFundingRate:ratio"),
    "bybit_ticker24h": ("bybit", "symbol", ("turnover24h", "price24hPcnt"),
                         "turnover24h:quote", "price24hPcnt:ratio"),
    "bybit_funding": ("bybit", "symbol", ("fundingRate", "nextFundingTime"),
                       "", "fundingRate:ratio"),
    "okx_ticker24h": ("okx", "instId", ("volCcy24h", "last", "open24h"),
                       "volCcy24h:base;quote:derived", "last/open24h:derived_ratio"),
    "okx_funding": ("okx", "instId", ("fundingRate", "nextFundingTime"),
                     "", "fundingRate:ratio"),
    "gate_ticker24h": ("gate", "contract", ("volume_24h_quote", "change_percentage"),
                        "volume_24h_quote:quote", "change_percentage:percentage_points"),
    "gate_funding": ("gate", "name", ("funding_rate", "funding_next_apply", "funding_interval"),
                      "", "funding_rate:ratio"),
    "kucoin_ticker24h": ("kucoin_uta", "symbol", ("quoteVolume", "priceChangePercent"),
                          "quoteVolume:quote", "priceChangePercent:percentage_points"),
    "kucoin_funding": ("kucoin_futures", "symbol",
                        ("fundingFeeRate", "nextFundingRateDateTime"), "", "fundingFeeRate:ratio"),
    "bitget_ticker24h": ("bitget", "symbol", ("turnover24h", "price24hPcnt"),
                          "turnover24h:quote", "price24hPcnt:ratio"),
    "bitget_funding": ("bitget", "symbol",
                        ("fundingRate", "fundingRateInterval", "nextUpdate"), "", "fundingRate:ratio"),
}


def _rows(envelope: str, value: Any) -> list[dict[str, Any]] | None:
    if envelope == "binance":
        if isinstance(value, dict) and "code" in value and "symbol" not in value:
            return None
        raw = value if isinstance(value, list) else [value]
    elif envelope == "bybit":
        if not isinstance(value, dict) or value.get("retCode") != 0:
            return None
        result = value.get("result")
        raw = result.get("list") if isinstance(result, dict) else None
    elif envelope == "okx":
        if not isinstance(value, dict) or str(value.get("code")) != "0":
            return None
        raw = value.get("data")
    elif envelope == "bitget":
        if not isinstance(value, dict) or str(value.get("code")) != "00000":
            return None
        raw = value.get("data")
    elif envelope == "kucoin_uta":
        if not isinstance(value, dict) or str(value.get("code")) != "200000":
            return None
        data = value.get("data")
        raw = data.get("list") if isinstance(data, dict) else None
    elif envelope == "kucoin_futures":
        if not isinstance(value, dict) or str(value.get("code")) != "200000":
            return None
        raw = value.get("data")
    elif envelope == "gate":
        if isinstance(value, dict) and ("label" in value or "message" in value):
            return None
        raw = value if isinstance(value, list) else [value]
    else:
        return None
    if not isinstance(raw, list):
        return None
    return [row for row in raw if isinstance(row, dict)]


def validate(contract: str, value: Any, expected_symbol: str = "") -> tuple[bool, dict[str, Any]]:
    if not contract:
        return False, {}
    spec = _CONTRACTS.get(contract)
    if spec is None:
        return False, {"contract": contract, "error": "unknown_contract"}
    envelope, symbol_field, required, volume_semantics, change_semantics = spec
    rows = _rows(envelope, value)
    evidence: dict[str, Any] = {
        "contract": contract, "volume": volume_semantics, "change_or_funding": change_semantics,
        "symbol_field": symbol_field, "expected_symbol": expected_symbol,
        "row_count": len(rows) if rows is not None else 0,
    }
    if not rows:
        return False, evidence | {"error": "logical_error_or_empty_rows"}
    selected = next((row for row in rows if not expected_symbol or row.get(symbol_field) == expected_symbol), None)
    if selected is None:
        return False, evidence | {"error": "expected_symbol_missing"}
    missing = [field for field in required if field not in selected or selected[field] in (None, "")]
    if contract == "kucoin_funding":
        intervals = [selected.get("currentFundingRateGranularity"),
                     selected.get("fundingRateGranularity")]
        intervals = [str(item) for item in intervals if item not in (None, "")]
        if not intervals or len(set(intervals)) != 1:
            missing.append("consistent_funding_granularity")
    evidence["native_symbol"] = selected.get(symbol_field, "")
    evidence["required_fields_present"] = int(not missing)
    if missing:
        evidence["missing_fields"] = missing
    return not missing, evidence
