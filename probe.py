"""Reusable, bounded read-only endpoint observations.

The CLI is deliberately a thin presenter over these functions.  Callers get
structured records and can store, compare or render them without parsing CLI
output.  No function in this module creates an order or exposes secret values.
"""
from __future__ import annotations

try:  # Supports both `python run.py` and `import exchange_api_probe`.
    from .capability_matrix import capability_for_case
    from .http_client import execute
    from .model import Credentials, ProductSpec, RestCase, WsCase
    from .redaction import shape, wire_shape
    from .rest_contracts import validate as validate_rest_contract
    from .ws_client import observe
except ImportError:  # pragma: no cover - direct script mode
    from capability_matrix import capability_for_case
    from http_client import execute
    from model import Credentials, ProductSpec, RestCase, WsCase
    from redaction import shape, wire_shape
    from rest_contracts import validate as validate_rest_contract
    from ws_client import observe


def public_rest(product: ProductSpec, case: RestCase, timeout: float) -> dict:
    result = execute(case, timeout)
    response_received = result.status > 0 and result.error in ("", "http_error")
    transport_success = 200 <= result.status < 300 and not result.error
    logical_success, semantics = validate_rest_contract(
        case.rest_contract, result.json_value, case.expected_symbol)
    if not case.rest_contract:
        logical_success = transport_success
    contract_match = logical_success == case.expected_logical_success
    ok = response_received and contract_match
    return {
        "kind": "public_rest", "venue": product.venue, "product": product.product,
        "case": case.name, "capability": capability_for_case(case), "transport": "rest",
        "wire": "json", "selection": case.selection, "ok": int(ok),
        "status": result.status, "elapsed_ms": result.elapsed_ms,
        "body_bytes": result.body_bytes, "error": result.error,
        "logical_success": int(logical_success),
        "expected_logical_success": int(case.expected_logical_success),
        "contract_match": int(contract_match), "semantics": semantics,
        "shape": shape(result.json_value), "orders_sent": 0, "secrets_printed": 0,
        "core_anchor": case.core_anchor, "parser_anchor": case.parser_anchor,
    }


def private_rest(product: ProductSpec, case: RestCase, credentials: Credentials,
                 timeout: float) -> dict:
    result = execute(case, timeout, credentials, product.signer)
    ok = 200 <= result.status < 300 and not result.error
    return {
        "kind": "private_rest", "venue": product.venue, "product": product.product,
        "case": case.name, "capability": capability_for_case(case, True), "transport": "rest",
        "wire": "json", "selection": case.selection, "ok": int(ok),
        "status": result.status, "elapsed_ms": result.elapsed_ms,
        "body_bytes": result.body_bytes, "error": result.error,
        "shape": shape(result.json_value), "orders_sent": 0, "secrets_printed": 0,
    }


def public_ws(product: ProductSpec, case: WsCase, timeout: float,
              raw_public: bool = False, attempts: int = 1) -> dict:
    if attempts < 1:
        raise ValueError("attempts_must_be_positive")
    result = None
    used = 0
    for used in range(1, attempts + 1):
        result = observe(case, timeout)
        if result.connected and (not case.subscribe or result.ack_complete) and \
                (not case.require_data_after_ack or result.data_complete):
            break
    assert result is not None
    ok = result.connected and (not case.subscribe or result.ack_complete) and \
        (not case.require_data_after_ack or result.data_complete)
    record = {
        "kind": "public_ws", "venue": product.venue, "product": product.product,
        "case": case.name, "capability": capability_for_case(case), "transport": "ws",
        "wire": case.selected_wire, "selection": case.selection, "ok": int(ok),
        "elapsed_ms": result.elapsed_ms, "opcode": result.opcode,
        "payload_bytes": result.payload_bytes, "ack_complete": int(result.ack_complete),
        "data_complete": int(result.data_complete), "protocol_stage": result.protocol_stage,
        "control_pings": result.control_pings, "error": result.error,
        "shape": wire_shape(case.selected_wire, result.public_payload, result.json_value),
        "core_anchor": case.core_anchor, "parser_anchor": case.parser_anchor,
        "selected_wire": case.selected_wire, "profile_status": case.status,
        "attempts_allowed": attempts, "attempts_used": used,
        "orders_sent": 0, "secrets_printed": 0,
    }
    if raw_public and result.public_payload:
        try:
            from .redaction import public_frame
        except ImportError:  # pragma: no cover - direct script mode
            from redaction import public_frame
        record["public_frame"] = public_frame(result.public_payload)
    return record
