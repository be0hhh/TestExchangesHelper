#!/usr/bin/env python3
"""Bounded, public-only diagnostic capture. No production route selection."""
import argparse
import hashlib
import http.client
import importlib
import json
import signal
import ssl
import sys
import time
import urllib.parse
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PROFILE = ROOT / "profiles" / "public_capture.json"
MAX_BYTES = 16 * 1024 * 1024
RESERVE = 256 * 1024
MAX_PAYLOAD = 1024 * 1024
MAX_RECORDS = 12000
MAX_SECONDS = 60


class Refused(Exception):
    pass


def encoded(value):
    return (json.dumps(value, separators=(",", ":"), ensure_ascii=True) + "\n").encode()


def decode_payload(payload, compression):
    """Validate the complete body before recording; never return partial gzip."""
    if len(payload) > MAX_PAYLOAD:
        raise Refused("application_payload_limit")
    if compression != "gzip":
        return payload
    decoder = zlib.decompressobj(16 + zlib.MAX_WBITS)
    plain = decoder.decompress(payload, MAX_PAYLOAD + 1)
    if len(plain) > MAX_PAYLOAD or decoder.unconsumed_tail:
        raise Refused("gzip_expanded_payload_limit")
    if not decoder.eof or decoder.unused_data:
        raise Refused("gzip_incomplete_or_concatenated_body")
    return plain


class Bundle:
    """One owner; <=16 MiB on disk, including the reserved final manifest."""
    def __init__(self, directory, routes, profile_bytes):
        directory.mkdir(parents=True, exist_ok=False)
        self.directory = directory
        self.payloads = (directory / "frames.bin").open("xb")
        self.index = (directory / "frames.jsonl").open("xb")
        self.used = self.offset = self.count = 0
        self.origin = time.monotonic_ns()
        self.manifest = {
            "schema": "exchange.public_capture",
            "owner": "exchange_api_probe_public_helper", "started_utc_ns": time.time_ns(),
            "routes": routes, "profile_sha256": hashlib.sha256(profile_bytes).hexdigest(),
            "limits": {"total_seconds": MAX_SECONDS, "artifact_bytes": MAX_BYTES,
                       "metadata_reserved_bytes": RESERVE, "application_bytes": MAX_PAYLOAD,
                       "http_requests": 8, "ws_sessions": 2, "attempts": 1},
            "payload_boundary": "original application message bytes; no TLS/TCP or WS wire headers",
            "analyzer_compatibility": "not asserted; dedicated schema",
            "clock": "recorded_* is artifact time; received_* is complete application read before validation/hash; monotonic_ns since capture start; utc_ns Unix epoch",
            "reconnect": False,
        }
        if len(encoded(self.manifest)) > RESERVE // 2:
            raise Refused("route_metadata_limit")

    def record(self, payload, **metadata):
        if len(payload) > MAX_PAYLOAD:
            raise Refused("application_payload_limit")
        row = dict(metadata, frame_id=self.count, offset=self.offset, length=len(payload),
                   sha256=hashlib.sha256(payload).hexdigest(),
                   recorded_monotonic_ns=time.monotonic_ns() - self.origin,
                   recorded_utc_ns=time.time_ns())
        line = encoded(row)
        if self.count >= MAX_RECORDS or self.used + len(payload) + len(line) > MAX_BYTES - RESERVE:
            raise Refused("artifact_limit_no_partial_payload_saved")
        payload_position, index_position = self.payloads.tell(), self.index.tell()
        try:
            self.payloads.write(payload)
            self.index.write(line)
        except BaseException:
            # A deadline or storage failure must not publish a partial record.
            self.payloads.seek(payload_position)
            self.payloads.truncate()
            self.index.seek(index_position)
            self.index.truncate()
            raise
        self.used += len(payload) + len(line)
        self.offset += len(payload)
        self.count += 1

    def finish(self, status, reason):
        self.payloads.close()
        self.index.close()
        self.manifest.update(status=status, reason=reason[:2048], finished_utc_ns=time.time_ns(),
                             records=self.count, payload_bytes=self.offset)
        inventory = {}
        for name in ("frames.bin", "frames.jsonl"):
            digest = hashlib.sha256()
            with (self.directory / name).open("rb") as source:
                for chunk in iter(lambda: source.read(65536), b""):
                    digest.update(chunk)
            inventory[name] = {"bytes": (self.directory / name).stat().st_size,
                               "sha256": digest.hexdigest()}
        self.manifest["files"] = inventory
        data = encoded(self.manifest)
        if len(data) > RESERVE:
            raise Refused("manifest_reservation_exceeded")
        (self.directory / "manifest.json").write_bytes(data)


def remaining(deadline):
    value = deadline - time.monotonic()
    if value <= 0:
        raise Refused("total_deadline")
    return value


def read_http_body(response):
    payload = response.read(MAX_PAYLOAD + 1)
    received_monotonic_ns, received_utc_ns = time.monotonic_ns(), time.time_ns()
    if len(payload) > MAX_PAYLOAD:
        raise Refused("application_payload_limit")
    # read(amt) can return short EOF without IncompleteRead for Content-Length.
    # Chunked read must also have consumed its terminating chunk/trailer cut.
    if response.length not in (None, 0) or (response.chunked and not response.isclosed()):
        raise Refused("http_incomplete_body_no_payload_saved")
    return payload, received_monotonic_ns, received_utc_ns


def https_read(request, bundle, deadline):
    url = urllib.parse.urlsplit(request["url"])
    if url.scheme != "https" or url.port not in (None, 443) or url.username or url.fragment:
        raise Refused("invalid_frozen_https_route")
    connection = http.client.HTTPSConnection(url.hostname, timeout=remaining(deadline),
                                            context=ssl.create_default_context())
    try:
        path = url.path + ("?" + url.query if url.query else "")
        connection.request("GET", path, headers={"Accept-Encoding": "identity",
                                                "User-Agent": "CXET-public-schema-capture/1"})
        response = connection.getresponse()
        payload, received_monotonic_ns, received_utc_ns = read_http_body(response)
        received_monotonic_ns -= bundle.origin
        compression = response.getheader("Content-Encoding", "identity").lower()
        if compression not in ("identity", "gzip"):
            raise Refused("unsupported_http_content_encoding")
        plain = decode_payload(payload, compression)
        bundle.record(payload, transport="https", direction="inbound", route_id=request["id"],
                      url=request["url"], http_method="GET", http_status=response.status,
                      content_type=response.getheader("Content-Type", "")[:512],
                      compression=compression, decoded_length=len(plain),
                      received_monotonic_ns=received_monotonic_ns, received_utc_ns=received_utc_ns,
                      decoded_sha256=hashlib.sha256(plain).hexdigest())
        if not 200 <= response.status < 300:
            raise Refused("http_status_" + str(response.status))
    finally:
        connection.close()


def websocket_library():
    try:
        module = importlib.import_module("websocket")
        frame_buffer = importlib.import_module("websocket._abnf").frame_buffer
    except ImportError as error:
        raise Refused("BLOCKED: prepared websocket-client library required; no auto-install") from error
    if getattr(module, "__version__", None) != "1.9.0":
        raise Refused("BLOCKED: websocket-client 1.9.0 required for audited bounded framing API")
    # websocket-client recv_frame normally allocates an unbounded advertised body.
    # Keep its framing/validation but reject the length before reading that body.
    class BoundedFrames(frame_buffer):
        def recv_length(self):
            super().recv_length()
            if self.length > MAX_PAYLOAD:
                raise Refused("websocket_frame_length_limit")
    return module, BoundedFrames


def send_application(connection, bundle, route, payload, kind):
    data = payload.encode("utf-8")
    if len(data) > MAX_PAYLOAD:
        raise Refused("outbound_payload_limit")
    # Record intent before the single write; disposition is separate evidence.
    bundle.record(data, route_id=route["id"], transport="ws", direction="outbound",
                  opcode="text", kind=kind, disposition="intent_before_send")
    connection.send(payload)
    bundle.record(b"", route_id=route["id"], transport="ws", direction="local",
                  kind="send_returned", acknowledges_exchange_receipt=False)


def open_ws(route, bundle, deadline, module, frames_class):
    connection = module.WebSocket(enable_multithread=False, fire_cont_frame=True)
    connection.frame_buffer = frames_class(connection._recv, False)
    try:
        # No proxy, redirect, compression-extension negotiation, credential or cookie.
        connection.connect(route["url"], timeout=remaining(deadline), redirect_limit=0,
                           http_no_proxy=["*"], suppress_origin=True)
        if connection.getstatus() != 101:
            raise Refused("websocket_upgrade_refused_no_redirect")
        bundle.record(b"", route_id=route["id"], transport="ws", direction="local",
                      kind="connected", url=route["url"], ws_extensions="none",
                      library="websocket-client", library_version=module.__version__)
        for subscription in route["subscribe"]:
            send_application(connection, bundle, route, subscription, "subscribe")
        return {"socket": connection, "route": route, "parts": bytearray(),
                "fragments": [], "message_opcode": None, "next_ping": time.monotonic()}
    except BaseException:
        if connection.sock:
            connection.sock.close()
        raise


def poll_ws(state, bundle, deadline, module):
    connection, route = state["socket"], state["route"]
    heartbeat = route.get("heartbeat", {})
    if heartbeat.get("interval_seconds") and time.monotonic() >= state["next_ping"]:
        send_application(connection, bundle, route, heartbeat["send"], "heartbeat")
        state["next_ping"] = time.monotonic() + heartbeat["interval_seconds"]
    connection.settimeout(min(0.25, remaining(deadline)))
    try:
        frame = connection.recv_frame()
    except module.WebSocketTimeoutException:
        return
    received_monotonic_ns, received_utc_ns = time.monotonic_ns() - bundle.origin, time.time_ns()
    payload = frame.data.encode() if isinstance(frame.data, str) else bytes(frame.data)
    opcode = frame.opcode
    if opcode in (8, 9, 10):
        bundle.record(payload, route_id=route["id"], transport="ws", direction="inbound",
                      opcode=opcode, final=bool(frame.fin), masked=bool(frame.mask_value), kind="control",
                      received_monotonic_ns=received_monotonic_ns, received_utc_ns=received_utc_ns)
        if opcode == 8:
            raise Refused("websocket_closed_no_reconnect")
        if opcode == 9:
            bundle.record(payload, route_id=route["id"], transport="ws", direction="outbound",
                          opcode=10, kind="pong", disposition="intent_before_send")
            connection.pong(payload)
        return
    if opcode not in (0, 1, 2) or any((frame.rsv1, frame.rsv2, frame.rsv3)):
        raise Refused("unsupported_ws_frame")
    if opcode != 0:
        if state["message_opcode"] is not None:
            raise Refused("interleaved_ws_message")
        state["message_opcode"] = opcode
    elif state["message_opcode"] is None:
        raise Refused("orphan_ws_continuation")
    if len(state["parts"]) + len(payload) > MAX_PAYLOAD or len(state["fragments"]) >= 256:
        raise Refused("ws_message_or_fragment_limit")
    state["parts"].extend(payload)
    state["fragments"].append({"opcode": opcode, "final": bool(frame.fin),
                               "masked": bool(frame.mask_value), "bytes": len(payload)})
    if not frame.fin:
        return
    data = bytes(state["parts"])
    compression = route.get("compression", "none")
    plain = decode_payload(data, compression)
    bundle.record(data, route_id=route["id"], transport="ws", direction="inbound",
                  opcode=state["message_opcode"], fragments=state["fragments"],
                  compression=compression, decoded_length=len(plain),
                  received_monotonic_ns=received_monotonic_ns, received_utc_ns=received_utc_ns,
                  decoded_sha256=hashlib.sha256(plain).hexdigest(), kind="application")
    state.update(parts=bytearray(), fragments=[], message_opcode=None)
    reply = None
    if heartbeat.get("echo_json_ping"):
        try:
            message = json.loads(plain)
        except (ValueError, UnicodeError):
            message = None
        if isinstance(message, dict) and "ping" in message:
            ping = message["ping"]
            if isinstance(ping, (int, str)) and not isinstance(ping, bool):
                reply = json.dumps({"pong": ping}, separators=(",", ":"))
    if heartbeat.get("reply_to_text") and plain == heartbeat["reply_to_text"].encode():
        reply = heartbeat["reply_text"]
    if heartbeat.get("reply_when_contains") and heartbeat["reply_when_contains"].encode() in plain:
        reply = heartbeat["reply_text"]
    if reply is not None:
        send_application(connection, bundle, route, reply, "heartbeat")


def select_routes(profile, venue, products):
    if profile.get("schema") != "exchange.public_capture.routes":
        raise Refused("route_profile_schema_mismatch")
    records = profile["venues"].get(venue)
    if records is None:
        raise Refused("venue_not_in_frozen_catalog")
    selected = records["products"] if products == "all" else {products: records["products"].get(products)}
    if not selected or any(value is None for value in selected.values()):
        raise Refused("product_not_in_frozen_catalog")
    for product, record in selected.items():
        if record.get("status") != "available":
            raise Refused("BLOCKED: " + product + ": " + record.get("reason", "unavailable"))
    ws, http = [], []
    for product, record in selected.items():
        for route in record.get("ws", []):
            url = urllib.parse.urlsplit(route["url"])
            if url.scheme != "wss" or url.port not in (None, 443) or url.username or url.fragment:
                raise Refused("invalid_frozen_wss_route")
            ws.append(dict(route, product=product, symbol=record["symbol"]))
        http.extend(dict(route, product=product, symbol=record["symbol"])
                    for route in record.get("http", []))
    if len(ws) > 2 or len(http) > 8 or not ws + http:
        raise Refused("frozen_route_count_limit")
    return ws, http


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--venue", required=True)
    parser.add_argument("--product", default="all")
    parser.add_argument("--seconds", type=int, default=MAX_SECONDS, choices=range(1, MAX_SECONDS + 1))
    parser.add_argument("--output-name", required=True, help="New child of ignored results/public_capture")
    args = parser.parse_args()
    if not args.output_name.isascii() or not args.output_name or any(
            character not in "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-" for character in args.output_name):
        parser.error("output-name must contain only ASCII letters, digits, underscore or hyphen")
    bundle, states = None, []
    previous_handler = None
    armed = False
    status, reason = "refused", ""
    try:
        profile_bytes = PROFILE.read_bytes()
        if len(profile_bytes) > 128 * 1024:
            raise Refused("route_profile_size_limit")
        ws, http = select_routes(json.loads(profile_bytes), args.venue, args.product)
        module, frames_class = websocket_library() if ws else (None, None)
        if not hasattr(signal, "setitimer"):
            raise Refused("BLOCKED: POSIX hard deadline support required")
        deadline = time.monotonic() + args.seconds
        def expire(_signal, _frame):
            raise Refused("total_deadline")
        previous_handler = signal.signal(signal.SIGALRM, expire)
        signal.setitimer(signal.ITIMER_REAL, args.seconds)
        armed = True
        bundle = Bundle(ROOT / "results" / "public_capture" / args.output_name,
                        {"venue": args.venue, "products": args.product, "ws": ws, "http": http}, profile_bytes)
        bundle.manifest["selected_seconds"] = args.seconds
        for request in http:
            https_read(request, bundle, deadline)
        for route in ws:
            states.append(open_ws(route, bundle, deadline, module, frames_class))
        while states:
            for state in states:
                poll_ws(state, bundle, deadline, module)
        status, reason = "completed", "frozen_public_http_reads_completed"
    except Refused as error:
        reason = str(error)
        status = "bounded_stop" if reason == "total_deadline" else "refused"
    except (Exception, KeyboardInterrupt) as error:
        reason = type(error).__name__ + ": " + str(error)[:1024]
        status = "interrupted" if isinstance(error, KeyboardInterrupt) else "failed_no_retry"
    finally:
        if armed:
            signal.setitimer(signal.ITIMER_REAL, 0)
            signal.signal(signal.SIGALRM, previous_handler)
        for state in states:
            if state["socket"].sock:
                state["socket"].sock.close()
        if bundle:
            bundle.manifest["incomplete_messages_discarded"] = sum(
                state["message_opcode"] is not None for state in states)
            bundle.finish(status, reason)
    print(json.dumps({"status": status, "reason": reason,
                      "bundle": str(bundle.directory) if bundle else None}))
    return 0 if status in ("completed", "bounded_stop") else 2


if __name__ == "__main__":
    sys.exit(main())
