# `src/wire/grpc/`

## What lives here

The OTLP/gRPC implementation of `IWireCodec`, `GrpcWireCodec`
([`grpc_wire_codec.hpp`](grpc_wire_codec.hpp)), built directly on the nghttp2
transport with no gRPC library. It owns the 5-byte length-prefix framing,
trailer parsing, status interpretation including `google.rpc.RetryInfo`
decoding for `RESOURCE_EXHAUSTED`, and the trailer-only and split-frame edge
cases from `docs/grpc-wire-protocol.md`. [`grpc_status.hpp`](grpc_status.hpp)
holds the status-code table and the `grpc-message` percent-decoder. Built as
`microtel_grpc_wire`.

This directory carries the project's central claim: unary gRPC works on top
of nghttp2 and OpenSSL with only the wire-level code here. The M1 spike
validated it end to end with all three variants (happy / trailer_only /
split_frame) against `otel/opentelemetry-collector:0.151.0`.

## Owner

Track C — OTLP/gRPC wire codec.

## What it implements

- The gRPC implementation of `internal::IWireCodec` (declared in
  [`include/microtel/internal/wire_codec.hpp`](../../../include/microtel/internal/wire_codec.hpp)).
  It does not override `SendAll`
  ([ICP 0007](../../../docs/icps/0007-wire-codec-send-all.md)), so batches go
  through the interface's default loop over `Send`.
- The codec state machine from `docs/grpc-wire-protocol.md` §3.
- Decoding of `grpc-status-details-bin`: base64, then a hand-written walk of
  `google.rpc.Status.details[]` and its `google.protobuf.Any` entries to find
  a `RetryInfo` delay (`TryDecodeRetryDelay` in
  [`grpc_wire_codec.cpp`](grpc_wire_codec.cpp)). It uses no generated code and
  no upb; there are no `google.rpc` protos under `proto/` or `gen/`.
- One codec per signal. `SdkBuilder` builds three over the shared transport,
  with `service_path` set to the traces, metrics or logs `Export` method.

## Dependencies and test doubles

- `EncodedPayload` (Track F)
- `ITransport` (Track D): `fake_transport.hpp` in
  [`tests/fakes/`](../../../tests/fakes/)
- `IAuthProvider` (Track E): `fake_auth_provider.hpp`
- `IDiagnosticsSink`: `fake_diagnostics_sink.hpp`
- The shared helpers one level up in [`src/wire/`](../): `gzip.hpp`
  (request compression and bounded response inflation), `otlp_response.hpp`
  (partial-success parsing), `auth_failure.hpp` and `header_validation.hpp`
  (CR/LF/NUL in any request field, checked last in `BuildHeaders`; ICP 0042
  §3).

## Tests

- `tests/unit/wire/grpc/`: `grpc_wire_codec_test.cpp` covers the codec state
  machine and every status-code row from `error-model.md` §7.2;
  `grpc_status_test.cpp` covers the status table and message decoding.
- [`tests/grpc-wire/README.md`](../../../tests/grpc-wire/README.md) holds the
  corpus from `docs/grpc-wire-protocol.md` §7.2, as a table mapping each
  required entry to the test that covers it. The framing entries (GOAWAY,
  RST_STREAM, a message split across DATA frames) are not reachable through
  `FakeTransport`, because they happen below `ITransport`. They live in
  [`tests/integration/transport/http2_send_test.cpp`](../../../tests/integration/transport/http2_send_test.cpp)
  against a real nghttp2 peer.
- `tests/fuzz/grpc_codec_fuzz.cpp`: a libFuzzer harness over the
  response-parser entry point, run by the fuzz CI (`tests/fuzz/README.md`).
- `tests/conformance/grpc/`: end-to-end against a real collector.

## Style notes

- The transport tells initial HEADERS from trailer HEADERS by
  `frame->headers.cat` (`NGHTTP2_HCAT_RESPONSE` vs `NGHTTP2_HCAT_HEADERS`) in
  its on-header callback ([`src/transport/http2_transport.cpp`](../../transport/http2_transport.cpp)).
  The codec then reads trailers from `TransportResult::response_trailers`. A
  trailer-only response is recognised by `END_STREAM=1` on the first HEADERS
  frame combined with `grpc-status` present (`docs/grpc-wire-protocol.md`
  §2.5, validated in the M1 spike), so the codec also looks for `grpc-status`
  among the response headers.
- **`RESOURCE_EXHAUSTED` without `RetryInfo` is non-retryable** (LOCKED —
  `error-model.md` §7.2). The codec parses
  `grpc-status-details-bin` as `google.rpc.Status` and walks `details[]`
  for `RetryInfo`; if none is found, `retryable=false`.
- **Single-caller**, the same contract as the HTTP codec. The transport
  underneath accepts concurrent `Send`, but each codec is driven by exactly one
  exporter worker.
- **5-byte prefix is big-endian length** (LOCKED — gRPC spec). Use
  explicit byte ops, not host-byte-order assumptions.
- **The parser does not assume one DATA frame == one gRPC message.**
  Split-prefix and split-body across frames are tested explicitly, and the M1
  spike confirmed this works against real collectors.
