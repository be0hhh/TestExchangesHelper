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

The default builds this owner's product incrementally with pinned Clang, GNU
Make, Release `-O3`, native CPU targeting and LTO OFF. `portable` disables CPU
targeting. Fresh foreign providers are reused silently; missing or stale foreign
modules are listed in one combined prompt before rebuilding them. Decline or EOF
cancels; a noninteractive invocation with stale providers exits with an explanatory
error. `--force` builds the selected product and necessary closure incrementally
with FULL LTO, without cleaning or prompting; ThinLTO is never selected.

`all` builds and runs only this owner's registered tests and needed dependencies.
`--force all` first builds the optimized product, then local tests. Unrelated
products, benchmarks and other owners' tests are not selected. An empty test
registry is reported explicitly and does not establish passing test proof.

Optimized trees use `build` (native) or `build/modes/portable`; development trees
use `build/modes/dev-native` or `build/modes/dev-portable`. `CXET_BUILD_DIR` is the
exact caller-supplied path; an incompatible existing profile is rejected. Only a
successful product build updates `build/.compile-active/<owner>.json`; default
launchers resolve that selected tree. Failed, UI-only and test-only runs do not
switch it. Project-owned libraries remain static `.a`; `p` selects available
processors and `-j N` overrides it. See `--help` for supported options.

Old local test and benchmark targets have been retired from the source graph.

## Continuous integration

Retired feed-comparison targets are absent. The current owner offline suite
checks protocol contracts and the cleanup regressions without network probes. CI, compiler and runtime results are separate evidence;
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

## Capability and reachability checks

`matrix` lists declared capabilities; `sandbox` validates bounded protocol
observations. `placement` reports endpoint/DNS/IP discovery and one bounded
TCP/TLS reachability attempt per selected natural or pinned direct route.
Results contain status, stage/error and connection identity, without timing
series, ping, throughput, histograms or speed rankings.

`latency`, `stability`, `compare` and the old `run` alias are removed.
`--lanes`, `--samples` and `--connection` are rejected. `--mode` selects only
the existing discovery budget; `--duration-seconds` bounds research capture.
Observation JSON uses `schema_version=4`; relation/findings outputs use their
current `v2` schema identifiers. Raw receive/exchange timestamps, normalized
identity, sequence/book validity and bounded-corpus completeness remain.

## Bundle compatibility

Research capture bundles use `exchange.api_probe.bundle.v3`. Older bundle
formats are deliberately not migrated or silently interpreted as current bundles.
