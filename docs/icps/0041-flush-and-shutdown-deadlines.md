# ICP 0041: Flush and shutdown wait for what was queued, under one deadline

**Status:** Accepted — 2026-10-27.
**Affected interfaces / docs:**
- `docs/threading-model.md` §5.3 (LOCKED): two bullets replaced, text below
- `include/microtel/provider.hpp`: the `ForceFlush` Doxygen, text below
- in the implementing PRs: `docs/threading-model.md` §6.1 and
  `docs/metrics-design.md` §5
- code: `src/exporter/otlp_{exporter,metric_exporter,log_exporter}.cpp`,
  `src/exporter/retry_engine.{hpp,cpp}`,
  `src/sdk/periodic_exporting_metric_reader.cpp`, `src/sdk/sdk_provider.cpp`,
  `src/sdk/sdk_builder.cpp`, `src/transport/http2_transport.cpp` (`Send`)

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
- **`Shutdown` cancels the export in flight at its deadline**, once, from
  the Provider: see "Shared transport" below.
  - **`ExportTransport`**: `ExportTransportChannel::CancelInFlight`, which
    calls the application's `ExportTransport::Cancel()`, as today.
  - **HTTP/2 and gRPC (new)**: `ITransport::Close(0ms)`. It stops the I/O
    thread, and every in-flight and queued request completes as `Cancelled`.
    The codec's wait returns, and the worker finishes. The Provider's own
    `Close` later in the sequence returns `AlreadyShutDown`, which the fold
    absorbs.
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

**Shared transport.**

Traces, metrics and logs share one transport: one `Http2Transport`, or one
`ExportTransportChannel`. Cancelling it stops all three signals, so it may
happen only once, and only at the overall `Shutdown` deadline. It never
happens at one stage's sub-deadline, and never from `ForceFlush`.

- **The Provider owns the call.** `SdkProvider` gets one
  `CancelAtShutdownDeadline()`:
  - It returns at once if `Shutdown` hasn't begun. `ForceFlush` therefore
    never cancels anything, even if a hook were reached from it.
  - Otherwise it sleeps until the Provider's shutdown deadline, if that is
    still in the future. Then it cancels the transport, exactly once. A
    `std::atomic<bool>` exchange elects the one caller; any later caller
    returns.
  - It cancels with `Close(0ms)` for HTTP/2, or `CancelInFlight()` for
    `ExportTransport`.
- **Why the exporters' hooks don't do it themselves.** A hook can't fire
  early on its own account: `OtlpExporter::Shutdown` calls it only when its
  bounded wait has expired with the worker unfinished, just before it joins
  that worker. But "expired" is measured against the timeout the exporter
  was handed, and a caller can hand it less than the time remaining. For
  example, the metric reader spends part of its share joining its own
  thread before it shuts its exporter down.
  - So each exporter's `on_shutdown_timeout` is wired to the Provider's
    `CancelAtShutdownDeadline()`, never to the transport itself.
  - A hook that fires before the deadline waits for it inside the Provider.
    That costs nothing: the exporter would otherwise block in `join` on the
    same worker.
  - A hook that fires after the cancel is a no-op.
  - The ICP 0036 hooks, which today call `CancelInFlight` directly, move to
    the same call.
- **`Close()` from the shutdown thread while a worker is mid-send.** Two
  things make it safe:
  - **Connection state is touched only on the I/O thread.** That means the
    nghttp2 session, the stream maps and the socket. `Close` publishes
    `Closed`, sets `m_stop` and wakes the reactor, so the shutdown is
    *posted to the transport loop*. It waits for the loop to exit and joins
    it. Only then does it complete the abandoned streams' promises and free
    the session.
  - **The only structure a worker shares with `Close` is the pending queue,
    under `m_pending_mu`.** Today `Send` checks `m_state` *outside* that
    lock. A `Close` between the check and the push leaves a request queued
    after `Close` drained the queue. Its promise is never set, and the codec
    waits out `per_export`. This can't happen today, because `Close` runs
    only after every worker has been joined. This ICP makes it possible, so
    the fix includes it: `Send` re-checks `m_state` under `m_pending_mu`
    before the push. Then a request is either drained and completed by
    `Close`, or refused as not connected.
  - `Connect` already refuses a `Closed` transport (`ClaimConnectSlot`), so a
    worker's lazy reconnect can't reopen it.
- **The test.** A TSan test is required, in the HTTP/2 cancel PR:
  - a real `Http2Transport` against a local peer that accepts a stream and
    never answers, so a send is in flight;
  - a second worker submitting in a loop;
  - `Provider::Shutdown(short)`.

  It asserts that `Shutdown` returns within the bound, that every submitted
  request's future completes, and that TSan reports nothing.

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
> I/O thread completing its in-flight request and closing the socket. The
> transport is shared by all three signals, so the in-flight send is
> cancelled once, by the Provider, at the overall deadline: the
> `ExportTransport`'s `Cancel()`, or the HTTP/2 transport's `Close(0ms)`. An
> exporter whose wait expires reaches that through its `on_shutdown_timeout`
> hook before joining its worker; it never cancels the transport itself.
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

## Amendment (2026-10-27): consecutive flush ranges

**Status:** Draft — 2026-10-27.

**Why.** Decision 1 bounds an exporter flush's range from below at "the batches already processed before it". That can't support the Provider-level promise, "Completed only if every record queued before the call was exported". `Provider::ForceFlush` runs its stages in turn, so this can happen:
1. The span processor's flush hands a batch to the trace exporter.
2. The exporter's worker sends the batch and the send fails.
3. Only then does the Provider call the exporter's `ForceFlush`.

The failed batch was processed before that call, so it is outside the range, and the Provider returns `Completed` for a span that was never exported. The #484 implementation's tests hit this intermittently under load.

**Change to Decision 1.** A flush's range starts where the last *completed* flush's range ended, not at the batches processed before the call. It ends, as before, at the last batch accepted before the call.
- **When a flush counts as completed.** A flush completes when every batch in its range has been processed within its timeout. That holds whether it returned `Completed` or `Failed`. A flush that timed out has not completed, and does not move the start of the next range.
- **Who sees a failure.** A batch that failed after its final attempt is reported to every flush called after it, until a flush covering it completes. That includes a batch that failed before the exporter's own `ForceFlush` was called, which closes the window above.
- **Overlapping flushes may report the same failure, and that is intended.** Two `ForceFlush` calls that overlap, with a failure before both, both return `Failed`. A flush returns a status, not a count; lost batches are counted once each in diagnostics, as `batches_failed` and the drop counters.
- **The first flush** covers every batch since the exporter started.

**Visible effect.** A periodic export that failed since the last completed flush makes the next `ForceFlush` return `Failed`. A metric export on the reader's interval is one example; a trace or log batch exported on its schedule is another. That is what "every batch queued before the call was exported" means. It is new behaviour, and the v1.2.5 release notes say so.

**Unchanged:**
- a batch in retry backoff is still pending;
- the statuses: `Completed`, `Failed`, `TimedOut`;
- the `Shutdown` rule, which applies the same range;
- there is no interface change.

**Implementation note.** `FlushTracker` keeps two marks:
- **The end of the last completed range.** A flush's `Waiter` reads it when it registers, and advances it, if it is larger, when it finishes having reached its range.
- **The highest sequence number that has failed.** A failure processed before a flush registered counts against that flush if its number is above the start of the range. Every number processed by then is at or below the range's end, so this check is enough.

Failures processed while a flush waits are marked on it as before.

**Tests:**
- A deterministic `FlushTracker` unit test.
- An exporter test with two overlapping `ForceFlush` calls and a failure before both. Both return `Failed`. A third flush, after both complete, returns `Completed`.

**Scope for v1.2.5: the HTTP/2 shutdown cancel is cut.** v1.2.5 ships Decision 1 (as amended above) and the shared deadline of Decision 2. These move to v1.3 (#492):
- the HTTP/2 and gRPC `Close(0ms)` cancel;
- the Provider-owned `CancelAtShutdownDeadline()` from "Shared transport";
- the `Http2Transport::Send` re-check under `m_pending_mu`, and its TSan test;
- the second §5.3 bullet text in Decision 3.

The ICP 0036 hooks stay as they are: each `ExportTransport` exporter cancels the channel when its own wait expires. Under the shared deadline that is never before the overall deadline.

**The v1.2.5 shutdown bound.** `Shutdown(timeout)` returns within `timeout`, plus the rest of any send already in flight, up to the per-export limit (`TimeoutOptions::per_export`, 10 s by default):
- **Over HTTP/2 and gRPC** this applies whenever a send is in flight at the deadline.
- **Over `ExportTransport`** it applies only when the application's `Send` ignores `Cancel()`. A `Send` that honours `Cancel()` keeps the bound at `timeout` plus its cancellation latency.

The v1.2.5 release notes list this under Known limitations.
