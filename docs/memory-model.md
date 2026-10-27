# microtel Memory Model

**Status:** M0 deliverable. Normative for resource ownership and byte budgets in v1.
**Companion documents:** `architecture.md` (layered structure), `threading-model.md` (where ownership transfers cross threads), `error-model.md` (what happens when a budget is exceeded), `interfaces.md` (per-interface allocation behavior).
**Source of truth for rationale:** this document, ICP 0001, `CLAUDE.md` Hard rules §RAII discipline.

---

## 1. Purpose and authority

This document is the canonical answer to two questions:

1. **For every resource microtel touches: who allocates it, who owns it, who frees it, when, and on which thread.**
2. **What are the bounded budgets — bytes, records, attributes — that microtel will not exceed under any input?**

Some rules in this document are non-negotiable in v1. They are flagged **(LOCKED)**. Changing a (LOCKED) rule requires an ICP, the same way a breaking interface change does.

Detail that fits better elsewhere is referenced rather than restated: thread interaction is in `threading-model.md`, the diagnostic surface for budget violations is in `error-model.md`.

---

## 2. Ownership taxonomy

Three categories. Every reference to a resource in microtel falls into exactly one.

| Category | Mechanism | When |
|---|---|---|
| **Sole owner** | `std::unique_ptr<T>` for heap; an RAII wrapper for C resources; a value member for owned subobjects. | Default. The vast majority of references. |
| **Borrowed (non-owning)** | Raw pointer `T*` or reference `T&`. Lifetime is documented in Doxygen. The borrow is shorter than the lifetime of the owner. | Pass to a function that does not retain the reference. Member back-pointer to a parent that outlives the child. |
| **Shared (`std::shared_ptr<T>`)** | Reference-counted. Justification required in code review per `CLAUDE.md` rule §8. | Only when shared ownership is structurally required — e.g., a `Provider` instance returned to user code and also held by a static registry. |

Cycles in shared ownership are broken with `std::weak_ptr<T>`. There are no homegrown smart pointers, no `std::auto_ptr`, no `boost::scoped_ptr`.

**Raw pointers are non-owning by definition (LOCKED).** A function taking `T*` does not delete; a function returning `T*` returns a borrowed reference whose lifetime is documented at the function. Violation is a banned construct under `coding-standards.md` §4.2.

---

## 3. The encoder boundary — upb arena and `EncodedPayload`

This section is the most consequential set of rules in the document. They are the contract between the encoder (`src/wire/encoder/`) and every layer downstream.

### 3.1 upb arena scope (LOCKED)

**One `UpbArena` per call to `IOtlpEncoder::Encode()`.** The arena is created inside `Encode()`, populated with upb messages built from the C++ batch, used to serialise into a freshly-allocated byte buffer, and **destroyed before `Encode()` returns**.

**upb arenas never escape `src/wire/encoder/`.** No upb type, no `upb_Arena*`, no upb-allocated string view appears in any header outside that directory. No pointer derived from an arena outlives the `Encode()` call that produced the arena. (LOCKED.)

Rationale recorded in ICP 0001:

- Re-encoding on retry costs well under 1% of export-batch CPU. Transport dominates. Optimising encoding to dodge a re-encode buys ~nothing.
- Per-batch arenas would cross the encoder/exporter and exporter/transport thread boundaries. `upb_Arena` is not thread-safe by default; sharing it requires synchronisation that undoes the savings.
- Arena lifetime tied to retry state (shutdown mid-retry, partial-success, retry with different headers) is a footgun. Per-encode means each encode is a self-contained transaction; the arena lifetime question never arises.
- Per-encode keeps the live-arena bound at "one arena per encode-in-progress" — `O(1)` in retry depth.

**The trace decoder follows the same rule** (v1.2, `leaf-concentrator-design.md` §3.4). `IOtlpTraceDecoder::Decode()` — upb's other use in the runtime, in the same directory — creates one arena per call over a counting `upb_alloc` that refuses to grow past `DecodeLimits::max_arena_bytes`, decodes into it with a depth limit, copies what it needs into plain C++ values (strings are copied, never aliased), and destroys the arena before `Decode()` returns. The cap turns a payload that would blow up in memory into a `TooLarge` rejection. The leaf receiver sets it to 16 × `max_payload_bytes` + 16 KiB: measured against upb v29.4, legitimate payloads need 6–15 × their wire size plus about 2 KiB of fixed arena overhead, so at the default 64 KiB `max_payload_bytes` one `Ingest` holds at most about 1 MiB of transient arena.

If a future change wants per-batch arenas (e.g., for a profile signal where re-encoding cost matters), it is an ICP. The burden of proof is on the proposer to show the cost analysis above no longer holds.

### 3.2 `EncodedPayload` shape (LOCKED — cites `include/microtel/internal/encoded_payload.hpp:EncodedPayload`)

`IOtlpEncoder::Encode()` returns:

```
class EncodedPayload
{
public:
    EncodedPayload() noexcept = default;  // the empty payload
    EncodedPayload(std::unique_ptr<std::byte[]> bytes, std::size_t size) noexcept;
    EncodedPayload(const EncodedPayload&) = delete;
    EncodedPayload& operator=(const EncodedPayload&) = delete;
    EncodedPayload(EncodedPayload&&) noexcept = default;
    EncodedPayload& operator=(EncodedPayload&&) noexcept = default;
    ~EncodedPayload() noexcept = default;

    [[nodiscard]] std::span<const std::byte> Bytes() const noexcept;
    [[nodiscard]] std::size_t Size() const noexcept;
    [[nodiscard]] std::unique_ptr<std::byte[]> Release() && noexcept;  // leaves it empty

private:
    std::unique_ptr<std::byte[]> m_bytes;
    std::size_t m_size = 0;
};
```

- **Owned bytes plus size.** The buffer is heap-allocated by the encoder; the encoder transfers ownership to the caller (the exporter) on return.
- **Move-only.** No copy. Sharing encoded bytes across owners is not a v1 concern.
- **No upb type appears in this header.** The bytes are plain `std::byte`.
- **Allocator.** Default `new[]`/`delete[]` (`std::default_delete<std::byte[]>`) in v1. A polymorphic-allocator (`pmr`) variant backed by a thread-local buffer pool is a v1.6 performance refinement gated on benchmark evidence; not in v1.

### 3.3 Lifetime of `EncodedPayload`

| Step | Owner | Thread |
|---|---|---|
| 1. Encoder allocates the buffer inside `Encode()` | encoder local variable | exporter worker |
| 2. Encoder returns the `EncodedPayload` by value | exporter worker thread (caller of `Encode`) | exporter worker |
| 3. Exporter passes the payload to `IWireCodec::Send` by **rvalue reference** | wire codec | exporter worker, at first |
| 4. Wire codec hands the bytes to `ITransport` (with a stable address until response or cancellation) | transport, borrowed; codec retains ownership | crosses to I/O thread |
| 5. On response (success, failure, retry) the codec releases the payload | dropped | exporter worker (after I/O completion) |

**On retry, the original `EncodedPayload` is released. The exporter calls `Encode()` again.** Encoded bytes do not survive a retry. (LOCKED — same rationale as §3.1.)

The exact synchronisation rule for handing the byte span to the I/O thread without copying is in `threading-model.md` §3.3 (transport request queue).

### 3.4 What is not in this header

`EncodedPayload` does not include:

- **Headers.** Wire-protocol headers are constructed by the codec, not the encoder.
- **Compression flag.** The encoder produces uncompressed bytes; per-protocol compression is a codec concern.
- **Resource or instrumentation-scope identity.** These are inputs to `Encode()`; they are folded into the bytes.
- **Retry metadata.** The exporter holds retry state separately from the payload.

If you find yourself wanting to add a field to `EncodedPayload` that is protocol-specific, it belongs on `WireResult` (the codec's return type) or on the exporter's internal retry state. ICP if unsure.

---

## 4. Per-resource ownership

The complete table of ownership in microtel v1. Rows are alphabetized within their layer.

### 4.1 Heap memory

| Resource | Owner | Allocated by | Released by | Notes |
|---|---|---|---|---|
| `Span` records (after `End()`) | MPSC queue inside the `BatchSpanProcessor` | caller thread | exporter worker, after batching | Move-only. Bounded by `max_queue_size` in records, `max_record_bytes` per record, and `max_total_queue_bytes` in aggregate — whichever binds first (§6). |
| `Resource` value | `Provider` | `SdkBuilder::Build()` | `Provider` destruction | Frozen at build; immutable after. |
| `Config` value | `Provider` | `SdkBuilder::Build()` | `Provider` destruction | Frozen at build; no runtime mutation in v1. |
| `EncodedPayload` | exporter worker → wire codec → transport (in-flight only) | encoder | last layer holding it on completion | See §3. |
| Pending request / response buffers | wire codec | codec, sized at `max_response_bytes` | on completion or cancel | Bounded (§6). |
| Diagnostic ring buffers | `IDiagnosticsSink` | sink | sink destruction | Bounded; rate-limited writes. |

No heap memory is allocated on the unsampled-span hot path (§7).

### 4.2 C resources (RAII-wrapped)

This table is the authoritative list of microtel's RAII wrappers ([ICP 0040](icps/0040-contract-docs-match-the-code.md)). All are in `src/common/raii/` except `UpbArena`, which lives in `src/wire/encoder/` because including it means including a upb header (§3.1). Each is move-only, has a `noexcept` destructor, and, where something can take ownership of the resource, exposes `Release()` (point 4 below).

| RAII type | Owns | Created by | Notes |
|---|---|---|---|
| `UniqueFd` | `int` file descriptor | transport (the socket, on `Connect`), `EpollReactor` (the epoll and wake descriptors), resource detectors (files read once) | Calls `close(2)` on destruction. |
| `SslCtx` | `SSL_CTX*` | transport, on `Connect` | `SSL_CTX_free` on destruction. Owned by `Transport` — one per transport, so a process running several named profiles has one per profile (v1.1, [ICP 0027](icps/0027-multi-profile-threading.md)). See [ICP 0003 §3.1](icps/0003-m0-deferred-decisions.md#31-sslctx-ownership--per-transport). |
| `SslSession` | `SSL*` | transport (per connection) | `SSL_free` on destruction. Cleanly closes the session if open. |
| `Nghttp2Session` | `nghttp2_session*` | transport (per connection) | `nghttp2_session_del` on destruction. |
| `BioMethod` | `BIO_METHOD*` | transport (`MakeBioMethod`, the SIGPIPE-safe socket BIO) | `BIO_meth_free` on destruction. |
| `DeflateStream` | zlib `z_stream` set up by `deflateInit2` | gzip (per compression) | `deflateEnd` on destruction if `Init` succeeded. No `Release()` (point 4). |
| `InflateStream` | zlib `z_stream` set up by `inflateInit2` | gzip (per decompression) | `inflateEnd` on destruction if `Init` succeeded. No `Release()` (point 4). |
| `UpbArena` | `upb_Arena*` | encoder (per `Encode()` call) | `upb_Arena_Free` on destruction. **Never escapes `src/wire/encoder/`** (§3). |

**RAII wrapper contract (LOCKED — cites `src/common/raii/unique_fd.hpp:Release`, `src/common/raii/deflate_stream.hpp:DeflateStream`).** Each wrapper:

1. Is move-only — copy constructor and copy assignment are `= delete`.
2. Has a `noexcept` destructor.
3. Has a `noexcept` move constructor and move assignment.
4. `Release()` is required where a C API or caller can take ownership of the resource; wrappers around state that no API adopts (e.g. `z_stream`) don't provide it. Where provided, `Release()` returns the underlying handle and leaves the wrapper in an "empty" state (handle equal to a documented sentinel; destructor on the empty state is a no-op).
5. Has no copyable shadow — no `clone()`, no implicit conversion to the underlying handle.

### 4.3 Synchronisation primitives and threads

| Resource | Owner | Notes |
|---|---|---|
| Span queue mutex / condition variable | `BatchSpanProcessor` | Member; lifetime tied to processor. |
| Transport request queue lock | `Transport` | Hands payloads from exporter worker to I/O thread; see `threading-model.md` §3. |
| Exporter worker thread | `BatchSpanProcessor` | Joined in `Shutdown()`. |
| I/O thread | `Transport` | Joined in `Transport::Close()`. |

Threads are owned by the component that started them. Both threads are joined before the owner's destructor returns. `Shutdown(timeout)` is the documented way to bound this — destructors invoke `Shutdown` with a finite timeout if not already shut down (`threading-model.md` §6.2).

### 4.4 Callback registrations

Any callback registered with an external library (nghttp2, OpenSSL, libuv-style reactor) is owned by an RAII handle returned at registration. The handle's destructor unregisters before the callback's memory could be reclaimed. Registrations never outlive their owning component.

---

## 5. Lifetime diagrams for the trickiest cases

### 5.1 Span — from `StartSpan` to drop or export

```
caller thread                  exporter worker thread             I/O thread
─────────────                  ──────────────────────             ──────────
StartSpan()                                                       
   │                                                              
   ▼                                                              
[Span object]  ◄─── owned by caller (RAII)                        
   │                                                              
   │ SetAttribute, AddEvent…                                      
   │                                                              
   ▼                                                              
End() ─── enqueue ────────────►  [MPSC queue]                     
                                       │                          
                                       │ batch deadline /         
                                       │ size trigger             
                                       ▼                          
                                 [Batch] ──── Encode() ───►       
                                                  │               
                                                  ▼               
                                          [EncodedPayload] ──►    
                                                            Send  
                                                              │   
                                                              ▼   
                                                     [request queue]
                                                              │   
                                                              ▼   
                                                       nghttp2 write
```

**Ownership transitions:**

1. `Span` is owned by the caller from `StartSpan()` until `End()` returns. RAII auto-end fires at scope exit if `End()` was not explicit.
2. Inside `End()`, the span record is **moved** into the MPSC queue. After enqueue, the caller's `Span` handle is in the moved-from state and is destroyed silently.
3. If the queue is full at enqueue time, the record is dropped (see §7.2 for which side decides which records are dropped). The caller's `Span` is still destroyed silently — the application sees no error.
4. Batch construction moves records out of the queue. The batch owns those records until `Encode()` consumes them.
5. `Encode()` produces an `EncodedPayload`. The batch and its records are released after encoding.

### 5.2 `EncodedPayload` — from `Encode()` to release

```
exporter worker thread                         I/O thread
──────────────────────                         ──────────
Encode(batch) ── new[] ──► [EncodedPayload]
                                  │
                                  │ Send(payload, deadline)
                                  ▼
                          [WireCodec request slot]
                                  │
                                  │ enqueue on transport
                                  ▼
                          [Transport request queue] ─── wakeup ──► I/O thread
                                                                       │
                                                                       │ borrows bytes
                                                                       │ via stable span
                                                                       ▼
                                                              nghttp2 DATA frames
                                                                       │
                                                                       │ END_STREAM
                                                                       ▼
                                                              response ready
                          on-response notification ◄───────────────────┘
                                  │
                                  ▼
                          parse → WireResult
                                  │
                                  ▼
                          release EncodedPayload (delete[])
```

**Ownership transitions:**

1. The encoder allocates the buffer and moves ownership into the returned `EncodedPayload`.
2. The exporter passes the `EncodedPayload` to `IWireCodec::Send` by rvalue reference. The codec stores it inside its in-flight request record.
3. The codec hands a `std::span<const std::byte>` to the transport. The transport **borrows** these bytes; the codec retains ownership and guarantees the buffer is stable until response, cancellation, or shutdown.
4. The I/O thread reads the borrowed bytes and writes them to nghttp2. nghttp2 may copy into its own send buffers; that's an nghttp2 implementation detail and not microtel's concern.
5. On response, the codec parses status, builds `WireResult`, and **releases** the `EncodedPayload` — `unique_ptr` destruction frees the buffer.
6. On retry, the exporter does not re-use the released buffer; it calls `Encode()` again.

The synchronisation that makes step 3 safe — guaranteeing the codec does not drop the buffer while the I/O thread is reading it — is documented in `threading-model.md` §3.3.

### 5.3 Connection — from `Connect()` to `Close()`

```
process start                  shutdown
─────────────                  ────────
Transport::Connect()
   │
   ▼
[UniqueFd]                     
[SslCtx]   ◄── one per Transport, shared across that transport's reconnects
[SslSession]                   
[Nghttp2Session]               
   │
   ├───────── SETTINGS exchange ─────────►
   │
   │ in-flight requests reference the session
   │
   ▼
… steady operation …
   │
   ▼
Transport::Close(timeout)
   │
   ├──── send GOAWAY ─────────────────────►
   │
   ├──── drain in-flight (up to timeout) ─►
   │
   ▼
[Nghttp2Session] destroyed
[SslSession]     destroyed (clean shutdown if reachable)
[UniqueFd]       destroyed (close)
```

`SslCtx` is shared across reconnects, **one per `Transport`**; `SslSession`, `Nghttp2Session`, and the socket's `UniqueFd` are per-connection and owned by the `Transport`. Reconnect after a socket-level failure releases the per-connection trio and creates a new one with backoff.

This said "one per process" until v1.1, contradicting §4.2's table two sections up and the code both, which have had `Http2Transport::m_ssl_ctx` — a per-transport member — since [ICP 0003](icps/0003-m0-deferred-decisions.md) §3.1 decided it. The two readings were indistinguishable while a process built one `Provider`; multi-profile ([ICP 0027](icps/0027-multi-profile-threading.md)) makes them differ, because each named profile builds its own transport and so owns its own `SslCtx`, socket and reactor.

---

## 6. Memory budgets

These byte budgets are normative; their settings are in `configuration.md` §3.8. Each value has a default and a configurable override; defaults are listed.

| Budget | Default | Layer that enforces | What happens at boundary |
|---|---|---|---|
| `max_total_queue_bytes` | 16 MiB | `BatchSpanProcessor::OnEnd` | Refuse the incoming record, or evict the oldest until it fits, per `drop_policy` — the same branch the `max_queue_size` cap takes. Drop counter `queue_full`. |
| `max_record_bytes` | 64 KiB | `BatchSpanProcessor::OnEnd` | Reject the incoming record before it is queued. Drop counter `record_too_large`. |
| `max_response_bytes` | 1 MiB | `Http2Transport`, as the response body is accumulated | Stop buffering, release what was buffered, `RST_STREAM(CANCEL)`, fail the request. The codec classifies it non-retryable. Drop counter `response_too_large`. |
| `max_trailer_bytes` | 64 KiB | `Http2Transport`, as the trailers are accumulated, **and** nghttp2, from the `SETTINGS_MAX_HEADER_LIST_SIZE` the client advertises | Trailers: same as `max_response_bytes`, and the same counter — `response_too_large` covers both (`error-model.md` §3). Response headers: nghttp2 refuses the oversized HEADERS block itself and the request fails at the session level (issue #213). |
| `max_decompressed_bytes` | 4 MiB | wire codec | Decompression-bomb protection. Fail the request, non-retryable. Drop counter `decompression_too_large` (`error-model.md` §7.1/§7.2). |

**Counting rules.**

- `max_record_bytes` counts a **size estimate** of the record, not the C++ object size: fixed per-record overhead plus the owned buffers (name, status description, attribute keys and values, events, links). `sdk::EstimateRecordBytes` is the single definition, and its Doxygen carries the formula.
- `max_total_queue_bytes` is the sum of the same estimate over everything currently queued. Each record carries the estimate it was admitted with, so the running total is decremented by exactly what it was incremented by when the record is evicted or drained. It measures what the queue *holds*, not what it has ever held.
- `max_response_bytes` is the count of DATA-frame bytes accumulated for one response. The check is made before each chunk is appended, so the limit is never exceeded even transiently, and the request fails rather than being handed a truncated body.
- `max_trailer_bytes` is the summed name+value bytes of one response's trailers, counted the same way. It is also what the client advertises as `SETTINGS_MAX_HEADER_LIST_SIZE`, so the non-trailer response headers — which the transport does not meter as it appends them — are bounded by the same number, enforced by nghttp2 (issue #213). The two HEADERS frames on a stream are both header lists; one budget covers them.
- `max_decompressed_bytes` is the count of bytes produced by the decompressor regardless of the compressed input size.

The transport, not the codec, enforces the two response budgets: the buffers they bound are the transport's own, and by the time a codec sees a `TransportResult` the bytes have already been spent. Retry *classification* stays in the codec (ICP 0001), which turns `TransportResult::response_too_large` into the terminal `response_too_large` row of `error-model.md` §7.1.

The diagnostic surface for each of these — drop counter increment, rate-limited log emission, exposure through `GetExporterHealth()` — is in `error-model.md`.

---

## 7. Span structural limits

These span limits are normative; their settings are in `configuration.md` §3.9.

| Limit | Default |
|---|---|
| `attribute_count_limit` | 128 |
| `event_count_limit` | 128 |
| `link_count_limit` | 128 |
| `attribute_value_length_limit` | 4096 |
| `event_attribute_count_limit` | 128 |
| `link_attribute_count_limit` | 128 |

**Behaviour on overflow.** The new attribute / event / link is **dropped and counted**. Existing values are not evicted (no LRU in v1). Drop counters: `span_attribute_limit`, `span_event_limit`, `span_link_limit`. String values exceeding `attribute_value_length_limit` are truncated and the truncation is counted (`attribute_value_truncated`) — the attribute itself is kept, so this is the one limit that shortens a record rather than shedding part of it. The limit is a **byte** budget and the cut backs up to a UTF-8 code point boundary: an OTLP attribute is a proto3 `string`, and a cut through a multi-byte sequence would make the whole batch unparseable at the collector. Each element of a string array is measured and counted separately.

**Where enforced.** In the API layer, at the `SetAttribute` / `AddEvent` / `AddLink` call site, before the value is recorded on the span. Enforcement at the API ensures unsampled spans never spend memory on enforcement state — the unsampled `Span` object is a no-op for these methods.

---

## 8. Hot-path allocation rules

### 8.1 Unsampled span (LOCKED — zero-allocation)

The unsampled-span path **does not allocate on the caller thread.** Specifically, in this sequence:

```
auto span = tracer->StartSpan("name");   // sampler returns NOT_SAMPLED
span->SetAttribute("k", "v");
span->AddEvent("e");
span->End();
```

every method must complete without calling `operator new`, `malloc`, or any allocating standard-library function.

**Implementation (LOCKED per [ICP 0003 §3.2](icps/0003-m0-deferred-decisions.md#32-unsampled-span-shape--unique_ptrspan-to-no-op-singleton)).** `Tracer::StartSpan` returns `microtel::SpanHandle`, defined as `std::unique_ptr<Span, internal::SpanDeleter>`. On the unsampled path:

- The `Span*` points at a **static no-op singleton** in non-heap storage.
- `internal::SpanDeleter::deleter` is `nullptr` (or a no-op function pointer); destruction does nothing.
- `Span::SetAttribute`, `AddEvent`, `AddLink`, `SetStatus`, `UpdateName`, `End` are `noexcept` no-ops.

```cpp
auto span = tracer->StartSpan("name");   // unsampled: SpanHandle wraps the singleton, no allocation
span->SetAttribute("k", "v");            // no-op, returns immediately
// scope exit: SpanDeleter::operator() is a no-op; nothing freed
```

The internal shape of `SpanDeleter` (function pointer vs. flag-in-`Span` branch vs. two-deleter-types) may be revised in v1.x based on benchmark findings; `SpanHandle` itself is the stable public surface. See ICP 0003 §3.2 for the implementation-flexibility note.

### 8.2 Sampled span — bounded allocation

The sampled path may allocate but does so within bounds:

- A single allocation for the `Span` record (or pulled from a per-thread freelist; freelist is an optimisation, not a requirement, in v1).
- Attributes, events, and links allocate when they are added. Each allocation is bounded by `attribute_value_length_limit` for strings and the count limits in §7 for structures.
- No allocation occurs on `End()` beyond the queue-push path. The MPSC queue uses pre-allocated nodes or a lock-free linked structure with bounded growth (chosen in `threading-model.md`).
- The queued element pairs the record with the tracer's `InstrumentationScope` ([ICP 0023](icps/0023-span-processor-scope.md)), so the push copies two short strings — normally within `std::string`'s SSO buffer, and bounded by `max_queue_size` in the worst case. The logs pipeline's `QueuedLog` has the same shape.

### 8.3 Encoder

- One arena per `Encode()` call.
- One byte-buffer allocation per `Encode()` call.
- Inside the arena, allocations are bump-pointer; freeing is `O(1)` at arena destruction.

### 8.4 Wire codec

- One in-flight request record per `Send()`. Bounded by the exporter's outstanding-request count (which is itself bounded by the batch concurrency setting).
- Response buffer allocation is sized eagerly to a small initial capacity and grown to at most `max_response_bytes`.

### 8.5 Transport

- nghttp2 owns its own buffers; microtel does not allocate on the I/O thread per-frame. Per-connection allocations (the socket's `UniqueFd`, `SslSession`, `Nghttp2Session`) happen at `Connect()` and reconnect.

---

## 9. Cross-thread ownership transfer

A complete summary; details and the concrete synchronisation are in `threading-model.md`.

| Hand-off | From | To | Mechanism |
|---|---|---|---|
| Span record at `End()` | caller thread | exporter worker | MPSC queue (move-into-queue) |
| `EncodedPayload` to wire codec | exporter worker | exporter worker (codec executes synchronously) | `Send` rvalue reference; codec stores in in-flight record |
| Request bytes to I/O thread | wire codec (on exporter worker) | I/O thread | transport request queue + `std::span<const std::byte>` borrow; ownership stays with the codec until response |
| Response bytes to wire codec | I/O thread | wire codec (on exporter worker, via callback dispatch) | request record, completed under transport's lock; bytes copied from nghttp2 into the codec's response buffer |
| `Shutdown` request | caller thread | exporter worker → I/O thread | shutdown signal; both threads observe it and drain |

**No exception ever crosses a thread boundary** (`error-model.md` §5). Errors are recorded as diagnostics on the producing thread; the consuming thread observes a failed result and acts on it.

---

## 10. What this document does not cover

- The exact synchronisation primitives, lock-ordering, and wakeup mechanisms for the cross-thread hand-offs in §9 — see `threading-model.md`.
- The drop-reason enum, the diagnostic surface, and the `Status` returned by lifecycle calls — see `error-model.md`.
- The interface signatures themselves — `IOtlpEncoder`, `IWireCodec`, `ITransport`, `ISpanProcessor` — see `interfaces.md`.
- Encoder implementation specifics inside the arena (which proto messages are built in which order) — these are internal to `src/wire/encoder/` and not architecturally significant.
- The shape of the unsampled-`Span` API (no-op singleton vs. unique_ptr-to-no-op vs. value type) — decided in `interfaces.md` and the public headers.
