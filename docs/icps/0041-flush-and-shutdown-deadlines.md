# ICP 0041: Flush and shutdown wait for what was queued, under one deadline

**Status:** Draft — 2026-10-27.
**Affected interfaces / docs:**
- `docs/threading-model.md` §5.3 (LOCKED): two bullets replaced, text below
- `include/microtel/provider.hpp`: the `ForceFlush` Doxygen, text below
- in the implementing PRs: `docs/threading-model.md` §6.1 and
  `docs/metrics-design.md` §5
- code: `src/exporter/otlp_{exporter,metric_exporter,log_exporter}.cpp`,
  `src/exporter/retry_engine.{hpp,cpp}`,
  `src/sdk/periodic_exporting_metric_reader.cpp`, `src/sdk/sdk_provider.cpp`,
  `src/sdk/sdk_builder.cpp`

**Affected tracks:** SDK and exporter, on the flush and shutdown paths only.
There is no public API change: the `ForceFlush` and `Shutdown` signatures and
the `Status` return type stay as they are. The span hot path (`StartSpan`,
`SetAttribute`, `AddEvent`, `End`, the processor enqueue) and the encoder are
not touched.

## Summary

The flush and shutdown changes, all in v1.2.5:
- An exporter's `ForceFlush` waits for the batches queued before the call,
  not for its queue to empty.
- `Provider::ForceFlush` and `Provider::Shutdown` share one deadline across
  all their stages, run every stage, and return the worst status.
- At the shutdown deadline, an export still in flight is cancelled.

## Motivation

There are two bugs, #484 and #486:
- **#484.** All three exporters complete a flush only when the worker finds
  its queue empty. Under sustained load the queue never empties. With a
  metric interval no longer than the export time, every `ForceFlush` times
  out.
- **#486.** `FlushPipeline` returns at the first stage that doesn't complete,
  so a slow trace or metric stage stops the logs from being flushed at all.
  `Shutdown` hands every stage the full timeout, so one slow stage uses up
  the time and the queued logs are lost.

The OpenTelemetry SDK specification bounds `ForceFlush` and `Shutdown` by the
caller's timeout, and its MetricReader `ForceFlush` collects once and exports
that collection.

## Decision

**1. What a flush waits for.**
- Each exporter numbers the batches it accepts. `ForceFlush` takes the
  number of the last batch accepted before the call, and waits until every
  batch up to it has been *processed*, or until its timeout.
- **Processed** means one of two things:
  - exported, including a partial success: the collector's rejected count is
    recorded as it is today;
  - failed after its final attempt: non-retryable, `max_attempts` reached, or
    the retry budget exhausted.
- **Still pending:**
  - a batch in retry backoff. Retries run on the worker inside the drain, so
    the flush waits for them, within its timeout;
  - a batch in flight.
- **The status:**
  - `Completed` only if every batch in that range was exported;
  - `Failed` if any of them failed;
  - `TimedOut` if the timeout came first.
- **Not in the range:**
  - batches accepted after the call;
  - batches that were already processed before it;
  - batches refused at `Export` (`QueueFull`, `PostShutdown`). They were
    never queued, and they are counted as drops, as now.
- **`PeriodicExportingMetricReader::ForceFlush`** runs one
  `DoCollectExport()`, then calls the exporter's `ForceFlush`. The range
  therefore ends with its own collection. If that collection can't be
  queued, the reader returns `Failed`.
- **The same rule applies to `Shutdown`**, for the batches queued when it is
  called. `Shutdown` still calls `RetryEngine::Abort` first (unchanged), so a
  batch in retry backoff at shutdown fails at once, and that makes the status
  `Failed`.
- **The worst-status order** is `WorseOf`'s: `Failed` > `TimedOut` >
  `Completed` > `AlreadyShutDown`.

**2. One deadline, every stage, and the real bound.**
- `Provider::ForceFlush(timeout)` and `Provider::Shutdown(timeout)` compute
  `deadline = now + timeout` on entry.
- Each stage runs in the existing order and is handed the time remaining
  until the deadline. Once the deadline has passed, that is 0 ms.
- **Every stage runs, whatever the stages before it returned.** A stage handed
  0 ms is still signalled, so its worker starts the flush or the drain; it
  just isn't waited for.
- The statuses fold through `WorseOf`. A folded `TimedOut` records one
  `ForceFlushTimeout` or `ShutdownTimeout` drop, as now.
- **`ForceFlush` never cancels anything.** At its deadline it returns, and
  sends in flight carry on.
- **`Shutdown` cancels the export in flight at its deadline.** An exporter
  whose wait expires calls its `on_shutdown_timeout` hook before it joins its
  worker. Today only an `ExportTransport` Provider wires this hook. With this
  ICP:
  - **`ExportTransport`**: the hook calls `ExportTransportChannel::CancelInFlight`,
    which calls the application's `ExportTransport::Cancel()`. This is
    unchanged.
  - **HTTP/2 and gRPC**: new. The hook calls `ITransport::Close(0ms)`. That
    stops the I/O thread and completes every in-flight and queued stream as
    `Cancelled`. The codec's wait returns, and the worker finishes. The
    Provider's own `Close` later in the sequence returns `AlreadyShutDown`,
    which the fold absorbs. Every stage after that point has no time left
    anyway, because the deadline is shared.
- **The bound.** `Shutdown` returns within `timeout` plus the cancellation
  latency of the one send in flight:
  - over HTTP/2, the I/O thread's exit after a reactor wake, since the sockets
    are non-blocking;
  - over `ExportTransport`, however long the application's `Cancel()` takes
    to make `Send` return.
- **A `Send` that ignores `Cancel()`** makes the bound `timeout` plus the rest
  of that send. That is at most `per_export` from when the send started. A
  send's deadline is fixed when the send starts: `BeginShutdown` clamps only
  sends that start after `Shutdown` began, so a send already in flight is
  ended by `Cancel()`, not by its deadline. `ExportTransport`'s Doxygen
  already warns about such a `Send`, and nothing changes there.
- **Checked** with a 3 s trace `Send` in flight and `Shutdown(1s)`, on the
  pre-fix code:

  | Transport behaviour | Shutdown took |
  |---|---|
  | `Send` honours `Cancel()` | 1000 ms, the bound |
  | `Send` ignores `Cancel()` | 2800 ms: 1 s plus the 1.8 s left of the send |
  | `Send` honours only its own deadline | 2800 ms, for the clamping reason above |

  In the #486 case (3 s trace latency, `ForceFlush(2s)`), the flush now
  returns `TimedOut` at 2 s with the log stage signalled, so the logs are
  exported, instead of skipped.

**3. Document changes.**

`threading-model.md` §5.3. The bullet "**Every component runs even if an
earlier one timed out.** …" keeps its text, and gains this line after its
first sentence:

> They share one deadline: `Shutdown` computes `now + timeout` on entry and
> hands each component the time remaining, 0 ms once it has passed, so the
> sequence as a whole is bounded by `timeout` plus the cancellation latency
> of one in-flight send ([ICP 0041](icps/0041-flush-and-shutdown-deadlines.md)).

The bullet "Each component implements its own drain against the timeout it
is handed …" becomes:

> Each component implements its own drain against the time remaining it is
> handed — the worker finishing the batches queued when `Shutdown` began, the
> I/O thread completing its in-flight request and closing the socket. An
> exporter whose wait expires cancels its in-flight send through its
> `on_shutdown_timeout` hook before joining its worker: the
> `ExportTransport`'s `Cancel()`, or the HTTP/2 transport's `Close(0ms)`.
> Those contracts belong to the components, not to a Provider-level state
> machine.

The LOCKED marker keeps citing `src/sdk/sdk_provider.cpp:Shutdown`.

`include/microtel/provider.hpp`, the `ForceFlush` brief and paragraph, become:

```cpp
/// @brief Export everything recorded before this call, up to `timeout`.
///
/// Does not stop accepting new records. Every signal's pipeline is flushed,
/// even if an earlier one fails or times out, and all of them share the one
/// `timeout`. Returns `Status::Completed` only if every record queued before
/// the call was exported; `Status::Failed` if any of it failed after its
/// final retry; `Status::TimedOut` if the timeout came first. Exports still
/// in flight at the timeout carry on.
```

The `Shutdown` Doxygen keeps its text.

## Migration

None for callers: there are no signature or type changes. Two behaviour
changes are visible:
- `ForceFlush` can now return `Failed`. Before, an exporter flush returned
  only `Completed` or `TimedOut`.
- `Shutdown` over HTTP/2 now returns close to its timeout instead of
  waiting up to `per_export` for a send in flight.

## Rationale & alternatives

- **Waiting for an empty queue.** That is the bug: under sustained load the
  queue never empties.
- **Flushing the three signals in parallel**, so a slow trace stage doesn't
  eat the logs' time. It needs a non-blocking flush request on the
  processors and exporters, which is an interface change. With a shared
  deadline, a stage that has run out of time is still signalled, so its data
  still goes out, just after the call returns.
- **Bounding shutdown by `per_export` instead of cancelling.** That gives a
  10 s default tail under any `Shutdown` timeout, which is the behaviour
  #486 reports.
- **Not in scope:**
  - **The batch processors.** `BatchSpanProcessor` and
    `BatchLogRecordProcessor` have the same "flush completes when the queue
    is empty" check. Their workers only move records into the exporter, so
    they starve only if records arrive faster than that hand-off, and the fix
    would touch the span enqueue path, which v1.2.5 leaves alone.
  - **Skipping a metric collection while an export is pending** (#487,
    v1.3).
