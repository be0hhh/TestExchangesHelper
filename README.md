# TestExchangesHelper

Bounded diagnostic for the cold exchange-data contract. It validates TLS
hostnames, caps each HTTP/WS response, never sends orders, and prints only
schema/count evidence. Account values, credentials, signatures, JWTs and wallet
identities are never printed.

Each matrix row is a precise `venue/product/capability/wire` contract. It is
either profiled, not profiled, credential-missing, or a real
transport/data/schema result. It never promotes an adjacent endpoint into a
capability it does not provide.

Requested public classes are exchange info, instrument catalogue/detail,
current funding all/by-symbol, funding history, historical trades, live trades,
BBO and L2. Private classes are balances, positions, open orders, order history,
fill history, and account/orders/trades streams. Spot funding rows are omitted;
unsupported venue methods remain explicit matrix rows.

## Matrix first

```bash
python3 run.py --capability-matrix
python3 run.py --catalog-audit
```

Both commands are static-only and do not use the network or credentials. The
matrix marks `core_selected` versus `diagnostic_variant` separately, so JSON,
SBE, FIX/SBE and gRPC/protobuf claims cannot be conflated.

Public REST example:

```bash
python3 run.py --mode public --transport rest
```

Credentialed read-only REST across every configured canonical API slot:

```bash
python3 run.py --mode private --transport rest \
  --confirm-read-only-private --env-file .env
```

The same credentials can be sourced from a local INI profile. The tool reads
only `env_path`, loads values into its own process and never prints their names
or values:

```bash
python3 run.py --mode private --transport rest \
  --confirm-read-only-private --profile-config local-profile.ini
```

Public WebSocket `welcome -> subscribe -> correlated ACK -> data` contract,
including an optional raw public frame:

```bash
python3 run.py --mode public --transport ws --raw-public
```

Use at most three bounded attempts when a public stream is sparse or a cold
edge closes a connection. The output records both `attempts_allowed` and
`attempts_used`; a retry never erases the final wire/schema evidence.

```bash
python3 run.py --mode public --transport ws --attempts 3
```

## Wires and reusable API

This tool is pure Python and has no C++ target or core helper. JSON REST, JSON
WebSocket and binary WebSocket/SBE profiles are observed directly. A FIX/SBE
row is retained as source-backed metadata but reports
`external_adapter_required`: Python does not pretend to reproduce an exchange
FIX signer/session safely. Finam and Finam Arena are not profiles of this tool.

The CLI is a thin view over importable functions when the repository parent is
on `PYTHONPATH`:

```python
from exchange_api_probe import public_rest, public_ws
```

They return redacted dictionaries with status, timing, shape and wire evidence;
they never place orders or return credentials.

The REST client follows the standard-library HTTPS proxy configuration. The
minimal public WebSocket observer connects directly and therefore does not
support HTTP CONNECT proxies.

Private REST is read-only and requires `--confirm-read-only-private`. Every
configured credential slot is attempted independently. A missing private
transport profile is reported as unsupported/missing, never silently
approximated by a public subscription.

`--catalog-audit` validates the profile catalog without a source checkout. Pass
`--source-root /path/to/source` only when a companion source tree is available
for cross-checking implementation references. It is not build, runtime or live
evidence. `--sandbox-file candidate.json` runs a public-only candidate profile
for investigating a new venue. Sandbox files cannot define credentials or
private calls.

This tool is diagnostic, not a benchmark or trading-readiness proof. A
successful probe proves only the named route, wire and API slot.
