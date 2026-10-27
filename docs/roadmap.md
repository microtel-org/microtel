# microtel Roadmap: From Exporter-First v1 to Full OpenTelemetry Coverage

**Companion to:** the design documents indexed in [`docs/README.md`](README.md)
**Status:** Draft v0.1. Implementation status updated 2026-10-27 against v1.2.4.
**Scope:** Multi-year evolution from a traces-only exporter through full OTel SDK conformance and embedded deployments.

---

## Implementation status (as of v1.2.4)

The release themes in §4 were planned as a sequence, but the code did not
follow it exactly. Metrics, logs, the spdlog log bridge and the otel-cpp shim
were all built during the v1 milestones (M12–M17), ahead of their themes. Logs
became supported in v1.2, with collector conformance tests, retry and three log
bridges. Metrics and the otel-cpp shim still ship as **experimental**: they
work and are tested, but carry no compatibility promise. Metrics gained
collector conformance tests in v1.2.4. The v1.3 theme is therefore mostly about
finishing and stabilizing that code, not writing it.

| Theme | Status | What remains |
|---|---|---|
| Trace runtime + OTLP exporter | Done | None open. #271 and #223 are fixed, and v1.2.2 applies the sampler's attributes and trace state (#340) |
| v1.1 Operational polish | Done, except the parts moved elsewhere | Python sugar (moved to M18); mTLS rotation (v1.4, #297) |
| v1.1.1 Patch | Done | Per-key merge of table-valued settings (#257); retry for metric and log export (#222); backoff before the first retry (#311); interruptible retry backoff (#310); the resolved Resource logged at startup, escaped (#284, #315) |
| v1.2 Logs | Done, supported | Logs cookbook (docs only) |
| v1.2 Leaf / concentrator | Done, experimental (ICP 0031, ICP 0036) | Leaf programming and concentrator deployment guides; v2.0 stabilises the API |
| v1.2.2 Patch | Done | Static request headers validated at `Build()`, and the auth callback's value on each export ([ICP 0038](icps/0038-reject-reserved-request-headers.md), #412); OTel key=value env lists trimmed and percent-decoded (#413); a Meter, its instruments and a Logger may outlive the Provider (#259, #417) |
| v1.2.3 Patch | Done | Install story: vcpkg port, Conan 2 recipe, use as a FetchContent/CPM subproject, `find_package(microtel COMPONENTS leaf concentrator)`, and a source archive with its SHA-512 on each release (#432) |
| v1.2.4 Patch | Done (#462) | Exponential histograms encoded (#447); metric data points carry time and start time ([ICP 0039](icps/0039-metric-start-timestamps.md), #449); observable sums report deltas under delta temporality (#453); span and link `trace_state` and flags encoded (#452); metrics conformance against the collector on both protocols, and a known-limitations section in the README (#459) |
| v1.3 Metrics | Mostly done, experimental | Async-callback deadline (#237); View aggregation override; per-instrument temporality; OTel exemplar reservoirs and `OTEL_METRICS_EXEMPLAR_FILTER`; exemplar export (#454); per-kind Delta/LowMemory presets (#455); `Timer`/`Counter` sugar; collector conformance for the cardinality-overflow series (#302) |
| v1.4 Control plane | Not started | Unix-socket server, `microtelctl`, threat model, operator guide (ICP 0024); mTLS rotation (#296, #297) |
| Tier 3 otel-cpp shim | Done for all three signals, experimental | Beta gates in §10 (real-world app testing, frozen API, deprecation policy) |
| v1.5 Conformance push | Not started | Custom samplers are reachable through `SamplerHandle` but not a documented extension point; histogram buckets can be set per instrument but not through Views |
| v1.6 Performance & footprint | Partial | Static-archive install and `find_package` exist (ICP 0020); measured size targets and compile-time feature selection are proposed in ICP 0030; coroutines, pooling, HTTP/3 not started |
| v2.x, v3.0 | Not started | v2.0 now stabilises the leaf and receiver APIs rather than introducing them |

Status markers in §4 and §5 use the same words: *done*, *partial* (with what
is missing), *not started*. Where a bullet names an API that ended up with a
different name, the bullet has been corrected to the shipped name.

---

## 1. How to read this document

This is a **theme-based roadmap**, not a dated commitment. Each minor release advances one cohesive theme; major versions bump when public C++ API or wire-level promises break. Effort estimates from the v1 spec are realistic ranges; v1.x and beyond are intentionally less precise because real signals from M0 and M1 will reshape them.

The roadmap is the source of truth for **what's deferred from v1**. Anything called out as "v1.1" or "v2.0" in the design documents is described here in detail. If a feature isn't on this roadmap, it isn't on the project's plan; new ideas land in §11 first and graduate to a milestone if they survive review.

v1.0 has shipped; the README and the design documents describe it. This document is brief on v1.0 and detailed on everything after.

---

## 2. Versioning and maintenance policy

- **Semantic versioning** for the public C++ API and `microtel.toml` schema.
- **Wire compatibility** is tracked against a pinned OTel spec version, called out per release.
- **One LTS line per major version.** When v2.0 ships, the v1.x line gets security fixes and OTel-spec updates for **18 months**. After that, v1.x is end-of-life. The deprecation calendar lives in `docs/release-policy.md`.
- **Pre-1.0 ABI is unstable**, period. Post-1.0 source compat within major; binary best-effort within minor ([`releasing.md`](releasing.md) §7).
- **Compat shims** version independently from microtel core. A shim `0.x` may target microtel `1.5` and remain experimental even as microtel itself is stable.

---

## 3. Compatibility tier progression

Each release advances along the four-tier model defined in [`docs/compatibility-matrix.md`](compatibility-matrix.md) §7. The progression below is the planned path; tier promotions require the gates listed in §10 to pass first.

| Release | Tier 1 (wire) | Tier 2 (data-model) | Tier 3 (API-adapter) | Tier 4 (SDK conformance) |
|---|---|---|---|---|
| **v1.0** | traces | traces | experimental: all three (shim, M17) | — |
| **v1.1** | traces | traces | experimental: all three | — |
| **v1.2** | traces, logs | traces, logs | experimental: all three | — |
| **v1.3** | all three signals | all three signals | experimental: all three | — |
| **v1.4** | all three | all three | experimental: all three | — |
| **v1.5** | all three | all three | **beta: all three** | partial |
| **v1.6** | all three | all three | beta: all three | partial |
| **v2.0** | all three + leaf | all three + leaf | **stable: all three** | partial+ |
| **v2.1** | + MCU leaf | + MCU leaf | stable | partial+ |
| **v2.2** | + concentrator HA | stable | stable | partial+ |
| **v3.0** | all + profiles | all + profiles | stable | **full conformance claim** |

The leaf ships as experimental in v1.2 with both encoder backends ([ICP 0031](icps/0031-leaf-concentrator-in-v1.3.md)). The v2.0 and v2.1 rows are where it is claimed at a tier.

"Profiles" refers to OpenTelemetry's continuous-profiling signal, which is still stabilizing upstream as of this writing. It graduates onto the roadmap once upstream marks it stable.

> **The Tier 3 column's original incremental plan (traces, then metrics, then
> all three) was superseded.** The shim was built as M17, after all three
> signals existed (M12–M14), so it covered traces, metrics and logs from its
> first release. It lives in `src/adapters/otelcpp/` and is installed as
> source under `include/microtel-shim/`, never as a prebuilt library (see
> [ICP 0014](icps/0014-otelcpp-shim-and-rule-13.md)). It is experimental;
> the beta gates in §10 are not met. Tier 1 and Tier 2 columns still describe
> the *supported* signals. Logs are claimed at both from v1.2. Metrics are on
> the wire today but are not claimed at those tiers until v1.3, when they
> become supported.

---

## 4. Release themes

### v1.0 — Trace Runtime + OTLP Exporter (release point)

**Theme:** Prove the wedge. Smallest credible OTel-compat trace runtime over OTLP/HTTP and OTLP/gRPC.

**Status:** done. The bugs once open here (#271, #223) are fixed.

Brief recap:

- C++20 trace SDK (Tracer, Span, W3C Trace Context, AlwaysOn / AlwaysOff / TraceIdRatio / ParentBased samplers, BatchSpanProcessor)
- OTLP/HTTP and OTLP/gRPC over nghttp2, no gRPC library
- Production correctness: partial-success, retry policies, GOAWAY/RST_STREAM, fork-safety, deterministic shutdown
- Static config + OTel env-var fallback, no hot reload
- Preflight CLI flag, exporter-health API, internal logging
- Experimental compat shims *(done for all three signals, but shipped as installed source rather than a separate package; see the Tier 3 note under §3)*

Python bindings are **not** part of v1.0. They ship post-v1.0 as **M18**, covering all three signals, per [ICP 0013](icps/0013-rescope-defer-python-bindings.md).

**v1.0 release gates** were met; the CI jobs that keep enforcing them are described in [`docs/ci-architecture.md`](ci-architecture.md).

---

### v1.1 — Operational Polish

**Theme:** Make v1 actually nice to operate, layer on the ergonomics that v1.0 deliberately deferred.

**Status:** done, shipped as v1.1.0. All five "ships when" gates are met. Python sugar moved to M18 with the rest of the Python bindings, and mTLS rotation has not been started.

- **Sugar layer** (`microtel::sugar`): function-scoped spans via `std::source_location`, RAII scoped spans with inline attributes, traced-lambda helpers, exception recording, scoped timers (active once metrics arrive in v1.3), pre-bound `AttrKey` for hot paths. Python equivalents using decorators and context managers. See §5 below for full sugar evolution. *(C++ done, [ICP 0028](icps/0028-sugar-surface.md); scoped timers not started; Python not started, moved to M18.)*
- **Hot reload via public setters.** Four thread-safe `Provider` setters — `SetBatchOptions`, `SetMetricInterval`, `SetSamplerRatio`, `SetLogLevel` — driven from whatever administrative surface the host application already has. Endpoint, protocol, TLS material, service.name, and resource attributes are explicitly **not** hot-reloadable in v1.1. The Unix-domain-socket server, its length-prefixed JSON wire, and the `microtelctl` client are deferred to **v1.4** ([ICP 0024](icps/0024-v1.1-rescope.md) moved them to v1.2, [ICP 0032](icps/0032-release-reorder-v1.1.1.md) to v1.4); see and `docs/control-plane-design.md`. *(Setters done, with a TSAN hammer test and a fuzz harness.)*
- **W3C Baggage propagation.** Inject + extract. *(Done, [ICP 0025](icps/0025-propagation-core.md).)*
- **Composable sampler chains.** `MakeChainSampler(ChainMode, ...)` with built-in rule-based combinators (`MakeAttributeRuleSampler`, `MakeSpanNameRuleSampler`, `MakeSpanKindRuleSampler`) over what a head sampler can see at `ShouldSample` time — sample-on-attribute, sample-on-name, sample-on-kind. **Not** sample-on-duration: the decision is made before the span runs, so duration-based selection is tail sampling and belongs in the collector. *(Done.)*
- **Multi-profile within one process.** Named providers with independent endpoints, samplers, and Resources. *(Done: `WithProfileName` and `microtel::GetProvider(name)`, [ICP 0027](icps/0027-multi-profile-threading.md).)*
- **Auth recipes, not built-in providers.** OAuth2 client credentials and AWS SigV4 ship as documented `AuthCallback` recipes — [`docs/auth-callback-recipes.md`](auth-callback-recipes.md) — over the shipped `WithAuthProvider` surface, with no code in the runtime. That document also records where the surface falls short of the claim: OAuth2 fits it; SigV4 needs per-request headers and a payload hash the callback never sees, so it signs at a sidecar until a per-request header hook exists. *(Recipes done.)* mTLS rotation likely v1.2 (more involved). *(Not started; now v1.4, #297.)*
- **Resource detectors:** process (`process.pid`, `process.executable.name`, `process.executable.path`, `process.command`, `process.command_args`) and host (`host.name`, `host.id`). *(Done.)*
- **`microtelctl` packaging — deferred to v1.4** with the socket server it drives: standalone Go binary in `.deb`/`.rpm`/`.tar.gz`, separate from the core runtime package — reconciles the v1.0 "single shared library + Python wheel" claim. *(Not started.)*

**Ships when:**

1. The sugar layer has a stable API, recorded in ICP 0028 and shipped as installed headers.
2. The hot-reload setters pass a TSAN-built concurrent hammer test — all four setters racing span/log emission, the BSP/BLRP workers, and metric collection — and their input validation is fuzz-tested by a harness driving randomized values and interleavings against a live provider in the standing fuzz job.
3. The propagation core passes W3C test vectors for traceparent, tracestate, and baggage, including grammar limits, and the baggage header parser has a fuzz target.
4. Roadmap and spec are amended for the socket/microtelctl deferral (threat-model + hot-reload-socket-fuzz gates move with it to v1.2), the OAuth2/SigV4 → AuthCallback-recipes substitution, and the sample-on-duration correction.
5. Every issue on the v1.1 milestone is closed or explicitly re-milestoned with a recorded reason.

**Anti-goals in v1.1:** no *supported* metrics or logs (both ship as experimental, see the status section at the top), no full SDK conformance claim, no Windows.

---

### v1.2 — Logs + Leaf / Concentrator

**Theme:** Logs go supported, and the embedded story starts: the leaf and concentrator ship as experimental. The two halves are independent: logs don't wait for the leaf, and if the leaf isn't ready it moves to the next 1.x minor ([ICP 0031](icps/0031-leaf-concentrator-in-v1.3.md), renumbered from v1.3 by [ICP 0032](icps/0032-release-reorder-v1.1.1.md)).

**Status:** done, shipped as v1.2.0. Logs are supported (conformance-tested, with retry and three log bridges); the leaf and concentrator ship as experimental with every ICP 0031 ship gate met, plus a full-SDK custom export transport (ICP 0036). Still open: a logs cookbook.

- **OTel Logs API:** Logger, LogRecord, severity levels, attribute schema. *(Done.)*
- **OTLP/logs export** on both wire protocols. *(Done, with retry since v1.1.1, #222.)*
- **Trace context correlation:** logs emitted within an active span carry the `trace_id` and `span_id` automatically. *(Done.)*
- **Bridge adapters as separate packages:**
  - `microtel-bridge-spdlog`: a spdlog sink that converts spdlog records to OTel LogRecords. The natural pairing given microtel's internal logging dependency. *(Done, in-tree as `microtel/adapters/spdlog_sink.hpp` rather than a separate package.)*
  - `microtel-bridge-glog`: same for `glog`. *(Done, #304: in-tree as `microtel/adapters/glog_sink.hpp`, header-only, glog 0.6–0.7, opt-in via `MICROTEL_BUILD_GLOG_BRIDGE`.)*
  - `microtel-bridge-log4cxx`: same for `log4cxx`. *(Done, #304: in-tree as `microtel/adapters/log4cxx_appender.hpp`, header-only, log4cxx 1.1+, opt-in via `MICROTEL_BUILD_LOG4CXX_BRIDGE`.)*
  - Bridges are independently versioned packages. *(Superseded: all three ship in-tree as header-only adapters versioned with microtel. None adds to microtel's link closure, and `ci/scripts/symbol-scan.sh` fails any shipped archive that references glog, gflags or log4cxx.)*

**Compatibility tier:** Tier 1 and Tier 2 add logs; metrics follow in v1.3. The Tier 3 shim is already experimental for all three signals.

**Leaf / concentrator (experimental).** Moved from v2.0. *(Done, experimental: [`docs/leaf-concentrator-design.md`](leaf-concentrator-design.md) describes what shipped. The C leaf with both backends, the ingest path, the `[concentrator]` configuration and the three time modes are in, and the ICP's ship gates are met: fuzzed ingest, a collector end-to-end test per backend and protocol, published footprints with a Cortex-M CI job ([`docs/bench-results/leaf-footprint.md`](bench-results/leaf-footprint.md)), and [`examples/leaf/`](../examples/leaf/).)*

- **microtel-leaf**, a pure-C library for constrained embedded systems. No threading, no batching, no retries, no TLS, no HTTP. Encodes OTLP messages and hands the bytes to an application-supplied transport.
- **Two encoder backends from the first release**, chosen at build time with `MICROTEL_LEAF_ENCODER=upb|nanopb`. upb covers Linux-on-ARM, OpenWrt-class and Cortex-A/R targets (`< 30 KB` flash target); nanopb covers Cortex-M (`< 15 KB` flash target). Same leaf API and identical OTLP bytes from both. nanopb is vendored, renamed to `microtel_pb_*`, and linked into the leaf only.
- **Concentrator role** in main microtel: the application hands it leaf payloads through an ingest call; it decodes them, enriches with Resource attributes from config (`device-id → service.*`), runs the standard sampling / batching / export pipeline, and ships to the upstream collector over OTLP/HTTP or OTLP/gRPC. No inbound socket.
- **Time handling:** three modes — concentrator-stamped, sync-relative, boot-relative — configurable per leaf in the concentrator's config.
- **Late Resource enrichment** and the **Receiver abstraction** arrive as new public API, marked experimental.
- **Full-SDK nodes on the leaf link:** `SdkBuilder::WithExportTransport` hands a C++ Provider's encoded OTLP requests to an application-supplied transport instead of HTTP/2 ([ICP 0036](icps/0036-custom-export-transport.md)).

**Anti-goals in v1.2:** no log-side sampling (collector handles it), no structured-log search features (not microtel's job); for the leaf, no RTOS ports, no leaf-side sampling, no PTP/NTP, no reliable delivery on the leaf-to-concentrator link (application transport's job).

---

### v1.3 — Metrics

**Theme:** Second signal lands.

**Status:** mostly done and shipping as experimental. The work left is finishing the gaps marked below. Collector conformance tests landed in v1.2.4, over both protocols, except for the cardinality-overflow series (#302). Open issues are on the v1.3 milestone. The control plane that ICP 0024 moved into this release is now its own release, v1.4 ([ICP 0032](icps/0032-release-reorder-v1.1.1.md)).

**Prerequisite milestone (M11 from spec):** `docs/metrics-design.md` lands first, with reviewer sign-off. Covers the semantic decisions that v1.0 deliberately deferred: aggregation temporality (delta vs cumulative), cardinality limits, histogram bucket configuration, async-instrument callback semantics, reader/exporter interaction, views, exemplars roadmap. *(Done.)*

Then implementation:

- **Sync instruments:** Counter, UpDownCounter, Gauge, Histogram. *(Done, plus an `ExponentialHistogram` instrument.)*
- **Async instruments:** ObservableCounter, ObservableUpDownCounter, ObservableGauge with callback semantics defined in the design doc. *(Partial: the per-collection callback deadline is not enforced, #237.)*
- **MetricReader / MetricExporter pipeline** sharing the existing OTLP encoder and transport infrastructure. *(Done over both protocols, with retry since v1.1.1, #222.)*
- **Aggregation temporality** with delta and cumulative paths; per-metric configuration. *(Partial: delta and cumulative work, but temporality is set once per provider; no per-instrument or per-View override.)*
- **Cardinality limits** with explicit overflow attribute (per OTel spec) and drop accounting. *(Done.)*
- **Views API** (basic — rename, attribute filter, aggregation override). Full views deferred to v1.5. *(Partial: rename, attribute allowlist and drop work; aggregation override is missing.)*
- **Exemplars** linked to active span context where present. *(Partial: one exemplar per attribute set rather than OTel reservoirs; `OTEL_METRICS_EXEMPLAR_FILTER` is not read.)*
- **`mt::Timer` sugar wired into histograms** — completes the sugar layer's deferred-during-v1.1 piece. *(Not started.)*

**Compatibility tier:** Tier 1 and Tier 2 advance to include metrics. Tier 3 shim adds metric instruments, still experimental. *(The shim's metric instruments are already done.)*

**Anti-goals in v1.3:** no advanced exemplar formats, no metric backfill / persistence.

---

### v1.4 — Control Plane

**Theme:** Out-of-process administration. Split out of the metrics release by [ICP 0032](icps/0032-release-reorder-v1.1.1.md), so metrics don't wait on it.

**Status:** not started. Tracked in #296 and #297.

- **Unix-domain-socket server** with the length-prefixed JSON wire from `docs/control-plane-design.md`, driving the four `Provider` setters that shipped in v1.1.
- **`microtelctl`**, packaged as a standalone binary in `.deb`/`.rpm`/`.tar.gz`, separate from the core runtime package.
- **Threat model** and a **fuzz target for the socket's request parser**, the gates [ICP 0024](icps/0024-v1.1-rescope.md) moved out of v1.1.
- **Control-plane operator guide.**
- **mTLS client certificate rotation** (#297), which the v1.1 notes had pencilled in as "likely v1.2".

**Anti-goals in v1.4:** no remote (network) control plane, no swapping the sampler *object* at runtime (Tier 4 in the design doc), still no Windows.

---

### v1.5 — Conformance Push

**Theme:** Move compat shims from experimental to beta; first partial Tier 4 claim.

**Status:** not started, apart from two pieces noted below.

- **Compat shims to beta.** Tested against ≥10 representative real-world applications drawn from the OTel community demos and contributed adopters. Shim API surface frozen at OTel spec version X.Y. Documented deprecation policy.
- **Auto-instrumentation for select libraries:** database clients (libpq, mysqlclient), HTTP clients (libcurl, cpp-httplib), gRPC clients via the standard interceptor mechanism. Each instrumentation is a separate package. Python auto-instrumentation follows the OTel-Python contrib pattern.
- **Resource detectors expanded:** Kubernetes (downward API), AWS (EC2 IMDS, ECS task metadata, EKS), GCP (metadata server), Azure (IMDS).
- **Span Processors as a public extension point.** Until v1.5, processors are internal-only. v1.5 publishes the `Processor` interface as stable, allowing third-party span processors (filtering, enrichment, fan-out). *(Not started: `ISpanProcessor` is internal.)*
- **Custom Samplers as a documented extension point.** Same pattern. *(Partial: a custom sampler can be passed through `SamplerHandle`, but the interface is in `internal::` and undocumented.)*
- **Persistent storage option.** Optional disk-backed retry queue for failed batches that survives process restart. Off by default, opt-in via config. Useful for satellite, edge, and intermittent-connectivity deployments.
- **Full Views API.** Custom buckets, attribute hashing, stream-level renames. *(Partial: custom histogram buckets can be set per instrument at creation, not through Views.)*

**Compatibility tier:** Tier 4 reaches "partial." Specifically: span semantics, metric instrument semantics, log record semantics conform; the "everything in the spec" surface is not yet 100%.

**Anti-goals in v1.5:** still no Windows, no auto-instrumentation for everything (just major libraries), no profiles signal.

---

### v1.6 — Performance & Footprint Refinement

**Theme:** Squeeze the last drop. The v1.0 numbers prove the wedge; v1.6 makes them luxurious.

**Status:** partial. Static archives with `find_package` support and a consumer build check exist today; the rest is not started.

- **Coroutine API.** Async export with `co_await` for users on coroutines-aware code. Returns `microtel::Task<ExportResult>`. Callback API stays as the supported v1.0 surface.
- **Connection pooling.** Optional multi-connection mode for very-high-throughput deployments where one HTTP/2 connection's flow-control becomes the bottleneck. Off by default.
- **Optimized hot path.** Compile-time attribute key encoding (for callers using `mt::AttrKey`), refined lock-free MPSC, possible move to a fully wait-free hot path on x86-64 / ARM64.
- **Static-link optimizations.** First-class CMake support for `-Bstatic` builds, with measured size targets. Currently mostly-static is supported; v1.6 makes it a tested release configuration. *(Partial: microtel ships only static archives, installed per [ICP 0020](icps/0020-install-and-package-config.md) and checked by `ci/scripts/consumer-smoke.sh`. Measured size targets and compile-time feature selection are proposed in ICP 0030 (PR #293).)*
- **HTTP/3 transport (experimental).** The `Transport` interface gains an nghttp3-based implementation. Configurable per-endpoint. Experimental in v1.6; may stabilize in v2.0 or stay experimental indefinitely depending on real-world usage signals.
- **Refined benchmarks.** The `bench/` directory gets richer scenarios: bursty traffic, high-cardinality metrics, long-tail latency under collector pressure. *(Partial: backpressure, soak and hot-loop-metrics profiles exist; bursty and high-cardinality do not.)*

**Anti-goals in v1.6:** still no Windows, no coroutine-only API (callback always supported).

---

### v2.0 — Leaf / Concentrator Goes Stable

**Theme:** Embedded story, stabilised. The leaf and concentrator introduced as experimental in v1.2 become a supported, stable API.

**Status:** not started.

- **Leaf C API stable** for both encoder backends.
- **Late Resource enrichment.** The public API introduced in v1.2 becomes stable.
- **Receiver abstraction.** Public stable API. Third parties can write custom receivers (e.g., legacy proprietary protocol → OTLP).

**Compatibility tier:** Tier 3 promotes to **stable** for all three signals — the leaf-and-concentrator deployment story is enough adoption surface that the shim API can no longer be experimental.

**Major-version bump because:** any breaking changes that the experimental leaf, `Receiver` and Resource-enrichment APIs turn out to need are collected here. (The original reason, making internal interfaces public, no longer applies: v1.2 introduces them as new API, per [ICP 0031](icps/0031-leaf-concentrator-in-v1.3.md).) v1.x line continues for 18 months.

**Anti-goals in v2.0:** unchanged from the leaf's v1.2 list: no full RTOS ports, no leaf-side sampling, no PTP/NTP, no reliable delivery on the leaf-to-concentrator link.

---

### v2.1 — True MCU Support

**Theme:** Reach the smallest devices.

**Status:** not started.

- **Static memory pools throughout** the leaf. No malloc anywhere.
- **Documented RAM/flash budgets** per leaf configuration, now **gated in CI**: **< 15 KB flash, < 2 KB RAM** for a minimal trace-only nanopb leaf on Cortex-M0+. (The nanopb backend itself moved to v1.3, per [ICP 0031](icps/0031-leaf-concentrator-in-v1.3.md).)
- **Concrete reference ports:** STM32 HAL, Zephyr, FreeRTOS examples in `examples/leaf/`. Not formal RTOS integrations — examples.

**Anti-goals in v2.1:** still no formal RTOS integrations as part of microtel core.

---

### v2.2 — Operational Excellence at Scale

**Theme:** Big-fleet deployments need operations features the single-process model doesn't have.

**Status:** not started.

- **Concentrator clustering.** HA pairs sharing leaf state. A leaf can fail over between concentrators without losing in-flight telemetry. Built on a small consensus protocol or a shared backing store; design TBD in v2.2 design doc.
- **Concentrator-side advanced sampling.** Head-based and tail-based sampling at the concentrator, since leaves emit raw and concentrators have the budget for more sophisticated decisions.
- **Leaf authentication.** A small protocol on top of the existing leaf-to-concentrator transport for mutual authentication and integrity. Detail TBD.
- **Multi-tenant concentrator routing.** Single concentrator process serving multiple downstream collectors with per-tenant config, enrichment rules, and rate limits.

---

### v3.0 — Full SDK Conformance

**Theme:** Tier 4 claim. microtel passes the full OpenTelemetry SDK conformance test suite.

**Status:** not started.

- **Pass full conformance.** Every requirement in the OTel SDK spec, with caveats explicit and minimal.
- **Stable shims.** Compat shims graduate from beta to stable; no longer "experimental migration aids" but supported peers of the native API.
- **All current OTel API surface.** Whatever the OTel spec defines as of v3.0's release, microtel implements.
- **Profiles signal**, if upstream-stable by then. Otherwise deferred again.
- **Auto-instrumentation expanded.** All major libraries with stable upstream OTel instrumentation get a microtel equivalent.
- **Decision: deprecate native API or keep both.** A real choice for v3.0 design: either native `microtel::*` API stays first-class with shims as alternative, or microtel commits fully to OTel API surface and natives become a thin convenience layer. Discussion happens in v2.x; decision lands in v3.0 design doc.

**Anti-goals in v3.0:** none meaningful at this point — the project has reached full coverage. Future work is depth (perf, ports, integrations) and breadth (more bridges, more auto-instrumentations).

---

## 5. Sugar Layer Evolution

The sugar layer (`microtel::sugar` and Python equivalents) grows continuously rather than landing at one milestone. Each release adds helpers as the underlying primitives become available.

### v1.0
None. The native API is direct OTel-style: `tracer->StartSpan(...)`, `span->SetAttribute(...)`, `span->End()`.

### v1.1 (sugar layer's introduction release)
The shipped names are in `microtel::sugar` ([ICP 0028](icps/0028-sugar-surface.md)); the plan wrote them as `mt::`.
- `MICROTEL_TRACE_FUNCTION(tracer)` — function-scoped span auto-named via `std::source_location`. *(Done.)*
- `sugar::Span("name", {attrs})` — RAII scoped span with inline attributes. *(Done.)*
- `sugar::Traced("name", lambda)` — trace a lambda; returns the lambda's value. *(Done.)*
- `sugar::RecordException(span, e)` — sets Error status + adds exception event. *(Done.)*
- `sugar::AttrKey("http.method")` — pre-bound attribute key for hot paths. *(Done as a stored key; the per-call lookup saving is left to v1.6's compile-time key encoding.)*
- Python: `@mt.trace_function` decorator, `mt.span(...)` context manager, `mt.traced(name, callable)`, `mt.record_exception(span, e)`. *(Not started, moved to M18.)*

### v1.3 (metrics arrive)
- `mt::Timer("histogram_name")` — RAII timer recording duration to a histogram on destruction. Was deferred from v1.1 because histograms didn't exist yet. *(Not started. Histograms now exist, so nothing blocks it.)*
- `mt::Counter<T>(...)` — pre-bound counter helper for hot paths. *(Not started.)*
- Python: `@mt.timed("histogram_name")` decorator, `mt.counter("name", ...)`. *(Not started.)*

### v1.5 (conformance / extension push)
*(Not started. The `Span::AddLink` primitive the linking helpers need already exists.)*
- Auto-instrumentation hooks for `std::async`, `std::thread`, coroutines.
- Span linking helpers: `mt::Linked(span)`, `mt::FollowsFrom(span)` for cross-trace relationships.
- `mt::TryCatch(span, lambda)` — wrap a lambda, auto-record exceptions to the span.
- Async-aware Tracer methods that propagate context across coroutine resumption.

### v2.0 (leaf opens new shape)
- **Leaf-side sugar in C macros**, since the leaf is C-only:
  - `MICROTEL_LEAF_TRACE(name)` — start-and-emit shorthand.
  - `MICROTEL_LEAF_ATTR(key, val)` — typed attribute setter.
  - `MICROTEL_LEAF_ERROR(msg)` — error span shorthand.
- The leaf API itself remains primitive; macros provide ergonomic shorthand.

### v3.0 (full conformance)
- Compile-time attribute set definitions (`mt::AttrSet<...>`) for high-cardinality APIs.
- `mt::Result<T>` integration — automatic span attribute capture from result types.
- Reflection-based attribute capture for structured types (using C++26 reflection if available; otherwise compile-time helpers).

---

## 6. Performance and footprint trajectory

The v1.0 footprint targets in [`docs/bench-spec.md`](bench-spec.md) §14.5 are stretch numbers pending prototype. The trajectory across releases:

*(Status: the table below was written assuming shared libraries. microtel ships static archives only (`libmicrotel_*.a` behind `microtel::microtel`). Sizes are now measured per library for the benchmark app, stripped and with every shared library it loads, in [`docs/bench-results/sizes/library-sizes.md`](bench-results/sizes/library-sizes.md): microtel's telemetry stack is 1.2 MB against 13.4 MB for opentelemetry-cpp with its OTLP/gRPC exporter, over the same 12.0 MB of OpenSSL, zlib and the C/C++ runtime. The per-archive figures this table asks for are still unmeasured; ICP 0030 (PR #293) proposes the CI size report that would fill them in.)*

| Release | Core exporter (`libmicrotel-exporter.so`) | Full SDK (`libmicrotel-sdk.so`) | Total dynamic closure |
|---|---|---|---|
| v1.0 | < 800 KB stretch | < 1.5 MB stretch | < 3 MB stretch |
| v1.1 | unchanged | + sugar (header-only-ish) | + spdlog |
| v1.2 | + leaf (experimental): upb < 30 KB / nanopb < 15 KB flash targets | + logs SDK, + concentrator ingest | leaf closure: upb or nanopb, plus libc |
| v1.3 | unchanged | + metrics SDK | unchanged |
| v1.4 | + control plane (separate so) | unchanged | + ctl binary |
| v1.5 | unchanged | + extension surface | + auto-instr packages (separate) |
| v1.6 | refined | refined | + nghttp3 (optional) |
| v2.0 | leaf API stable | unchanged | unchanged |
| v2.1 | nanopb leaf: < 15 KB flash, gated | unchanged | unchanged |
| v3.0 | TBD | TBD (full conformance) | TBD |

**The overarching size discipline:** every minor release publishes its full footprint matrix as part of release notes. Regressions versus the previous release require a documented justification or a fix.

---

## 7. Adoption story progression

How the pitch to potential users evolves:

- **v1.0:** *"OTLP/gRPC and OTLP/HTTP wire compat, no gRPC library, < 3 MB closure."* Best for: embedded Linux, edge, CNF, air-gapped, anyone whose pain point is the gRPC dependency closure.
- **v1.1:** *"…plus a real operational surface — preflight, and hot reload you drive from the admin surface you already have."* Adds: ops-heavy deployments where the gRPC closure isn't the only friction.
- **v1.2:** *"…plus logs with built-in trace correlation, and a leaf library for embedded fleets, down to Cortex-M."* Adds: full-signal users currently running stock OTel-cpp and wanting the footprint reduction, and the constrained-device fleet audience the project's embedded positioning was always aimed at.
- **v1.3:** *"…plus production-quality metrics with explicit cardinality control."* Adds: teams currently using the OTLP/HTTP exporter or Prometheus push gateway and wanting cleaner aggregation.
- **v1.4:** *"…plus out-of-process administration."* Adds: operators who manage many instrumented processes from outside them.
- **v1.5:** *"…plus auto-instrumentation for major libraries and a beta compat shim."* Adds: teams that want to migrate from stock OTel-cpp without code changes.
- **v2.0:** *"…and the leaf API is stable."* Adds: fleet operators who need a compatibility promise before committing firmware to it.
- **v3.0:** *"Full OpenTelemetry SDK, just smaller and faster."* The general pitch.

---

## 8. Anti-goals progression

What we're explicitly **not** doing in each phase:

| Phase | Not doing |
|---|---|
| v1.x core | Windows; full SDK conformance claim; a stable leaf API (experimental from v1.2); control plane (until v1.4); auto-instrumentation (until v1.5) |
| v2.x | Full SDK conformance claim; profiles signal; formal RTOS integrations as core; leaf-side sampling; concentrator-side persistence beyond optional disk queue |
| v3.0 | Nothing meaningful left as anti-goals — coverage is full |
| All phases | Vendor-specific exporters (Datadog, New Relic, etc.) — collectors handle that; semantic-convention helper packages tied to specific OTel spec versions (we stay out of the semconv treadmill in core, may ship as separate optional package) |

---

## 9. Cross-cutting threads

A few themes don't fit a single milestone but progress across releases:

### Wire-protocol freshness
- **OTel spec version:** pinned per release, called out in release notes.
- **opentelemetry-proto pin:** updated quarterly during the v1.x line; per-release after v2.0.
- **Wire-protocol conformance test corpus:** grows release-over-release as new edge cases are caught.

### Performance benchmarks
The `bench/` directory evolves alongside the project:
- **v1.0:** establishes baseline against `opentelemetry-cpp` for traces.
- **v1.2:** adds logs workload profiles. *(Done: `hot-loop-logs`.)*
- **v1.2:** adds leaf footprint measurement (both backends) and concentrator throughput. *(Moved from v2.0. Done: the `leaf-footprint` CI job and the `leaf-fanin` profile.)*
- **v1.3:** adds metrics workload profiles. *(Done early: `hot-loop-metrics`.)*
- **v1.6:** adds high-cardinality, bursty, and outage-recovery scenarios.

### Documentation
- **v1.0:** spec, migration guide, README, compatibility matrix, interop matrix.
- **v1.1:** hot-reload setter guide — what is reloadable, what is not, and why. *(Partial: covered by `examples/hot_reload/README.md` and `docs/control-plane-design.md` §2; no standalone guide.)*
- **v1.2:** logs cookbook with bridge examples. *(Partial: `docs/logs-design.md` and the spdlog, glog and log4cxx adapter READMEs only.)*
- **v1.2:** leaf programming guide, concentrator deployment guide, embedded examples. *(Moved from v2.0. Partial: `examples/leaf/README.md`, `leaf/README.md` and the design doc; no standalone guides.)*
- **v1.3:** metrics design doc (M11 from v1 spec). *(Done.)*
- **v1.4:** control plane operator guide, threat model. *(Not started.)*
- **v1.5:** conformance matrix, extension-author guide, auto-instrumentation cookbook.

### Community and governance
- **Pre-1.0:** small core team; CODEOWNERS for each track.
- **v1.0 → v1.4:** maintain the security policy and a regular release cadence (monthly pre-1.0, quarterly stable).
- **v2.0:** consider donating to a foundation (CNCF if the leaf/concentrator pattern resonates) — depends on adoption signals.
- **v3.0:** stable maintainer model with multiple organizations contributing if the project has reached real adoption.

---

## 10. Tier-promotion gates

A release cannot promote a compatibility tier without meeting the gate.

### Tier 3 experimental → beta (planned for v1.5)

- Compat shims tested against ≥10 representative real-world applications.
- Migration guide validated by ≥5 external users.
- Shim API surface frozen at a specific OTel spec version.
- Documented deprecation policy in place.
- No load-bearing user reports of "I migrated and it broke" without a documented workaround.

### Tier 3 beta → stable (planned for v2.0)

- Two release cycles in beta with no breaking changes.
- ≥3 production users (named or anonymous) using shims at scale.
- Shim API matches the targeted OTel spec version with documented deltas.

### Tier 4 partial → full conformance (planned for v3.0)

- microtel passes the full upstream OpenTelemetry SDK conformance test suite.
- Conformance test runs in CI on every PR.
- Caveats document is short and explicit.
- Sign-off from at least one OTel maintainer or CNCF reviewer (if pursued).

---

## 11. Decision log

Brief notes on decisions whose rationale spans multiple releases and influences the roadmap.

| Decision | Adopted in | Rationale | Affected releases |
|---|---|---|---|
| Exporter-first v1, traces only | v0.9 spec | Scope was too broad to be credible v1; reviewer pushed; user agreed. Made everything else achievable. | v1.0 baseline |
| Traces before metrics | v0.9 spec | Reviewer pushed against v0.8's metrics-first; trace SDK semantics are more contained than metrics aggregation. | v1.0, v1.2 ordering |
| upb over protobuf-cpp / nanopb | v0.7 spec | Order-of-magnitude smaller; pure C; aligns with project ethos. nanopb deferred to v2.1 for tighter MCU tier. | v1.0+ encoding, v2.0/v2.1 leaf |
| Implement gRPC wire on nghttp2 directly | v0.5 spec | gRPC unary is small (~500-800 LOC) on top of HTTP/2; library closure is multi-MB. Whole project rationale. | v1.0+ |
| C++20 floor, devtoolset-11 for RHEL 8 | v0.6 spec, refined v0.7 | `std::span`, concepts, designated initializers; coroutines deferred. C++17 fallback evaluated post-prototype if needed. | All releases |
| Internal logging via spdlog with `MICROTEL_USE_SPDLOG=OFF` fallback | v0.7 spec, build flag added v0.10 | Standard, header-only, std::format mode avoids fmt dep. Minimal stderr fallback for embedded/constrained. | All releases |
| Config precedence: code > env > file > defaults | v0.10 spec | Containerized-deployments standard; OTel SDK convention. | All releases |
| Compat shims experimental, separate package | v0.9 spec | Don't make shim correctness a v1.0 release blocker. Promote to beta in v1.4 once tested. | v1.0 (experimental), v1.4 (beta), v2.0 (stable) |
| Sugar in v1.1, not v1.0 | v0.9 spec | v1.0 messaging is "drop-in for OTel-cpp users"; sugar layer competes with that pitch. Sugar arrives once core is proven. | v1.1+ |
| Control plane in v1.1, not v1.0 | v0.9 spec | Adds Unix socket server, JSON wire, CLI, REPL, attack surface, threat model. Too much for v1.0 alongside transport correctness. | v1.1+ |
| Leaf encoder is upb first, nanopb later | v0.5 spec | Larger embedded targets are most of the addressable audience and reuse microtel's existing encoder closure. nanopb adds reach to true MCU class. | v2.0, v2.1 |
| Control-plane socket in v1.2, not v1.1; v1.1 hot reload ships as public setters | [ICP 0024](icps/0024-v1.1-rescope.md) | Reverses the row above's release target. Four knobs are the whole user-visible capability, and thread-safe `Provider` setters deliver them with no socket, parser, fourth thread, signal handler, or threat model. Out-of-process administration is the part that waits for real deployment feedback. | v1.1, v1.2 |
| Metrics, logs and the otel-cpp shim built ahead of their themes, shipped as experimental | M12–M17 | The code was ready before the release themes that name it. Shipping it marked experimental lets people use it now, while the v1.2 and v1.3 themes keep the job of stabilizing it: closing the gaps, adding conformance tests, and making the compatibility promise. | v1.2, v1.3, Tier 3 |
| Leaf / concentrator in v1.3 as experimental, with upb **and** nanopb leaf backends | [ICP 0031](icps/0031-leaf-concentrator-in-v1.3.md) | Reverses the v1.0 design's release target for the leaf (v2.0) and the "upb first, nanopb later" row above. The leaf is the strongest pitch for IoT fleets, and most of those devices are Cortex-M, which only nanopb reaches. Nothing in the design needed a major version: the receiver and enrichment hooks never existed as internal interfaces, so they arrive as new API. v2.0 becomes the release where that API goes stable. | v1.3, v2.0, v2.1 |
| v1.1.1 patch for the concentrator's prerequisites; release order becomes v1.2 logs + leaf, v1.3 metrics, v1.4 control plane, v1.5 conformance push, v1.6 performance | [ICP 0032](icps/0032-release-reorder-v1.1.1.md) | Only #257 and #222 block the concentrator, so they ship first as a patch. Metrics and the control plane share no code and have very different amounts of work left, so they no longer share a release. Earlier rows keep the numbers they were decided under. | v1.1.1–v1.6 |

This log is appended to, never rewritten. When a decision is reversed, the original entry stays and a new entry records the reversal with rationale — the control-plane deferral is the first.

---

## 12. Open roadmap questions

1. **OTel Profiles signal — when?** Stabilizes upstream first; microtel adds support after that. Currently scheduled for v3.0 but could land earlier if upstream ships sooner and adoption pressure is real.
2. **Foundation donation — CNCF?** Depends on adoption. v2.0 is a natural inflection point if the leaf/concentrator pattern resonates with embedded and edge audiences.
3. **Native API vs OTel API in v3.0.** Real choice: either native `microtel::*` API stays first-class with shims as alternative, or microtel commits fully to OTel API surface and natives become a thin convenience layer. Discussion happens during v2.x; decision lands in v3.0 design doc.
4. **Windows support — ever?** Currently a hard non-goal. Could land in v3.x if there's real demand. The transport layer abstraction makes it possible; the work is in IOCP-based I/O and Windows packaging.
5. **HTTP/3 graduation.** Experimental in v1.6. Could stabilize in v2.0 or stay experimental indefinitely depending on real-world signal.
6. **HTTP/1.1 for plaintext OTLP/HTTP — worth it?** microtel is HTTP/2-only, so a plaintext `http://` endpoint is h2c with prior knowledge and cannot reach an HTTP/1.1-only receiver — including the OpenTelemetry Collector's own plaintext `:4318`. v1.0 deliberately **documents** this rather than fixing it: `Build()` warns, `Connect()` fails with a targeted message, and `docs/compatibility-matrix.md` §4 marks it unsupported. The OTLP specification does permit HTTP/1.1, so a fallback is implementable — but it is a second request path through the transport, with its own framing, chunking, and connection reuse, for a configuration whose two working alternatives (`https://`, or OTLP/gRPC) are each a one-line change. Revisit if real deployments turn up where neither is available. Would need an ICP. Issue #166.

---

## 13. References

- [`docs/README.md`](README.md) (the design documents)
- `docs/bench-spec.md` (benchmark harness)
- [OpenTelemetry Specification](https://opentelemetry.io/docs/specs/otel/)
- [OpenTelemetry SDK Specification](https://opentelemetry.io/docs/specs/otel/sdk/)
- [OpenTelemetry Collector](https://github.com/open-telemetry/opentelemetry-collector)
