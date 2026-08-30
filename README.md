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

## Exchange feed race diagnostic

The opt-in root build flag `CXET_EXCHANGE_FEED_RACE_BUILD=ON` adds the
isolated `exchange-feed-race` binary. It is not part of production low builds.
Its compact `records.bin` format uses the versioned `RaceRecord` contract and
is analyzed without network access:

```bash
./build/exchange-feed-race/tools/exchange_api_probe/exchange-feed-race plan
./build/exchange-feed-race/tools/exchange_api_probe/exchange-feed-race \
  analyze results/feed-race results/feed-race/report
```

`validate-command` prints the deterministic short command and
`capture-command` prints the 3x10-minute command. Implemented venue capture
discovers requested USDT perpetuals through the venue instrument endpoint and
opens one physical WS/TCP/TLS connection per `{symbol, feed}`. Bybit uses four
independent BBO-capable book feeds (`orderbook.1`, `.50`, `.200`, `.1000`) and
one separate `publicTrade` activity feed:

```bash
./build/exchange-feed-race/tools/exchange_api_probe/exchange-feed-race capture \
  --output results/bybit-btr-race \
  --symbols BTR --venues bybit \
  --sessions 1 --warmup-seconds 20 --measured-seconds 60 --validation
```

Venues without implemented live orchestration still fail closed with an
explicit `capture_venue_not_implemented` result. The command never silently
substitutes the older neutral research capture.

Offline analysis accepts either one `session-*` directory or the capture root
containing multiple sessions. The Bybit podium is strict: only the same valid
BBO transition and sequence observed by all four book feeds in one session can
enter it. Partial events remain visible as excluded coverage; `publicTrade`
never enters the BBO denominator. The output contains:

- `dashboard.html`: self-contained four-lane event scrubber, rolling leader,
  podium, pairwise matrix, coverage/health, and browser-side PNG export;
- `report.md`: readable summary and technical tables with signed
  `recv_A - recv_B` semantics;
- `plots/race_overview.svg`, `rolling_leader.svg`, `coverage_health.svg`, and
  `pairwise_matrix.svg`, plus the existing technical SVG appendix.

An observed primary/complement candidate is emitted only after three clean
sessions with at least 1,000 strict events per session and the same unique
leader in every session. It is evidence, not an automatic production choice.

## BBO reconstruction diagnostic

The same opt-in root build also adds `exchange-bbo-reconstruction-probe`.
It consumes the exact CXET RuntimeV1 book-ticker, trade and optional depth
routes declared by the product descriptor; a wire, transport, lane or parser
contract mismatch fails the capture before observations are accepted. The
provenance check compares the callbacks actually bound into every runtime
route with the selected canonical descriptor, not only their enum labels.

```bash
./build/exchange-feed-race/tools/exchange_api_probe/exchange-bbo-reconstruction-probe catalog
./build/exchange-feed-race/tools/exchange_api_probe/exchange-bbo-reconstruction-probe \
  capture results/reconstruction.bin ETHUSDT TICK_RAW EXCHANGE_RAW \
  MARKET_RAW PROFILE_RAW 60 --depth
./build/exchange-feed-race/tools/exchange_api_probe/exchange-bbo-reconstruction-probe \
  analyze results/reconstruction.bin results/reconstruction.md
```

`campaign OUTPUT_ROOT [SESSIONS] [SECONDS] [--no-depth]` defaults to three
fresh 600-second sessions. It selects ETH plus the two highest-volume eligible
USDT perpetuals only when the exact ticker route supports that ranking;
otherwise it fails the volume selection closed and keeps ETH only.
An eligible product without instrument-rule discovery is reported as an
explicit product failure instead of being silently omitted.

The capture schema is completion-aware: records are first written to a
`.partial` file, followed by a count-bearing footer, then atomically published
without replacing an existing result. Offline analysis accepts only completed,
schema-valid captures and preserves route provenance. Both writing and reading
fail closed above 10,000,000 observations (about 1.28 GB plus framing), so a
malformed footer cannot trigger unbounded record growth or allocation.

Reports keep two explicitly different counterfactuals:

- `strict`: exchange-time-only eligibility, matching the production
  reconstruction contract;
- `diagnostic`: receive-time fallback for measuring what exchange snapshots
  may have hidden when exact exchange timestamps are unavailable.

Both modes require the confirming BBO/depth observation to be temporally after
the accepted trade in the selected clock domain. The diagnostic mode is
research evidence only; it is not a silent production fallback or proof of
matching-engine causality.

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
