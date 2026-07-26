# exchange-api-probe

Standalone Linux C++20 diagnostic for CXETCPP exchange API profiles. It does
not link CXETCPP or reuse its connectors: source anchors and network
observations remain separate evidence.

The probe covers 28 spot/futures products:

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
- CTest has independent CLI, REST, WS/protobuf, redaction, and gzip binaries.

## Commands

Tables are the default. `--jsonl` selects schema version 2 machine output.

```bash
./build/exchange-api-probe matrix
./build/exchange-api-probe matrix --venue binance --product futures --jsonl

./build/exchange-api-probe audit \
  --source-root /path/to/CXETCPP --jsonl

./build/exchange-api-probe run \
  --venue okx --product futures \
  --surface public --transport rest

./build/exchange-api-probe run \
  --venue mexc --product spot \
  --surface public --transport ws \
  --timeout-ms 15000 --attempts 2
```

`--venue`, `--product`, and `--case` are repeatable. Empty selections are
configuration errors. `--raw-public` adds at most 4096 public payload bytes.

### Proxy

`HTTPS_PROXY`/`https_proxy` and `NO_PROXY`/`no_proxy` apply to HTTPS and WSS.
Only HTTP CONNECT proxies are supported. An invalid or unavailable configured
proxy fails explicitly; there is no silent direct fallback.

### Read-only private REST

Private probing is explicit:

```bash
./build/exchange-api-probe run \
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
- `run` is a point-in-time external network observation only.
- None proves hft-trader readiness, production runtime selection, end-to-end
  parser delivery, account permissions, or safe live order capability.
