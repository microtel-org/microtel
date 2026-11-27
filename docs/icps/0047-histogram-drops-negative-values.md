# ICP 0047: Histogram instruments drop negative values

**Status:** Accepted — 2026-11-27. Milestone v1.3; tracking issue #458.
**Affected interfaces / docs:**
- `include/microtel/provider.hpp`: append `DropReason::NegativeHistogramValue = 27`; `kDropReasonCount` 27 → 28 (**public health surface**)
- `include/microtel/meter.hpp`: Doxygen for `Histogram::Record` and `ExponentialHistogram::Record`
- `src/sdk/drop_reason_names.hpp`: the `"negative_histogram_value"` arm
- `docs/error-model.md` §3 (a new row), `docs/metrics-design.md` §3, `docs/troubleshooting.md` (the metrics drop counters)
- code: `src/sdk/metric_histogram_storage.cpp`, `src/sdk/metric_exp_histogram_storage.cpp`
- tests: `tests/unit/sdk/drop_reason_test.cpp`; the two histogram storage tests; `examples/health_and_backpressure/main.cpp`, whose switch over `DropReason` is exhaustive

**Affected tracks:** A (SDK). The encoder is unchanged.

## Summary

A Histogram or ExponentialHistogram instrument drops a negative value, counts it as `NegativeHistogramValue` and logs it with rate limiting. This is a behaviour change: today the value is accepted into the sum.

## Motivation

`HistogramStorage::Record` and `ExponentialHistogramStorage::Record` reject only non-finite values. A negative value goes into `sum` and `min`, and into the lowest bucket (explicit) or the negative buckets (exponential). The OTel API says a Histogram's value is expected to be non-negative, and leaves validation to the implementation. The SDK spec says a histogram should not collect `sum` from instruments that record negative measurements. Today one bad call makes a series' `sum` wrong for the rest of its life.

## Decision

1. **Drop it.** In both storages, after the existing non-finite check and before any exemplar or aggregation work, a value `< 0` is dropped. `-0.0` is not less than zero, so it is recorded as zero, as now. This applies to `std::int64_t` and `double` instruments alike.
2. **Count it.** Each dropped value records one `DropReason::NegativeHistogramValue`, appended as value 27, so `kDropReasonCount` becomes 28. Like `NonFiniteValue`, it is counted per measurement.
3. **Log it** through the diagnostic log's existing per-`(level, reason)` rate limiter (`docs/error-model.md`, "Rate limiting (LOCKED)"), at `warn`, naming the instrument.
4. **Document it** on `Histogram::Record` and `ExponentialHistogram::Record`: "`value` must be non-negative. A negative value is dropped and counted as `NegativeHistogramValue`." `metrics-design.md` §3 states the rule.
5. **Scope: the instruments only.** Observable instruments, counters and gauges are unchanged. The exponential histogram's negative buckets stay in the storage and the encoder. They become reachable again when a View aggregates an up-down counter or gauge as a histogram (#298), and that case, where `sum` is omitted, is #554.
6. **The otel-cpp shim** forwards `Record` to the microtel instrument, so it drops and counts in the same way. Its own drop accounting (ICP 0016) is unchanged.

## Migration

- **Applications that record negative values into a Histogram:** those values stop appearing in `sum`, `count`, `min` and the buckets, and `NegativeHistogramValue` counts them. Metrics are experimental until v1.3, so this is outside any compatibility promise, but the v1.3 release notes say it. A signed quantity belongs on an UpDownCounter or a Gauge.
- **Consumers iterating `HealthSnapshot::drop_counters`:** one more slot. As with ICP 0011, the addition is append-only and existing indices don't move. A `switch` over `DropReason` with no `default` needs a new arm.

## Rationale & alternatives

- **Stop exporting the sum for a series that has seen a negative value.** Rejected. One bad value would change a series' shape partway through its life, which backends and dashboards handle badly. It also needs per-series state, and it leaves `min` and the buckets carrying the bad value.
- **Clamp to zero.** Rejected: it records a value the caller never gave.
- **Reuse `NonFiniteValue`.** Rejected: an operator who sees the counter rise needs to know which caller error to look for.
- **Fold this into ICP 0046.** Rejected: the two are separate decisions about separate code, and this one changes the public health surface on its own.
