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
