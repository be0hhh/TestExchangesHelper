"""Capability inventory for the cold exchange API probe.

The inventory deliberately distinguishes a route that exists in a profile from
the product-level requirement it satisfies.  Missing rows are emitted by the
runner as `missing_in_core`; they are never inferred as supported from a
similarly named endpoint.
"""
from __future__ import annotations

from dataclasses import replace

from model import CapabilityCase, ProductSpec


PUBLIC_COMMON = (
    "exchange_info",
    "instrument_catalog",
    "instrument_detail",
    "historical_trades",
    "live_trades",
    "live_bbo",
    "live_l2",
)
PUBLIC_FUTURES = (
    "funding_current_all",
    "funding_current_symbol",
    "funding_history",
)
PRIVATE_COMMON = (
    "balances",
    "fills_history",
    "positions",
    "open_orders",
    "order_history",
    "user_account_stream",
    "user_orders_stream",
    "user_trades_stream",
)


def required_names(product: ProductSpec) -> tuple[str, ...]:
    names = list(PUBLIC_COMMON)
    if product.product == "futures":
        names.extend(PUBLIC_FUTURES)
    names.extend(PRIVATE_COMMON)
    return tuple(names)


def _rest_name(case_name: str, path: str, private: bool) -> str | None:
    if private:
        return {
            "account": "balances",
            "balances": "balances",
            "positions": "positions",
            "open_orders": "open_orders",
            "order_history": "order_history",
            "personal_fills": "fills_history",
        }.get(case_name)
    if case_name == "instrument_rules":
        return "instrument_catalog"
    if case_name == "exchange_info":
        return "exchange_info"
    if case_name == "instrument_detail":
        return "instrument_detail"
    if case_name == "public_trades":
        return "historical_trades"
    if case_name == "funding_history":
        return "funding_history"
    if case_name == "funding_current":
        compact = path.lower()
        return "funding_current_symbol" if any(
            token in compact for token in ("symbol=", "instid=", "contract=", "/btc_", "/btcusdt")
        ) else "funding_current_all"
    return None


def _ws_name(case_name: str) -> str | None:
    lowered = case_name.lower()
    if "trade" in lowered:
        return "live_trades"
    if "book_ticker" in lowered or "bbo" in lowered:
        return "live_bbo"
    if "orderbook" in lowered or "depth" in lowered:
        return "live_l2"
    if "funding" in lowered:
        return "funding_current_symbol"
    return None


def _private_stream_name(case_name: str) -> str | None:
    lowered = case_name.lower()
    if "account" in lowered or "balance" in lowered:
        return "user_account_stream"
    if "order" in lowered:
        return "user_orders_stream"
    if "trade" in lowered or "fill" in lowered:
        return "user_trades_stream"
    return None


def capability_for_case(case: object, private: bool = False) -> str:
    """Return the explicit profile mapping or the conservative legacy mapping."""
    explicit = getattr(case, "capability", "")
    if explicit:
        return explicit
    if hasattr(case, "path"):
        return _rest_name(case.name, case.path, private) or ""
    if hasattr(case, "selected_wire"):
        return _ws_name(case.name) or ""
    return _ws_name(case.name) or ""


def _add(seen: list[CapabilityCase], capability: CapabilityCase) -> None:
    """Keep every distinct wire lane; never collapse diagnostic evidence."""
    identity = (capability.name, capability.surface, capability.transport, capability.wire,
                capability.selection, capability.private)
    if all(identity != (row.name, row.surface, row.transport, row.wire, row.selection, row.private)
           for row in seen):
        seen.append(capability)


def infer_capabilities(product: ProductSpec) -> tuple[CapabilityCase, ...]:
    seen: list[CapabilityCase] = []
    for case in product.public_rest:
        name = case.capability or _rest_name(case.name, case.path, False)
        if name:
            _add(seen, CapabilityCase(name, "public", "rest", "json", case.selection,
                                      native=case.native))
    for case in product.private_rest:
        name = case.capability or _rest_name(case.name, case.path, True)
        if name:
            _add(seen, CapabilityCase(name, "private", "rest", "json", case.selection,
                                      private=True, requires_confirmation=True, native=case.native))
    for case in product.public_ws:
        name = case.capability or _ws_name(case.name)
        if name:
            _add(seen, CapabilityCase(name, "public", "ws", case.selected_wire, case.selection,
                                      native=True))
    for case in product.fix_sessions:
        name = case.capability or _ws_name(case.name)
        if name:
            _add(seen, CapabilityCase(name, "public", "fix", case.wire,
                                      "external_adapter_required", native=False))
    for case in product.public_ws:
        if not case.private:
            continue
        name = _private_stream_name(case.name)
        if name:
            _add(seen, CapabilityCase(name, "private", "ws", case.selected_wire, case.selection,
                                      private=True, requires_confirmation=True))
    for name in required_names(product):
        if any(row.name == name for row in seen):
            continue
        private = name in PRIVATE_COMMON
        _add(seen, CapabilityCase(name, "private" if private else "public", "none", "none",
                                  "missing_in_core", private=private,
                                  requires_confirmation=private, native=False))
    requested_order = {name: index for index, name in enumerate(required_names(product))}
    return tuple(sorted(seen, key=lambda row: (requested_order[row.name], row.selection != "core_selected",
                                               row.transport, row.wire)))


def enrich(products: tuple[ProductSpec, ...]) -> tuple[ProductSpec, ...]:
    return tuple(replace(product, capabilities=infer_capabilities(product)) for product in products)


def capability_rows(product: ProductSpec) -> tuple[CapabilityCase, ...]:
    return product.capabilities or infer_capabilities(product)
