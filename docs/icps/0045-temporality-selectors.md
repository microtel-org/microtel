# ICP 0045: Temporality chosen per instrument kind

**Status:** Accepted — 2026-11-27. Milestone v1.3; tracking issue #455.
**Affected interfaces / docs:**
- `include/microtel/internal/metric_producer.hpp`: `IMetricProducer::Collect` takes a `CollectRequest` holding a `TemporalitySelector`, not one `AggregationTemporality` (**locked internal interface**)
- `include/microtel/internal/metric_batch.hpp`: new `TemporalitySelector` type, next to `AggregationTemporality`
- `include/microtel/sdk_builder.hpp`: Doxygen only, for the `Delta` and `LowMemory` enumerators of `TemporalityPreference`; the enum's values are unchanged
- `docs/metrics-design.md` §1, `docs/configuration.md` §3.13
- code: `src/sdk/metric_producer.{hpp,cpp}`, `src/sdk/metric_stream.hpp` and the streams that implement it, `src/sdk/periodic_exporting_metric_reader.{hpp,cpp}`, `src/sdk/sdk_provider.cpp`
- tests: the unit tests that call `Collect(AggregationTemporality)` (`tests/unit/sdk/metric_*_test.cpp`, `periodic_exporting_metric_reader_test.cpp`); a new selector test; a conformance case per preset

**Affected tracks:** A (SDK). The encoder (F) is unchanged: it already writes each record's own temporality.

## Summary

The `Delta` and `LowMemory` presets choose temporality per instrument kind, as the OTLP exporter spec defines them. `Collect` takes a request carrying a kind-to-temporality selector, and each stream resolves its own temporality from it.

## Motivation

Today `SdkProvider` turns the preference into one `AggregationTemporality`, and `Collect` applies it to every stream. Both `Delta` and `LowMemory` therefore export everything as delta. The OTLP exporter spec ([`sdk_exporters/otlp.md`](https://github.com/open-telemetry/opentelemetry-specification/blob/main/specification/metrics/sdk_exporters/otlp.md), `OTEL_EXPORTER_OTLP_METRICS_TEMPORALITY_PREFERENCE`) defines them per kind:

| Instrument kind | `Cumulative` | `Delta` | `LowMemory` |
|---|---|---|---|
| Counter, Histogram, ExponentialHistogram | Cumulative | Delta | Delta |
| ObservableCounter | Cumulative | Delta | **Cumulative** |
| UpDownCounter, ObservableUpDownCounter | Cumulative | **Cumulative** | **Cumulative** |
| Gauge, ObservableGauge | n/a | n/a | n/a |

Microtel exports up-down counters as delta under both presets, and observable counters as delta under `LowMemory`, where an OTel SDK would not. One `AggregationTemporality` cannot express the table, so the parameter of a locked interface has to change.

## Decision

1. **`TemporalitySelector`** (`internal/metric_batch.hpp`): a value type holding one `AggregationTemporality` per `microtel::InstrumentKind`, with `[[nodiscard]] AggregationTemporality For(InstrumentKind) const noexcept`. Three built-in selectors, `TemporalitySelector::Cumulative()`, `Delta()` and `LowMemory()`, are `constexpr` and encode the table above. Gauges map to `Cumulative` in all three: OTLP `Gauge` has no temporality, and the gauge storages already ignore the value.
2. **`IMetricProducer::Collect(const CollectRequest& request = {})`** replaces `Collect(AggregationTemporality)`. `CollectRequest` is an aggregate in `internal/metric_producer.hpp`, next to `Collect`:
   ```cpp
   struct CollectRequest
   {
       TemporalitySelector selector = TemporalitySelector::Cumulative();
   };
   ```
   Callers name what they set, as in `Collect({.selector = TemporalitySelector::Delta()})`. A later ICP that adds a field to the request gives it a default, so existing calls and the signature stay as they are ([ICP 0046](0046-async-callback-deadline.md) adds `callback_deadline` this way). `Collect` is still single-caller, on the reader thread, and not `noexcept`.
3. **Each stream resolves its own temporality.** `IMetricStream::Collect` (in `src/sdk`, not locked) takes the selector, `request.selector`; streams never see the whole request. A stream knows its kind from creation and calls `selector.For(m_kind)`. The producer does not resolve temporality itself.
4. **The periodic reader holds a selector.** `PeriodicExportingMetricReader` stores a `TemporalitySelector` in place of an `AggregationTemporality` and passes it in each `CollectRequest`. `SynchronousMetricReader` keeps calling `Collect()` with the default request. `SdkProvider` maps `TemporalityPreference` to the matching built-in selector, replacing `ToAggregationTemporality`.
5. **Room for #299 without changing `Collect` again.** Per-instrument or per-View temporality (#299, after v1.3) becomes an optional override that a stream holds and resolves as `override.value_or(selector.For(m_kind))`. Neither `Collect` signature changes for it, and neither does `CollectRequest`.
6. **No public API change.** `TemporalityPreference` keeps its enumerators and values, and `WithMetricTemporality` and its env var are unchanged. Only the Doxygen for `Delta` and `LowMemory` changes, to state the table.

## Migration

- **Users of `Delta` or `LowMemory`:** up-down counters (sync and observable) now export as cumulative. Under `LowMemory`, observable counters also export as cumulative. Backends see `aggregationTemporality: 2` on those series where they saw `1`. Metrics are experimental until v1.3, so this is outside any compatibility promise, but the v1.3 release notes say it.
- **`Cumulative` users:** no change.
- **Contributors:** tests that call `Collect(AggregationTemporality::Delta)` call `Collect({.selector = TemporalitySelector::Delta()})`. Tests that need every stream as delta, whatever its kind, build a selector with all kinds set to delta.

## Rationale & alternatives

- **Resolve in the producer** (`stream->Collect(selector.For(stream->Kind()))`). Rejected: #299's per-instrument override would then need the producer to know about overrides, or `IMetricStream::Collect` would change again.
- **A virtual selector interface or `std::function`.** Rejected: three fixed presets plus a stream-held override cover every case on the roadmap. A table is `constexpr`, never allocates and needs no lifetime rules.
- **Pass `TemporalityPreference` itself to `Collect`.** Rejected: it would tie the internal interface to the public enum's three values, and #299 needs a selector that is not one of them.
- **Keep `Collect` and apply the preset only in `SdkProvider`.** Not possible: one temporality per collection cannot express the table.
- **Pass the selector as its own parameter, `Collect(const TemporalitySelector&)`.** Rejected in review: ICP 0046 needs a deadline on the same call, and each further input would change a locked signature again. A `CollectRequest` aggregate takes new fields with defaults, and designated initializers keep call sites readable.
