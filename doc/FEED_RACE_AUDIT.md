# Exchange feed race audit

Status: implementation boundary, not live evidence.

## Existing diagnostic

`exchange-api-probe` is an isolated Boost.Beast/OpenSSL research utility.  Its
current research capture creates one WebSocket session per selected channel,
but materializes the payload and enters a global bundle-writer mutex before it
records the frame timestamp.  That boundary cannot answer sub-millisecond
first-arrival questions honestly.  The existing command and bundle schemas are
therefore retained for compatibility; `exchange-feed-race` owns a new bounded
capture path.

The new root-only target may consume public CXET contracts and Trading Core
headers.  It must not change connector registration, production subscriptions,
readiness, strategies or order transport.

## Reused CXET surfaces

- `metrics::nowMonoRawNs()` / `CLOCK_MONOTONIC_RAW` for canonical application
  frame arrival and periodic monotonic-to-realtime mapping.
- `runtime::SpscRing` for one-producer/one-consumer compact-record handoff.
- `os::setCurrentThreadAffinity` and Linux CPU topology for explicit physical
  core placement.
- `parse::hotjson`, fixed-point Price/Amount primitives and the existing SBE
  cursor/header implementation.  SBE decoders must validate blockLength,
  templateId, schemaId and version.
- Existing Bitget and Gate SBE market parsers where their output contract
  covers the requested feed.
- Canonical `composite::BboReconstructionState` direct, reverse and bounded
  same-price nibbling semantics.  The probe does not maintain a parallel
  reconstruction implementation.
- Trading Core `LocalOrderBook` semantics for bounded local state.  Venue
  sequence continuity, invalidation and resynchronization remain owned by each
  research adapter.

## Timestamp decision

Canonical `recv_mono_ns` is sampled immediately after a complete WebSocket
message becomes available to the application and before payload parsing,
normalization, derived state, logging or report work.  The record also carries
the existing first-read TSC when the selected CXET transport exposes it, but
TSC is secondary and never replaces the stable nanosecond scale.

Realtime is reconstructed from periodically sampled
`CLOCK_MONOTONIC_RAW <-> CLOCK_REALTIME` mapping pairs.  Kernel receive
timestamps are diagnostics only: TCP segmentation, TLS records and WebSocket
framing do not currently provide a proven packet-to-message mapping.

Derived states inherit the parent frame/event arrival timestamp.  Separate
instrumentation measures parse/derive cost; it is not added to the network
race timestamp.

## Isolation and overload policy

Every `{venue, contract, logical feed, transport variant}` has a dedicated
TCP/TLS/WebSocket connection, receive thread, source identity and SPSC ring.
No feed multiplexing is silently introduced.  One logical CPU per physical
core is used; writer/status/time mapping reserve a physical core.  When the
receiver set is larger, deterministic race groups repeat direct-BBO, trade and
fastest-valid-depth anchors.  Exact pairwise comparison is only legal inside
one simultaneous group.

Receiver work is limited to frame timestamp, bounded decode/normalization,
compact record publication and counters.  It performs no console output,
filesystem work, JSON result formatting, global locking or reconnect sleep.
Ring overflow marks the feed and session degraded and invalidates affected
post-drop pairwise results.

## Requested venue matrix

This table is the requested experiment matrix.  `planned` does not assert that
the current public endpoint accepts the channel.  Official documentation,
changelog and a later authorized live subscription attempt determine the
terminal status.

| Venue | Planned feeds |
|---|---|
| Bitget UTA v3 | JSON books, books1, books5, books50, publicTrade; SBE books1, books50, publicTrade |
| Bybit | orderbook.1, orderbook.50, orderbook.200, orderbook.1000, publicTrade; no ticker/SBE |
| Gate USDT Futures | JSON+SBE trades, book ticker, legacy order book levels 1/5/10/20/50/100, legal order_book_update combinations and OBU 50/400 |
| OKX | bbo-tbt, books, books5, premium books50-l2-tbt/books-l2-tbt attempts, trades and documented trades-all |
| KuCoin Futures UTA | depth 1/5/50, deprecated increment attempt, increment@10ms and public trade |
| Binance USD-M | bookTicker, accepted diff/partial-depth speeds including historical 0ms attempts, aggTrade and documented raw trade; raw/SUBSCRIBE/combined anchor transport test |
| Aster Futures | bookTicker, current diff/partial-depth levels and speeds, current trade feeds; raw/SUBSCRIBE/combined anchor transport test |

The checked-in legacy profiles cover only a subset of this matrix.  Missing
profile entries are not evidence that an exchange lacks a feed; the race
catalog is a separate, bounded research catalog.

## Symbols and session policy

MAGMA, ETH and BTR are requested as base assets.  Each venue's official
instrument catalog resolves the actual USDT perpetual identifier and trading
status.  Missing, delisted, suspended, prelaunch or incompatible instruments
produce `UNAVAILABLE` with the exact response classification and do not select
a substitute.

Each available `{venue, symbol, race group}` runs three fresh sessions.  Warmup
defaults to 20 seconds and measured duration to exactly 600 seconds.  DNS,
TCP/TLS/WS and book synchronization restart each session, with deterministic
connection-order rotation.  Full research remains gated behind a successful
short validation and separate current live permission.

## Output boundary

The versioned bundle contains manifest, instrument discovery, connection and
subscription metadata, time mappings, compact records, health/gap/drop data,
raw samples, normalized matches, signed pairwise CSV, `report.md` and
question-driven SVG plots.  Raw market values remain integer-scaled.

The report preserves per-session and pooled results, exact simultaneous group
identity, unmatched sample counts and signed `recv_A - recv_B` deltas.  It does
not choose a production feed set or run a combinatorial optimizer.

## BBO reconstruction experiment boundary

`exchange-bbo-reconstruction-probe` is a separate root-only diagnostic target.
It derives eligible products and exact RuntimeV1 wire/transport/lane/parser
provenance from CXET descriptors.  Capture fails closed on missing instrument
rules when campaign discovery needs them, missing required timestamps,
unsupported routes, capacity overflow or a runtime route that differs from the
declared contract.  That comparison includes the exact parser callbacks bound
to each runtime `ObjectConfig`, not merely the descriptor's wire and transport
labels. Explicit capture may use a caller-supplied symbol and tick, but still
requires the exact declared public routes.

The binary capture is written as `.partial`, terminated by a schema/footer
record containing the observation count, closed, and atomically published with
no-replace semantics.  Offline analysis rejects partial, malformed, truncated,
reserved-field or provenance-invalid input. Writer and reader both reject more
than 10,000,000 observations (about 1.28 GB plus header/footer) before any
unbounded record growth can occur.

Analysis runs two named counterfactuals.  `strict` permits only exact exchange
timestamps and mirrors the production eligibility contract.  `diagnostic` may
fall back to monotonic receive order and exists solely to quantify snapshot
blind spots.  A raw BBO/depth observation confirms a reconstructed move only
when it occurs after the accepted trade in that counterfactual's clock domain;
older or equal observations cannot manufacture confirmation.

The default campaign is three fresh 600-second sessions over all eligible
products.  Per product it selects ETH and, only with an exact supported 24-hour
ticker route, the two highest-volume eligible USDT perpetuals.  If volume truth
is unavailable, the fallback is ETH only rather than an inferred ranking.
An otherwise eligible product without instrument-rule discovery is recorded as
an explicit product failure rather than silently skipped.
This campaign remains a live/network action and requires separate current
permission; implementation or offline tests do not constitute live evidence.
