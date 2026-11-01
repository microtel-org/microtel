# `tests/conformance/`

End-to-end tests against a real OpenTelemetry Collector: thirteen test
binaries holding 53 tests, plus the two leaf binaries (ten tests each) when
the leaf and the concentrator are built, none of them disabled. Every test skips
unless [`ci/scripts/conformance.sh`](../../ci/scripts/conformance.sh)
has started a collector and exported the environment contract below.

## What this proves

Tier 1 of the four-tier compatibility model in `docs/compatibility-matrix.md` §7:
payloads emitted by microtel are accepted by receivers implementing the
pinned OTLP specification version, over both OTLP/HTTP-protobuf and
OTLP/gRPC. Traces, logs and metrics are covered; the metrics
cardinality-overflow series is not yet (see "Deliberately excluded").

This is the gate the `conformance` CI job runs (`docs/ci-architecture.md`):

> OTLP/HTTP trace export passes integration tests against the pinned
> OpenTelemetry Collector matrix.
> OTLP/gRPC trace export passes integration tests against the pinned
> OpenTelemetry Collector matrix.

## Boundary vs. the other tiers

A real receiver is the only thing that proves acceptance. Every other
tier has microtel, or a peer microtel's authors wrote, on both sides
of the wire, so a green run there says the client agrees with itself.

- `unit/` tests one type against mocks. That includes the byte-level
  wire corpus under [`tests/unit/wire/`](../unit/wire/): what microtel
  *writes*, asserted against hand-encoded byte fixtures in the test
  sources.
- `integration/` wires real components together against fakes at the
  system boundary (sockets, the clock).
- `conformance/` sends the bytes out of the process to a receiver
  written by the OpenTelemetry and gRPC projects. The assertions read
  the collector's own output, which is downstream of its protobuf
  decode.

Consequence, enforced in [`CMakeLists.txt`](CMakeLists.txt):
`${CMAKE_SOURCE_DIR}/src` is **not** on the include path here. A test
that needs an internal header to make its point belongs in the
integration tier.

## Layout

### `http/` — OTLP/HTTP-protobuf, 7 binaries

| Binary | Source | Tests |
|---|---|---|
| `conformance_http_basic_test` | `basic_export_test.cpp` | `Connect()` preflight; one span's full round trip; `traceState` and `flags` of a span with a remote parent and of its link (#451) |
| `conformance_http_batching_test` | `batching_test.cpp` | multi-batch exactly-once, gzip, multi-DATA-frame body |
| `conformance_http_tls_test` | `tls_test.cpp` | custom CA, mTLS, SNI override, and two negative cases |
| `conformance_http_auth_test` | `auth_test.cpp` | static bearer header, auth callback, wrong-token 401 |
| `conformance_http_plaintext_http1_test` | `plaintext_http1_test.cpp` | plaintext OTLP/HTTP to the HTTP/1.1-only `:4318`: `Connect()`, a span and a log record delivered over the HTTP/1.1 fallback, and gRPC to the same port still refused (ICP 0042; was `plaintext_gap_test.cpp`, issue #166) |
| `conformance_http_logs_test` | `logs_test.cpp` | logs to `/v1/logs`: see [Logs](#logs) |
| `conformance_http_metrics_test` | `metrics_test.cpp` | metrics to `/v1/metrics`: see [Metrics](#metrics) |

### `grpc/` — OTLP/gRPC, 6 binaries

| Binary | Source | Tests |
|---|---|---|
| `conformance_grpc_basic_test` | `basic_export_test.cpp` | `Connect()` preflight; one span's full round trip; `traceState` and `flags` of a span with a remote parent and of its link (#451) |
| `conformance_grpc_batching_test` | `batching_test.cpp` | multi-RPC exactly-once, `grpc-encoding: gzip`, message spanning DATA frames |
| `conformance_grpc_tls_test` | `tls_test.cpp` | custom CA, mTLS, SNI override, and two negative cases |
| `conformance_grpc_auth_test` | `auth_test.cpp` | static bearer header, auth callback, wrong-token `grpc-status: 16` |
| `conformance_grpc_logs_test` | `logs_test.cpp` | logs to `LogsService/Export`: see [Logs](#logs) |
| `conformance_grpc_metrics_test` | `metrics_test.cpp` | metrics to `MetricsService/Export`: see [Metrics](#metrics) |

There is no gRPC mirror of `plaintext_http1_test.cpp`: the gRPC suite
runs over `http://` except for `tls_test.cpp`, and gRPC never falls back,
which `plaintext_http1_test.cpp` itself asserts against `:4318`.

One binary per theme rather than one per directory. A conformance
binary is bounded by a ctest `TIMEOUT` covering a flush plus a poll of
the collector's output file; a single binary would make that budget the
sum of every theme's worst case.

### Logs

Both `logs_test.cpp` files run the same six scenarios, defined once in
[`support/log_conformance.hpp`](support/log_conformance.hpp); each file only
decides how its `Provider` reaches the collector (HTTP over the TLS receiver,
gRPC over plaintext, exactly as the trace suites do).

| Test | What the collector must have decoded |
|---|---|
| `RecordRoundTrip` | one record, exactly once: `time` and `observed_time`, severity number and text, a string body, one attribute of each scalar type, `droppedAttributesCount`, `eventName`; `service.name` on its `ResourceLogs`, the `GetLogger(name, version)` scope on its `ScopeLogs`; no trace ids |
| `EverySeverity` | all 25 `SeverityNumber` values, 0 (the proto default, so absent) to 24 |
| `BodyTypes` | a body of each `AttributeValue` alternative: four scalars and four homogeneous arrays |
| `ObservedTimeBackfilled` | a record with neither timestamp: no `timeUnixNano`, and an `observedTimeUnixNano` inside the `Emit()` window |
| `TraceCorrelation` | a record emitted inside `StartAsCurrentSpan` carries that span's `traceId`, `spanId` and `flags: 1`; its sibling emitted after the scope closed carries none of the three |
| `GzipAccepted` | 50 records through a gzip-compressed provider, each exactly once |

A key/value-list (structured map) body is not tested because the public
`LogRecord::body` cannot hold one: `AttributeValue` has no map alternative.

### Metrics

Both `metrics_test.cpp` files run the same five scenarios, defined once in
[`support/metric_conformance.hpp`](support/metric_conformance.hpp), over HTTP
(TLS receiver) and gRPC (plaintext), read back from `metrics.jsonl`. Every
assertion is narrowed to one metric's object by its per-run unique name, and
every data point must carry `0 < startTimeUnixNano <= timeUnixNano` (a gauge:
a `timeUnixNano` and no start), per
[ICP 0039](../../docs/icps/0039-metric-start-timestamps.md).

| Test | What the collector must have decoded |
|---|---|
| `SyncInstruments` | a Counter, UpDownCounter, Gauge, explicit-bucket Histogram and exponential Histogram: each one's value, buckets, count, sum, min/max, scale and offset as applicable, cumulative temporality, `isMonotonic` on the counter only; the description and unit; `service.name` on the resource and the `GetMeter(name, version)` scope |
| `ObservableInstruments` | an ObservableCounter, ObservableUpDownCounter and ObservableGauge, each with the value its callback reported during `ForceFlush` |
| `CumulativeTemporality` | a counter exported twice reports 3 then 7, with one start time |
| `DeltaTemporality` | the same counter on a delta provider reports 3 then 4, the second window starting where the first ended |
| `GzipAccepted` | 20 counters through a gzip-compressed provider, each exactly once |

### `leaf/` — the leaf / concentrator gate, 2 binaries

ICP 0031 gates 3 and 5 ([`leaf-concentrator-design.md`](../../docs/leaf-concentrator-design.md)
§7.5): C leaves → an in-memory link → `LeafReceiver::Ingest` in a concentrator
`Provider` → the collector. One source, `leaf_e2e_test.cpp`, builds
`conformance_leaf_nanopb_test` and `conformance_leaf_upb_test`, one per leaf
encoder backend; each runs every test over OTLP/HTTP (TLS) and OTLP/gRPC.
Built only with `-DMICROTEL_BUILD_LEAF=ON -DMICROTEL_WITH_CONCENTRATOR=ON`.

| Test | What the collector must have decoded |
|---|---|
| `ManyLeavesShareOneRequest` | eight leaves' spans on **one** line, i.e. one request, with eight `ResourceSpans`; each with its transport id as `device.id`, its own Resource, no `microtel.leaf.*` key, every attribute type, the event and the status, and timestamps corrected exactly (`t + R − E`) |
| `TimeModesCorrectTimestamps` | concentrator-stamped, sync-relative and boot-relative timestamps to the nanosecond, including a low boot-relative outlier that must not move the anchor |
| `MalformedPayloadRejectedAndCounted` | a truncated payload returns `Malformed`, counts in `LeafReceiverStats` and `leaf_payload_malformed`, and never reaches the collector; the same leaf's intact payload does |
| `FullNodeReachesTheCollectorThroughTheConcentrator` | a full C++ node ([ICP 0036](../../docs/icps/0036-custom-export-transport.md)) exporting with `WithExportTransport` over a real loopback UDP socket, ingested under its source `address:port` by a concentrator that trusts it as `unix`: its span shares the line with a C leaf's, keeps its own `service.name`, carries the transport id as `device.id`, and has its timestamps unchanged (`t' = t`) |
| `FullNodeRequestOverThePayloadLimitIsDropped` | a node whose `max_request_bytes` is above a concentrator's `max_payload_bytes` sends a request between the two: `TooLarge`, one `leaf_payload_too_large`, and nothing at the collector |

These tests read their own receiver, `otlp/leaf` (below), whose pipeline has no
batch processor, so the collector's output line for a request is exactly that
request. Each binary also checks that it runs the backend it is named for:
nanopb streams a payload in many `write` calls, upb in one.

### `support/` — 6 headers, included as `conformance/support/<name>.hpp`

| Header | What |
|---|---|
| [`conformance_env.hpp`](support/conformance_env.hpp) | `GetEnv`, `UniqueMarker` (a per-run needle), and `ConformanceEnabled`, which implements the skip-or-fail contract below. |
| [`collector_output.hpp`](support/collector_output.hpp) | Reads back what the collector wrote: `PollForLineContaining`, `CountOccurrences`, and `EnclosingObject`, which narrows a line to one record (or its scope, or its resource). |
| [`provider_builder.hpp`](support/provider_builder.hpp) | `ConfigureConformanceBuilder`: endpoint, protocol, and timeouts short enough to fail fast instead of turning into a ctest timeout. |
| [`log_conformance.hpp`](support/log_conformance.hpp) | The logs scenarios and their expected protojson fragments, shared by both protocols. |
| [`metric_conformance.hpp`](support/metric_conformance.hpp) | The metrics scenarios, their expected fragments and the timestamp checks, shared by both protocols. |
| [`trace_context_conformance.hpp`](support/trace_context_conformance.hpp) | The span and link `traceState` / `flags` scenario, shared by both `basic_export_test.cpp` files. |

### `collector/config.yaml`

Four named `otlp` receivers, each on its own port pair, so a test picks
a transport-security posture by picking an endpoint rather than by
restarting the collector. All four feed one traces pipeline (the
`batch` processor, then the `file` exporter), one logs pipeline (the
same `batch` processor, then a `file/logs` exporter) and one metrics
pipeline (the same, then a `file/metrics` exporter), so every test
asserts against one output file per signal whichever port it used.
Keeping the files apart means an assertion about one signal can never be
satisfied by another's line.

A fifth receiver, `otlp/leaf`, belongs to the leaf gate alone. Its traces
pipeline has no processor and its own `file/leaf` exporter, so the collector
writes exactly one line per request it receives: the only way a test can
see how many requests microtel sent, which the `batch` processor would
otherwise hide by merging them.

| Receiver | gRPC | HTTP | Posture |
|---|---|---|---|
| `otlp/plain` | 4317 | 4318 | no TLS |
| `otlp/tls` | 4327 | 4328 | server cert |
| `otlp/mtls` | 4337 | 4338 | server cert + client CA |
| `otlp/auth` | 4347 | 4348 | `bearertokenauth` extension |
| `otlp/leaf` | 4357 | 4358 | HTTP with the `otlp/tls` server cert; its own traces pipeline with **no processor**, into `/out/leaf-traces.jsonl` |

Also exposed: the `health_check` extension on 13133 (the runner's
readiness signal) and Prometheus telemetry on 8888 (dumped as a
diagnostic when the gate fails).

**The auth receiver is asymmetric on purpose.** Its gRPC port (4347) is
plaintext, which is the original intent: no TLS means an auth failure
cannot possibly be a trust failure. Its HTTP port (4348) carries the
same server certificate as `otlp/tls`, because until v1.2.6 microtel
could not reach a plaintext collector HTTP receiver at all: that
receiver is HTTP/1.1-only, and microtel had no HTTP/1.1 (issue #166,
[`docs/interop-matrix.md`](../../docs/interop-matrix.md) §4). The HTTP
auth tests buy the isolation back by pinning the correct CA in every
test, so the negative test differs from the positive ones in exactly
one field: the token.

The same history is why every OTLP/HTTP test here except
`plaintext_http1_test.cpp` points at a TLS endpoint. Those endpoints
were not chosen to test TLS; TLS is where ALPN negotiates `h2`. Plaintext
now reaches the stock collector over HTTP/1.1, serially, and
`plaintext_http1_test.cpp` covers that path.

## Running it locally

```bash
cmake -S . -B build -DMICROTEL_BUILD_TESTS=ON
cmake --build build
ci/scripts/conformance.sh build
```

The build-dir argument defaults to `build`. The script generates a
throwaway ECDSA CA, server, client and wrong-CA certificate set under
`<build-dir>/conformance/certs`, starts the pinned collector with
`<build-dir>/conformance/out` bind-mounted, waits up to 60 s on the
health endpoint, and then runs `ctest -L conformance`.

| Exit code | Meaning |
|---|---|
| 0 | every conformance test passed |
| 1 | at least one conformance test failed |
| 2 | the gate could not run — no container engine, collector never healthy, or no test carries the `conformance` label |

Engine selection: `podman` if present, else `docker`. Two overrides:

| Variable | Effect |
|---|---|
| `MICROTEL_CONTAINER_ENGINE` | force `podman` or `docker` |
| `MICROTEL_COLLECTOR_IMAGE` | override the pinned collector image |

On a failing run the script dumps the collector's logs and its
Prometheus metrics to stderr before removing the container.

**Plain `ctest` skips this tier, by design.** The binaries are
registered like any other test, so `ctest --test-dir build` runs them,
and every test skips because none of the environment variables below
is set. That is what keeps a developer box, and the `compile`,
`sanitizers` and `coverage` CI jobs, green without a collector.

## Environment contract

Exported by [`ci/scripts/conformance.sh`](../../ci/scripts/conformance.sh),
consumed through [`support/conformance_env.hpp`](support/conformance_env.hpp).

| Variable | Meaning |
|---|---|
| `MICROTEL_CONFORMANCE_REQUIRE` | set once the collector is healthy; turns a missing variable from a skip into a failure |
| `MICROTEL_CONFORMANCE_HTTP_ENDPOINT` | `http://127.0.0.1:4318`, plaintext, used only by the negative test |
| `MICROTEL_CONFORMANCE_GRPC_ENDPOINT` | `http://127.0.0.1:4317`, plaintext, the gRPC default |
| `MICROTEL_CONFORMANCE_HTTP_TLS_ENDPOINT` | `https://localhost:4328` |
| `MICROTEL_CONFORMANCE_GRPC_TLS_ENDPOINT` | `https://localhost:4327` |
| `MICROTEL_CONFORMANCE_HTTP_MTLS_ENDPOINT` | `https://localhost:4338` |
| `MICROTEL_CONFORMANCE_GRPC_MTLS_ENDPOINT` | `https://localhost:4337` |
| `MICROTEL_CONFORMANCE_HTTP_AUTH_ENDPOINT` | `https://localhost:4348` (`https`, because of the asymmetry above) |
| `MICROTEL_CONFORMANCE_GRPC_AUTH_ENDPOINT` | `http://127.0.0.1:4347` |
| `MICROTEL_CONFORMANCE_CA` | the run's CA, for `TlsOptions::ca_bundle` |
| `MICROTEL_CONFORMANCE_WRONG_CA` | an unrelated CA, the negative control for CA pinning |
| `MICROTEL_CONFORMANCE_CLIENT_CERT` | client certificate for the mTLS receivers |
| `MICROTEL_CONFORMANCE_CLIENT_KEY` | matching client key |
| `MICROTEL_CONFORMANCE_AUTH_TOKEN` | the bearer token `bearertokenauth` accepts |
| `MICROTEL_CONFORMANCE_OUTPUT_FILE` | `<build-dir>/conformance/out/traces.jsonl` |
| `MICROTEL_CONFORMANCE_LOGS_OUTPUT_FILE` | `<build-dir>/conformance/out/logs.jsonl` |
| `MICROTEL_CONFORMANCE_METRICS_OUTPUT_FILE` | `<build-dir>/conformance/out/metrics.jsonl` |
| `MICROTEL_CONFORMANCE_LEAF_HTTP_ENDPOINT` | `https://localhost:4358` |
| `MICROTEL_CONFORMANCE_LEAF_GRPC_ENDPOINT` | `http://127.0.0.1:4357` |
| `MICROTEL_CONFORMANCE_LEAF_OUTPUT_FILE` | `<build-dir>/conformance/out/leaf-traces.jsonl` |

### Why an all-skip run cannot masquerade as green

`ConformanceEnabled(env_name, out_value)` resolves one variable or
explains why the test cannot run:

```cpp
std::string endpoint;
if (!ConformanceEnabled(kHttpEndpointEnv, endpoint))
{
    GTEST_SKIP() << "collector not configured";
}
```

Outside the gate it returns false and the test skips. Inside the gate
`MICROTEL_CONFORMANCE_REQUIRE` is set, so a missing variable records an
`ADD_FAILURE()` first, so broken plumbing in the runner fails loudly
instead of silently skipping everything. The runner's second guard is
independent: before exporting anything it counts
`ctest -N -L conformance` and exits 2 if the answer is zero, so a
build that registered no conformance tests cannot report success
either.

## Readback strategy

The collector runs the `file` exporter, which appends one compact
protojson object per batch: `ResourceSpans` to `/out/traces.jsonl`,
`ResourceLogs` to `/out/logs.jsonl`, `ResourceMetrics` to
`/out/metrics.jsonl`. Those files are the only place a
test can observe what the collector *understood*, as opposed to what
microtel claims it sent.

- Positive assertions poll that file for a line containing the run's
  `UniqueMarker()`, and then assert substrings of that line. Polling is
  needed because delivery is asynchronous: `ForceFlush` returning only
  means microtel handed the batch over. The expected fragments are the pinned
  image's protojson rendering: hex ids, camelCase keys, typed value
  envelopes, int64 as a quoted string. Every one was read off a real
  run against the pinned image rather than derived from the proto
  definitions, which is also why a pin bump is a procedure and not a
  version edit. See
  [`docs/interop-matrix.md`](../../docs/interop-matrix.md) §2 for the
  pin and §5 for how to move it.
- Exactly-once assertions use `CountOccurrences` instead of a presence
  check, so a retry the collector accepted twice fails instead of
  passing quietly.
- Log assertions are narrowed to one record. The collector's `batch`
  processor may merge several exports into one line, so "the line
  contains a `traceId`" would not prove *this* record has one — and the
  correlation test has to prove one record lacks what its neighbour in
  the same line has. Every log record carries a per-run unique
  `eventName`, and `EnclosingObject` cuts the line down to that record's
  JSON object (or its `ScopeLogs`, or its `ResourceLogs`) before any
  fragment is checked.
- Negative assertions read `Provider::GetExporterHealth()`
  (`connection_state`, `batches_failed`, `last_error_message`) and then
  assert the marker never appears in the output file at all.

The two suites share one set of fragment constants because encoding is
protocol-independent, and that was measured rather than assumed: the
collector's output line for the OTLP/HTTP basic-export span and the one
for its OTLP/gRPC twin are byte-identical once ids, timestamps and the
per-run marker are normalised
([`docs/interop-matrix.md`](../../docs/interop-matrix.md) §3).

## Deliberately excluded

| Not here | Where instead | Why |
|---|---|---|
| Partial-success responses | [`tests/unit/wire/otlp_response_test.cpp`](../unit/wire/otlp_response_test.cpp) and the two codec tests, against hand-encoded byte fixtures | A collector configured to accept spans never returns a `partial_success` body, so this tier cannot elicit one. |
| Retry and backoff timing | [`tests/unit/exporter/retry_policy_test.cpp`](../unit/exporter/retry_policy_test.cpp), [`tests/integration/sdk/exporter_health_test.cpp`](../integration/sdk/exporter_health_test.cpp) against fakes | Needs a clock the test controls. Here the negative tests run a near-zero retry budget precisely to *avoid* the schedule. |
| Restart recovery (collector bounced mid-export) | deferred; see [`docs/interop-matrix.md`](../../docs/interop-matrix.md) §6 | |
| The metrics cardinality-overflow series | deferred to v1.3 (#302); see [`docs/interop-matrix.md`](../../docs/interop-matrix.md) §6 | |
| Throughput and delivery rate at volume | the weekly [`interop.yml`](../../.github/workflows/interop.yml) workflow | Slow, and not a merge gate. |

## Known-defect tripwires

Five defects were found by building this tier. None of them is asserted
as desired behaviour. One is still open and pinned by a test that fails
when the defect is fixed; the other four are fixed, and the test that
guarded each one was turned into a regression test instead of being
deleted.

The pinned test for #166 was inverted, not deleted, when ICP 0042 made
it fail.

| Issue | What guards it |
|---|---|
| #166 (fixed in v1.2.6 by the HTTP/1.1 fallback, [ICP 0042](../../docs/icps/0042-http1-fallback-for-otlp-http.md)): plaintext OTLP/HTTP unreachable (h2c vs an HTTP/1.1-only receiver) | `http/plaintext_http1_test.cpp`, inverted from `plaintext_gap_test.cpp`: `Connect()` to `:4318` succeeds and a span and a log record arrive over HTTP/1.1; `GrpcToTheHttp1OnlyReceiverStillFails` keeps the gRPC half. |
| #167 (fixed): `InstrumentationScope` carried the service name instead of the `GetTracer(name, version)` scope | `{http,grpc}/basic_export_test.cpp` assert the scope name and version on the wire (`kScopeJson`). Until the fix (ICP 0023), this tier deliberately asserted nothing about `ScopeSpans.scope`, so as not to enshrine the bug. |
| #168 (fixed): `TraceId::ToHex()` / `SpanId::ToHex()` declared in a public header but defined nowhere | `src/api/` (`microtel_api`) now defines both, and `{http,grpc}/basic_export_test.cpp` call the public formatter directly. This tier builds against public headers only, so it is what hit the link error, and it is now what proves the encoding matches the collector's. |
| #169 (fixed): 22 of 24 `drop_counters` never written | `WrongTokenIncrementsNonRetryableDropCounter` in both `http/auth_test.cpp` and `grpc/auth_test.cpp`. It ran as a `DISABLED_` test until the counters were wired, and was kept separate from `WrongTokenRejected` so an accounting regression is distinguishable from a classification one. |
| #171 (fixed): `grpc-status` and `grpc-message` discarded; `last_error_message` was a fixed literal | `grpc/auth_test.cpp` asserts both halves separately: `last_error_message` names the status (`"UNAUTHENTICATED (16)"`, matching what its HTTP sibling does with `"401"`) and carries a fragment of the collector's own `grpc-message`. |
