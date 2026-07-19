#!/usr/bin/env python3
from __future__ import annotations

import argparse
import json
from pathlib import Path

import credentials as credential_store
import catalog_audit
import probe
import sandbox
from venues import PRODUCTS


def emit(record: dict) -> None:
    print(json.dumps(record, sort_keys=True, separators=(",", ":")), flush=True)


def selected(args: argparse.Namespace):
    for product in PRODUCTS:
        if args.venue and product.venue not in args.venue:
            continue
        if args.product and product.product not in args.product:
            continue
        yield product


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Bounded read-only REST/public-WS exchange contract probe")
    parser.add_argument("--env-file", default=".env",
                        help="dotenv file used only for read-only private probes")
    parser.add_argument("--profile-config",
                        help="INI profile; imports its env_path without printing values")
    parser.add_argument("--venue", action="append")
    parser.add_argument("--product", action="append", choices=("spot", "futures"))
    parser.add_argument("--case", action="append",
                        help="limit WebSocket/FIX observation to a named case")
    parser.add_argument("--mode", choices=("public", "private", "all"), default="public")
    parser.add_argument("--transport", choices=("rest", "ws", "fix", "all"), default="rest")
    parser.add_argument("--timeout", type=float, default=10.0)
    parser.add_argument("--attempts", type=int, default=1,
                        help="bounded retry count for public WebSocket observations (1..3)")
    parser.add_argument("--confirm-read-only-private", action="store_true")
    parser.add_argument("--raw-public", action="store_true",
                        help="include bounded raw public data frames; private values are never emitted")
    parser.add_argument("--sandbox-file", help="public-only candidate profile JSON; never reads credentials")
    parser.add_argument("--catalog-audit", action="store_true",
                        help="emit static registration/profile/source-anchor coverage and exit")
    parser.add_argument("--source-root",
                        help="optional source tree for registration/anchor cross-checking")
    parser.add_argument("--capability-matrix", action="store_true",
                        help="emit every requested capability/wire row and exit (static-only)")
    args = parser.parse_args()
    if not 1.0 <= args.timeout <= 120.0:
        parser.error("--timeout must be in 1..120 seconds")
    if not 1 <= args.attempts <= 3:
        parser.error("--attempts must be in 1..3")
    if args.sandbox_file and args.mode != "public":
        parser.error("sandbox profiles are public-only")
    if args.mode in ("private", "all") and not args.confirm_read_only_private:
        parser.error("private probes require --confirm-read-only-private")
    if args.catalog_audit:
        return catalog_audit.emit(PRODUCTS, Path(args.source_root) if args.source_root else None)
    if args.capability_matrix:
        for product in selected(args):
            for capability in product.capabilities:
                emit({"kind": "capability_matrix", "evidence": "static-only",
                      "venue": product.venue, "product": product.product,
                      "capability": capability.name, "surface": capability.surface,
                      "transport": capability.transport, "wire": capability.wire,
                      "selection": capability.selection, "private": int(capability.private),
                      "requires_confirmation": int(capability.requires_confirmation),
                      "native": int(capability.native), "orders_sent": 0,
                      "secrets_printed": 0})
        return 0
    if args.sandbox_file and args.profile_config:
        parser.error("sandbox profiles cannot import profile credentials")
    if not args.sandbox_file:
        try:
            if args.profile_config:
                credential_store.load_profile_config(Path(args.profile_config))
            else:
                credential_store.load_env(Path(args.env_file))
        except (OSError, UnicodeError, ValueError) as error:
            emit({"kind": "configuration", "ok": 0,
                  "error": f"env_{type(error).__name__}",
                  "secrets_printed": 0, "orders_sent": 0})
            return 2
    failures = 0
    try:
        products = (sandbox.load(Path(args.sandbox_file)),) if args.sandbox_file else tuple(selected(args))
    except (OSError, UnicodeError, ValueError, json.JSONDecodeError) as error:
        emit({"kind": "configuration", "ok": 0,
              "error": f"sandbox_{type(error).__name__}",
              "secrets_printed": 0, "orders_sent": 0})
        return 2
    for product in products:
        base = {"venue": product.venue, "product": product.product,
                "secrets_printed": 0, "orders_sent": 0}
        for note in product.notes:
            emit(base | {"kind": "note", "detail": note})
        if args.transport in ("rest", "all") and args.mode in ("public", "all"):
            for case in product.public_rest:
                record = probe.public_rest(product, case, args.timeout)
                failures += not bool(record["ok"])
                emit(record)
        if args.transport in ("rest", "all") and args.mode in ("private", "all"):
            configured = credential_store.slots(product.credential_prefix)
            if not configured:
                emit(base | {"kind": "private_rest", "ok": 0,
                             "error": "no_configured_slots"})
            elif not product.private_rest:
                emit(base | {"kind": "private_rest", "ok": 0,
                             "error": "explicit_unsupported"})
            for slot in configured:
                resolved = credential_store.resolve(product.credential_prefix, slot)
                if resolved is None:
                    failures += 1
                    emit(base | {"kind": "private_rest", "api_slot": slot,
                                 "ok": 0, "error": "incomplete_credentials"})
                    continue
                for case in product.private_rest:
                    record = probe.private_rest(product, case, resolved, args.timeout)
                    failures += not bool(record["ok"])
                    emit(record | {"api_slot": slot})
        if args.transport in ("ws", "all") and args.mode in ("public", "all"):
            if not product.public_ws:
                emit(base | {"kind": "public_ws", "ok": 0,
                             "error": "explicit_unsupported"})
            for case in product.public_ws:
                if args.case and case.name not in args.case:
                    continue
                record = probe.public_ws(product, case, args.timeout, args.raw_public,
                                         args.attempts)
                failures += not bool(record["ok"])
                emit(record)
        if args.transport in ("ws", "all") and args.mode in ("private", "all"):
            configured = credential_store.slots(product.credential_prefix)
            if not configured:
                emit(base | {"kind": "private_ws", "ok": 0,
                             "error": "missing_credentials"})
            else:
                for slot in configured:
                    emit(base | {"kind": "private_ws", "api_slot": slot, "ok": 0,
                                 "error": "private_lifecycle_profile_unimplemented"})
        if args.transport in ("fix", "all"):
            for case in product.fix_sessions:
                if args.case and case.name not in args.case:
                    continue
                record = base | {"kind": "fix_sbe", "case": case.name, "capability": case.capability,
                                 "host": case.host,
                                 "port": case.port, "session_kind": case.session_kind,
                                 "selected_wire": case.wire, "selection": case.selection, "core_anchor": case.core_anchor,
                                 "payload_anchor": case.payload_anchor, "orders_sent": 0}
                emit(record | {"ok": 0, "error": "external_adapter_required"})
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
