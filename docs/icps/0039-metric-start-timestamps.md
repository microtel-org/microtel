# ICP 0039: Metric data-point time and start time

**Status:** Accepted — 2026-10-26.
**Affected interfaces / docs:**
- `src/sdk/metric_stream.hpp` (`IMetricStream::Collect` takes the collection time)
- `src/sdk/metric_{sum,gauge,histogram,exp_histogram}_storage.{hpp,cpp}`,
  `src/sdk/metric_observable_instruments.hpp`, `src/sdk/metric_producer.cpp`
- `docs/metrics-design.md` §1 (a pointer to this ICP)

**Affected tracks:** SDK (metrics). No public header, wire codec or encoder
change.

## Summary

Every metric data point carries `time_unix_nano` and, except gauges,
`start_time_unix_nano`, set by the rules below. Before this, no storage set
either, so every point was exported with both at 0 (#448).

## Decision

`metrics-design.md` and the earlier ICPs define no start time, so microtel
follows the OpenTelemetry metrics SDK specification, section
["Start timestamps"](https://github.com/open-telemetry/opentelemetry-specification/blob/5740e9af3c82/specification/metrics/sdk.md#start-timestamps)
(read at commit `5740e9af3c82`). That section is marked **Development**; if it
changes, this ICP is the place to record the follow-up.

- **`time`** is the collection time. `MetricProducer::Collect` reads the
  clock once and passes that one value to every stream, so all points of one
  collection share it.
- **Delta** points start at the previous collection's time, or at the
  instrument's creation on its first collection. Every delta point of an
  instrument shares that start, and successive windows chain with no gap,
  including across a window with no measurements.
- **Cumulative, synchronous** (Counter, UpDownCounter, Histogram,
  ExponentialHistogram): a series starts at its first measurement and keeps
  that start for every later collection. The clock is read once, when the
  series is created.
- **Cumulative, observable** (ObservableCounter, ObservableUpDownCounter): a
  series starts at the collection before the one in which it was first
  observed, which is the instrument's creation if that was the first
  collection.
- **Gauges** (Gauge, ObservableGauge) carry `time` only. OTLP leaves a gauge's
  start time optional, and the specification's rules are written for
  cumulative and delta series.

The previous collection's time is tracked per storage (per observable
stream), which is per instrument and reader: microtel has one metric reader
per `Provider`.

## Consequences

- `IMetricStream::Collect` gains a `std::chrono::system_clock::time_point`
  parameter. A non-virtual `Collect(temporality)` forwards with `now()`, so
  callers that don't care keep compiling. The storages take the time as a
  defaulted trailing parameter.
- Tests: `tests/unit/sdk/metric_timestamps_test.cpp`. The metrics
  conformance suite (pre-launch PR) adds `0 < start <= time` against the
  collector on both protocols.

## Amendment (2026-10-26): observable sums under delta (#450, #453)

The rules above cover timestamps. #453 also fixed the *values* an
ObservableCounter or ObservableUpDownCounter reports under delta temporality,
which were cumulative totals labelled as delta. The specification states the
goal but not three of the cases, so this records what microtel chose.

What the SDK specification says (`sdk.md` at the commit above):

- `## MetricReader`: "For asynchronous instruments with Delta temporality,
  this means converting Cumulative to Delta aggregation temporality", and "For
  asynchronous instruments with Delta or Cumulative aggregation temporality,
  MetricReader.Collect MUST only receive data points with measurements recorded
  since the previous collection."
- `### Observations inside asynchronous callbacks`: "The implementation SHOULD
  NOT produce aggregated metric data for a previously-observed attribute set
  which is not observed during a successful callback."

Neither section says how the conversion handles the cases below. The
[supplementary guidelines](https://github.com/open-telemetry/opentelemetry-specification/blob/5740e9af3c82/specification/metrics/supplementary-guidelines.md#asynchronous-example-delta-temporality)
("Asynchronous example: Delta temporality") show the first two only as worked
values, and don't cover the third:

- **First observation of a series: reported in full.** The example reports
  pid 1001's first value, 50, as its first delta.
- **A series a collection doesn't observe is forgotten.** It gets no point
  (the "SHOULD NOT" above), and its last value is dropped. When it is observed
  again it is a new series, and its first value is reported in full. The
  example's restarted pid 1001 reports 10, its own count, not a difference from
  the old process. Under cumulative temporality too, a returning series is
  new: it starts at the collection before it came back, which tells a backend
  its value restarted. The stored values (`m_previous`) and the per-series
  start times both hold only the series observed in the last collection, so
  neither grows past the cardinality limit plus the overflow series.
- **A monotonic counter whose value goes down has reset.** Its new value is
  the delta, so a monotonic sum never reports a negative delta. Not addressed
  by either document; it is how Prometheus `rate()` treats a counter that
  goes down. An up-down counter is not
  monotonic, so it reports the change, negative or not.

Cumulative collections report the observation unchanged, and their starts
follow the rules above, a returning series counting as new. Tests:
`tests/unit/sdk/metric_observable_delta_test.cpp`, and
`ObservableSumCumulative_ReturningSeriesStartsAfresh` in
`tests/unit/sdk/metric_timestamps_test.cpp`.
