from __future__ import annotations

import json
import ssl
import time
import urllib.error
import urllib.request
from dataclasses import dataclass
from typing import Any

from model import Credentials, RestCase, Signer


MAX_RESPONSE_BYTES = 32 * 1024 * 1024


@dataclass(frozen=True)
class HttpObservation:
    status: int
    elapsed_ms: int
    body_bytes: int
    json_value: Any | None
    error: str = ""


def execute(case: RestCase, timeout: float, credentials: Credentials | None = None,
            signer: Signer | None = None) -> HttpObservation:
    now_ms = int(time.time() * 1000)
    start = time.monotonic_ns()
    try:
        path = case.path
        headers: dict[str, str] = {
            "Accept": "application/json",
            "User-Agent": "cxet-read-only-probe/1",
        }
        body = b""
        if case.private:
            if not credentials or not signer:
                return HttpObservation(
                    0, 0, 0, None, "credentials_or_signer_unavailable")
            path, signed_headers, body = signer(case, credentials, now_ms)
            headers.update(signed_headers)
        request = urllib.request.Request(
            f"https://{case.host}{path}", data=body or None,
            headers=headers, method="GET")
        with urllib.request.urlopen(request, timeout=timeout, context=ssl.create_default_context()) as response:
            payload = response.read(MAX_RESPONSE_BYTES + 1)
            elapsed_ms = (time.monotonic_ns() - start) // 1_000_000
            if len(payload) > MAX_RESPONSE_BYTES:
                return HttpObservation(response.status, elapsed_ms, len(payload), None,
                                       "response_capacity_exceeded")
            try:
                value = json.loads(payload)
            except (UnicodeDecodeError, json.JSONDecodeError):
                return HttpObservation(response.status, elapsed_ms, len(payload), None,
                                       "invalid_json")
            return HttpObservation(response.status, elapsed_ms, len(payload), value)
    except urllib.error.HTTPError as error:
        payload = error.read(min(MAX_RESPONSE_BYTES, 64 * 1024))
        elapsed_ms = (time.monotonic_ns() - start) // 1_000_000
        try:
            value = json.loads(payload)
        except (UnicodeDecodeError, json.JSONDecodeError):
            value = None
        return HttpObservation(error.code, elapsed_ms, len(payload), value, "http_error")
    except (OSError, TimeoutError, urllib.error.URLError) as error:
        elapsed_ms = (time.monotonic_ns() - start) // 1_000_000
        return HttpObservation(0, elapsed_ms, 0, None, type(error).__name__)
    except (TypeError, ValueError) as error:
        elapsed_ms = (time.monotonic_ns() - start) // 1_000_000
        return HttpObservation(0, elapsed_ms, 0, None,
                               f"request_{type(error).__name__}")
