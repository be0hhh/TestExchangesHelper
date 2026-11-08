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

## T-Bank gRPC GOAWAY research

The root-graph build includes a bounded, read-only comparison of the CXET gRPC
client and an independent nghttp2 client. Standalone builds keep the independent
client but refuse this paired command when the CXET adapter is unavailable.
The diagnostic requires OpenSSL, Boost.JSON and the `libnghttp2` development
package discoverable by `pkg-config`.

The experiment connects both clients to the same numeric peer while retaining
the T-Bank hostname for TLS and HTTP/2 authority. It observes four sequential
phases: idle without PING, idle with 10-second PING, then two phases that each
issue one public trading-status query per connection and keep one SBER order
book stream open, swapping clients between the idle and stream roles. It opens
at most two connections at once, performs no automatic reconnect, and never
submits orders or calls account methods. A changed peer, missing subscription
acknowledgement or transport error is reported as inconclusive.

The selected env file must contain `TBANK_API_1_API`, using the same T-Bank
Bearer token convention as CXET. Only this key is read for slot 1; no token or
raw authenticated payload is written to the report. An existing protected
Parser environment file can be selected directly without copying it.
Select a CA bundle explicitly with `--ca-file`; both clients verify TLS against
that path, with no fallback to another trust store. Relative paths are resolved
from the working directory. From the repository root, the T-Bank Parser uses
the bundle documented in [server/certs](../../server/certs/README.md).

Example commands from the repository root, to run only after selecting the
exact build and live actions:

```bash
CXET_BUILD_DIR=build/probes/tbank-goaway ./tools/exchange_api_probe/compile.sh portable tbank -j 1 --memory-limit-gib 8
build/probes/tbank-goaway/tools/exchange_api_probe/exchange-api-probe research tbank-goaway --engines cxet,nghttp2 --env-file apps/hft-parser/.env --ca-file server/certs/tbank-ca.pem --api-slot 1 --symbol e6123145-9665-43e0-8413-cd61b8aa9b13 --duration-seconds 60 --overall-timeout-seconds 300 --timeout-ms 5000 --attempts 1 --max-artifact-bytes 4194304 --output-dir results/tbank-goaway --no-open
```

`research tbank-lifetime` is a separate diagnostic experiment. It requires
`--duration-seconds 600` or `28800`, the same env file, CA, SBER UID and output
directory, and both engines. It makes one empty `GetUserTariff` request, checks
that the published unary groups cover both methods and that the experiment's
maximum six status requests per minute fit every applicable published window,
then schedules the two clients at 20-second intervals with a 10-second offset.
The first two status slots are 10 and 20 seconds after tariff completion. Missed
slots are not replayed; a slot delayed by a full 20-second period ends the
experiment as incomplete. After READY there is no reconnect or RPC retry. Before
the first READY, the standard CXET connector may try different DNS addresses
following an initial connection failure. The artifact records physical age as
unknown rather than inferring it from READY. The startup-only
`--overall-timeout-seconds` bounds connection and tariff preparation; each RPC
uses `--timeout-ms` in the range 100–5000. A GOAWAY, limit response, malformed or unexpected response,
transport failure, deadline, peer mismatch or artifact limit ends the experiment
as incomplete. Artifact output is bounded to 4 MiB and does not contain raw
private responses or credentials.

The published tariff does not reveal current shared usage or remaining quota.
An operator must separately authorize accepting that unknown for this diagnostic
experiment; it is not authorization for production keepalive or a guarantee of
headroom. Build, test and live execution require separate selection.

`research tbank-ping-handoff` is a separate diagnostic with no credentials or
RPCs. It opens CXET and nghttp2 TLS connections to one numeric peer, starting
the standby 10 seconds after primary READY. Both use a diagnostic 10-second
idle PING interval. If primary sends a NO_ERROR GOAWAY with empty debug data within 60 seconds, the
standby can queue at most one extra PING when no other PING is outstanding.
The ACK must arrive strictly before the two-second deadline. It records
queueing, handoff to TLS and an ACK matching the exact nonce as
separate facts, then observes the standby for five seconds. Any unavailable
standby, wrong or missing ACK, other error, GOAWAY or deadline gives an
inconclusive result. No reconnect after READY or RPC retry occurs; normal
initial CXET DNS connection attempts may use different addresses before READY.
The output is bounded and contains no tokens or raw GOAWAY debug data. This
experiment does not establish production PING safety; running it requires
separate selection.

Each invocation creates a new private run directory with a bounded JSONL event
file. GOAWAY error and last-stream fields, received PING ACKs, locally reported
errors and HTTP/gRPC statuses are distinct. A received CXET PING ACK does not
expose whether it matched the client's pending nonce; the corresponding report
field is `null`. Times are measured from each client's ready event, not from
the unobserved opening of the physical socket. These observations do not by
themselves establish why the peer closed the connection.

A bounded Python public-only capture helper and its unexecuted command recipes
are documented in [scripts/README.public_capture.md](scripts/README.public_capture.md).
Its frozen diagnostic routes are independent of native profile selection.

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
switch it. Project-owned libraries remain static `.a`; `p` selects automatic
memory-aware parallelism, using available processors as an upper bound and
reserving 2 GiB per compilation job below `MemoryHigh`, with at least one job.
An explicit positive `-j N` overrides this selection. See `--help` for supported
options.

Old local test and benchmark targets have been retired from the source graph.

## Continuous integration

Retired feed-comparison targets are absent. The current owner offline suite
checks protocol contracts and the cleanup regressions without network probes. CI, compiler and runtime results are separate evidence;
this source migration does not establish a passing CI revision.

## Profiles

Profiles are strict JSON documents under `profiles/`, described by
`schemas/profile.schema.json`. The built-in catalog contains 26
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

Analyze an existing neutral bundle without network access:

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
Profile schema records adapter identities and the language-neutral protocol,
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
Observation JSON and relation/findings outputs use their current shapes and
unversioned schema identifiers. Raw receive/exchange timestamps, normalized
identity, sequence/book validity and bounded-corpus completeness remain.

## Research bundles

Research capture bundles use `exchange.api_probe.bundle`. Only the current
bundle shape is supported; no migration or compatibility aliases are provided.

## T-Bank market source research

`research tbank-market-sources` observes one explicit instrument UID through
nghttp2. It first uses one connection for three sequential public unary RPCs:
`GetInstrumentBy`, `TradingSchedules`, and `GetTradingStatuses`. The returned
UID must match; the calendar section comes from `instrument.exchange`, not MIC
or `class_code`. The bank may return the same section in uppercase in the
calendar and lowercase in the card. Matching permits only ASCII case differences;
output retains both returned `exchange` and original `requested_exchange`.
A unary result needs HTTP 200, explicit gRPC status 0, one valid
body and matching RPC/stream identity. The next RPC starts after `poll` returns.

After closing the reference connection, the command opens two fresh market
connections. Each requests both depth-50 order books and trades in one
`MarketDataServerSideStream`: EXCHANGE (1) on one connection, ALL (3) on the other.
One owner loop services both, recording separate ACK/data results for all four
logical groups. Queue, ACK and first-data times distinguish setup from the common
observation interval. Received sources remain observations: a dealer trade (2)
under ALL is allowed, and missing/unknown sources never become the configured
request by inference.

`--schedule-days` selects 1–7 days (default 7), measured from a single UTC run
start. Output preserves native logical dates and interval timestamps, renders
UTC and Moscow time, and retains dealer/unknown interval types. A false
`is_trading_day` does not erase dealer intervals. Calendar gaps and future
calendar entries are not evidence of trading permission or future market data.

Contract and semantics sources are the bank's official portal:

- https://developer.tbank.ru/invest/services/quotes/marketdata
- https://developer.tbank.ru/invest/services/instruments/methods
- https://developer.tbank.ru/invest/intro/useful-info/weekend-trading/extended
- https://developer.tbank.ru/invest/intro/developer/protocols/grpc

The gRPC page links current protobuf contracts on the bank's own
`opensource.tbank.ru/invest/invest-contracts` site. Do not substitute an SDK
mirror or archived GitHub schema for these contracts.

Build through the canonical root wrapper (p uses memory-bounded parallelism):

```sh
CXET_BUILD_DIR=build/probes/tbank-market-sources ./tools/exchange_api_probe/compile.sh portable tbank p --memory-limit-gib 8
```

Example separately authorized live invocation for GAZP/TQBR:

```sh
# Bounds the process even if operating-system DNS cancellation stalls.
timeout --kill-after=5s 125s build/probes/tbank-market-sources/tools/exchange_api_probe/exchange-api-probe research tbank-market-sources \
  --engines nghttp2 --env-file apps/hft-parser/.env \
  --ca-file server/certs/tbank-ca.pem --api-slot 1 \
  --symbol 962e2a95-02a9-4171-abd7-aa198dbe643a --schedule-days 7 \
  --duration-seconds 60 --overall-timeout-seconds 120 --timeout-ms 5000 \
  --attempts 1 --max-artifact-bytes 4194304 \
  --output-dir results/tbank-market-sources --no-open
```

The host is resolved once and the first numeric address is pinned, retaining the
original hostname for TLS SNI/HTTP2 authority and requiring the explicit CA file.
There are at most three TCP attempts and five RPCs, with at most two connections
active together. Duration is 1–60 seconds (default 60), the overall budget is at
most 120 seconds, and connect/unary/resolution deadlines are at most 5000 ms.
There is no retry, reconnect, endpoint fallback, account, order or tariff RPC.
A shortened observation interval or failed reference/transport stage is
inconclusive. A rejected subscription does not stop the other logical groups.

A new private run directory (0700) contains `public-frames.bin`,
`public-frames.index.jsonl` and `events.jsonl` (0600). Raw files retain sampled
complete public request/response protobuf bodies without gRPC framing; the index
attributes direction, RPC, connection/source, channel and stream ID. Responses
are saved within the borrowed callback lifetime before payload semantics are
decoded. Oversized bodies refused by transport before the callback are not saved.
Readable observations include book levels/consistency and trade direction,
quantity and timestamps. Quotations preserve signed `units`/`nano` exactly.
Authorization metadata, HTTP headers, bearer tokens, environment contents,
private bodies, grpc-message/GOAWAY debug text, PCAP and TLS keys are excluded.

Sampling is bounded to 32 bodies/384 KiB per logical market group and 64 KiB per
body. All requests, reference responses, raw data, indexes and decoded JSON share
4 MiB, with 64 KiB reserved for summaries. Sampling exhaustion keeps only bounded
counters; artifact write failure is irreversible and stops the run. Filesystem
write rollback prevents an index from claiming a partially written raw body.

Exit 0 means the bounded observation completed, including a recognized rejection
or `accepted_no_data`; exit 1 is inconclusive; exit 2 is invalid configuration or
artifact preparation failure. Four accepted ACKs and usable real samples in every
group are needed to substantiate a complete four-way comparison. No-data results
finish the bounded observation without proving that channel's data format. The
command does not merge books, replay the production parser, establish fill or
trader readiness, or change production's EXCHANGE-only subscription policy.

The same command accepts a futures UID. The 2026-11-05 GZZ6/SPBFUT observation
used `--symbol 2e2d733d-ed60-4fca-82b8-e8bfab1ffe29`; its calendar section was
`forts_futures_weekend` in the card and `FORTS_FUTURES_WEEKEND` in the response.

ALL is a subscription selection, not an inferred label for subsequent data.
In the 2026-11-05 GAZP and GZZ6 exchange-session observations, ALL ACKs returned
source 3 while all received book/trade data reported EXCHANGE source 1. Retained
messages from the two selections overlapped. Dealer data and any combined-book
semantics still need observation during an actual dealer session; accepted ALL
ACKs alone do not prove either.

### Bounded native reference HTTP frequency diagnostic

`research reference-http-frequency` uses a separate instance of Parser COMMON
`QuoteSource`, with the registered MOCK (source 22) request builder and parser.
It supports the exact `ANTHROPIC` reference symbol. A real provider, including
Nasdaq, requires a separately selected connector; this command does not create
one. The standalone tool refuses this command when the root Parser target is
unavailable.

Example for a future explicitly selected network run (not a build/test command):

```sh
exchange-api-probe research reference-http-frequency \
  --endpoint-env MOCK_REFERENCE_URL --token-env MOCK_REFERENCE_TOKEN \
  --ca-file /path/to/trusted-ca.pem --symbol ANTHROPIC \
  --output-dir /path/to/owned-results --no-open
```

Set the two named environment values through the existing secret provisioning
flow. The command reads only those selected values and never emits their names,
values, request headers, response bodies or environment dumps. TLS trust is
explicit, with no insecure fallback. It creates a private run directory and a
mode-0600 `events.jsonl` under the selected output root; it never opens a browser.

The default ladder is 1000, 500, 250, 100 and 50 ms, each for 60 seconds. Bounds
are one symbol, one connection, one request in flight, 330 seconds overall,
2230 request attempts and 4 MiB of artifacts including reserved summary space.
`--periods-ms` selects at most five strictly decreasing periods within 50–1000 ms;
`--duration-seconds` lowers each stage to 1–60 seconds. The request ceiling is
recomputed as the sum of `ceil(stage_ms / period_ms) + 2` for each selected stage;
`--max-requests` can only lower it. `--overall-timeout-seconds` can only lower the
330-second deadline, `--timeout-ms` is 100–5000 ms, and
`--max-artifact-bytes` is 65536–4194304. Unsupported concurrency, provider,
retry, raw capture and browser flags are refused. Requests are scheduled by the
native source after completion, with no catch-up bursts. Period changes preserve
the session; actual connection attempts remain visible in the report.

A rejected HTTP status (including 401, 403 and 429), invalid native quote,
transport failure or exhausted resource bound stops the ladder before another
request. There is no quota-bypass retry. Reports separate nominal request rate,
achieved rate, matching confirmations and distinct sample updates, and include
full-response latency p50/p95/p99. The send timestamp marks native request
transfer initiation, not a measured wire timestamp. Source age stays unknown
without a proved clock mapping. Reports project Retry-After and cache Age as
bounded numeric seconds when valid, plus header presence. HTTP-date Retry-After
and duplicate or invalid numeric values stay unknown; raw header values are
never retained. Cache-Control presence is recorded. A request interrupted at
the final boundary remains an incomplete tail. An interrupted or
failed range is incomplete; a completed range only describes the observations
and never establishes a provider's maximum allowed request rate.
