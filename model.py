from __future__ import annotations

from dataclasses import dataclass, field
from typing import Any, Callable


@dataclass(frozen=True)
class Credentials:
    key: str
    secret: str
    passphrase: str = ""
    account_id: str = ""
    jwt: str = ""


@dataclass(frozen=True)
class RestCase:
    name: str
    host: str
    path: str
    private: bool = False
    capability: str = ""
    selection: str = "diagnostic_variant"
    native: bool = True
    rest_contract: str = ""
    expected_symbol: str = ""
    expected_logical_success: bool = True
    core_anchor: str = ""
    parser_anchor: str = ""


@dataclass(frozen=True)
class WsCase:
    name: str
    host: str
    path: str
    subscribe: bytes = b""
    expected_ack_keys: tuple[str, ...] = ()
    subscribe_opcode: int = 0x1
    expected_inbound_opcode: int | None = None
    handshake_headers: tuple[tuple[str, str], ...] = ()
    read_before_subscribe: bool = False
    welcome_validator: "WsValidator | None" = None
    ack_validator: "WsValidator | None" = None
    data_validator: "WsValidator | None" = None
    require_data_after_ack: bool = False
    data_implies_ack: bool = False
    binary_data: bool = False
    ack_implies_data: bool = False
    compression: str = ""
    application_heartbeat: str = ""
    core_anchor: str = ""
    parser_anchor: str = ""
    selected_wire: str = "json"
    status: str = "core_aligned"
    capability: str = ""
    selection: str = "core_selected"
    private: bool = False


@dataclass(frozen=True)
class FixCase:
    name: str
    host: str
    port: int
    session_kind: str
    wire: str
    credential_prefix: str
    core_anchor: str
    payload_anchor: str
    capability: str = ""
    selection: str = "core_selected"


@dataclass(frozen=True)
class CapabilityCase:
    """One separately reportable exchange capability/wire contract."""
    name: str
    surface: str
    transport: str
    wire: str = "json"
    selection: str = "core_selected"
    private: bool = False
    requires_confirmation: bool = False
    mutation: bool = False
    native: bool = True


Signer = Callable[[RestCase, Credentials, int], tuple[str, dict[str, str], bytes]]
WsValidator = Callable[[Any, int], bool]


@dataclass(frozen=True)
class ProductSpec:
    venue: str
    product: str
    credential_prefix: str
    public_rest: tuple[RestCase, ...]
    private_rest: tuple[RestCase, ...] = ()
    public_ws: tuple[WsCase, ...] = ()
    signer: Signer | None = None
    notes: tuple[str, ...] = field(default_factory=tuple)
    fix_sessions: tuple[FixCase, ...] = ()
    capabilities: tuple[CapabilityCase, ...] = ()
