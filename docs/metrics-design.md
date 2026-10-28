# microtel Metrics Design

**Status:** Accepted — M11 signed off 2026-07-09. M12 implementation complete
(PRs #75–#80).

This document settles the semantic decisions that v1.0 deliberately deferred so
that the metrics signal (v1.2) can be implemented against a locked design, the
same way `docs/interfaces.md` locked the trace contracts before M3. It is a
design document, not an interface header: it decides *what* and *why*; the
concrete `include/microtel/` headers it implies are drafted during M12 and, if
they touch any locked surface, follow the ICP process.

Each section now carries a concrete **Decision**. Defaults follow the
OpenTelemetry SDK spec unless a project constraint (footprint, the existing
trace conventions) dictates otherwise; where that's the case it is called out.

## Scope

In scope for v1.2 (this doc decides all of these):

- Synchronous instruments: `Counter`, `UpDownCounter`, `Gauge`, `Histogram`.
- Asynchronous instruments: `ObservableCounter`, `ObservableUpDownCounter`,
  `ObservableGauge`.
- `Meter` surface and lifecycle (acquired from the existing `Provider`).
- `MetricReader` / `MetricExporter` pipeline reusing the OTLP encoder
  (`src/wire/encoder/`) and nghttp2 transport (`src/transport/`).
- Aggregation temporality (delta / cumulative), per-metric configurable.
- Cardinality limits with the OTel overflow attribute and drop accounting.
- Views (basic: rename, attribute filter, aggregation override).
- Exemplars linked to active span context.
- `mt::Timer` sugar wired into histograms.

Explicitly **out of scope** for v1.2 (anti-goals):

- Logs (v1.3).
- Advanced exemplar storage/formats beyond span-context linkage.
- Metric backfill / persistence.
- Full Views (regex matchers, full selector grammar) — deferred to v1.4.

## Sign-off checklist

Each item has a proposed **Decision** below. Reviewer approves by checking every
box (and editing any decision they want changed first).

- [x] §1 Aggregation temporality — **default cumulative**, per-metric override
- [x] §2 Cardinality limits — default 2000, OTel overflow attribute, new `DropReason` (**ICP**)
- [x] §3 Histogram buckets — **explicit + exponential**, configurable
- [x] §4 Async-instrument callback semantics
- [x] §5 MetricReader / MetricExporter interaction
- [x] §6 Views — selector surface, transforms, conflict rules
- [x] §7 Exemplars — **in for v1.2**, reservoir + span-context linkage
- [x] §8 Instrument API surface (`Provider::GetMeter` + the seven instruments)
- [x] §9 Memory & threading model
- [x] §10 Wire mapping (OTLP metrics protobuf; encoder/transport reuse)
- [x] §11 Configuration & compatibility

---

## §1 Aggregation temporality

**Context.** OTLP supports DELTA and CUMULATIVE temporality. The choice affects
collector compatibility, exporter memory (cumulative retains per-series state),
and counter-reset semantics.

**Decision.**
- **Default: CUMULATIVE for all instrument kinds.** Matches the OpenTelemetry SDK
  default and is accepted by every OTLP backend without reconfiguration — the
  safest default for an exporter-first project whose headline claim is wire
  compatibility.
- The **delta path is fully implemented** and selectable per-reader via a
  temporality preference (`cumulative` | `delta` | `lowmemory`, where
  `lowmemory` = delta for Counter/Histogram, cumulative for UpDownCounter — the
  OTel-defined preset). Per-View aggregation override can also force temporality
  on a specific instrument.
- Honor `OTEL_EXPORTER_OTLP_METRICS_TEMPORALITY_PREFERENCE`.
- **Point timestamps** (`time_unix_nano`, `start_time_unix_nano`) follow the
  OTel SDK "Start timestamps" rules; [ICP 0039](icps/0039-metric-start-timestamps.md)
  records them.

## §2 Cardinality limits

**Context.** Unbounded attribute combinations are the dominant metrics failure
mode. OTel defines an overflow attribute (`otel.metric.overflow=true`) once a
limit is hit.

**Decision.**
- **Default limit: 2000 distinct attribute sets per instrument** (OTel spec
  default), configurable.
- On overflow, measurements collapse into a single overflow series carrying
  `otel.metric.overflow=true` (per OTel) rather than being dropped outright.
- **Drop accounting requires a new `DropReason::CardinalityOverflow`.**
  `DropReason` is the **ICP-gated** public health array in
  `include/microtel/provider.hpp` (its enumerator order is part of the
  `HealthSnapshot::drop_counters` indexing contract), so M12 must land an ICP
  adding this (and any other metric drop reasons) before implementation.
- Configured via a `MetricLimitOptions` struct on `SdkBuilder`, mirroring the
  existing `SpanLimitOptions`.

## §3 Histogram bucket configuration

**Decision.**
- **Ship both explicit-bucket and base-2 exponential histograms** in v1.2 (full
  OTel histogram parity).
- **Explicit** default boundaries = the OTel default ladder
  `[0, 5, 10, 25, 50, 75, 100, 250, 500, 750, 1000, 2500, 5000, 7500, 10000]`,
  overridable per-View.
- **Exponential** defaults: `max_scale = 20`, `max_buckets = 160` (OTel
  defaults), with automatic downscale when the bucket budget is exceeded.
- Instrument advice / View selects which aggregation a given histogram uses;
  explicit is the default when unspecified.

## §4 Async-instrument callback semantics

**Context.** Observable instruments report via callbacks invoked at collection
time. Threading and re-entrancy must be pinned given the strict threading model.

**Decision.**
- Callbacks are registered at instrument creation, and additionally via
  `Meter::RegisterCallback(...)` which returns an **RAII registration handle**
  (move-only; unregisters on destruction — consistent with the project's RAII
  rules). **Status: planned, not built.** `Meter::RegisterCallback` does not
  exist; callbacks are given only at instrument creation.
- Callbacks run **on the MetricReader's collection thread, never on a caller
  hot path.** They are serialized per collection cycle and bounded by a
  per-collection deadline; a callback that exceeds it is abandoned, its
  measurements for that cycle dropped, and a diagnostic incremented.
- The measurement-recording API exposed to a callback is `noexcept`; an
  exception escaping user callback code is caught at the boundary and counted
  (no `catch(...)` — caught as `std::exception`, matching the existing exporter
  boundary).
- **No re-entrancy:** a callback must not record into the same `Meter` it is
  serving (documented contract; not enforced in v1.2).
- Duplicate measurements for the same attribute set within one callback →
  last-write-wins + diagnostic.

## §5 MetricReader / MetricExporter interaction

**Decision.**
- **`PeriodicExportingMetricReader` is the default** — push model, default
  interval **60s** (OTel default), configurable; honor
  `OTEL_METRIC_EXPORT_INTERVAL`. Manual/pull collection is available via an
  explicit `Collect()` for tests and synchronous flush.
- `ForceFlush` / `Shutdown` propagate reader → exporter and reuse the existing
  `Status` taxonomy in `include/microtel/status.hpp`
  (`Completed`/`TimedOut`/`AlreadyShutDown`/`Failed`). `ForceFlush` collects
  once, then waits for the batches queued before the exporter's flush, which
  end with that collection, not for the exporter's queue to empty; a
  collection the exporter refuses to queue makes it `Failed`
  ([ICP 0041](icps/0041-flush-and-shutdown-deadlines.md)).
- **The metrics pipeline is separate from the span `BatchSpanProcessor`** (its
  own collection + serialization path) but **shares the nghttp2 transport
  connection** to the endpoint. Rationale: metrics collection is pull-at-interval
  (reader-driven), spans are push-at-enqueue (processor-driven) — different
  lifecycles, same socket.

## §6 Views

**Decision.**
- v1.2 selector surface: **instrument name** (with a single trailing `*`
  wildcard), **instrument kind**, and **meter name/version**. Full regex /
  selector grammar deferred to v1.4.
- Transforms: **rename**, **attribute-key allow-list filter**, **aggregation
  override** (including explicit-bucket boundaries, exponential params, or
  `drop`).
- Conflict rule: every matching View produces a stream (an instrument can feed
  multiple streams). When two streams collide on identical
  (name, unit, attributes), emit both and raise a **conflict diagnostic** — per
  OTel guidance — rather than silently dropping one.

## §7 Exemplars

**Context.** Exemplars sample raw measurements and attach the trace context of
the span active at record time, letting a backend jump from a metric to an
example trace.

**Decision.**
- **In for v1.2.** Reservoirs follow the OTel defaults:
  - Histograms: `AlignedHistogramBucketExemplarReservoir` — one exemplar slot
    per bucket.
  - Sum / Gauge: `SimpleFixedSizeExemplarReservoir` — fixed size (default = number
    of CPU cores, capped, per OTel) with reservoir sampling.
- **Default filter = `trace_based`** (OTel default): an exemplar is recorded only
  when a *sampled* span is active in the current `Context`. Honor
  `OTEL_METRICS_EXEMPLAR_FILTER` (`always_off` | `always_on` | `trace_based`).
  **Status: planned, not built.** `OTEL_METRICS_EXEMPLAR_FILTER` is not read;
  the filter is always `trace_based`.
- Captured fields: `trace_id`, `span_id`, timestamp, value, and any filtered
  attributes.
- **Dependency:** exemplars read the current span from the thread-local
  `Context`. That machinery landed in v1.1 packet 2.3b
  ([ICP 0025](icps/0025-propagation-core.md) §3, issue #221):
  `sdk::CurrentSpanSource` applies the `trace_based` filter — it returns the
  current `SpanContext` only when it is valid **and** sampled — and
  `SdkProvider` hands it to every `SdkMeter`, which sets it as
  `StorageOptions::span_source` on every stream it registers. The seam is no
  longer `nullptr`; `tests/integration/sdk/exemplar_wiring_test.cpp` asserts
  exported exemplars carry the enclosing span's real trace and span ids.

## §8 Instrument API surface

**Decision.**
- **No separate `MeterProvider`.** Extend the existing `Provider` with
  `GetMeter(name, version)` returning `std::shared_ptr<Meter>`, mirroring
  `Provider::GetTracer` exactly (same ownership and lifetime contract).
- `Meter` factory methods, value type `T ∈ {std::int64_t, double}`:
  - Sync: `CreateCounter<T>`, `CreateUpDownCounter<T>`, `CreateGauge<T>`,
    `CreateHistogram<T>`.
  - Async: `CreateObservableCounter<T>(callback)`,
    `CreateObservableUpDownCounter<T>(callback)`,
    `CreateObservableGauge<T>(callback)`.
- Record API (hot path, `noexcept`, drop-and-count on failure — same contract as
  the span API in `src/api/README.md`): `Counter<T>::Add(value, AttributeSpan)`,
  `Histogram<T>::Record(value, AttributeSpan)`, etc.
- `mt::Timer` sugar: RAII; records elapsed wall-time to a bound `Histogram` on
  destruction (completes the v1.1-deferred sugar piece).
  **Status: planned, not built.** There is no `mt::Timer`.

## §9 Memory & threading model

**Decision.**
- Hot-path `Add`/`Record` are `noexcept` and allocate nothing in steady state:
  aggregation state for a known (instrument, attribute-set) is updated in place.
  A *new* attribute set allocates once, bounded by the §2 cardinality limit.
- Aggregation storage: a per-instrument map keyed by attribute-set, guarded by a
  **per-instrument mutex** for v1.2. Sharded/striped locking is an explicit
  follow-up if benchmarks show contention — same "measure before optimizing"
  stance as ICP 0003's span-allocation decision; not premature in v1.2.
- A per-metrics memory budget is added to `MemoryLimitOptions` (additive public
  field — no ICP needed, unlike the `DropReason` change in §2).
  **Status: planned, not built.** `MemoryLimitOptions` has no metrics field.
- Collection (callback invocation + snapshot) runs on the reader thread; caller
  threads only touch the live aggregation state under the per-instrument lock.

## §10 Wire mapping

**Decision.**
- Extend the upb `OtlpEncoder` (`src/wire/encoder/`) with the OTLP metrics
  messages: `MetricsData` / `ResourceMetrics` / `ScopeMetrics` / `Metric` and the
  point types `Sum`, `Gauge`, `Histogram`, `ExponentialHistogram`, plus
  `Exemplar`. The encoder remains the **only** file that touches upb (per the
  CLAUDE.md rule).
- Resource and instrumentation-scope resolution reuse the trace path unchanged.
- **No new transport surface:** `MetricsService/Export` rides the existing
  OTLP/HTTP-protobuf and OTLP/gRPC (unary-RPC-over-nghttp2) paths; HTTP and gRPC
  reach parity automatically because they share the transport.

## §11 Configuration & compatibility

**Decision.**
- `SdkBuilder` additions: `WithPeriodicMetricReader(interval)` /
  `WithMetricReader(...)`, `WithMetricTemporality(pref)`,
  `WithMetricLimits(MetricLimitOptions)`, `WithView(...)`,
  `WithExemplarFilter(...)`.
  **Status: planned, not built.** `WithPeriodicMetricReader`,
  `WithMetricReader` and `WithExemplarFilter` do not exist; the interval is
  set with `WithMetricInterval(interval)`.
- Env vars honored: `OTEL_METRIC_EXPORT_INTERVAL`,
  `OTEL_EXPORTER_OTLP_METRICS_TEMPORALITY_PREFERENCE`,
  `OTEL_METRICS_EXEMPLAR_FILTER`.
- Compatibility tiers (`docs/compatibility-matrix.md` §7): **Tier 1 and Tier 2 advance to
  include metrics**; the Tier 3 shim gains metric instruments (still
  experimental). The interop matrix (`.github/workflows/interop.yml`) gains a
  metrics-export check against otel-collector.

---

## Open items flagged for the reviewer

- **§2 ICP:** adding `DropReason::CardinalityOverflow` (and any sibling metric
  drop reasons) is an interface change to the locked `provider.hpp` health
  surface — must be PR'd as an ICP at the start of M12.
- **§7 dependency:** exemplars require the thread-local `Context` / current-span
  machinery that `tracer.hpp` defers to v1.1. M12 cannot start exemplars until
  that lands.
- **§9 lock strategy:** per-instrument mutex chosen for simplicity; flagged as a
  benchmark-driven revisit, not a locked decision.

## References

- `docs/roadmap.md` §1.2 — v1.2 scope; M11 prerequisite.
- `docs/compatibility-matrix.md` §7 — compatibility tiers. (The M11/M12
  milestone table and the "one signal at a time" rule were in the v1.0 design
  specification, retired by ICP 0037.)
- `docs/interfaces.md` — the locked trace contracts this doc parallels for metrics.
- `docs/error-model.md` — `DropReason` / `Status` taxonomy to extend.
- `docs/memory-model.md`, `docs/threading-model.md` — hot-path and allocation rules.
- `docs/icps/0003-m0-deferred-decisions.md` — precedent for "measure before
  optimizing" deferrals (§9 lock strategy).
- OpenTelemetry Metrics SDK & data-model specification (pinned version: _TBD —
  set alongside the `opentelemetry-proto` tag in M12_).
