# `src/wire/http/`

## What lives here

The OTLP/HTTP-protobuf implementation of `IWireCodec`, `HttpWireCodec`
([`http_wire_codec.hpp`](http_wire_codec.hpp)). It builds the HTTP/2 request
headers, parses `Retry-After`, interprets status codes per the matrix in
`error-model.md` §7.1, and captures the response body for diagnostics. Built
as `microtel_http_wire`.

The M1 spike validated it end to end against
`otel/opentelemetry-collector:0.151.0`, and that work produced no ICPs.

## Owner

Track B — OTLP/HTTP wire codec.

## What it implements

- The HTTP implementation of `internal::IWireCodec` (declared in
  [`include/microtel/internal/wire_codec.hpp`](../../../include/microtel/internal/wire_codec.hpp)),
  including an overridden `SendAll` that submits every pending request before
  waiting on any response
  ([ICP 0007](../../../docs/icps/0007-wire-codec-send-all.md)).
- One codec per signal. `SdkBuilder` builds three over the shared transport;
  the metrics and logs codecs get `signal_path` set to `/v1/metrics` and
  `/v1/logs`.

## Dependencies and test doubles

- `EncodedPayload` (Track F; input only, bytes plus size)
- `ITransport` (Track D): `fake_transport.hpp` in
  [`tests/fakes/`](../../../tests/fakes/)
- `IAuthProvider` (Track E): `fake_auth_provider.hpp` in
  [`tests/fakes/`](../../../tests/fakes/)
- `IDiagnosticsSink` and the optional `ISteadyClock`:
  `fake_diagnostics_sink.hpp` and `fake_steady_clock.hpp` in
  [`tests/fakes/`](../../../tests/fakes/)
- The shared helpers one level up in [`src/wire/`](../): `gzip.hpp`,
  `otlp_response.hpp` (partial-success parsing), `auth_failure.hpp` and
  `header_validation.hpp` (CR/LF/NUL in any request field, checked last in
  `BuildHeaders`; ICP 0042 §3).

## Tests

- `tests/unit/wire/http/http_wire_codec_test.cpp`: every status-code row from
  `error-model.md` §7.1.
- [`tests/wire/README.md`](../../../tests/wire/README.md) maps the byte-level
  themes (partial success, gzip) to their tests under `tests/unit/wire/`; the
  responses are built in the tests rather than loaded from fixture files.
- `tests/conformance/http/`: end-to-end against a real collector
  (shared with Track C's gRPC).

## Style notes

- **Codec owns retry classification** (LOCKED — `interfaces.md` §4.3).
  The exporter respects `WireResult.retryable` and `retry_after` as
  returned; HTTP-specific knowledge (the `Retry-After` header) stays
  inside this directory.
- **Single-caller.** Only its exporter worker calls `Send`, and concurrent
  calls on one codec are a contract violation (LOCKED — `interfaces.md`
  §4.3). The transport underneath is shared and does accept concurrent `Send`.
- **Endpoint path semantics** per `docs/configuration.md` §3.3: an empty
  path or `/` becomes `/v1/traces`, and any other path is treated as a base
  with `/v1/traces` appended. A non-empty `signal_path` replaces all of that
  and is used as the `:path` verbatim.
- **Compression:** `compression_gzip` gzips the request body and sets
  `content-encoding: gzip` (`docs/configuration.md` §3.3). It is off by default for the low-CPU
  profile. Response inflation is independent: `accept-encoding: gzip` is
  always sent, and inflation stops at `max_decompressed_bytes`.
- **Response handling:** capture the body for diagnostics. It arrives already
  bounded, because the transport stops at `max_response_bytes`. `415` and
  `404` are non-retryable; `429`/`502`/`503`/`504` are retryable and respect
  `Retry-After`.
