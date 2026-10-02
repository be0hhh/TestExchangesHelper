# exchange-api-probe

[![CI](https://github.com/be0hhh/TestExchangesHelper/actions/workflows/ci.yml/badge.svg?branch=main)](https://github.com/be0hhh/TestExchangesHelper/actions/workflows/ci.yml?query=branch%3Amain)

[Probe Actions](https://github.com/be0hhh/TestExchangesHelper/actions) ·
[CXETCPP Actions](https://github.com/be0hhh/CXETCPP/actions)

`exchange-api-probe` is an independent Linux C++20 diagnostic for exchange
HTTP, WebSocket and explicitly enabled read-only private surfaces. Its standalone
build uses the shared Clang 24 toolchain owned by the CXET root.

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

From this directory in the canonical CXET checkout:

```bash
./compile.sh p
./compile.sh all p
./compile.sh all portable p
```

The default incrementally builds this repository's production targets and their
full required dependency closure, including missing or stale foreign providers.
Ready targets are reused. `all` builds and runs only this repository's registered
tests and their required production dependencies; it does not select unrelated
production daemons, benchmarks or another repository's tests. If no
local tests are registered, the command reports that and succeeds.

Libraries are static `.a` archives. Release uses `-O3`, host-native CPU targeting
and full LTO; `portable` selects portable CPU targeting. Use `p` or `-j N` for
parallelism and `--help` for the supported options.

Old local test and benchmark targets have been retired from the source graph.

## Continuous integration

The retired offline test and feed comparison targets are absent from the
current source graph. CI, compiler and runtime results are separate evidence;
this source migration does not establish a passing CI revision.

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

The old live cadence/race/reconstruction capture binaries and their independent
runtime paths have been retired; existing capture data is preserved.

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

Research and profiler bundles use `exchange.api_probe.bundle.v3`. Older bundle
formats are deliberately not migrated or silently read by compare.
