from __future__ import annotations

import base64
import hashlib
import json
import os
import socket
import ssl
import struct
import time
import zlib
from dataclasses import dataclass
from typing import Any

from model import WsCase


MAX_HTTP_HEAD = 64 * 1024
MAX_MESSAGE = 8 * 1024 * 1024
GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"


@dataclass(frozen=True)
class WsObservation:
    connected: bool
    elapsed_ms: int
    opcode: int
    payload_bytes: int
    json_value: Any | None
    ack_complete: bool
    data_complete: bool
    control_pings: int
    protocol_stage: str
    public_payload: bytes = b""
    error: str = ""


def _exact(sock: ssl.SSLSocket, size: int, pending: bytearray) -> bytes:
    data = bytearray()
    if pending:
        take = min(size, len(pending))
        data.extend(pending[:take])
        del pending[:take]
    while len(data) < size:
        chunk = sock.recv(size - len(data))
        if not chunk:
            raise ConnectionError("unexpected_eof")
        data.extend(chunk)
    return bytes(data)


def _send(sock: ssl.SSLSocket, opcode: int, payload: bytes) -> None:
    mask = os.urandom(4)
    size = len(payload)
    if size < 126:
        header = bytes((0x80 | opcode, 0x80 | size))
    elif size <= 0xFFFF:
        header = bytes((0x80 | opcode, 0xFE)) + struct.pack("!H", size)
    else:
        header = bytes((0x80 | opcode, 0xFF)) + struct.pack("!Q", size)
    masked = bytes(value ^ mask[index & 3] for index, value in enumerate(payload))
    sock.sendall(header + mask + masked)


def _receive(sock: ssl.SSLSocket, pending: bytearray) -> tuple[int, bytes, int]:
    fragments = bytearray()
    message_opcode = 0
    pings = 0
    while True:
        first, second = _exact(sock, 2, pending)
        final = bool(first & 0x80)
        opcode = first & 0x0F
        masked = bool(second & 0x80)
        size = second & 0x7F
        if masked:
            raise ValueError("server_frame_masked")
        if size == 126:
            size = struct.unpack("!H", _exact(sock, 2, pending))[0]
        elif size == 127:
            size = struct.unpack("!Q", _exact(sock, 8, pending))[0]
        if size > MAX_MESSAGE or len(fragments) > MAX_MESSAGE - size:
            raise ValueError("message_capacity_exceeded")
        payload = _exact(sock, size, pending)
        if opcode == 0x9:
            pings += 1
            _send(sock, 0xA, payload)
            continue
        if opcode == 0xA:
            continue
        if opcode == 0x8:
            raise ConnectionError("peer_close")
        if opcode in (0x1, 0x2):
            if message_opcode:
                raise ValueError("interleaved_message")
            message_opcode = opcode
        elif opcode != 0x0 or not message_opcode:
            raise ValueError("invalid_opcode")
        fragments.extend(payload)
        if final:
            return message_opcode, bytes(fragments), pings


def _payload(case: WsCase) -> bytes:
    if not case.subscribe:
        return b""
    try:
        value = json.loads(case.subscribe)
    except (UnicodeDecodeError, json.JSONDecodeError):
        return case.subscribe
    if isinstance(value, dict) and "time" in value:
        value["time"] = int(time.time())
    return json.dumps(value, separators=(",", ":")).encode()


def _json(payload: bytes) -> Any | None:
    try:
        return json.loads(payload)
    except (UnicodeDecodeError, json.JSONDecodeError):
        return None


def _application_payload(case: WsCase, payload: bytes) -> bytes:
    if case.compression != "gzip":
        return payload
    decoder = zlib.decompressobj(16 + zlib.MAX_WBITS)
    decoded = decoder.decompress(payload, MAX_MESSAGE + 1)
    decoded += decoder.flush(MAX_MESSAGE + 1 - len(decoded))
    if len(decoded) > MAX_MESSAGE or decoder.unused_data:
        raise ValueError("gzip_payload_capacity_or_trailer")
    return decoded


def _matches_ack(case: WsCase, value: Any | None, opcode: int) -> bool:
    if case.ack_validator:
        return case.ack_validator(value, opcode)
    if case.expected_ack_keys:
        return isinstance(value, dict) and all(key in value for key in case.expected_ack_keys)
    return not case.subscribe


def _matches_data(case: WsCase, value: Any | None, opcode: int, payload: bytes) -> bool:
    if case.expected_inbound_opcode is not None and opcode != case.expected_inbound_opcode:
        return False
    if case.binary_data:
        return len(payload) >= 8
    return case.data_validator(value, opcode) if case.data_validator else value is not None


def _application_pong(case: WsCase, value: Any | None) -> bytes:
    """Return the documented application-level pong payload, if this is one."""
    if case.application_heartbeat == "htx" and isinstance(value, dict) and "ping" in value:
        return json.dumps({"pong": value["ping"]}, separators=(",", ":")).encode()
    return b""


def observe(case: WsCase, timeout: float) -> WsObservation:
    start = time.monotonic_ns()
    sock: ssl.SSLSocket | None = None
    pings = 0
    stage = "connecting"
    last_opcode = 0
    last_payload = b""
    ack_complete = False
    data_complete = False
    try:
        raw = socket.create_connection((case.host, 443), timeout=timeout)
        sock = ssl.create_default_context().wrap_socket(raw, server_hostname=case.host)
        sock.settimeout(timeout)
        key = base64.b64encode(os.urandom(16)).decode()
        extra_headers = "".join(f"{name}: {value}\r\n" for name, value in case.handshake_headers)
        request = (
            f"GET {case.path} HTTP/1.1\r\nHost: {case.host}\r\n"
            "Upgrade: websocket\r\nConnection: Upgrade\r\n"
            f"Sec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\n"
            "User-Agent: cxet-read-only-probe/1\r\n"
            f"{extra_headers}\r\n"
        ).encode()
        sock.sendall(request)
        head = bytearray()
        while b"\r\n\r\n" not in head:
            chunk = sock.recv(4096)
            if not chunk:
                raise ConnectionError("handshake_unexpected_eof")
            head.extend(chunk)
            if len(head) > MAX_HTTP_HEAD:
                raise ValueError("handshake_capacity_exceeded")
        header, _, remainder = bytes(head).partition(b"\r\n\r\n")
        pending = bytearray(remainder)
        lines = header.split(b"\r\n")
        if not lines or b" 101 " not in lines[0] + b" ":
            raise ConnectionError("upgrade_rejected")
        headers = {}
        for line in lines[1:]:
            name, separator, value = line.partition(b":")
            if separator:
                headers[name.strip().lower()] = value.strip()
        expected = base64.b64encode(hashlib.sha1((key + GUID).encode()).digest())
        if headers.get(b"sec-websocket-accept") != expected:
            raise ConnectionError("accept_mismatch")
        subscribe = _payload(case)
        stage = "connected"
        if case.read_before_subscribe:
            opcode, payload, received_pings = _receive(sock, pending)
            last_opcode, last_payload = opcode, payload
            pings += received_pings
            if not case.welcome_validator or not case.welcome_validator(_json(payload), opcode):
                raise ValueError("welcome_mismatch")
            stage = "welcome"
        if subscribe:
            _send(sock, case.subscribe_opcode, subscribe)
            stage = "subscribe_sent"
        deadline = time.monotonic() + timeout
        ack_complete = not subscribe
        while time.monotonic() < deadline:
            opcode, payload, received_pings = _receive(sock, pending)
            last_opcode, last_payload = opcode, payload
            pings += received_pings
            value = _json(_application_payload(case, payload))
            application_pong = _application_pong(case, value)
            if application_pong:
                _send(sock, 0x1, application_pong)
                continue
            if not ack_complete and _matches_ack(case, value, opcode):
                ack_complete = True
                stage = "ack"
                if case.ack_implies_data and case.data_validator and case.data_validator(value, opcode):
                    data_complete = True
                    return WsObservation(True, (time.monotonic_ns() - start) // 1_000_000,
                                         opcode, len(payload), value, True, True, pings, "data", payload)
                if not case.require_data_after_ack:
                    return WsObservation(True, (time.monotonic_ns() - start) // 1_000_000,
                                         opcode, len(payload), value, True, False, pings, stage)
                continue
            if not ack_complete and case.data_implies_ack and _matches_data(case, value, opcode, payload):
                stage = "data_implies_ack"
                return WsObservation(True, (time.monotonic_ns() - start) // 1_000_000,
                                     opcode, len(payload), value, True, True, pings, stage, payload)
            # Data observed before a correlated ACK cannot prove subscription.
            if ack_complete and _matches_data(case, value, opcode, payload):
                stage = "data"
                data_complete = True
                return WsObservation(True, (time.monotonic_ns() - start) // 1_000_000,
                                     opcode, len(payload), value, True, True, pings, stage, payload)
        if ack_complete and case.require_data_after_ack:
            raise TimeoutError("data_after_ack_timeout")
        raise TimeoutError("ack_timeout" if subscribe else "observation_timeout")
    except (OSError, ValueError, ConnectionError, TimeoutError) as error:
        failure_stage = "data_timeout_after_ack" if ack_complete and case.require_data_after_ack else \
            f"{stage}_failed"
        return WsObservation(False, (time.monotonic_ns() - start) // 1_000_000,
                             last_opcode, len(last_payload), _json(last_payload), ack_complete, data_complete,
                             pings, failure_stage, last_payload,
                             str(error) or type(error).__name__)
    finally:
        if sock:
            try:
                _send(sock, 0x8, b"")
            except OSError:
                pass
            sock.close()
