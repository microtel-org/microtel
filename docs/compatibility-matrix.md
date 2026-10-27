# Compatibility Matrix

## 1. Purpose

What microtel supports, signal by signal and protocol by protocol, with the
evidence for each row, and the ledger of transport and security features that
are either tested or explicitly marked unsupported. Two questions, answered in
one place:

- **Does microtel do this?** — and if so, what test says so.
- **What does *not* work**, stated plainly, so nobody discovers it against a
  production collector.

Sibling of [`interop-matrix.md`](interop-matrix.md), which records the pinned
*versions* the conformance gates run against. This file records the *features*.
Nothing reads either mechanically.

---

## 2. Signals and protocols

| Area | v1 status | Evidence |
|---|---|---|
| OTLP/HTTP traces | Supported over TLS; **unsupported over plaintext against HTTP/1.1-only receivers** — see §4 | `tests/conformance/http/` |
| OTLP/gRPC traces | Supported, plaintext and TLS | `tests/conformance/grpc/` |
| OTLP metrics, both protocols | Supported, conformance-tested (OTLP/HTTP over TLS, OTLP/gRPC over plaintext, as for logs). OTLP/HTTP metrics share the plaintext gap in §4 | `tests/conformance/{http,grpc}/metrics_test.cpp` |
| OTLP logs, both protocols | Supported, conformance-tested in v1.2 (OTLP/HTTP over TLS, OTLP/gRPC over plaintext; TLS and auth are the transport the trace rows already cover). OTLP/HTTP logs share the plaintext gap in §4 | `tests/conformance/{http,grpc}/logs_test.cpp`; `Provider::GetLogger` per [ICP 0012](icps/0012-provider-get-logger.md) |
| W3C Baggage propagation | Supported in v1.1 — `W3CBaggagePropagator`, full grammar, percent-encoding, opaque property tail, and the 180 / 4096 / 8192 limits | `tests/unit/api/baggage_test.cpp`, `tests/unit/api/baggage_propagator_test.cpp`, `tests/fuzz/baggage_fuzz.cpp`; [ICP 0025](icps/0025-propagation-core.md) §2 |
| OTel SDK env vars (subset) | Partial | `tests/unit/common/config/`, [`configuration.md`](configuration.md) §3 |
| Log bridges: spdlog, glog, log4cxx | Experimental, header-only, add nothing to microtel's link closure. spdlog is built by default; glog (0.6–0.7) and log4cxx (1.1+) are opt-in (`MICROTEL_BUILD_GLOG_BRIDGE`, `MICROTEL_BUILD_LOG4CXX_BRIDGE`). The glog bridge sees `VLOG` as `INFO`; the log4cxx bridge is programmatic-only (not nameable in a log4cxx config file) | `tests/unit/adapters/{spdlog_sink,glog_sink,log4cxx_appender}_test.cpp`; [`logs-design.md`](logs-design.md) §10 |
| Leaf (`microtel-leaf`, C11) and concentrator (`LeafReceiver`) | Experimental in v1.2, both off by default (`MICROTEL_BUILD_LEAF`, `MICROTEL_WITH_CONCENTRATOR`). Traces only. The nanopb and upb leaf backends produce identical bytes; leaf payloads reach a collector only through a concentrator, which re-exports them over either protocol with the same plaintext OTLP/HTTP gap as §4. Footprint targets (< 15 KB nanopb on Cortex-M, < 30 KB upb) are measured, not gated, until v2.1 | `tests/conformance/leaf/` (both backends × both protocols, against the pinned collector), `tests/unit/leaf/`, `tests/integration/sdk/leaf_ingest_test.cpp`, `leaf_ingest_fuzz` / `leaf_backend_diff_fuzz`, the `leaf-footprint` and `leaf-target` CI jobs (Cortex-M0+ / M4 under QEMU, aarch64, i686) and [`bench-results/leaf-footprint.md`](bench-results/leaf-footprint.md); [`leaf-concentrator-design.md`](leaf-concentrator-design.md), [ICP 0031](icps/0031-leaf-concentrator-in-v1.3.md), [ICP 0034](icps/0034-leaf-receiver-api.md) |
| `opentelemetry-cpp` API shim | Experimental; source-only, adds nothing to the consumer's link closure | `src/adapters/otelcpp/`, `tests/unit/adapters/`, `tests/integration/otelcpp_shim/`; [ICP 0014](icps/0014-otelcpp-shim-and-rule-13.md) |
| `opentelemetry-python` API shim | Not in v1 — Python ships post-v1.0 as M18 | [ICP 0013](icps/0013-rescope-defer-python-bindings.md) |
| Collector versions | Pinned matrix | [`interop-matrix.md`](interop-matrix.md) §2 |

Logs and metrics were both once implemented and unit-tested but never decoded
by a real collector. Logs left that state in v1.2 (issue #303) and metrics in
PR #459, when the conformance suites started asserting on what the collector
decodes from microtel's exports. The cardinality-overflow series is not yet
conformance-tested ([`interop-matrix.md`](interop-matrix.md) §6).

---

## 3. Transport and security ledger

TLS system trust, custom CA, mTLS, static headers, auth callback, and proxy
behaviour each either pass integration tests **or are explicitly marked
unsupported here**; v1.0 was gated on exactly that. This is that ledger.

| Capability | Status | Evidence |
|---|---|---|
| TLS with the system trust store | Supported | `SSL_CTX_set_default_verify_paths` when no `ca_bundle` is set; verification itself is covered by `tests/integration/transport/http2_tls_connect_test.cpp` (`TrustedCa_ConnectSucceeds`, `UntrustedCert_ConnectFails`). No test pins the *host's* trust store — that would assert on the CI image, not on microtel. |
| TLS with a custom CA (`ca_bundle`) | Supported | `TlsCustomCa`, `UntrustedCaFails` in `tests/conformance/{http,grpc}/tls_test.cpp` |
| mTLS (`client_cert` + `client_key`) | Supported | `MutualTls`, `MutualTlsWithoutClientCertFails`, both suites |
| SNI override | Supported | `SniOverride`, both suites; [ICP 0022](icps/0022-tls-peer-verification.md) |
| **TLS 1.0 / 1.1 receivers** | **UNSUPPORTED** | microtel sets `SSL_CTX_set_min_proto_version(TLS1_2_VERSION)` itself rather than inheriting the linked OpenSSL's floor, so a receiver below TLS 1.2 fails the handshake on every build alike; there is no option to lower it. TLS 1.3 is preferred where the receiver offers it. Issue #216. Evidence: `TlsFloor_ServerLimitedToTls11_ConnectFails` and `TlsFloor_Tls12Server_StillConnects` in `tests/integration/transport/http2_tls_connect_test.cpp`. The negative test is only decisive on an OpenSSL that would otherwise permit the downgrade — a host whose own crypto policy already pins TLS 1.2 (Fedora and Ubuntu both) refuses ahead of microtel. |
| Static auth headers | Supported | `StaticBearerHeader`, both suites |
| Auth callback (`WithAuthProvider`) | Supported | `AuthCallback` + `WrongTokenRejected`, both suites; TTL caching in `tests/unit/common/auth/auth_providers_test.cpp`. Recipes and execution-context caveats: [`auth-callback-recipes.md`](auth-callback-recipes.md) |
| **AWS SigV4 in-process** | **UNSUPPORTED** | `AuthCallback` sets one header, takes no arguments, and runs before the payload is attached; SigV4 needs a per-request `x-amz-date`, a per-request payload hash, and the request body. Sign at a sidecar — [`auth-callback-recipes.md`](auth-callback-recipes.md) §5. |
| `insecure = true` | Supported, and **warns** at `Build()` | `SdkBuilderTest.Build_InsecureTls_Warns`. A hard ban is `MICROTEL_FORBID_INSECURE_TLS=ON`, which turns it into `ConfigError::InsecureDisallowed`. |
| **HTTP proxy (`https_proxy` / `http_proxy` / `no_proxy`, `CONNECT`)** | **UNSUPPORTED in v1** | Not implemented — zero occurrences in `src/`. The variable names are reserved; setting them changes nothing. Deferred, see [`roadmap.md`](roadmap.md). |
| **Plaintext OTLP/HTTP against an HTTP/1.1-only receiver** | **UNSUPPORTED** | §4 below; [`interop-matrix.md`](interop-matrix.md) §4; issue #166 |
| A peer that hangs up under an in-flight write | Survivable: the export fails, the host process does not | `SIGPIPE` is suppressed per write — `MSG_NOSIGNAL` on plaintext sends, and a custom `BIO` carrying the same flag for TLS, covering `SSL_write` and the handshake writes inside `SSL_connect`. The host needs no `signal(SIGPIPE, SIG_IGN)` of its own, and microtel installs no handler and changes no process-wide disposition (`threading-model.md` §7.1). Evidence: `NoSignalIoTest` / `NoSignalBioTest` in `tests/unit/transport/nosignal_io_test.cpp`, plus the peer-reset tests in `tests/integration/transport/`. Issue #177. |
| gzip request compression | Supported | conformance suites, both protocols |
| Response decompression (`grpc-encoding` / `content-encoding: gzip`) | Supported, bounded by `max_decompressed_bytes` | `tests/unit/wire/grpc/`, `tests/unit/wire/http/`, `tests/fuzz/response_decompression_fuzz.cpp` |
| HTTP/3 | Out of scope for v1 | v1.6 experiment per [roadmap](roadmap.md) §4 |
| Windows | Out of scope | a hard non-goal: [roadmap](roadmap.md) §8 and §12 |

---

## 4. Plaintext OTLP/HTTP cannot reach a stock collector

microtel's transport is HTTP/2-only (nghttp2), so a plaintext `http://`
endpoint means **h2c with prior knowledge**. The OpenTelemetry Collector's
plaintext OTLP/HTTP receiver serves HTTP/1.1 only — it does not wrap its
handler in `h2c` — so it answers the HTTP/2 connection preface with an
HTTP/1.1 response and the SETTINGS exchange never completes.

**This is a documented limitation, not a bug being fixed.** microtel has no
HTTP/1.1 mode and none is planned for v1; whether to add one is a roadmap item
(see [`roadmap.md`](roadmap.md) §12) and issue #166.

**What works instead**, in order of preference:

| Configuration | Works? |
|---|---|
| `https://collector:4318` + `Protocol::Http` | Yes — the same collector negotiates `h2` through ALPN |
| `http://collector:4317` + `Protocol::Grpc` | Yes — gRPC is h2c by definition |
| `https://collector:4317` + `Protocol::Grpc` | Yes |
| `http://collector:4318` + `Protocol::Http` | **No** — unless the receiver speaks h2c |
| `http://proxy:8080` + `Protocol::Http`, where the proxy is h2c-capable | Yes — this is why the configuration is not rejected outright |

**How it reports itself.** Since the transport learned to name this case, all
three of these surfaces say so:

- `Build()` logs a warning through the internal log channel
  (`microtel::SetLogSink` to capture it):

  > plaintext OTLP/HTTP (http:// with protocol=http) is HTTP/2 with prior
  > knowledge and cannot reach an HTTP/1.1-only OTLP receiver such as a stock
  > OpenTelemetry Collector - use https:// or OTLP/gRPC; see
  > docs/compatibility-matrix.md

- `Connect()` fails with `Error::Kind::Protocol`:

  > peer answered the HTTP/2 preface with an HTTP/1.1 response - endpoint
  > appears to be HTTP/1.1-only; use https:// (ALPN h2) or OTLP/gRPC; see
  > docs/compatibility-matrix.md

- The same message reaches `HealthSnapshot::last_error_message`, and the
  failure is **not retried**: a protocol mismatch is permanent, and a retry
  budget spent on it only buries the message.

The neighbouring case — a TLS endpoint that never agrees to `h2` — is caught
by an ALPN check right after the handshake and reports `Error::Kind::Protocol`
naming what was negotiated.

One boundary is worth knowing, because it moves with the linked OpenSSL. If
such a server answers ALPN with a protocol microtel never offered (microtel
offers only `h2`), OpenSSL **3.2 and later** enforce RFC 7301 themselves and
fail the handshake, so the refusal arrives as a TLS error rather than the ALPN
message; **3.0 and 3.1** let it through and microtel's check is what refuses
it. Either way the connection is refused — which is what
`Http2TlsConnectTest.AlpnAnswersHttp11_ConnectFails` asserts.

---

## 5. Open-issue caveats

Claims above are true as written; these are the known gaps behind them, all
open at the time of writing.

- **Instrumentation scope on the wire is the service name** (issue #167).
  `GetTracer(name, version)` does not reach the collector, so a backend groups
  spans by service rather than by instrumentation library. Affects both
  protocols identically. No conformance test asserts on `ScopeSpans.scope` —
  that would enshrine the bug.
- **`tracestate` round-trips** (issue #208, fixed in v1.1; follow-up to the
  fixed #188). `microtel::TraceState` holds a real entry list, so a vendor's
  `tracestate` now survives a microtel hop instead of being dropped: `Extract`
  populates `SpanContext::trace_state` and `Inject` emits the header whenever
  it is non-empty. Parsing and serialisation follow the W3C Trace Context §3.3
  key and value grammars and the 32-member limit; §4.3's whole-header option
  is the one microtel takes, so one malformed member, one duplicate key, or a
  33rd member discards the header rather than half-forwarding it — and never
  costs the `traceparent`. `Get` / `Set` / `Erase` are copy-on-write, with the
  mutated member moved to the front per §3.3.1.
  The storage sits behind a `shared_ptr` to an immutable list so that
  `SpanContext`'s copy stays `noexcept` inside
  `Span::GetContext() const noexcept`; that made it the v1.0 → v1.1 ABI event
  sanctioned by the ABI policy in [`releasing.md`](releasing.md) §7 and specified in
  [ICP 0025](icps/0025-propagation-core.md) §1. Consumers recompile; no
  consumer source changes. Evidence: `tests/unit/api/trace_state_test.cpp`
  (W3C vector suite), `tests/unit/api/propagator_test.cpp`,
  `tests/unit/adapters/otelcpp_context_conversion_test.cpp`.
- **`HealthSnapshot::drop_counters` is live** (issue #169, fixed). 26 of 27
  `DropReason` counters have producers. `metric_callback_timeout` reads zero
  until the per-collection callback deadline lands with v1.3 metrics
  ([`error-model.md`](error-model.md)). The conformance tests
  assert the wired counters directly.
- **gRPC failures name the status** (issue #171, fixed). Both the
  `grpc-status` name and number and the collector's percent-decoded
  `grpc-message` reach `HealthSnapshot::last_error_message` — e.g.
  `UNAUTHENTICATED (16): provided authorization does not match expected scheme
  or token`. Before this, every non-zero status read `"grpc error"` and a
  rejected credential was indistinguishable from a malformed payload.
- **Response decompression shipped** (issue #161). Both codecs advertise
  `gzip` and inflate a compressed response under `max_decompressed_bytes`.
  Before it shipped, a collector that compressed its response body was not
  interoperable on the gRPC path — and the pinned collector does exactly that
  whenever the request is compressed, so `compression = "gzip"` deployments
  were silently discarding partial-success counts.

---

## 6. Update procedure

A row changes in the same PR as the behaviour it describes. When a row moves
from unsupported to supported, the test that proves it moves with it — a
status without evidence in the third column is not a status.

§4 in particular is load-bearing: `tests/conformance/http/plaintext_gap_test.cpp`
asserts the limitation *and* its error message, and the sibling HTTP
conformance tests point at TLS endpoints because of it. If that test starts
failing, this section and [`interop-matrix.md`](interop-matrix.md) §4 are part
of the change.

---

## 7. Compatibility tiers

Compatibility is defined in tiers so the promise is testable, not aspirational:

| Tier | Promise |
|---|---|
| **Tier 1: OTLP wire compatibility** | Exported payloads are accepted by receivers implementing the pinned OTLP specification version, over both OTLP/HTTP-protobuf and OTLP/gRPC. The client correctly parses success, failure, retryable failure, and partial-success responses. Compatibility is measured against a pinned `opentelemetry-proto` tag and an interop matrix of collector and backend versions (§2 of this file and [`interop-matrix.md`](interop-matrix.md)). |
| **Tier 2: OpenTelemetry data-model compatibility** | Traces, attributes, resources, and instrumentation scope map to the OpenTelemetry protobuf schemas. |
| **Tier 3: API-adapter compatibility** | Optional shims support common `opentelemetry-cpp` and `opentelemetry-python` use cases. The supported subset is documented in §2 of this file. |
| **Tier 4: Full SDK conformance** | Every requirement in the OpenTelemetry SDK specification is met. |

This section defines the tiers. Which signals each release
claims at which tier is in [`roadmap.md`](roadmap.md) §3,
and the gates a release must pass to promote a tier are in its §10. The rows
in §2–§5 are the per-feature evidence. For traces, Tier 1 holds over OTLP/gRPC
and over OTLP/HTTP with TLS; plaintext OTLP/HTTP does not reach an
HTTP/1.1-only receiver (§4).
