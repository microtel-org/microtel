# Interop Matrix

## 1. Purpose

The pinned-version registry behind the conformance gates
([`ci-architecture.md`](ci-architecture.md), `conformance`). Those gates say
export "passes integration tests against the pinned OpenTelemetry Collector
matrix" — this file is what "pinned" means, in one place, so a version bump is a
reviewable diff rather than an archaeology exercise across three trees.

Two readers:

- **The per-PR `conformance` job**, which runs
  [`ci/scripts/conformance.sh`](../ci/scripts/conformance.sh) against the
  collector pinned below.
- **The weekly [`interop.yml`](../.github/workflows/interop.yml) workflow**,
  which exercises delivery against collector and Jaeger.

Nothing reads this file mechanically. It records what the pins *are*; the pins
themselves live in the files named in the table.

---

## 2. Pins

| Component | Version | Pinned where | Exercised by |
|---|---|---|---|
| `opentelemetry-proto` | `v1.10.0` | vendored; [`proto/README.md`](../proto/README.md) | wire encoder, `regen-check` |
| `protoc` / upb | `v29.4` | [`gen/`](../gen/) + `regen-check` in [`ci.yml`](../.github/workflows/ci.yml) | generated accessors, zero-diff regen gate |
| `otel/opentelemetry-collector-contrib` | `0.160.0` | [`ci/scripts/conformance.sh`](../ci/scripts/conformance.sh), [`bench/sink/collector/Dockerfile`](../bench/sink/collector/Dockerfile), [`bench/versions.lock`](../bench/versions.lock), [`examples/stack/compose.yaml`](../examples/stack/compose.yaml) | `conformance` job, bench collector sink, examples stack |
| `jaegertracing/jaeger` (v2) | `2.22.0` | [`bench/scripts/jaeger_interop.py`](../bench/scripts/jaeger_interop.py), [`interop.yml`](../.github/workflows/interop.yml); config [`bench/sink/jaeger/config.yaml`](../bench/sink/jaeger/config.yaml) | weekly interop only |

Collector image digest (multi-arch index) for `0.160.0`:

```
sha256:799dc6cf12c96192af37b5bdba804da8c10b3bc563b43cb90c3f3c58d9572ad6
```

The script pins by **tag**, not digest: a tag is readable in a diff and the
digest above is the audit trail if a tag is ever re-pushed. Verify with
`podman inspect --format '{{json .RepoDigests}}' otel/opentelemetry-collector-contrib:0.160.0`.

The examples stack reuses this tag deliberately, so a reader running an example
exercises the receiver the conformance gate is validated against. Its other two
images — `grafana/tempo` and `grafana/grafana` — are pinned in the same
`compose.yaml` but are **not** interop targets and are not listed above: nothing
in microtel talks to them, and no gate depends on their behaviour. They are
recorded in [`examples/stack/README.md`](../examples/stack/README.md).

### Jaeger

The interop job runs Jaeger **v2** (`jaegertracing/jaeger:2.22.0`), pinned by
tag. Jaeger v1 (`jaegertracing/all-in-one`) reached end of life on
2025-12-31. v2 is built on the OpenTelemetry Collector and is configured by a
collector-style YAML file: OTLP gRPC on 4317, OTLP/HTTP over TLS on 4318 (see
§4), in-memory storage, and the query UI/API on 16686. v2 serves only the v3
query API (`/api/v3/services`, `/api/v3/traces`); the legacy `/api/services`
returns 404.

---

## 3. Coverage split

The two suites answer different questions and are deliberately not merged.

**Per-PR conformance** — [`ci/scripts/conformance.sh`](../ci/scripts/conformance.sh),
`tests/conformance/`. Correctness against a real receiver: does the collector
accept what microtel emits, and is what it decodes what microtel meant?

| Area | OTLP/HTTP | OTLP/gRPC |
|---|---|---|
| Acceptance (connect, export, `ForceFlush`) | ✅ | ✅ |
| Content round-trip (ids, attribute types, events, status) | ✅ | ✅ |
| Multi-batch delivery, exactly-once, large multi-DATA-frame bodies | ✅ | ✅ |
| gzip request compression | ✅ | ✅ (per-message, `grpc-encoding`) |
| TLS, mTLS, CA pinning, SNI override | ✅ | ✅ |
| Bearer-token auth (static header, callback, wrong-credential rejection) | ✅ (401) | ✅ (`grpc-status: 16`) |
| Plaintext transport | ✗ — pinned as a negative test, issue #166 | ✅ — h2c, the counterpoint |
| Logs round-trip (every severity, scalar and array bodies, attributes, resource, scope, both timestamps) | ✅ (`/v1/logs`, TLS) | ✅ (`LogsService/Export`) |
| Logs trace correlation (in-span record carries the span's ids; out-of-span record carries none) | ✅ | ✅ |
| Logs with gzip request compression | ✅ | ✅ |
| Metrics round-trip (Counter, UpDownCounter, Gauge, explicit and exponential Histogram, the three observable kinds; data points, attributes, resource, scope) | ✅ (`/v1/metrics`, TLS) | ✅ (`MetricsService/Export`) |
| Metrics temporality (cumulative and delta) and data-point timestamps (`0 < start ≤ time`; delta windows chain, ICP 0039) | ✅ | ✅ |
| Metrics with gzip request compression | ✅ | ✅ |

The two suites are deliberate per-file duplicates rather than one parameterised
harness: they diverge on which endpoint is reachable, on what a rejection looks
like on the wire, and on what the failure surface says, and a shared template
would have to be taught all three differences to hide none of them.

Three caveats behind those ticks:

- **`drop_counters` is not yet evidence.** The negative tests assert
  `batches_failed` and the collector's output file, not the `DropReason`
  counters `docs/error-model.md` §7 names, because only two of the 24 counters
  are ever incremented (issue #169). The assertion is parked as
  `DISABLED_WrongTokenIncrementsNonRetryableDropCounter` in both
  `tests/conformance/http/auth_test.cpp` and
  `tests/conformance/grpc/auth_test.cpp` until they are wired.
- **Instrumentation scope is not asserted.** `ScopeSpans.scope` currently
  carries the service name rather than the `GetTracer(name, version)` scope
  (issue #167), so no test here asserts on it — that would enshrine the bug.
  Confirmed identical on both protocols.
- **The gRPC failure surface names the status** (issue #171, fixed). A
  rejected credential reaches `HealthSnapshot::last_error_message` as
  `UNAUTHENTICATED (16): provided authorization does not match expected scheme
  or token` — the status name, its number, and the collector's own
  percent-decoded `grpc-message`. `tests/conformance/grpc/auth_test.cpp`
  asserts the status text and a fragment of the collector's sentence
  separately, so a regression says which half broke. Until this landed the
  field read `"grpc error"` for every non-zero status.

**Encoding is protocol-independent, and that was measured.** The collector's
file-exporter line for the OTLP/HTTP basic-export span and the one for its
OTLP/gRPC twin are byte-identical after normalising trace/span ids, timestamps
and the per-run marker — same key order, same typed value envelopes. The gRPC
suite therefore reuses the HTTP suite's protojson fragment constants verbatim
rather than maintaining a second set.

The logs rows are one set of scenarios run over both protocols
([`tests/conformance/support/log_conformance.hpp`](../tests/conformance/support/log_conformance.hpp)),
read back from a separate `logs.jsonl` the collector's second `file`
exporter writes, and they share one set of fragments on the same
protocol-independence argument. Logs over OTLP/HTTP inherit the traces'
plaintext gap (issue #166): the HTTP logs tests use the TLS receiver.

The metrics rows work the same way: one set of scenarios
([`tests/conformance/support/metric_conformance.hpp`](../tests/conformance/support/metric_conformance.hpp))
over both protocols, read back from `metrics.jsonl`. The cardinality-overflow
series is not yet covered (#302).

**Weekly interop** — [`interop.yml`](../.github/workflows/interop.yml).
Behaviour at volume against collector and Jaeger: delivery rate and
throughput. Slow, and not a merge gate.

---

## 4. Known gaps

**OTLP/HTTP over plaintext does not reach a stock collector.** microtel is
HTTP/2-only, so a plaintext endpoint means h2c with prior knowledge. The
collector's plaintext OTLP/HTTP receiver serves HTTP/1.1 only — it does not
wrap its handler in `h2c` — so the HTTP/2 connection preface is answered with
an HTTP/1.1 error and the SETTINGS exchange never completes. `curl
--http2-prior-knowledge` against `:4318` fails identically, which is how the
finding was confirmed against the pinned image.

Consequences:

- The HTTP conformance tests use the **TLS** receiver, where the same collector
  negotiates `h2` through ALPN.
- The gap is pinned deliberately by
  `tests/conformance/http/plaintext_gap_test.cpp`, which asserts that
  `Connect()` to the plaintext receiver **fails**. When microtel gains an
  HTTP/1.1 fallback, or the collector gains h2c, that test fails and is to be
  inverted into a positive delivery test rather than deleted.
- The collector's bearer-auth **http** receiver (`:4348`) carries server TLS
  material for the same reason, even though the point of that receiver is to
  isolate the `Authorization` header. Its **grpc** twin (`:4347`) stays
  plaintext.
- OTLP/**gRPC** over plaintext is unaffected: gRPC is h2c by definition and the
  collector's gRPC receiver speaks it. This is no longer an argument — the
  whole gRPC conformance suite except `tls_test.cpp` runs over `http://` against
  the same collector, so the two directories side by side are the measurement of
  how far the gap reaches.
- `bench/sink/blackhole` already wraps its handler in `h2c.NewHandler` for this
  reason, so the bench harness does not see the gap.
- The interop workflow (`.github/workflows/interop.yml`) runs its OTLP/HTTP
  legs against the stock collector and Jaeger over **TLS** for the same reason:
  `bench/driver/tls_material.py` generates a throwaway CA, the receivers serve
  it, and the emit-app trusts it via `OTEL_EXPORTER_OTLP_CERTIFICATE`.

Since this was found, the gap names itself rather than arriving as a generic
nghttp2 failure: `Build()` warns, `Connect()` returns an `Error::Kind::Protocol`
naming the two configurations that do work, and the codecs do not retry it.
[`compatibility-matrix.md`](compatibility-matrix.md) §4 is the operational
version — what to use instead, and what each surface says.

Whether microtel should gain an HTTP/1.1 fallback — the OTLP specification
permits HTTP/1.1 for OTLP/HTTP — is not something this file decides. It is
parked as open roadmap question 6 in
[`roadmap.md`](roadmap.md) §12, and would need an ICP.
Issue #166 tracks it.

---

## 5. Update procedure

A pin bump is one PR:

1. Bump the tag in [`ci/scripts/conformance.sh`](../ci/scripts/conformance.sh).
2. Verify the components the config depends on still exist:
   `podman run --rm otel/opentelemetry-collector-contrib:<TAG> components`
   — needs `otlp`, `batch`, `file`, `debug`, `health_check`, `bearertokenauth`.
   Receiver and extension config schemas change between releases; a rejected
   config fails at startup, which the health wait reports as a timeout.
3. Run `ci/scripts/conformance.sh <build-dir>` locally and confirm green.
4. Update this file (tag **and** digest) plus
   [`bench/sink/collector/Dockerfile`](../bench/sink/collector/Dockerfile) and
   [`bench/versions.lock`](../bench/versions.lock) in the same PR — three
   places, one commit, or they drift.

---

## 6. Deferred

| Item | Target |
|---|---|
| Metrics cardinality-overflow series against the collector (#302) | v1.3 |
| Restart-recovery scenario (collector bounced mid-export) | post-v1.0 |
