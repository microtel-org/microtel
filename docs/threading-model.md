# microtel Threading Model

**Status:** M0 deliverable. Normative for which threads exist, what they own, and how data crosses them in v1.
**Companion documents:** `architecture.md` (layered structure), `memory-model.md` (resource ownership), `error-model.md` (no-exceptions-across-threads rule), `interfaces.md` (per-method threading tags).
**Source of truth for rationale:** this document and the ICPs it cites (ICP 0021, ICP 0027).
**Citation policy:** complete — per ICP 0021, every LOCKED marker below cites the code that makes it true, or is marked `intent`. Enforced by `ci/scripts/citation-check.py`.

---

## 1. Purpose and authority

This document is the canonical answer to:

1. **Which threads exist in microtel and what does each own?**
2. **For every public method, which thread is the caller expected to be on?**
3. **For every cross-thread hand-off, what's the synchronisation contract?**
4. **What does it mean concretely that the caller-thread API is `noexcept` and non-blocking?**

Doxygen `@threadsafety` tags on individual methods reference categories defined here.

Some rules are non-negotiable in v1; they are flagged **LOCKED**. Changing a LOCKED rule requires an ICP.

Per ICP 0021, each marker also says what makes it true:

```
(LOCKED — cites `src/sdk/sdk_provider.cpp:Shutdown`)    a claim about code
(LOCKED — intent)                                       a claim about intent
```

A citation names a **function or member, never a line number** — line numbers rot, and one added in #149 was already stale by #144. `ci/scripts/citation-check.py` (CI job `citation-check`) fails the build when a cited symbol is absent from the cited file, and when a marker in a document whose citation policy is `complete` carries neither annotation.

The check is deliberately weak: it proves a symbol exists, not that the sentence around it is true. It exists because **LOCKED has never meant "verified"** — every marker in this document was written in the M0 commit, before there was code to check it against, and six of them turned out to describe a system that does not exist (ICP 0021, issue #134).

---

## 2. The threads

v1 has **four thread roles** — producer, processor worker, exporter worker, I/O — and a fully-configured single `Provider` runs **seven threads** in six of its own classes plus the transport. There is no thread pool, no fiber scheduler, no work-stealing in v1.

This section said "exactly three thread roles" until ICP 0021 checked it. Both halves were wrong: the count, and the taxonomy behind it — §2.2 described one "exporter worker" that drained the span queue *and* encoded *and* sent. Those are two threads, one per side of a hand-off.

**Thread inventory** (LOCKED — cites `src/sdk/batch_span_processor.hpp:m_worker`, `src/sdk/batch_log_record_processor.hpp:m_worker`, `src/sdk/periodic_exporting_metric_reader.hpp:m_thread`, `src/exporter/otlp_exporter.hpp:m_worker`, `src/exporter/otlp_metric_exporter.hpp:m_worker`, `src/exporter/otlp_log_exporter.hpp:m_worker`, `src/transport/http2_transport.hpp:m_io_thread`).

| Role | Thread | Loop | Signal |
|---|---|---|---|
| Producer | any application thread | — (owned by the application) | all |
| Processor worker | `BatchSpanProcessor::m_worker` | `WorkerLoop` | traces |
| Processor worker | `BatchLogRecordProcessor::m_worker` | `WorkerLoop` | logs |
| Processor worker (timer-driven) | `PeriodicExportingMetricReader::m_thread` | `RunLoop` | metrics |
| Exporter worker | `OtlpExporter::m_worker` | `WorkerLoop` | traces |
| Exporter worker | `OtlpMetricExporter::m_worker` | `WorkerLoop` | metrics |
| Exporter worker | `OtlpLogExporter::m_worker` | `WorkerLoop` | logs |
| I/O | `Http2Transport::m_io_thread` | `IoThreadLoop` | all (one shared transport) |

Seven, not eight: the producer row is the application's own thread, not one microtel creates. The metric reader is grouped with the processor workers because it plays their part in the metrics pipeline — it collects and hands off to an exporter — but it is woken by an interval rather than by an enqueue, which is why it is called out.

The inventory is the maximum. A `Provider` with no `GetMeter` and no `GetLogger` call has only the trace pipeline's two workers plus the I/O thread: the metrics and logs pipelines are built lazily (`SdkProvider::GetMeter`, `SdkProvider::GetLogger`) and spawn nothing until they are.

**With an application `ExportTransport`** (`SdkBuilder::WithExportTransport`, [ICP 0036](icps/0036-custom-export-transport.md)) there is **no I/O thread**: no `Http2Transport` is built. The metric and log exporter workers exist only for the signals `ExportTransportOptions` enables (traces only by default), so the default custom-transport Provider runs two threads, the span processor's and the trace exporter's. **Application code runs on the exporter workers**: each calls `ExportTransport::Send` for its own signal, so `Send` has up to three concurrent callers, one per enabled signal. `Send` may block until its deadline and stalls only its own signal. If an exporter's `Shutdown` wait expires while a `Send` is in flight, the thread running `Provider::Shutdown` calls `ExportTransport::Cancel` once, before joining that worker; a `Send` that ignores both its deadline and `Cancel` blocks the join, and so the `Provider` destructor, forever.

### 2.1 Caller thread (any application thread)

**Identity.** Any thread that calls a public microtel API. Plural — many caller threads share the role. The thread is owned by the application; microtel never creates a caller thread.

**Owns:** the `Span` handles handed back from `StartSpan`. RAII-auto-end fires on the caller thread when the handle goes out of scope.

**May call:** the public API in `include/microtel/`. Specifically `Tracer::StartSpan`, `Span::SetAttribute / AddEvent / AddLink / SetStatus / End`, the propagator inject/extract methods, `Provider::ForceFlush`, `Provider::Shutdown`, `Provider::GetExporterHealth`.

**May not:** access the SDK's internal queues, the encoder, the wire codec, the transport, or any internal interface directly. Caller-thread code never includes a header from `include/microtel/internal/`.

**Hot-path guarantee** (LOCKED — cites `include/microtel/tracer.hpp:StartSpan`, `include/microtel/span.hpp:SetAttribute`). `StartSpan`, `SetAttribute`, `AddEvent`, `AddLink`, `End` are `noexcept` and never wait on I/O. See §8 for the precise contract.

### 2.2 Pipeline worker threads — processor workers and exporter workers

**One worker per pipeline, and every pipeline belongs to one `Provider`** (LOCKED — cites `src/sdk/batch_span_processor.cpp:WorkerLoop`, `src/sdk/batch_log_record_processor.cpp:WorkerLoop`, `src/exporter/otlp_exporter.cpp:WorkerLoop`, `src/exporter/otlp_metric_exporter.cpp:WorkerLoop`, `src/exporter/otlp_log_exporter.cpp:WorkerLoop`), of which a fully-configured `Provider` has **five**: a processor worker and an exporter worker each for traces and logs, and an exporter worker for metrics (whose producer side is the reader thread of §2, not a queue drain). The count is **per `Provider`, not per process**: a process running N named profiles runs N such sets, sharing no thread, queue, or transport between them.

This section said "v1 always has exactly one worker per process", justified by multi-profile being a v1.1 feature. The justification was a non-sequitur even when it was written: the multiplicity comes from **three signals inside one `Provider`**, not from multiple `Provider` instances. One `Provider` was v1.0's supported configuration; from v1.1 a process may run several named ones ([ICP 0027](icps/0027-multi-profile-threading.md), shipped — `SdkBuilder::WithProfileName`, `microtel::GetProvider`). Neither fact touches the per-pipeline rule, which was always a statement about one `Provider`'s internals.

The two roles are separated by a queue, and conflating them is what the old text did:

**Processor worker.** Owned by a `BatchSpanProcessor` / `BatchLogRecordProcessor`. Created at processor construction; joined at `Shutdown` or destruction.

- **Owns:** batch construction state — the record queue and its batching deadline.
- **Drains:** the record queue (consumer side; multiple producers).
- **Calls into:** `IExporter::Export` / `ILogExporter::Export`, which **enqueues** to the exporter's own queue and returns (`BatchSpanProcessor::ExportBatch`). The hand-off is where this thread's work ends.
- **Sleep state.** Waits on a condition variable when the queue is short of `max_export_batch_size` and no batch deadline is pending. Wake sources: enqueue notification, batch-deadline timer, `ForceFlush` request, `Shutdown` request.

**Exporter worker.** Owned by an `OtlpExporter` / `OtlpMetricExporter` / `OtlpLogExporter`. Created at exporter construction; joined at `Shutdown` or destruction.

- **Owns:** retry orchestration state for the in-flight batch, the per-thread randomness source for backoff jitter.
- **Drains:** the exporter's batch queue, filled by the processor worker (or, for metrics, by the reader thread).
- **Calls into:** `IOtlpEncoder::Encode` (synchronously), `IWireCodec::Send` (synchronously, but the I/O it triggers happens on the I/O thread), `IDiagnosticsSink::Record*`.
- **Sleep state.** Waits on a condition variable when its queue is empty. Wake sources: enqueue notification, `ForceFlush` request, `Shutdown` request.

**Neither may** call any caller-facing API. A worker thread never invokes `Tracer::StartSpan` or any other public API; doing so would risk a queue self-feed.

### 2.3 I/O thread (one per `Provider`)

**Identity.** Owned by the `Transport`. Created in `Http2Transport::Create()`
— **not** at `Connect`, as this line said until it was checked against the
code. The loop starts polling immediately and runs whether or not a
connection exists; `IoThreadLoop` simply finds `m_nghttp2_session` invalid and
skips the drain steps. Joined at `Transport::Close`, which is accurate.

**Exactly one I/O thread per `Provider`** (LOCKED — cites `src/transport/http2_transport.hpp:m_io_thread`, `src/sdk/sdk_builder.cpp:Build`). One nghttp2 session, one socket, one reactor: `Http2Transport` holds a single `m_io_thread`, and each `SdkBuilder::Build` constructs one transport, shared by every pipeline of the `Provider` it builds.

This said "one I/O thread per process" until [ICP 0027](icps/0027-multi-profile-threading.md). It is the same claim while a process has one `Provider`, which was v1.0's only supported configuration; the rescoping is in effect from v1.1, where a process may hold up to eight named `Provider`s. Each of them builds its own transport and therefore owns its own socket, its own reactor, its own `SslCtx` ([ICP 0003](icps/0003-m0-deferred-decisions.md) §3.1 having already put that ownership on the `Transport`), and its own I/O thread. Nothing is shared across profiles.

**Reads:** the OpenSSL `SslCtx` reference, the `SslSession`, the
`Nghttp2Session`, the socket fd (a `common::raii::UniqueFd` — there is no
`Socket` type), the `IReactor` (epoll on Linux), the per-stream in-flight
request state.

These are *constructed by* `Connect`, on whichever thread calls it — the
application thread via `Provider::Connect()`, or an **exporter worker** thread
via `IWireCodec::Send`'s lazy connect (ICP 0017). They are published to the I/O
thread by the release-store of `m_state = Connected`, which the I/O thread
acquire-loads before touching them. The per-stream state is genuinely
I/O-thread-only.

**Calls into:** OpenSSL, nghttp2, libc syscalls. Receives completion notifications from nghttp2 callbacks.

**May not:** call into the encoder, the SDK, or any caller-facing API. The I/O thread is purely the byte-mover.

**Sleep state.** Blocked in `epoll_wait` with a **fixed 100 ms tick**
(`Http2Transport::IoThreadLoop`), not a computed deadline. This section
previously described a timeout derived from "the nearest of: pending HTTP/2
timeout, retry-after deadline, shutdown deadline"; no such computation exists.
Wake
sources are socket readability/writability and a **single** eventfd —
`EpollReactor::m_wake_fd`, driven by `Wake()` — shared by the request-queue and
shutdown signals. The two separate eventfds this section named do not exist.

---

## 3. Inter-thread channels

Three channels, in canonical order. Each has a fixed shape, owner, and synchronisation contract.

### 3.1 Caller → processor worker — the span queue

**Producer:** any caller thread, on `End()`.
**Consumer:** the **processor** worker (`BatchSpanProcessor`), in batches — not the exporter worker, which sits one queue further down (§2.2). This section named the exporter worker until ICP 0021.

**Shape.** Bounded MPSC queue. Capacity is `max_queue_size` from the batch processor configuration (default 8192 records; `configuration.md` §3.7).

**Backpressure** (LOCKED — cites `src/sdk/batch_span_processor.cpp:OnEnd`). When the queue is full, the producer **drops the incoming record** by default (`drop_newest`). The producer never blocks. The drop is recorded against the `queue_full` counter (`error-model.md` §3). Drop-oldest is an opt-in alternative (`drop_policy`, `configuration.md` §3.7); when configured, the worker thread (not the producer) is responsible for shedding the oldest entry on overflow.

**Producer-side synchronisation contract.** The enqueue path:

1. Acquires a slot in the bounded queue using a wait-free or near-wait-free protocol.
2. Moves the `Span` record into the slot.
3. Publishes the slot.
4. If the queue transitioned from empty to non-empty, signals the worker's wakeup primitive.

The exact data-structure choice (lock-free atomic ring vs. mutex-protected ring vs. linked list with per-thread freelists) is a v1 implementation decision pinned during M3 against benchmark evidence. The architectural contract M0 commits to is the four numbered guarantees above plus:

- **Producer never waits on I/O** (LOCKED — cites `src/sdk/batch_span_processor.cpp:OnEnd`).
- **Producer never holds a lock spanning the move-into-slot step** if a mutex implementation is chosen — the lock window is bounded to slot acquisition, not the move payload work.
- **Allocation in the producer path is bounded to `O(1)` and may be zero** depending on implementation; see `memory-model.md` §8.2.

**Consumer-side semantics.** The worker drains up to `max_export_batch_size` records (default 512; `configuration.md` §3.7) per batch cycle. Drain is non-blocking with respect to producers — the worker never blocks producers, even briefly. After drain the worker releases the slots back for re-use.

**Wakeup primitive.** A `std::condition_variable` paired with the queue's mutex if the implementation uses one, or a `eventfd(2)` on Linux / pipe-pair on BSD that the worker waits on alongside its deadline timer. Implementation choice is M3-era; the contract is "the worker can sleep until either an enqueue or a deadline fires."

### 3.2 Exporter worker → I/O thread — the transport request queue

**Producers:** the exporter workers — **three of them** since M12/M14 (traces,
metrics, logs), each driving its own `IWireCodec` over the one shared
transport.
**Consumer:** the I/O thread, in its reactor loop.

**Shape.** MPSC (multi-producer, single-consumer), per ICP 0009. This section
said SPSC until that ICP was applied; it had been wrong since M12, when the
metrics pipeline began sharing the transport with traces.

**Bounded** at `ConnectOptions::max_pending_requests` (default 64), adopted per
connection alongside the response budgets. `m_pending_queue` is a plain
`std::vector`; the bound is the depth `Send` will push to, not a reserved
capacity.

**Producer-side contract.** An exporter worker calls into the transport synchronously; the transport acquires the request-queue lock (`m_pending_mu`), pushes a request descriptor (carrying a borrowed `std::span<const std::byte>` over the `EncodedPayload` bytes — see `memory-model.md` §3.3), wakes the I/O thread via eventfd, releases the lock, returns to the worker. The worker then awaits a completion (described in §3.3 below).

**Consumer-side contract.** The I/O thread's reactor wakes on the eventfd, drains pending requests under the same lock, attaches each to a new nghttp2 stream, and returns to its reactor sleep until socket activity or another wake.

**Backpressure.** At `max_pending_requests` the transport refuses rather than
queues: `Send` returns a handle whose id is 0 and whose future is already
resolved with `TransportResult::transport_busy` and a `ResourceExhausted`
error. Nothing is queued and nothing reaches the wire. The codec turns that
into `DropReason::TransportBusy` and keeps the request **retryable** — a full
queue drains, unlike an oversized response the peer will send again. The
capacity check is made under `m_pending_mu`, in the same critical section as
the push, so concurrent submitters (ICP 0009) cannot both claim the last slot.

The bound exists for the case where the I/O thread is the thing that is stuck —
a peer that stopped reading, a reconnect in backoff. A healthy process never
approaches it: each of the three exporter workers blocks on its own completion,
so the steady-state depth is at most three. `Send` refusing is not a latch —
once the I/O thread drains, the next `Send` is accepted (issue #181).

### 3.3 I/O thread → exporter worker — request completion

**Producer:** the I/O thread, when nghttp2 emits the response (HEADERS + DATA + trailer HEADERS, or trailer-only HEADERS).
**Consumer:** the exporter worker, which is parked waiting on the completion.

**Shape.** A per-request `std::promise` / `std::future` pair — `Http2Transport::StreamState::promise`, handed to the caller as `RequestHandle::Future()`. The exporter worker, after handing a request to the transport (§3.2), blocks in `future::wait_for` with the request's deadline. The I/O thread, on completion, copies the response bytes from nghttp2's owned buffers into the codec's response buffer (sized at `max_response_bytes` from `memory-model.md` §6), fills in the `TransportResult`, and fulfils the promise.

This section described "a `std::condition_variable` keyed to the in-flight request" until ICP 0021. Functionally equivalent, but the wording was load-bearing elsewhere: §4's lock table listed a per-request `m_completion` mutex on the strength of this one sentence, and no such lock exists — the completion path takes no lock at all.

**Why copy at the boundary.** nghttp2 owns its receive buffers and may recycle them on subsequent reads. Copying the bytes into a codec-owned buffer (bounded by `max_response_bytes`) means the worker can parse the response without holding a reference into nghttp2 internals. Copy cost is well within the budget — responses are tiny relative to the request and the parse work.

**On the worker side.** The worker calls into `IWireCodec::Send`, which internally enqueues to the transport (§3.2) and then waits on the completion. When the wait returns, the codec parses the copied response bytes into a `WireResult` and returns up through the exporter. The worker's `Send` call appears synchronous to the worker — but its **internal** behaviour spans both the worker thread (enqueue, parse) and the I/O thread (write, read).

**Cancellation.** If the exporter aborts the request (timeout, `Shutdown` mid-flight), it signals the I/O thread to send `RST_STREAM` (gRPC) or close the stream (HTTP); the I/O thread completes the request record with a cancellation result; the worker observes the result and returns. The completion path is symmetric whether the result is success, failure, or cancellation.

---

## 4. Lock-ordering rules

Every mutex in v1, by owner. This table named five locks until ICP 0021; three of those names appeared in no source file, and two of them — `m_completion` and `m_shutdown` — were not mutexes at all, which made the two rules stated over them unfalsifiable. The real inventory:

| Lock | Owner | Held during |
|---|---|---|
| `m_error_mu` | `DiagnosticsCounters` | last-error timestamp + message write |
| `m_mu` | `BatchSpanProcessor` | span queue enqueue / drain, flush bookkeeping, **`m_opts` read or write** |
| `m_mu` | `BatchLogRecordProcessor` | log queue enqueue / drain, flush bookkeeping, **`m_opts` read or write** |
| `m_mu` | `OtlpExporter`, `OtlpMetricExporter`, `OtlpLogExporter` | batch queue enqueue / drain, flush bookkeeping |
| `m_mu`, `m_collect_mu` | `PeriodicExportingMetricReader` | wake flag **and `m_interval` read or write**; one collect+export cycle (`CollectSlot`) |
| `m_mu` | `MetricProducer` | snapshot of the scope / stream structure |
| `m_mu` | `SumStorage`, `GaugeStorage`, `HistogramStorage`, `ExponentialHistogramStorage` | one point update, or one collect |
| `m_meter_mu`, `m_logger_mu` | `SdkProvider` | lazy construction of the metrics / logs pipeline; **the `m_metric_interval` / `m_log_batch_opts` seeds and the borrowed reader / log-processor pointers** |
| `m_mu` | `DescriptionSlot` (retunable sampler descriptions) | appending and publishing one new `Description()` string |
| `m_ratio_mu` | `TraceIdRatioSampler` | one `TrySetRatio`: the same-value check, the threshold store, the description publish |
| `m_mu` | `CallbackAuthProvider` | cached-token read / refresh |
| `m_mu` | `EpollReactor` | `m_callbacks` register / unregister / dispatch lookup |
| `m_pending_mu`, `m_cancel_mu` | `Http2Transport` | request-queue push / drain; cancel-queue push / drain |
| `m_io_done_mu` | `Http2Transport` | the I/O-loop-exited flag `Close` waits on |
| `m_mu` | `LeafTable` (the leaf receiver's, v1.2 concentrator builds only) | one leaf lookup, insert (with its idle and LRU evictions) or boot-anchor update; a leaf lock, never held across a call out — the leaf's settings (including the user's resolver) and its Resource are resolved before it is taken and the processor is called after it is released ([`leaf-concentrator-design.md`](leaf-concentrator-design.md) §3.5) |

There is **no completion lock and no shutdown lock**: request completion is a `std::promise` / `std::future` pair (§3.3), and shutdown is a set of atomic flags (§5.3).

**Rules.** Each is LOCKED and cites the code that keeps it true.

1. **`m_error_mu` is a leaf** (LOCKED — cites `src/sdk/diagnostics_counters.cpp:RecordBatchFailed`) — no other lock is acquired while it is held. It guards two fields, a timestamp and a bounded string; every counter is a `std::atomic<uint64_t>` and takes no lock at all (`RecordDrop`).
2. **A thread holds at most one non-leaf lock at a time** (LOCKED — cites `src/sdk/metric_producer.cpp:SnapshotScopes`, `src/exporter/otlp_exporter.cpp:DrainQueue`, `src/sdk/batch_span_processor.cpp:ExportBatch`). v1 intentionally has no nested locks, and the code is written to keep it that way: `MetricProducer::Collect` snapshots the structure under `m_mu` and releases it before calling `IMetricStream::Collect`, which takes its own; `OtlpExporter::DrainQueue` unlocks before `FanOutAndProcess`; `BatchSpanProcessor` releases `m_mu` when `WaitAndCollect` returns, before `ExportBatch`. Any future code that wants to break this needs an ICP.

    The four hot-reload setters (ICP 0026) are written to the same rule, and it is what shapes them. `SdkProvider::SetBatchOptions` is **two phases, never nested**: it retunes the span processor, then takes `m_logger_mu` only long enough to store the seed and read back the borrowed log processor, releases it, and retunes that — the price being that the change is not atomic across the two pipelines. `SetMetricInterval` does the same around `m_meter_mu`. `SetSamplerRatio` takes no provider lock at all, and a composite sampler recomposes its description *after* forwarding to its delegates rather than around the call, so a chain never holds two setter locks at once. `SetLogLevel` takes no lock: it is one atomic store.
3. **The transport's queue locks are leaves** (LOCKED — cites `src/transport/http2_transport.cpp:Send`, `src/transport/http2_transport.cpp:DrainPendingRequests`). Per ICP 0009, `m_pending_mu` is a leaf *any* submitting thread may take — three exporter workers do. `m_cancel_mu` is the same shape for cancellations. Both are held only for a push or a drain, never across a call-out.
4. **Completion and shutdown take no lock** (LOCKED — cites `src/transport/http2_transport.hpp:StreamState`, `src/sdk/sdk_provider.hpp:m_shut_down`). The I/O thread fulfils a `std::promise` and the waiting worker is parked in `future::wait_for`; `SdkProvider::m_shut_down`, `OtlpExporter::m_shutdown` and `Http2Transport::m_state` are atomics, read and written without a mutex. The old rules 3 and 4 governed a `m_completion` and an `m_shutdown` mutex that have never existed.

---

## 5. Wakeups and shutdown signalling

Two wakeup mechanisms, used uniformly:

### 5.1 In-process wakeups for waiting threads

- **Exporter worker:** waits on `std::condition_variable_any` paired with the queue's wakeup state. The worker's wait predicate is `(queue_non_empty || deadline_reached || shutdown_requested)`.
- **I/O thread:** waits in `epoll_wait`. **One** file descriptor is registered besides the socket — `EpollReactor::m_wake_fd`, an eventfd written by `EpollReactor::Wake()` and shared by every wake reason: a new transport request, a cancellation, and shutdown. The two descriptors this section named, `m_request_eventfd` and `m_shutdown_eventfd`, have never existed; one fd carries them all, because the loop re-checks every queue after any wake rather than deciding what to do from which descriptor fired.

### 5.2 Cross-thread wakeups for completions

The I/O thread fulfils the request's `std::promise` when it completes (§3.3). The waiting worker is parked in `future::wait_for` on the matching `std::future`, not on a condition variable — there is no `m_completion` lock.

### 5.3 Shutdown signal (LOCKED — cites `src/sdk/sdk_provider.cpp:Shutdown`)

There is no shutdown state machine and no single ground-truth variable. Shutdown
is **a flag, then an ordered sequence of per-component shutdowns**, each component
owning its own idempotence.

`SdkProvider::m_shut_down` is a `std::atomic<bool>` (`src/sdk/sdk_provider.hpp`).
`Provider::Shutdown(timeout)` release-stores it `true` *before* tearing anything
down, then drives each component in a fixed order:

```
m_shut_down = true
  → span processor
  → metric reader (or the bare metric exporter, if no reader)
  → log processor → log exporter
  → trace exporter
  → transport (Close)
```

- **The flag's only job** is to stop `GetMeter` / `GetLogger` from building a new
  pipeline component — and spawning its thread — after `Shutdown` has begun. It
  does not gate the export path, and it is not a progress indicator: it is `true`
  for the whole of the teardown and afterwards, with no intermediate value.
- **Every component runs even if an earlier one timed out.** A partial teardown
  would leak threads and sockets. They share one deadline: `Shutdown` computes
  `now + timeout` on entry and hands each component the time remaining, 0 ms
  once it has passed ([ICP 0041](icps/0041-flush-and-shutdown-deadlines.md)).
  The statuses fold worst-first — `Failed` >
  `TimedOut` > `Completed` > `AlreadyShutDown` — into the single `Status` the
  caller sees (`WorseOf` in `sdk_provider.cpp`), and a `TimedOut` fold records
  exactly one `ShutdownTimeout` drop however many components ran out of time.
- **Ordering is the explicit call order above**, plus member declaration order in
  `SdkProvider` for destruction. Nothing consults a state enum to decide what to
  tear down next.
- Each component implements its own drain against the timeout it is handed —
  the worker finishing its in-flight batch, the I/O thread completing its
  in-flight request and closing the socket. Those contracts belong to the
  components, not to a Provider-level state machine.

`Shutdown` is **idempotent** (LOCKED — cites `src/sdk/sdk_provider.cpp:WorseOf`) — but by composition, not by a
Provider-level short-circuit. A second call re-runs the whole sequence; each
component observes its own already-shut-down state and returns
`AlreadyShutDown`, and the fold above turns six of them back into one.

**What this section used to say, and why it is worth recording.** It asserted "a
single `std::atomic<ShutdownState> m_state` on the `Provider` is the ground truth
for shutdown progress", over a `NotShutDown → Draining → Closed` table. No such
member and no such enum has ever existed in this repository — `ShutdownState`
appears in no commit (issue #134). The claim entered in the M0 commit, already
marked LOCKED, at a phase with no source code to check it against; **LOCKED means
"changing this needs an ICP", never "this has been verified true"**. The nearest
real thing to the documented `m_state` is `Http2Transport::m_state`, an
`std::atomic<ConnectionState>` on the *transport* — the release-store §2.3 refers
to, and the reason `Transport::Close` can return `AlreadyShutDown` on a second
call.

---

## 6. `ForceFlush` and `Shutdown` lifecycle

### 6.1 `ForceFlush(timeout)`

**Caller.** Any caller thread, including from the application's main thread before `Shutdown`.
**Effect.** Wakes the worker with a flush request; the worker drains the queue (without exiting its loop) and waits for the in-flight batch to complete or for the timeout to elapse. Returns one of `Completed` / `TimedOut` / `AlreadyShutDown` / `Failed`.

The worker treats a `ForceFlush` request as a synthesised batch deadline of "now". After the queue is empty and the in-flight request returns, the worker signals the flush-completion future and returns to its normal loop.

`ForceFlush` does not stop accepting new records. New `End()` calls during a flush proceed normally; if they enqueue after the flush's drain pass, they will be picked up on the next batch cycle, not the flush.

### 6.2 `Shutdown(timeout)`

**Caller.** Any caller thread, but in practice the application's shutdown path.
**Effect.** Sets `m_shut_down`, then shuts down each pipeline component in the order given in §5.3 — each signalling and joining its own thread within the timeout — and folds their statuses into one `Completed` / `TimedOut` / `AlreadyShutDown` / `Failed`.

After `Shutdown` returns, no further records are accepted; producers see `post_shutdown` drops.

**Destructor of `Provider`.** Invokes `Shutdown(small_finite_timeout)` if not already shut down. The destructor itself is `noexcept` (LOCKED — cites `src/sdk/sdk_provider.cpp:~SdkProvider`) — if `Shutdown` returns `TimedOut` or `Failed`, the destructor logs a diagnostic and returns. It does not block indefinitely.

The full sequence diagram for `Shutdown` is `docs/sequences/shutdown-drain.md`.

---

## 7. Fork semantics

Forking a process that has microtel running raises real correctness questions because the child inherits half-finished state (mid-flight nghttp2 stream, half-written socket buffers, locked mutexes that the worker thread no longer exists to release).

**Rule** (LOCKED — cites `src/sdk/provider_registry.cpp:ForkChildHandler`). After `fork()`, the child process starts with **exporter workers disabled** until the application explicitly reinitialises microtel.

Concretely:

- A `pthread_atfork` handler runs in the child and marks **every live
  `Provider`** shut down. Worker and I/O threads are not present in the child
  (only the forking thread survives `fork`), so any API entry point that
  consults the shutdown flag before touching shared state drops instead of
  blocking.

  The plural is v1.1's, and it is a fix rather than an extension: this said "the
  live `Provider`" while the handler could reach exactly one, through a single
  `g_live_provider` slot that a second provider's construction overwrote — so a
  fork with two live providers left the *first* one unmarked and reachable from
  the child. [ICP 0027](icps/0027-multi-profile-threading.md) replaced that slot
  with a fixed array of atomic slots (`src/sdk/provider_registry.hpp`), which the
  handler walks in `kMaxProfiles` loads. It takes no lock and reads no
  `std::string`: the name lives in the provider, the slot holds a bare pointer.

  The handler also **clears** each slot as it marks it, so
  `microtel::GetProvider(name)` in the child answers `nullptr` and the child can
  re-`Build()` under the very names the parent was using (ICP 0027 §3). A
  pointer or `shared_ptr` obtained before the fork is unaffected and answers
  `AlreadyShutDown`.

  The mechanism is a flag, not a state machine: this section previously
  specified `m_state = Closed`, and no such member has ever existed
  (see ICP 0018 and issue #134). The flag it sets is
  `SdkProvider::m_shut_down`, and the child handler does nothing but a bounded
  sequence of atomic loads and stores — it takes no lock, because a lock held at
  `fork()` time by a thread that does not exist in the child would never be
  released, and it allocates nothing.

  **This does not make a forked child fully safe, and the previous wording
  overstated it.** It covers entry points that check the flag first —
  `GetLogger` and `GetMeter`. It does not cover `Span::End()`, which reaches
  `BatchSpanProcessor::OnEnd` and takes that processor's mutex with no path to
  the flag; a child calling it can still deadlock on a mutex held at fork time.
  Closing that needs a per-component fork check, tracked separately.
- The handler is **registered once, while the library loads** — by a static
  initialiser in `src/sdk/provider_registry.cpp`, not by the first `Build()`.
  Until v1.2 the first registration installed it through `std::call_once`, and
  a `fork()` while another thread was inside that call left the child's flag
  permanently "in progress", so the child's first `Build()` — the recovery this
  section supports — blocked forever (issue #271; ICP 0027 Discrepancy 4).
  `pthread_once` and a function-local `static` have the same hole, and a
  non-blocking compare-exchange trades it for a loser that registers before the
  handler is in place. Load time needs no guard: nothing waits, and nothing can
  register the handler twice.
- There is **no parent and no prepare handler.** The rule previously asked the
  parent handler to "record a diagnostic that fork was observed"; there is
  nothing to record it to. `LogImpl` has no production call sites and is not
  async-signal-safe, and no `DropReason` covers fork. Registering an empty
  handler would only disguise that.
- `SdkBuilder::Build()` may be called again in the child to construct a fresh `Provider`. The new provider initialises fresh threads, sockets, and nghttp2 sessions. The parent's I/O state is **not shared**.

The fork-survival sequence diagram is `docs/sequences/fork-survival.md`.

### 7.1 Signal disposition

Signals belong in the same register as fork: process-wide state a library
shares with a host it does not own.

**Rule** (LOCKED — cites `src/transport/nosignal_io.hpp:SendNoSignal`). microtel installs **no signal handler** and changes **no
process-global signal disposition**. Nothing in the runtime calls `signal`,
`sigaction`, or `pthread_sigmask` — not for `SIGPIPE`, not for anything else.
A host that has its own handlers keeps them; a host that has none still has
none after linking microtel.

That leaves `SIGPIPE`, whose default disposition terminates the process and
which a peer can provoke at will simply by hanging up under a write — a
collector restart, a GOAWAY followed by a close, or a load balancer draining
a backend. A library must not be able to kill its host that way, so
**`SIGPIPE` is suppressed per write, not per process**:

- Plaintext sends go through `SendNoSignal` (`src/transport/nosignal_io.hpp`),
  which is `::send(..., MSG_NOSIGNAL)`.
- TLS sends go through the same call, one layer down: microtel hands `SSL` a
  custom `BIO` whose write callback is that `send`, rather than the stock
  socket BIO OpenSSL would otherwise build from `SSL_set_fd`. That covers the
  handshake writes inside `SSL_connect` as well as `SSL_write`.
- `SO_NOSIGPIPE` is set on the socket where the platform has it. v1 is
  Linux-only, where it does not exist and the per-write flag is the mechanism.

Per-write, rather than a per-thread `pthread_sigmask`, because writes are not
confined to the I/O thread: `Connect` runs the TCP connect, the TLS handshake
and the HTTP/2 handshake — all of them writing — on the **calling** thread,
which is an application thread or an exporter worker. microtel does not alter
the signal mask of threads it did not create. See issue #177.

---

## 8. What `noexcept` and non-blocking on the hot path mean

This section is the precise contract the API delivers to caller threads. The numbered guarantees are (LOCKED — cites `include/microtel/span.hpp:SetAttribute`, `include/microtel/tracer.hpp:StartSpan`).

For every method on `Tracer` and `Span` listed in §2.1:

1. **`noexcept`**: the method is declared `noexcept` and will not throw. Implementation is responsible for catching anything that would unwind and converting it into a drop-and-count.
2. **No I/O**: no syscall that can block on network, disk, or DNS. `clock_gettime(CLOCK_MONOTONIC)` is permitted and treated as effectively non-blocking.
3. **No allocation on the unsampled path**: see `memory-model.md` §8.1.
4. **Bounded allocation on the sampled path**: bounded by the span structural limits in `memory-model.md` §7.
5. **Bounded synchronisation**: no mutex held for unbounded duration. Lock windows are `O(1)` work, not `O(batch size)`.
6. **No exceptions across thread boundaries**: an internal failure on the caller thread (e.g., a queue-overflow drop) is recorded as a diagnostic on the same thread. The exporter worker observing a malformed batch records on its own thread. No `std::exception_ptr` is moved between threads.
7. **`Shutdown`-safe**: calling any of these methods after `Shutdown` returns silently with a `post_shutdown` drop.

If any of guarantees 1–7 cannot be met for a method, the method is **not** in the public API. It goes on `Provider` (where the lifecycle/structured-status contract applies) or on an internal interface.

---

## 9. Test seams

Three seams exist specifically so tests can drive the threading model deterministically.

### 9.1 `IClock` / `ISteadyClock`

Production injects `std::chrono::system_clock` and `std::chrono::steady_clock`. Tests inject a fake clock that advances on demand. The exporter worker's batch-deadline arithmetic, retry backoff, and timeout enforcement all consume clocks via these interfaces (`memory-model.md` §5.7, `interfaces.md`).

### 9.2 `IReactor`

The transport's epoll/kqueue loop is behind `IReactor`. Tests inject a fake reactor that delivers events on test-driven schedules — useful for verifying GOAWAY mid-batch, RST_STREAM mid-stream, and partial-frame edge cases without an actual socket.

### 9.3 Synchronous-mode `BatchSpanProcessor` (test-only)

The processor exposes a `_test_only_drain_synchronously()` method (named per the suffix rule in `coding-standards.md` §11) that runs one drain cycle on the calling thread. Tests can drive the entire pipeline from the caller thread, with no worker thread, and observe deterministic results. The method is gated behind a test-only header (`include/microtel/internal/test_support.hpp` — added when the test harness lands in M2; not present in M0).

These three seams collectively make every cross-thread contract in this document unit-testable without spinning up real threads.

---

## 10. Thread-safety category for every public type

| Type | Category | Doxygen tag |
|---|---|---|
| `Tracer` | Thread-safe | `@threadsafety Thread-safe` |
| `Span` | Thread-safe **for distinct spans**; **a single `Span` is externally synchronised** | `@threadsafety Externally synchronized` |
| `Provider` | Thread-safe (lifecycle methods may be called from any caller thread) | `@threadsafety Thread-safe` |
| `SdkBuilder` | **Externally synchronised** — caller serialises chained `WithXxx` calls | `@threadsafety Externally synchronized` |
| `Resource` | Immutable after construction; thread-safe for read | `@threadsafety Thread-safe` |
| `Context` | Immutable value; thread-safe for read. Copying is `noexcept` and allocation-free — both growable members, `TraceState` and `Baggage`, are `shared_ptr` refcount bumps | `@threadsafety Thread-safe` |
| `Baggage` | Immutable value; thread-safe for read. `Set` / `Erase` are copy-on-write and return a new value rather than mutating a shared list (LOCKED — cites `src/api/baggage.cpp:Baggage`) | `@threadsafety Thread-safe` |
| `ScopedContext` | **Thread-confined** — constructed and destroyed on one thread, never shared. Restore is positional: destroy scopes in reverse order of creation (LOCKED — cites `src/api/context.cpp:ScopedContext`) | `@threadsafety Thread-confined` |
| `ScopedSpan` | **Thread-confined**, for the `ScopedContext` it holds. Ends its span before restoring the context (LOCKED — cites `include/microtel/span.hpp:ScopedSpan`) | `@threadsafety Thread-confined` |
| `LeafReceiver` | Thread-safe: `Ingest` and `Stats` may be called concurrently from any application thread, and `Ingest` never blocks. It runs on the caller's thread; microtel starts no thread for it ([ICP 0034](icps/0034-leaf-receiver-api.md)) | `@threadsafety Thread-safe` |
| `ExportTransport` (application-supplied) | **Must be thread-safe when metrics or logs are on**: `Send` is called from one exporter worker per enabled signal (at most three at once), never concurrently for one signal; `Cancel` from the thread running `Provider::Shutdown`, concurrently with `Send` ([ICP 0036](icps/0036-custom-export-transport.md)) | `@threadsafety` in `export_transport.hpp` |
| `LogSink` (callback) | Caller-supplied; microtel makes no thread-safety assumption beyond "may be called from any internal thread" | documented in `log_sink.hpp` |
| `microtel::GetProvider` | Thread-safe, **lock-free and allocation-free** — `kMaxProfiles` acquire-loads and at most that many short string compares. Returns a **borrowed** pointer: valid across that provider's `Shutdown`, dangling after its destruction, `nullptr` in a forked child until it re-builds (LOCKED — cites `src/sdk/provider_registry.cpp:FindProvider`) | `@threadsafety Thread-safe` |

**The current-context slot.** `CurrentContext()` returns a reference into one
`thread_local Context` defined in a single translation unit
(`src/api/context.cpp`), so a process that links `microtel_api` once has exactly
one slot per thread. There is **no cross-thread inheritance**: a newly created
thread starts from a default-constructed `Context`, and a caller that wants
context on a worker copies `CurrentContext()` across the hand-off and installs
it there with `ScopedContext`. §7 (fork) is unaffected — a `Context` is a value
holding no thread, fd, or lock, so the child simply keeps the forking thread's.
See [ICP 0025](icps/0025-propagation-core.md) §3.

**The profile registry.** The other piece of process-scoped state is
`src/sdk/provider_registry.hpp`'s fixed array of atomic slots, which
`SdkBuilder::Build` claims one of and `~SdkProvider` releases before it tears
anything down. It is the only structure a `pthread_atfork` child handler has to
walk, which is why every operation on it is lock-free: a registry mutex held at
`fork()` time by a thread that does not exist in the child would hang the child's
re-`Build()`, the one recovery §7 supports. It owns nothing and extends no
lifetime — a slot is a borrowed pointer, and `GetProvider` hands that borrow on.
See [ICP 0027](icps/0027-multi-profile-threading.md) §2.

For every internal interface, the corresponding contract is in `interfaces.md`.

---

## 11. What this document does not cover

- The exact data-structure choice for the span queue (lock-free ring vs mutex queue) — pinned in M3 against benchmarks. The contract here is sufficient for M0.
- The retry timing and backoff algorithm — that's an exporter detail, captured in the retry sequence diagram.
- The drop counter enumeration and the specific diagnostic surface — see `error-model.md`.
- Per-method `@threadsafety` tags below the public API — see `interfaces.md`.
