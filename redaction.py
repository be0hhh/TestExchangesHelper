from __future__ import annotations

import base64
import struct
from typing import Any


SENSITIVE = {
    "apikey", "api_key", "key", "secret", "signature", "sign",
    "passphrase", "token", "listenkey", "authorization", "accountid",
    "account_id", "uid", "userid", "user_id", "wallet", "address",
}
MAX_RAW_PUBLIC_BYTES = 4096


def _sensitive(name: str) -> bool:
    normalized = "".join(ch for ch in name.lower() if ch.isalnum() or ch == "_")
    return normalized in SENSITIVE or any(part in normalized for part in (
        "secret", "signature", "passphrase", "privatekey", "listenkey",
    ))


def shape(value: Any, depth: int = 0) -> Any:
    """Return schema/count evidence only; never return account values."""
    if depth >= 4:
        return type(value).__name__
    if isinstance(value, dict):
        return {
            str(key): "<redacted>" if _sensitive(str(key)) else shape(item, depth + 1)
            for key, item in sorted(value.items(), key=lambda pair: str(pair[0]))
        }
    if isinstance(value, list):
        keys: set[str] = set()
        for item in value[:32]:
            if isinstance(item, dict):
                keys.update(str(key) for key in item)
        return {
            "type": "array",
            "count": len(value),
            "item_keys": sorted("<redacted>" if _sensitive(key) else key for key in keys),
        }
    if value is None:
        return "null"
    return type(value).__name__


def public_frame(payload: bytes) -> dict[str, Any]:
    """Raw bounded evidence for public routes only."""
    truncated = len(payload) > MAX_RAW_PUBLIC_BYTES
    payload = payload[:MAX_RAW_PUBLIC_BYTES]
    try:
        return {"encoding": "utf8", "value": payload.decode("utf-8"), "truncated": int(truncated)}
    except UnicodeDecodeError:
        return {"encoding": "base64", "value": base64.b64encode(payload).decode("ascii"),
                "truncated": int(truncated)}


def wire_shape(selected_wire: str, payload: bytes, json_value: Any) -> Any:
    """Bounded public wire evidence; SBE is described, never decoded as JSON."""
    if selected_wire in {"sbe_binary", "fix_sbe_binary"} and len(payload) >= 8:
        block_length, template_id, schema_id, version = struct.unpack_from("<4H", payload)
        return {"wire": selected_wire, "bytes": len(payload), "block_length": block_length,
                "template_id": template_id, "schema_id": schema_id, "version": version}
    return shape(json_value)
