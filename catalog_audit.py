"""Static-only profile audit, optionally cross-checked against a source tree."""
from __future__ import annotations

import json
import re
from pathlib import Path

from model import ProductSpec


DEFAULT_SOURCE_ROOT = Path(__file__).resolve().parents[2]
EXCLUDED_FAMILIES = ("finam",)


def registered_families(source_root: Path | None) -> tuple[str, ...]:
    if source_root is None:
        return ()
    source = source_root / "src/src/exchanges/register_all.cpp"
    if not source.is_file():
        return ()
    return tuple(re.findall(r"\s([a-z_]+)::register[A-Z][A-Za-z]+\(\);",
                            source.read_text(encoding="utf-8")))


def audit(products: tuple[ProductSpec, ...], source_root: Path | None = DEFAULT_SOURCE_ROOT) -> dict:
    profile_families = sorted({product.venue for product in products})
    anchors: list[dict] = []
    for product in products:
        cases = tuple(product.public_ws) + tuple(product.fix_sessions)
        for case in cases:
            for anchor_kind in ("core_anchor", "payload_anchor", "parser_anchor"):
                anchor = getattr(case, anchor_kind, "")
                if not anchor:
                    continue
                source_name, separator, symbol = anchor.partition(":")
                source = source_root / source_name if source_root is not None else None
                present = bool(source is not None and separator and source.is_file() and symbol and
                               symbol in source.read_text(encoding="utf-8"))
                anchors.append({"venue": product.venue, "product": product.product,
                                "case": case.name if hasattr(case, "name") else case.case_id,
                                "anchor_kind": anchor_kind, "anchor": anchor, "present": int(present)})
    registered = registered_families(source_root)
    source_available = bool(registered)
    return {
        "kind": "static_catalog_audit",
        "evidence": "static-only",
        "registered_families": registered,
        "source_root_available": int(source_available),
        "included_families": sorted(set(registered) - set(EXCLUDED_FAMILIES)) if source_available else [],
        "profile_families": profile_families,
        "excluded_families": EXCLUDED_FAMILIES,
        "missing_profile_families": (sorted(
            set(registered) - set(profile_families) - set(EXCLUDED_FAMILIES)) if source_available else []),
        "ws_profiles": sum(len(product.public_ws) for product in products),
        "core_aligned_ws_profiles": sum(
            case.status == "core_aligned" for product in products for case in product.public_ws),
        "capability_rows": sum(len(product.capabilities) for product in products),
        "anchors": anchors,
        "anchors_ok": int(all(row["present"] for row in anchors)) if source_available else None,
    }


def emit(products: tuple[ProductSpec, ...], source_root: Path | None = DEFAULT_SOURCE_ROOT) -> int:
    print(json.dumps(audit(products, source_root), sort_keys=True, separators=(",", ":")))
    return 0
