# ICP 0046: A per-collection deadline for async callbacks

**Status:** Accepted — 2026-11-27. Milestone v1.3; tracking issue #237.
**Affected interfaces / docs:**
- `include/microtel/internal/metric_producer.hpp`: `CollectRequest` gains a `callback_deadline` field (**locked internal interface**; it builds on [ICP 0045](0045-temporality-selectors.md)'s `Collect(const CollectRequest&)`, whose signature is unchanged)
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
7. **The interface.** ICP 0045's `CollectRequest` gains a field, and `Collect(const CollectRequest& request = {})` keeps its signature:
   ```cpp
   struct CollectRequest
   {
       TemporalitySelector selector = TemporalitySelector::Cumulative();
       std::chrono::steady_clock::time_point callback_deadline =
           std::chrono::steady_clock::time_point::max();
   };
   ```
   `time_point::max()` means no deadline. It is the default for tests and for `SynchronousMetricReader`, which has no interval. The periodic reader sets both fields, as in `Collect({.selector = m_selector, .callback_deadline = deadline})`. `IMetricStream` gains `[[nodiscard]] virtual bool RunsCallback() const noexcept`, so the producer can tell async streams from sync ones without a cast.
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
- **A known cost: a first callback that is always too slow starves the rest.** If the first async stream's callback always takes longer than the interval, the deadline has passed every time it returns, so every later async stream is skipped on every cycle and never exported. The `MetricCallbackTimeout` counter and the rate-limited log are the only signals that this is happening. The fix belongs in the application: make the callback faster, or lengthen the interval.
- **Rotate which async stream goes first.** Deferred. Rotation would end the starvation above, but registration order is predictable and testable, and rotation can be added later without changing the interface.
- **A second `Collect` parameter for the deadline.** Rejected in review: ICP 0045's `CollectRequest` takes the deadline as a defaulted field, so the locked signature does not change again.

## Amendment (2026-11-27): rate-limited logging

Decision 5 names the diagnostic log's "existing per-`(level, reason)` rate limiter". That limiter is specified in `docs/error-model.md` ("Rate limiting (LOCKED)") but not built (`src/common/internal_log.hpp`). The implementation therefore limits its own warnings as `SdkLeafReceiver::RecordOutOfMemory` already does: it logs the 1st, 2nd, 4th, 8th … occurrence, with the running count. When the shared limiter is built, this log moves onto it; the counting is unchanged.
