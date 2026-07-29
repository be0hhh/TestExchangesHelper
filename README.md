# exchange-api-probe

`exchange-api-probe` is an independent Linux C++20 diagnostic for exchange
HTTP, WebSocket and explicitly enabled read-only private surfaces. It can be
cloned and built without any parent repository.

The tool separates four kinds of evidence:

- `exact`: the profile declares enough wire semantics for normalization;
- `observed_only`: raw capture is supported, but semantic decoding is not
  asserted;
- `adapter_required`: raw capture may be supported and a language-neutral
  adapter is required for decoding;
- `candidate` or `unavailable`: discovery input or an explicit unsupported
  surface.

None of these labels proves matching-engine location, trading readiness,
causality, or production latency.

## Build

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

Dependencies are OpenSSL, Boost.JSON, pthreads and a C++20 compiler. The
optional development viewer uses Node.js:

```bash
cd web
npm install
npm run build
```

Production viewer assets are embedded in the native executable; Node.js is not
required to inspect a bundle.

## Profiles

Profiles are strict JSON documents under `profiles/`, described by
`schemas/profile.v1.schema.json`. The built-in catalog contains 26
venue/product entries. Profile edits are intentionally separate from observed
discovery evidence.

```bash
./build/exchange-api-probe profile list
./build/exchange-api-probe profile search --query depth
./build/exchange-api-probe profile show --venue binance --product futures
./build/exchange-api-probe profile validate
./build/exchange-api-probe profile scaffold \
  --venue example --product spot --output-dir proposals
./build/exchange-api-probe profile promote \
  --input results/discovery/profile-proposal.json \
  --output-dir proposals
```

`profile promote` creates a bounded promotion-review packet and reports
`catalog_mutated=false`; it never edits a trusted profile implicitly. Review
the packet and copy accepted fields into a profile in version control.

## Bounded discovery

Discovery executes only candidates already present in a profile and writes
immutable evidence plus a proposal:

```bash
./build/exchange-api-probe discover \
  --venue binance --product futures \
  --output-dir results/discovery
```

The output is `evidence.jsonl` and `profile-proposal.json`. It is not an
automatic profile mutation.

## Research capture and analysis

The default research topology is one WebSocket session per channel, three
rounds of five minutes each. Connection order is reversed between rounds to
reduce a fixed startup-order bias.

```bash
./build/exchange-api-probe research run \
  --venue binance --product futures --symbol btcusdt \
  --channel trades --channel book_ticker --channel depth_100ms \
  --output-dir results/binance-futures
```

Use `--no-open` for automation. Without it, the localhost read-only viewer is
started after capture and the browser is opened. Capture stores exact inbound
frame bytes in `frames.bin` and an immutable index in `frames.jsonl`.

Analyze an existing neutral v3 bundle without network access:

```bash
./build/exchange-api-probe research analyze \
  --input results/binance-futures
./build/exchange-api-probe serve --input results
```

Analysis produces:

- normalized `events.jsonl`;
- BBO/depth `state_transitions.jsonl`;
- evidence-labelled `relations.jsonl`;
- `findings.json` and `REPORT.md`.

The four time domains remain distinct: local monotonic, local UTC, exchange
event time and exchange transaction time. Missing exchange clocks remain null;
they are never replaced with receive time.

Relation modes are independent:

- native identity;
- exchange-time cohort;
- state convergence;
- bounded receive-window market-effect heuristic.

Heuristic relations explicitly state `causality: not_asserted`.

## Private and packet capture safety

Existing private REST probes are read-only and require both credentials and
`--confirm-private`. Order creation, amendment, cancellation and account
mutation are outside the tool.

Private WebSocket and FIX research additionally require an explicit lifecycle
profile and `--confirm-session-lifecycle`. A missing lifecycle fails closed.
Profile schema v1 records adapter identities and the language-neutral protocol,
but the native analyzer deliberately remains fail-closed with
`binary_adapter_required`; subprocess execution is not enabled in this version.
`--allow-adapter` and `--allow-private-adapter` reserve the future explicit
trust gates and currently do not execute anything.

Packet capture is public-only, opt-in, bounded, and may require OS capabilities.
TLS key logging is never enabled implicitly. See `doc/ADAPTER_PROTOCOL.md` for
the adapter JSONL contract.

## Existing diagnostics

The prior commands remain available:

```text
matrix
latency
stability
placement
compare
sandbox
```

They use bounded attempts, timeouts and artifact limits. `latency` reports the
measured client boundary only; it must not be presented as matching-engine or
order-path latency.

## Bundle compatibility

Research accepts only `exchange.api_probe.bundle.v3`. Older prefixed bundle
formats are deliberately not migrated or silently read. The legacy profiler
and compare path use neutral `exchange.api_probe.bundle.v2`.
