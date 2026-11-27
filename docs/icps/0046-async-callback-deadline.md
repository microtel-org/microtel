# ICP 0046: A per-collection deadline for async callbacks

**Status:** Draft — 2026-11-26. Milestone v1.3; tracking issue #237.
**Affected interfaces / docs:**
- `include/microtel/internal/metric_producer.hpp`: `IMetricProducer::Collect` gains a deadline parameter (**locked internal interface**; it builds on [ICP 0045](0045-temporality-selectors.md)'s signature)
- `include/microtel/provider.hpp`: Doxygen only, for `DropReason::MetricCallbackTimeout`; its value (21) and `kDropReasonCount` are unchanged
- `docs/metrics-design.md` §4, `docs/error-model.md` §3 (`metric_callback_timeout` loses *(not yet produced)*)
- code: `src/sdk/metric_producer.{hpp,cpp}`, `src/sdk/metric_stream.hpp`, `src/sdk/periodic_exporting_metric_reader.{hpp,cpp}`
- tests: new unit tests in `tests/unit/sdk/` for the cases below; the existing producer and reader tests move to the new signature

**Affected tracks:** A (SDK). The encoder and the transport are unchanged.

## Summary

Each collection has a deadline: the export interval, capped by the time the caller has left. Once it passes, the remaining async callbacks are skipped for that collection, counted as `MetricCallbackTimeout` and logged with rate limiting. Their series keep their state.

## Motivation

`metrics-design.md` §4 promises a per-collection deadline for async callbacks, and ICP 0008 reserved `MetricCallbackTimeout` for it, but nothing enforces one. A slow callback holds up every callback after it, and the export behind them. A reader `ForceFlush(timeout)` or `Shutdown(timeout)` can then run past the time it was given, which breaks the shared-deadline rule of [ICP 0041](0041-flush-and-shutdown-deadlines.md).

## Decision

1. **The deadline.** A collection starts at `t0`, and its deadline is `t0 + interval`, where `interval` is the reader's current export interval (`OTEL_METRIC_EXPORT_INTERVAL`, `WithMetricInterval`, `SetMetricInterval`). The reader has no collection or export timeout of its own (`OTEL_METRIC_EXPORT_TIMEOUT` is not implemented), so no new setting is added.
2. **Capped by the caller's time.** A collection run for `ForceFlush(timeout)`, `Shutdown(timeout)` or `Collect(timeout)` has the deadline `min(t0 + interval, t0 + timeout)`. This holds for `Shutdown`'s final collection as well as for `ForceFlush`. Under ICP 0041 the Provider passes each stage the time left of its one deadline, so the cap is that remainder. The background thread's own cycles use `t0 + interval`.
3. **What "capped" means when little time is left.** The collection still runs, even with 0 ms left:
   - Synchronous streams are always collected. They run no user code.
   - **At least one async callback gets its turn.** The first async stream in registration order is always collected.
   - Every later async stream checks the deadline before its callback runs. Once the deadline has passed, it and all the async streams after it are **skipped** for this collection.
4. **A callback is never interrupted.** Callbacks run synchronously on the reader thread, and a running callback cannot be stopped. A callback that returns after the deadline **keeps its measurements**. It is exported, and it is not counted. Only the callbacks that are skipped are counted.
5. **Counting and logging.** Each skipped async stream (one skipped callback invocation) records one `MetricCallbackTimeout` drop. That is ICP 0008's unit: three skipped instruments in one collection count three. Each skip is also logged through the diagnostic log's existing per-`(level, reason)` rate limiter (`docs/error-model.md`, "Rate limiting (LOCKED)").
6. **Skipped series keep their state.** A skipped stream contributes no record to that collection, and its stored state is not touched:
   - Its series are **not** treated as unobserved under [ICP 0039](0039-metric-start-timestamps.md). Nothing is forgotten, and the next collection does not report any series as a first observation in full.
   - **Delta:** the next collection that runs the callback reports the change since the last observation that was exported, over a window starting at that observation's collection. One skipped cycle widens one window and over-counts nothing.
   - **Cumulative:** the series is absent from one export, then resumes with its running value and its original start time.
7. **The interface.** ICP 0045's `IMetricProducer::Collect(const TemporalitySelector&)` becomes `Collect(const TemporalitySelector& selector, std::chrono::steady_clock::time_point callback_deadline)`. `callback_deadline` defaults to `time_point::max()`, meaning no deadline, for tests and for `SynchronousMetricReader`, which has no interval. `IMetricStream` gains `[[nodiscard]] virtual bool RunsCallback() const noexcept`, so the producer can tell async streams from sync ones without a cast.
8. **`MetricCallbackTimeout` Doxygen** changes from "exceeded the per-collection deadline; its measurements were dropped" to "skipped because the per-collection deadline had passed; its series kept their state".

## Tests the implementing PR adds

- A slow first callback, past the deadline: it is exported, and every later async stream is skipped and counted once each.
- `ForceFlush(0ms)`: the collection runs, the first async callback runs, and the rest are skipped and counted.
- `Shutdown(timeout)` with a short timeout: the same cap applies to its final collection.
- **An observable delta counter, one skipped cycle:** the next export reports exactly the change since the last exported observation, with no over-count, and its start time is that observation's collection.
- An observable cumulative counter, one skipped cycle: it resumes with its running value and its original start time.
- Sync streams are collected even after the deadline has passed.

## Migration

No public API change: `MetricCallbackTimeout` already exists and keeps its value. An application whose callbacks are slow enough to overrun the interval, or a flush's timeout, now sees those callbacks skipped and counted where today they hold up the export. Callbacks that return in time are unaffected.

## Rationale & alternatives

- **Discard a late callback's measurements.** Rejected: the work is already done, so throwing away what it observed helps no one, and the next callback was delayed by the same amount either way.
- **A new `OTEL_METRIC_EXPORT_TIMEOUT` setting.** Not needed: the reader has no export timeout to reuse, and the interval already bounds how long a collection may take before the next one is due.
- **Run callbacks on worker threads so a slow one can be abandoned.** Rejected: threads per collection, plus a callback still running against a meter that the next collection reads.
- **Rotate which async stream goes first.** Not now. Registration order is predictable and testable. Rotation only matters if flushes arrive with almost no time left over and over again, and it can be added later without changing the interface.
- **Fold the selector and the deadline into one `CollectRequest` struct.** It would spare `Collect` the next parameter as well. Left to review: it is a one-line change to ICP 0045 if preferred.
