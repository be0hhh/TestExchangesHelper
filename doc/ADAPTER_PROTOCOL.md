# Adapter protocol v1

Exchange adapters are optional offline decoders. They may be implemented in
any language and are never loaded into the collector process.

An adapter runner must start an adapter directly, without a shell. Standard
input and standard output are JSONL. Every line uses schema
`exchange.api_probe.adapter.v1`.

Input `frame` records contain immutable frame metadata and either
`payload_text` for UTF-8 application data or `payload_base64` for arbitrary
bytes. Output `event` records reference the originating `frame_id` and contain
normalized integer/string fields. Unknown exchange timestamps and identities
must be `null`; an adapter must not substitute local receive time.

Adapters must be bounded by wall timeout, output bytes and output record count.
Malformed output fails the channel closed. The current native analyzer records
the declared adapter identity and reports `binary_adapter_required`; it does
not execute adapter processes yet. Consequently, `--allow-adapter` and
`--allow-private-adapter` reserve the trust decision but do not enable
execution in profile schema v1.

The adapter contract does not provide a security sandbox. Only trusted local
executables should be allowed.
