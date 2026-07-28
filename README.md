# exchange-api-probe

Standalone Linux C++20 diagnostic for CXETCPP exchange API profiles. It does
not link CXETCPP or reuse its connectors: source anchors and network
observations remain separate evidence.

The probe covers 26 spot/futures products:

- public HTTPS and WebSocket observations;
- read-only private HTTPS for six implemented HMAC families;
- capability matrix and optional CXETCPP source-anchor audit;
- bounded public-only sandbox profiles;
- JSON, binary JSON, protobuf, SBE and FIX/SBE declarations.

Success is fail-closed. TCP/TLS/HTTP alone is not an exchange-contract proof.
Exact REST contracts validate the native logical envelope, symbol and fields.
Negative-symbol cases pass only on explicitly allowed statuses or exchange
codes. WebSocket profiles validate acknowledgement and/or matching data.

## Dependencies and build

Ubuntu 24.04:

```bash
sudo apt update
sudo apt install build-essential cmake libboost-all-dev libssl-dev
cmake -S . -B build -G "Unix Makefiles" -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

There is no Ninja, vcpkg, Conan, FetchContent, vendored library, Python or
system-zlib dependency. HTTPS/WSS uses Boost.Asio/Beast, JSON uses Boost.JSON,
and TLS/HMAC use OpenSSL. HTX/BingX gzip uses Beast's header-only deflate and
Boost.CRC.

The implementation is split into independently compiled owners:

- `cli.cpp` parses arguments; `application.cpp` orchestrates commands;
- `probe_runner.cpp` classifies REST/WS observations;
- `http_client.cpp`, `ws_client.cpp`, and `net_common.cpp` own transport stages;
- `contracts.cpp`, `gzip.cpp`, and `redaction.cpp` own payload validation;
- profile factories, major venues, extended venues, and miscellaneous venues
  are separate profile units;
- `placement.cpp` owns endpoint inventory, DNS/IP and TCP/TLS placement;
- `profiler.cpp` owns bounded latency/stability runs and versioned bundles;
- `compare.cpp` owns offline Pareto comparison without an automatic winner;
- CTest has independent CLI, REST, WS/protobuf, redaction, and gzip binaries.

## Commands

Tables are the default. `--jsonl` selects schema version 3 machine output.

```bash
./build/exchange-api-probe matrix
./build/exchange-api-probe matrix --venue binance --product futures --jsonl

./build/exchange-api-probe audit \
  --source-root /path/to/CXETCPP --jsonl

./build/exchange-api-probe latency \
  --venue okx --product futures \
  --surface public --transport rest \
  --mode low --connection cold

./build/exchange-api-probe stability \
  --venue mexc --product spot \
  --surface public --transport ws \
  --mode low --duration-seconds 30

./build/exchange-api-probe placement \
  --venue binance --product futures \
  --surface public --route both --mode low

./build/exchange-api-probe compare \
  --input results/server-a --input results/server-b \
  --output-dir results/compare-a-b
```

`--venue`, `--product`, and `--case` are repeatable. Empty selections are
configuration errors. `--raw-public` adds at most 4096 public payload bytes.
The legacy `run` command was removed in version 3; use `latency`.

### Profiler owners and evidence

`placement`, `latency`, `stability`, `audit`, and `compare` are independent
owners and must be run separately. Placement reports DNS answers, natural and
pinned routes, IPv4/IPv6, proxy/direct TCP, verified TLS and optional GeoIP.
It does not infer an exchange backend, availability zone or matching cluster.

Latency samples expose bounded stage timings in microseconds:

- DNS, TCP, proxy CONNECT and TLS;
- HTTP write, response-header completion (`ttfb_us`), body completion and JSON
  parsing;
- WebSocket handshake, welcome, subscribe, ACK and first matching data;
- optional Linux `TCP_INFO` and TLS/certificate metadata.

Low mode uses one lane and five latency samples. Standard uses four lanes and
100 samples. High mode requires explicit lanes plus samples or duration and
`--confirm-load`. Stability defaults to 30 seconds in low mode and five minutes
in standard mode. Sample storage, matching state and raw capture remain bounded
for every mode.

Cold connections are the supported measurement contract. `warm` and `both`
fail closed until the selected exchange has an explicit persistent-session
profile; repeated reconnects are never labeled as warm measurements. Natural
is the default route. A mixed `--route both` bundle keeps route labels in raw
samples, but is excluded from offline Pareto comparison; run natural and pinned
as separate bundles for comparison.

Each latency/stability run bundle contains `manifest.json`, `samples.jsonl`,
`summary.json`, `metrics.csv`, `REPORT.md`, and `latency_histogram.svg`.
Percentiles use a bounded 16-subbucket logarithmic histogram and are labeled as
bucket upper bounds. Reaching the artifact cap sets
`artifact_complete=false`; it is never silently ignored.
Bundle schema v2 records the percentile method in its comparison contract;
v1 log2 bundles are rejected by `compare` instead of being mixed silently.

GeoIP and raw public payload retention are opt-in. Private payloads and
credential values are never persisted. Event age remains unavailable unless
an exchange-owned timestamp contract and bounded clock uncertainty exist.

### Proxy

`HTTPS_PROXY`/`https_proxy` and `NO_PROXY`/`no_proxy` apply to HTTPS and WSS.
Only HTTP CONNECT proxies are supported. An invalid or unavailable configured
proxy fails explicitly; there is no silent direct fallback.

### Read-only private REST

Private probing is explicit:

```bash
./build/exchange-api-probe latency \
  --venue binance --product spot \
  --surface private --transport rest \
  --confirm-private --env-file ./credentials.env
```

Public commands never load credential variables or env files. Private profiles
contain GET-only account, balance, open-order and position probes. There is no
order mutation route. Credentials are non-copyable and cleansed on destruction;
output contains shapes, not private values.

Variables use a product prefix and optional numeric slot:

```text
BINANCE_SPOT_API_KEY=...
BINANCE_SPOT_API_SECRET=...

BINANCE_SPOT_API_2_KEY=...
BINANCE_SPOT_API_2_SECRET=...
```

Some families additionally use `_PASSPHRASE` or `_ACCOUNT_ID`.

Private WS/FIX profiling additionally requires
`--confirm-session-lifecycle`. It currently fails closed as unsupported because
the probe has no exchange-owned auth/logon lifecycle profiles. Such profiles
must declare venue-specific create, keepalive and local-stop semantics; cleanup
cannot be assumed to mean revoke/delete. Order, cancel and amend messages are
outside this tool.

### Sandbox

Sandbox input is public-only, bounded to 1 MiB, and rejects private,
credential, header and auth fields:

```json
{
  "venue": "example",
  "product": "spot",
  "public_rest": [
    {
      "name": "time",
      "host": "api.example.com",
      "path": "/v1/time",
      "capabilities": ["server_time"]
    }
  ],
  "public_ws": [
    {
      "name": "trades",
      "host": "stream.example.com",
      "path": "/ws",
      "subscribe": "{\"op\":\"subscribe\",\"topic\":\"trades\"}",
      "capabilities": ["live_trades"]
    }
  ]
}
```

```bash
./build/exchange-api-probe sandbox --file profile.json --transport ws
```

Sandbox results are diagnostic variants; they do not claim CXETCPP
registration or live readiness.

## Evidence boundary

- `matrix` describes compiled probe profiles only.
- `audit` is static source evidence only.
- `latency` is a bounded point-in-time contract and latency observation.
- `stability` is a bounded diagnostic sample series, not a load guarantee.
- `placement` reports front-door transport evidence only.
- `compare` produces Pareto fronts and never an automatic best placement.
- None proves hft-trader readiness, production runtime selection, end-to-end
  parser delivery, account permissions, or safe live order capability.
