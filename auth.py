from __future__ import annotations

import base64
import datetime
import hashlib
import hmac
from urllib.parse import urlsplit

from model import Credentials, RestCase


def _hmac_hex(secret: str, payload: str, digest=hashlib.sha256) -> str:
    return hmac.new(secret.encode(), payload.encode(), digest).hexdigest()


def _hmac_b64(secret: str, payload: str) -> str:
    return base64.b64encode(hmac.new(secret.encode(), payload.encode(), hashlib.sha256).digest()).decode()


def _append_query(path: str, query: str) -> str:
    return f"{path}{'&' if '?' in path else '?'}{query}"


def binance(case: RestCase, credentials: Credentials, now_ms: int) -> tuple[str, dict[str, str], bytes]:
    unsigned = _append_query(case.path, f"timestamp={now_ms}&recvWindow=5000")
    signature = _hmac_hex(credentials.secret, urlsplit(unsigned).query)
    return _append_query(unsigned, f"signature={signature}"), {"X-MBX-APIKEY": credentials.key}, b""


def bybit(case: RestCase, credentials: Credentials, now_ms: int) -> tuple[str, dict[str, str], bytes]:
    query = urlsplit(case.path).query
    recv_window = "5000"
    signature = _hmac_hex(credentials.secret, f"{now_ms}{credentials.key}{recv_window}{query}")
    return case.path, {
        "X-BAPI-API-KEY": credentials.key,
        "X-BAPI-TIMESTAMP": str(now_ms),
        "X-BAPI-RECV-WINDOW": recv_window,
        "X-BAPI-SIGN": signature,
    }, b""


def okx(case: RestCase, credentials: Credentials, now_ms: int) -> tuple[str, dict[str, str], bytes]:
    timestamp = datetime.datetime.fromtimestamp(now_ms / 1000, datetime.timezone.utc).isoformat(timespec="milliseconds").replace("+00:00", "Z")
    signature = _hmac_b64(credentials.secret, f"{timestamp}GET{case.path}")
    return case.path, {
        "OK-ACCESS-KEY": credentials.key,
        "OK-ACCESS-SIGN": signature,
        "OK-ACCESS-TIMESTAMP": timestamp,
        "OK-ACCESS-PASSPHRASE": credentials.passphrase,
    }, b""


def gate(case: RestCase, credentials: Credentials, now_ms: int) -> tuple[str, dict[str, str], bytes]:
    parsed = urlsplit(case.path)
    timestamp = str(now_ms // 1000)
    body_hash = hashlib.sha512(b"").hexdigest()
    sign_base = f"GET\n{parsed.path}\n{parsed.query}\n{body_hash}\n{timestamp}"
    return case.path, {"KEY": credentials.key, "Timestamp": timestamp,
                       "SIGN": _hmac_hex(credentials.secret, sign_base, hashlib.sha512)}, b""


def kucoin(case: RestCase, credentials: Credentials, now_ms: int) -> tuple[str, dict[str, str], bytes]:
    timestamp = str(now_ms)
    signature = _hmac_b64(credentials.secret, f"{timestamp}GET{case.path}")
    passphrase = _hmac_b64(credentials.secret, credentials.passphrase)
    return case.path, {
        "KC-API-KEY": credentials.key,
        "KC-API-SIGN": signature,
        "KC-API-TIMESTAMP": timestamp,
        "KC-API-PASSPHRASE": passphrase,
        "KC-API-KEY-VERSION": "2",
    }, b""


def bitget(case: RestCase, credentials: Credentials, now_ms: int) -> tuple[str, dict[str, str], bytes]:
    timestamp = str(now_ms)
    signature = _hmac_b64(credentials.secret, f"{timestamp}GET{case.path}")
    return case.path, {
        "ACCESS-KEY": credentials.key,
        "ACCESS-SIGN": signature,
        "ACCESS-TIMESTAMP": timestamp,
        "ACCESS-PASSPHRASE": credentials.passphrase,
    }, b""
