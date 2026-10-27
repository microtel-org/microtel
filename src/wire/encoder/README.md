# `src/wire/encoder/`

## What lives here

`OtlpEncoder` ([`otlp_encoder.hpp`](otlp_encoder.hpp)), which turns trace,
metric and log batches into OTLP protobuf bytes with upb, and the `UpbArena`
wrapper it uses ([`upb_arena.hpp`](upb_arena.hpp)). Built as
`microtel_encoder`.

`OtlpTraceDecoder` ([`otlp_trace_decoder.hpp`](otlp_trace_decoder.hpp)), the
upb decoder behind the concentrator's leaf receiver
([`leaf-concentrator-design.md`](../../../docs/leaf-concentrator-design.md)
§3.4; ICP 0031 Decision 5 makes this directory the upb codec in both
directions). Compiled only with `MICROTEL_WITH_CONCENTRATOR=ON`. And
`ConcatenateTraceRequests` ([`trace_request_concat.hpp`](trace_request_concat.hpp)),
which joins encoded trace requests into one (§3.6.1); byte-level, no upb.

**This is the only production directory that includes upb headers or
references upb symbols** (LOCKED — `memory-model.md` §3.1, ICP 0001).
No upb type appears in any header outside this directory; the `OtlpEncoder`
C++ wrapper is the contract surface. Tests may still include upb to decode
what the encoder produced.

## Owner

Track F — OTLP encoder, a foundational track that landed before tracks A, B,
C and E could unblock (`docs/development.md` §2).

## What it implements

- `internal::IOtlpEncoder`, `internal::IMetricEncoder` and
  `internal::ILogEncoder`, all on the one `OtlpEncoder` class (declared in
  [`otlp_encoder.hpp`](../../../include/microtel/internal/otlp_encoder.hpp),
  [`metric_encoder.hpp`](../../../include/microtel/internal/metric_encoder.hpp)
  and [`log_encoder.hpp`](../../../include/microtel/internal/log_encoder.hpp)
  under `include/microtel/internal/`). `SdkBuilder` creates one encoder per
  `Provider` and shares it across the three exporters.
- Production of the `EncodedPayload` value type (declared in
  [`include/microtel/internal/encoded_payload.hpp`](../../../include/microtel/internal/encoded_payload.hpp)).
- `internal::IOtlpTraceDecoder`, as `OtlpTraceDecoder`. Its arena is its own
  RAII type over a counting `upb_alloc` that caps what one decode may use.
- The per-call `UpbArena` RAII wrapper. It lives here rather than in
  `src/common/raii/` because nothing outside `src/wire/encoder/` may
  reference it.

## Dependencies

- upb, vendored at [`third_party/upb/`](../../../third_party/upb/) with the
  pinned commit recorded in its README. Its globals carry a `microtel_` prefix via the
  force-included `third_party/upb/microtel_upb_rename.h`
  ([ICP 0020](../../../docs/icps/0020-install-and-package-config.md)
  Decision 4).
- The generated upb C accessors in [`gen/`](../../../gen/) (target
  `microtel_upb_gen`). They are committed, and CI checks that regeneration
  produces zero diff (`regen-check`, `docs/ci-architecture.md`).
- `Resource`, `BatchHandle`, `SpanRecord` and the metric and log batch types:
  plain C++ values, so no upb leaks across the boundary.
- `microtel_api` (linked PRIVATE), for `TraceState::ToHeader()`, which
  writes span and link `trace_state`.

## Tests

- `tests/unit/wire/otlp_encoder_test.cpp`: every span shape (status codes,
  attribute types, events, links), decoded back and compared field by field.
- `tests/unit/wire/otlp_metric_encoder_test.cpp` and
  `otlp_log_encoder_test.cpp`: the same for metrics and logs.
- `tests/unit/wire/upb_arena_test.cpp`: arena lifetime.
- `tests/unit/wire/otlp_trace_decoder_test.cpp`: the decoder, round-tripped
  against the encoder and fed shapes the encoder cannot produce;
  `tests/fuzz/otlp_trace_decoder_fuzz.cpp` fuzzes it.
- `tests/unit/wire/trace_request_concat_test.cpp`: concatenated requests decode
  to the union of their ResourceSpans.
- [`tests/wire/README.md`](../../../tests/wire/README.md) maps the byte-level
  coverage themes to these tests; there are no fixture files.
- The CI job `regen-check` runs `ci/scripts/regen-protos.sh` against the pinned
  upb and `opentelemetry-proto` versions and fails on any diff under `gen/`
  (`docs/ci-architecture.md`, `regen-check`).

## Style notes

- **upb arena lifetime is per-`Encode()` call** (LOCKED), and per-`Decode()`
  call for the decoder (`memory-model.md` §3.1). The arena is
  constructed at entry and destroyed before `Encode` returns. No arena
  outlives a single call, and no upb pointer survives it.
- **`EncodedPayload` is a `std::unique_ptr<std::byte[]>` + `size_t`
  pair** (LOCKED — `memory-model.md` §3.2, ICP 0003). The encoder
  allocates a fresh byte buffer on each call and transfers ownership; the
  arena is destroyed independently.
- **Containment rule.** The build enforces it: only `microtel_encoder` links
  `microtel_upb_gen` and gets the `gen/` and `third_party/upb/` include paths,
  all `PRIVATE`, so an upb `#include` in any other `src/` target fails to
  compile. `ci/scripts/symbol-scan.sh` then checks that no unprefixed upb
  symbol ships.
- **Stateless across calls.** Each `Encode` is a pure function of its input
  and keeps everything in its own arena. `SdkBuilder` hands the same encoder
  to the trace, metric and log exporters, whose workers run concurrently, even
  though the header's `@threadsafety` tag still says single-caller.
