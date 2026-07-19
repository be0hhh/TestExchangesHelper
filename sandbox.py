"""Public-only candidate profiles for pre-core exchange investigation."""
from __future__ import annotations

import json
from pathlib import Path

from model import ProductSpec, RestCase, WsCase


def load(path: Path) -> ProductSpec:
    value = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(value, dict):
        raise ValueError("sandbox_root_must_be_object")
    forbidden = {"private_rest", "private_ws", "credential_prefix", "signer", "credentials", "auth"}
    if forbidden & set(value):
        raise ValueError("sandbox_private_or_credential_field_forbidden")
    venue = value.get("venue")
    product = value.get("product")
    if not isinstance(venue, str) or not venue or not isinstance(product, str) or not product:
        raise ValueError("sandbox_requires_venue_and_product")
    rest: list[RestCase] = []
    for item in value.get("public_rest", []):
        if not isinstance(item, dict) or not all(isinstance(item.get(key), str) for key in ("name", "host", "path")):
            raise ValueError("sandbox_invalid_public_rest")
        rest.append(RestCase(item["name"], item["host"], item["path"]))
    ws: list[WsCase] = []
    for item in value.get("public_ws", []):
        if not isinstance(item, dict) or not all(isinstance(item.get(key), str) for key in ("name", "host", "path")):
            raise ValueError("sandbox_invalid_public_ws")
        payload = item.get("subscribe", "")
        if not isinstance(payload, str):
            raise ValueError("sandbox_subscribe_must_be_text")
        keys = item.get("expected_ack_keys", [])
        if not isinstance(keys, list) or not all(isinstance(key, str) for key in keys):
            raise ValueError("sandbox_invalid_ack_keys")
        ws.append(WsCase(item["name"], item["host"], item["path"], payload.encode("utf-8"),
                         tuple(keys), require_data_after_ack=bool(item.get("require_data_after_ack", True)),
                         status="sandbox_candidate", selected_wire="unknown"))
    # No credential prefix, signer or private routes are ever accepted from a
    # sandbox file. A candidate cannot become core-aligned by running it.
    return ProductSpec(venue, product, "", tuple(rest), public_ws=tuple(ws),
                       notes=("sandbox candidate: public-only and not core-aligned",))
