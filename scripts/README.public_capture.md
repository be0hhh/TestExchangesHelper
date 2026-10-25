# Public schema capture helper

`capture_public.py` is a cold Python diagnostic. It reads only the frozen
`profiles/public_capture.json` catalog. It does not change native profile
selection. `available` means a public route was documented, not live reachability,
successful subscription, semantic coverage or production readiness. Frozen
recipes cover Bybit public V5 spot/linear, BingX public spot/swap, and Bitget
public HTTPS recent fills (SPOT/USDT) plus current funding (USDT). Bitget WS/SBE
is not selected. Other selected owners have explicit refusal reasons; XT's
permessage-deflate requirement is unsupported. Permissioned routes never fall
back to public routes.

No command below has been executed as part of this implementation. Live commands
require separate selection. From the repository root, a proposed live command is:

```bash
python3 tools/exchange_api_probe/scripts/capture_public.py --venue bybit --product all --seconds 60 --output-name bybit_public_001
```

This opens at most two public WS sessions, subscribes to trades, L1 book and
ticker for BTCUSDT spot/linear, and sends public heartbeat messages. The total
live deadline includes DNS, TLS, HTTP/bootstrap and WS connection setup; it is
not 60 seconds per product. There are no retries or reconnects. Missing prepared
`websocket-client` 1.9.0, unsupported POSIX hard deadlines or unavailable catalog
records return `BLOCKED`/refused. No packages are installed. HTTPS uses Python
stdlib and verified TLS. Proxy inheritance and redirects are disabled for WS;
HTTPS never follows redirects. There are no arbitrary URL, request-body, symbol,
script or credential CLI arguments.

`--seconds` can only reduce the 60-second ceiling. The catalog allows at most
eight public GET requests and two WS sessions per venue invocation. There is no
batch launcher: separately selected invocations own separate limits. KuCoin
public POST-token bootstrap is currently refused. HTTP funding reads may be
frozen when their exact route is confirmed; missing funding fields/messages in
any capture stay absent. A short run cannot prove complete funding/event coverage.

Artifacts live in the already ignored directory
`tools/exchange_api_probe/results/public_capture/<output-name>/`. The output
child must be new. Existing artifacts are not overwritten. `/mnt/d/recordings`
is unavailable in this environment; these diagnostics do not replace that corpus.

The disk budget is 16 MiB including a 256 KiB final metadata reservation.
Individual application messages and gzip expansion are limited to 1 MiB;
oversized or incomplete bodies are refused before recording. WS frame lengths
are rejected before body allocation, reassembly permits at most 256 fragments,
and at most 12,000 complete records are saved. A record that exceeds the remaining
budget is omitted entirely, with refusal in the manifest. Payload bytes remain
original: compressed application bodies are retained compressed after bounded
validation, and their decoded length/hash is metadata only. Native protobuf
messages may be retained as bytes, without a decoding claim.

The bundle reuses `frames.bin` and `frames.jsonl` filenames, with a dedicated
`exchange.public_capture` manifest. Its schema is **not** the
C++ research analyzer schema and analyzer compatibility is not asserted.
Index rows retain direction, route, exact URL where applicable, opcode,
fragment/masking metadata, local UTC/monotonic receipt and recording times, byte offset/length
and SHA-256. All inbound application messages are retained without filtering,
including ACK, heartbeat and venue errors. WS control ping/pong/close frames are
recorded; outgoing subscription/heartbeat intent and returned writes are separate
records. A returned write is not an exchange ACK. The manifest contains exact
route/subscription/product provenance URLs and hashes of both data files.
Payloads exclude TLS/TCP and WS wire headers; permessage-deflate is not requested.
`received_*` timestamps are sampled immediately after a completed HTTP body or
final WS frame read, before gzip validation, payload hashing and artifact writes.
`recorded_*` timestamps describe artifact recording instead. Incomplete HTTP
Content-Length/chunked bodies are refused before any payload record is saved.
Application gzip is supported only for explicitly frozen gzip routes.
The frame-length hook is tied to the inspected upstream
[websocket-client 1.9.0 framing implementation](https://github.com/websocket-client/websocket-client/blob/v1.9.0/websocket/_abnf.py);
other versions fail closed before connecting.

The hard timer ends live I/O; bounded local socket disposal, file closing and
manifest hashing follow it. An interrupted/failed capture retains its manifest
and complete records without automatically retrying. A network refusal or expiry
does not prove a venue is unavailable. Filesystem write failure may prevent
final metadata publication and must be treated as incomplete artifact evidence.

Focused offline synthetic fixtures are in `test_capture_public.py`. They test
gzip expansion/truncation, incomplete HTTP Content-Length, whole-record budget refusal and unavailable-route
refusal, without venue data or network. A proposed separate lightweight check is:

```bash
python3 -m unittest discover -s tools/exchange_api_probe/scripts -p test_capture_public.py
```

These fixtures have not been run. They are not recorded exchange schemas.
