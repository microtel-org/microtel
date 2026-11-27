# microtel Error Model

**Status:** M0 deliverable. Normative for the three error regimes, the drop-counter enum, the retry-classification matrix, and the diagnostic surface in v1.
**Companion documents:** `architecture.md`, `threading-model.md` (no-exceptions-across-threads), `memory-model.md` (which budget enforcement triggers which drop), `interfaces.md` (per-method error annotation).
**Source of truth for rationale:** this document and the ICPs it cites; the gRPC wire detail is in `grpc-wire-protocol.md`.

---

## 1. Purpose and authority

This document is the canonical answer to:

1. **For every public method, how does it report failure?** (return `microtel::Expected`, return a structured `Status`, drop-and-count, log-only.)
2. **What are the named drop reasons, and which layer increments which counter?**
3. **For both wire protocols, which response is retryable, which is non-retryable, which is malformed?**
4. **What does a caller see when something fails?**

Some rules are non-negotiable in v1; they are flagged **(LOCKED)**. Changing a (LOCKED) rule requires an ICP.

---

## 2. The three error regimes

Every microtel method falls into one of three regimes. The choice is structural — never mixed.

### 2.1 Initialisation regime — `microtel::Expected<T, E>` (LOCKED)

`SdkBuilder::Build()` and any other one-shot configuration entry point that can fail validates eagerly and returns `microtel::Expected<std::shared_ptr<Provider>, ConfigError>`. No exceptions on init paths.

`microtel::Expected` is the project-local alias defined in `include/microtel/expected.hpp` per [ICP 0002](icps/0002-vendor-tl-expected.md). It resolves to `std::expected<T, E>` when the floor moves to C++23; on C++20 it aliases the vendored `tl::expected` polyfill. Callers see the same shape regardless of the active backend.

Why expected and not exceptions:

- Init failures are programmer-actionable. The application should branch on them, log them, and either retry with adjusted config or exit. Exceptions encourage swallowing.
- Init failures often happen during static-init of larger systems where exceptions are awkward.
- `microtel::Expected` composes naturally with structured logging.

### 2.2 Hot-path regime — `noexcept` and drop-and-count (LOCKED)

Every method on `Tracer` and `Span` listed in `threading-model.md` §2.1 is `noexcept`. On any internal failure (queue full, span limit exceeded, post-shutdown call, malformed input), the method:

1. Drops the record / field / event.
2. Increments the appropriate counter on `IDiagnosticsSink` (§3 below).
3. Returns silently.

The application sees no error. This is intentional — telemetry instrumentation must never disturb the host application's control flow.

### 2.3 Lifecycle regime — structured `Status` (LOCKED)

`Provider::ForceFlush(timeout)` and `Provider::Shutdown(timeout)` return a single small enum:

```
enum class Status : std::uint8_t
{
    Completed       = 0,    // operation finished within the timeout
    TimedOut        = 1,    // partial work; some data may not have been flushed
    AlreadyShutDown = 2,    // idempotent re-call after Shutdown
    Failed          = 3,    // unrecoverable internal error; see GetExporterHealth()
};
```

The `Status` is `[[nodiscard]]`. Callers must inspect it. Detail beyond the four values lives in `Provider::GetExporterHealth()` and the diagnostic log; the lifecycle return is intentionally coarse.

---

## 3. Drop-counter enum (LOCKED)

Each drop reason maps to exactly one counter. The counter is incremented exactly once per drop event by the layer that decides the drop. Counters are exposed via `Provider::GetExporterHealth()` and rate-limited in the diagnostic log.

| Reason (counter name) | Where incremented | Triggered by |
|---|---|---|
| `queue_full` | `BatchSpanProcessor` / `BatchLogRecordProcessor` on enqueue, and each exporter's `Export` | queue at capacity — in records (`max_queue_size`) or, for spans, in bytes (`max_total_queue_bytes`); whichever fills first. An exporter's queue is bounded in batches, and the trace exporter's also in the spans they carry (`max_queued_spans`, issue #345). Counted in records, not batches: a batch the exporter refuses costs every record in it. Both drop policies lose one record per rejection — the policy picks which one. The byte cap may need more than one eviction to admit a record, and each one is counted |
| `record_too_large` | `BatchSpanProcessor::OnEnd`, before the record is queued | record's size estimate (`sdk::EstimateRecordBytes`) exceeds `max_record_bytes`. Counted in records: the record is refused, never queued, and the rest of the batch is unaffected |
| `span_attribute_limit` | API layer, in `SetAttribute` | per-span `attribute_count_limit` reached |
| `span_event_limit` | API layer, in `AddEvent` | per-span `event_count_limit` reached |
| `span_link_limit` | API layer, in `AddLink` | per-span `link_count_limit` reached |
| `event_attribute_limit` | API layer, in `AddEvent` (per-event attributes) | per-event `event_attribute_count_limit` reached; counts the surplus attributes, the event itself is kept |
| `link_attribute_limit` | API layer, in `AddLink` (per-link attributes) | per-link `link_attribute_count_limit` reached; counts the surplus attributes, the link itself is kept |
| `attribute_value_truncated` | SDK, in `SdkSpan::SetAttribute` and in the event / link attribute copies | a string attribute value exceeded `attribute_value_length_limit` and was truncated. The attribute is **kept** — key, type and the surviving prefix — so this counts data lost, not a record lost. One per truncated *value*: a string array counts each element it clipped. The limit is a byte budget and the cut backs up to a UTF-8 code point boundary, because an OTLP attribute is a proto3 `string` and a cut through a multi-byte sequence would cost the whole batch at the collector |
| `post_shutdown` | `BatchSpanProcessor` / `BatchLogRecordProcessor`, and each exporter's `Export` | call after `Shutdown` returned. Counted in records, as `queue_full` is. A leaf payload that reaches `LeafReceiver::Ingest` after `Shutdown` is not decoded and is not counted here; it is `LeafReceiverStats::payloads_post_shutdown` ([ICP 0035](icps/0035-leaf-post-shutdown-counter.md)) |
| `response_too_large` | wire codec, on a transport result flagged `response_too_large` | response body exceeded `max_response_bytes`, **or** the trailers exceeded `max_trailer_bytes`. The transport detects both as it accumulates (it owns the buffers), releases what it had, resets the stream, and fails the request; the codec counts it and classifies it terminal. `max_trailer_bytes` has no counter of its own — the `Error` message names which cap it was |
| `decompression_too_large` | wire codec, per response whose decompression hit the ceiling | decompressed body exceeded `max_decompressed_bytes`. Recorded by both codecs: `content-encoding: gzip` on OTLP/HTTP, a `CF = 0x01` message on OTLP/gRPC. Decompression stops at the ceiling, so the bomb is never materialised |
| `malformed_response` | wire codec, per observed malformed response | response could not be parsed (missing trailers, bad framing, unparseable proto). A 2xx / `OK` Export response whose body is not a well-formed protobuf message counts here too: `ParseRejectedSpans` tells an absent `partial_success` from an unparseable body, and the codec classifies the latter terminal (§7.1, §7.2) |
| `partial_success_rejection` | exporter, in the final-outcome funnel | rejected items count from the response — see §6. The codec parses the count; the exporter records it, so one request yields one accounting whatever the retry path did. The trace exporter may send several batches as one request (`leaf-concentrator-design.md` §3.6.1); the collector's count is then for the whole request and is recorded once |
| `non_retryable_failure` | exporter, in the final-outcome funnel | the batch's terminal outcome was a non-retryable failure (415, gRPC `INVALID_ARGUMENT`, etc.). Classification stays in the codec (§7); only the counting moved, so intermediate attempts cannot double-count. A trace request that carries several batches is classified once and counted once per batch in it, so the count is what it was before batches were joined into requests; `batches_failed` likewise |
| `retryable_failure_recovered` | exporter | a retryable failure that subsequently succeeded — counted for visibility, not a drop. Once per batch in the recovered request, as `non_retryable_failure` is |
| `retry_budget_exhausted` | exporter, in the final-outcome funnel | the batch was retried and still lost. Covers budget exhaustion, running out of `max_attempts`, and a retry ended by `Shutdown` (§7): all are "retried, still gone" to an operator, and one exit is taken per request, counted once per batch in it |
| `transport_busy` | wire codec, on a transport result flagged `transport_busy` | the transport's request queue was already at `ConnectOptions::max_pending_requests`, so `Send` refused the request instead of queueing it (§3.2 of `threading-model.md`). Nothing was written to the wire. Unlike `response_too_large` the classification stays **retryable** — a full queue drains — so the counter records an attempt that lost its batch, not a terminal outcome |
| `connect_failure` | wire codec `EnsureConnected` (lazy path) and `Provider::Connect` (eager path) | TCP / TLS / ALPN handshake failed during initial connect or reconnect. The transport owns no diagnostics sink, so its callers record what they observe; a given attempt runs through exactly one of the two paths. One failed connect is one increment however many batches were waiting behind it |
| `force_flush_timeout` | `Provider::ForceFlush` | `ForceFlush` deadline elapsed with records still queued. Recorded at the Provider and nowhere else: it drives several components that can each time out, and one user call must produce one drop |
| `shutdown_timeout` | `Provider::Shutdown` | `Shutdown` deadline elapsed with in-flight work. Recorded at the Provider only, same reasoning as `force_flush_timeout` |
| `cardinality_overflow` | SDK, metric aggregation store | attribute set exceeded the per-instrument cardinality limit; the measurement is folded into the `otel.metric.overflow` series, not lost (ICP 0008, `metrics-design.md` §2) |
| `metric_callback_timeout` | *(not yet produced)* | async instrument callback exceeded the per-collection deadline (ICP 0008, `metrics-design.md` §4) — awaiting a per-collection callback deadline |
| `non_finite_value` | SDK, instrument record path | NaN / ±Inf measurement dropped, as the OTel spec requires (ICP 0008) |
| `log_attribute_limit` | SDK, `SdkLogger::Emit` | a `LogRecord`'s attribute set exceeded the per-record attribute limit; surplus attributes dropped and `dropped_attributes_count` incremented (ICP 0011, `logs-design.md` §5) |
| `leaf_payload_malformed` | `LeafReceiver::Ingest` | a leaf payload failed to decode or failed validation (`leaf-concentrator-design.md` §3.4), declared an unsupported wire version, declared a time mode its leaf's config does not allow (§5.1), declared no mode at all from a sender not configured `unix` (§5.6), carried no span and no positive `microtel.leaf.dropped_*` ([ICP 0036](icps/0036-custom-export-transport.md); a metrics or logs request handed to `Ingest` usually ends here), or carried no usable leaf id. Counted in **payloads**: a payload that cannot be decoded has no trustworthy span count, and the whole payload is rejected ([ICP 0034](icps/0034-leaf-receiver-api.md)) |
| `leaf_payload_too_large` | `LeafReceiver::Ingest` | a leaf payload exceeded `max_payload_bytes`, `max_spans_per_payload`, the decode depth limit or the decode arena cap (§3.7). Counted in payloads. An allocation failure is **not** counted here: it is `IngestStatus::OutOfMemory` and `LeafReceiverStats::payloads_out_of_memory`, because it is the concentrator that is short of memory, not the leaf that sent too much (ICP 0034) |
| `leaf_unknown` | `LeafReceiver::Ingest` | a payload came from a leaf with no configuration while `unknown_leaf = reject` (§4.4). Counted in payloads |
| `negative_histogram_value` | SDK, histogram storage record path | a negative measurement recorded on a `Histogram` or `ExponentialHistogram` instrument was dropped before aggregation, so it reaches neither `sum`, `count`, `min` nor the buckets; `-0.0` is recorded as zero. Counted per measurement, as `non_finite_value` is, and logged at `warn` naming the instrument ([ICP 0047](icps/0047-histogram-drops-negative-values.md), `metrics-design.md` §3). Counters, gauges and observable instruments are unaffected |

**Leaf spans after decode.** A leaf payload that passes validation feeds its spans into the same pipeline as in-process spans, so they are counted by the reasons above as records: the span limits (`span_attribute_limit` … `attribute_value_truncated`) are applied by the leaf receiver as `SdkSpan` applies them, `span_attribute_limit` also counts a decoded attribute whose value `AttributeValue` cannot hold, and `queue_full` / `record_too_large` / `post_shutdown` come from the processor. Leaf Resource attributes dropped over `max_leaf_resource_bytes` (or `max_node_resource_bytes` for a `unix` full node, ICP 0036) are not a drop reason; they are `LeafReceiverStats::resource_attributes_dropped`, with one `Warn` per leaf-table entry that loses any (design §4.5).

**Counters are `std::atomic<uint64_t>`** (LOCKED). The increment path is lock-free and fits the leaf-lock rule in `threading-model.md` §4, so a counter may be moved while a mutex is held, and is always moved on the thread that detected the drop (`threading-model.md` §8.6).

**Ownership rule.** Two kinds of counter sit in this table, and they are placed differently:

- **Final-outcome counters** (`partial_success_rejection`, `non_retryable_failure`, `retry_budget_exhausted`, `retryable_failure_recovered`) are recorded by the *exporter*, once per batch, after every retry has resolved. The wire codec still owns the classification (§7, ICP 0001) — the exporter reads `WireResult` without reinterpreting it. Recording in the codec instead would count every retry attempt as a separate outcome.
- **Observation counters** (everything else) are recorded at the site that detects the drop.

**One counter has no producer yet** and is marked *(not yet produced)* above. It is enumerated because `DropReason`'s order is a locked part of the public health surface; it awaits the feature whose limit it reports, not a wiring fix — `metric_callback_timeout` needs the per-collection async-callback deadline of ICP 0008, which is v1.3 metrics work. Every other counter in the table left that list as the v1.0 limits gate closed (issue #181): `record_too_large` at `BatchSpanProcessor::OnEnd`, `response_too_large` (and `max_trailer_bytes`) in the transport, `decompression_too_large` in the gzip inflate path, `attribute_value_truncated` in the SDK's attribute copies, `transport_busy` at the transport's bounded request queue. `max_total_queue_bytes` is enforced at `BatchSpanProcessor::OnEnd` too, and needs no counter of its own: a record refused for it is `queue_full`.

**Adding a new counter is an ICP** because every counter is part of `GetExporterHealth()`'s public surface. Renaming a counter is an ICP. Re-attributing an existing counter to a different layer is not — the counter's meaning is what is locked, not which file writes it.

---

## 4. Error types

Two value types and one enum cover the regimes in §2.

### 4.1 `microtel::Error` — runtime error description

A small value type used as the `E` in any non-init `microtel::Expected` path, and as the carried payload in `WireResult::error` for non-success cases. Defined in `include/microtel/error.hpp`.

```
class Error
{
public:
    enum class Kind : std::uint8_t
    {
        Unspecified           = 0,
        Network               = 1,   // socket / TLS / nghttp2 transport error
        Protocol              = 2,   // wire/protocol mismatch: OTLP status, or a peer that is not h2
        ResourceExhausted     = 3,   // peer signalled overload; retryable depending on RetryInfo
        Cancelled             = 4,   // local cancel (timeout, shutdown)
        Malformed             = 5,   // unparseable response or trailer
        InternalFailure       = 6,   // microtel bug; should never happen in production
    };

    Kind        kind = Kind::Unspecified;
    std::string message;          // short, redacted, safe to log
    int         os_errno = 0;     // optional OS errno or library code
};
```

`message` is short and pre-redacted. It is safe to log at any level. It does not include user-supplied payload bytes.

### 4.2 `microtel::ConfigError` — init-time error description

Used only in the `microtel::Expected<Provider, ConfigError>` returned from `SdkBuilder::Build()` and similar init paths. Defined in `include/microtel/error.hpp`.

```
class ConfigError
{
public:
    enum class Kind : std::uint8_t
    {
        Unspecified                = 0,
        InvalidValue               = 1,    // a setting parsed but failed validation
        UnknownKey                 = 2,    // strict-mode unknown TOML key
        EnvParseFailure            = 3,    // OTEL_* / MICROTEL_* env var malformed
        FileNotFound               = 4,
        FileParseFailure           = 5,    // TOML syntax error
        TlsMaterialUnreadable      = 6,    // CA bundle / client cert / key not openable or invalid
        EndpointMalformed          = 7,
        ProtocolMismatch           = 8,    // explicit protocol disagrees with URL scheme
        InsecureDisallowed         = 9,    // MICROTEL_FORBID_INSECURE_TLS=ON and insecure=true
        BuildAlreadyConsumed       = 10,   // SdkBuilder::Build() called twice
    };

    Kind        kind = Kind::Unspecified;
    std::string field;              // dotted path to the offending setting; empty if not field-bound
    std::string message;            // human-readable, safe to log
};
```

`field` carries a dotted path like `"exporter.endpoint"` so applications can surface a precise diagnostic to the operator.

### 4.3 `microtel::Status` — lifecycle status

The four-value enum from §2.3.

---

## 5. Cross-thread rules

**No exception ever crosses a thread boundary** (LOCKED).

Three concrete consequences:

1. **The exporter worker never throws.** Internal failures on the worker (encoder bug, allocation failure, parse error) are caught at the worker's top-level loop, recorded as a diagnostic, and the worker continues. The caller of `ForceFlush` / `Shutdown` observes a `Status` of `Failed`; detail is in `GetExporterHealth()`.
2. **The I/O thread never throws.** Same pattern. Internal failures complete the in-flight request with `Error::Kind::InternalFailure` and a short message; the worker observes a failed `WireResult` and proceeds.
3. **Caller-thread methods are `noexcept`.** Anything that would unwind is caught at the API boundary; the path drops-and-counts.

**No `std::exception_ptr` is moved between threads.** Errors are recorded as diagnostics on the producing thread; the consuming thread observes a *result* (a counter increment, a failed `WireResult`, a `Status`) and acts on it.

---

## 6. Partial success (LOCKED — never retried)

When the wire codec parses an OTLP response that includes `partial_success` with a non-zero rejected count:

1. The codec records `partial_success_rejection` with the rejected count on `IDiagnosticsSink`.
2. The codec returns a `WireResult` with `success=true` and `partial_success_rejected=N`.
3. **The exporter does not retry the request.** Retrying would re-send the items the receiver already accepted.

This is the most counterintuitive rule in the error model, which is exactly why it has its own sequence diagram (`docs/sequences/partial-success.md`) and its own counter.

The rejected-items error message from the response is captured (capped at `max_response_bytes`) and surfaced via `GetExporterHealth()` so operators can investigate. Partial-success responses are still logged, at `warn`, with rate limiting.

---

## 7. Retry classification matrix

The wire codec — not the exporter — owns retry classification (per ICP 0001 and `interfaces.md`). The exporter respects the `WireResult::retryable` flag without reinterpretation.

The trace, metric and log exporters share one retry engine (`src/exporter/retry_engine.hpp`), so this matrix, the backoff and retry budget, and the §3 final-outcome counters apply to every signal alike. Until v1.1.1 the metric and log exporters made one attempt per batch and never retried (issue #222). `Shutdown` wakes a retry that is sleeping in its backoff and ends that batch's retries; the batch counts as `retry_budget_exhausted`.

### 7.1 OTLP/HTTP

| Response | `success` | `retryable` | `retry_after` | Counter |
|---|---|---|---|---|
| 2xx, no body | true | n/a | n/a | (success) |
| 2xx, partial-success body, rejected = 0 | true | n/a | n/a | (success) |
| 2xx, partial-success body, rejected > 0 | true | **false** (never retried) | n/a | `partial_success_rejection` |
| 429 | false | true | from `Retry-After` if present, else jittered backoff | (counted on retry outcome) |
| 502, 503, 504 | false | true | from `Retry-After` if present, else jittered backoff | (counted on retry outcome) |
| 404 | false | false | n/a | `non_retryable_failure` |
| 415 | false | false | n/a | `non_retryable_failure` |
| Other 4xx | false | false | n/a | `non_retryable_failure` |
| Other 5xx (not in retryable list) | false | false | n/a | `non_retryable_failure` |
| Connection failure / TLS failure / read timeout | false | true (limited attempts) | jittered backoff | `connect_failure` if pre-request |
| Response > `max_response_bytes`, or trailers > `max_trailer_bytes` | false | false | n/a | `response_too_large` |
| Decompressed body > `max_decompressed_bytes` | false | false | n/a | `decompression_too_large` |
| Body unparseable as protobuf | false | false | n/a | `malformed_response` |

The last row applies to a 2xx body: it is parsed as the signal's Export response, and a body that fails to parse — truncated, trailing bytes after a valid `partial_success`, or not protobuf at all — is terminal rather than a clean success. Unknown fields are skipped, so a body with no `partial_success` is still a success. Until v1.2 such a body was classified as a clean success (issue #223).

### 7.2 OTLP/gRPC

Every row below is keyed on `grpc-status`, which presupposes that a response
arrived. The first row covers the case where none did — it is evaluated before
any `grpc-status` is inspected, and its omission is why the transport-failure
gap in this table went unnoticed (`ClassifyResponse` returned `retryable=false`
here while OTLP/HTTP returned `true` for the identical failure).

| `grpc-status` | `success` | `retryable` | `retry_after` | Counter |
|---|---|---|---|---|
| *(no response — connection failure / TLS failure / read timeout)* | false | true (limited attempts) | jittered backoff | `connect_failure` if pre-request |
| *(no usable response — body > `max_response_bytes`, or trailers > `max_trailer_bytes`)* | false | **false** | n/a | `response_too_large` |
| `OK (0)` | true | n/a | n/a | (success) |
| `OK` with partial-success rejected > 0 | true | **false** (never retried) | n/a | `partial_success_rejection` |
| `CANCELLED (1)` | false | true | jittered backoff | |
| `UNKNOWN (2)` | false | false | n/a | `non_retryable_failure` |
| `INVALID_ARGUMENT (3)` | false | false | n/a | `non_retryable_failure` |
| `DEADLINE_EXCEEDED (4)` | false | true | jittered backoff | |
| `NOT_FOUND (5)` | false | false | n/a | `non_retryable_failure` |
| `ALREADY_EXISTS (6)` | false | false | n/a | `non_retryable_failure` |
| `PERMISSION_DENIED (7)` | false | false | n/a | `non_retryable_failure` |
| `RESOURCE_EXHAUSTED (8)`, with `RetryInfo` in details | false | true | from `RetryInfo.retry_delay` | |
| `RESOURCE_EXHAUSTED (8)`, **without `RetryInfo`** | false | **false** | n/a | `non_retryable_failure` |
| `FAILED_PRECONDITION (9)` | false | false | n/a | `non_retryable_failure` |
| `ABORTED (10)` | false | true | jittered backoff | |
| `OUT_OF_RANGE (11)` | false | true | jittered backoff | |
| `UNIMPLEMENTED (12)` | false | false | n/a | `non_retryable_failure` |
| `INTERNAL (13)` | false | false | n/a | `non_retryable_failure` |
| `UNAVAILABLE (14)` | false | true | jittered backoff | |
| `DATA_LOSS (15)` | false | true | jittered backoff | |
| `UNAUTHENTICATED (16)` | false | false | n/a | `non_retryable_failure` |
| Trailer-only response without `grpc-status`, HTTP `:status` 429/502/503/504 | false | true | jittered backoff | |
| Trailer-only response without `grpc-status`, other HTTP `:status` | false | false | n/a | `malformed_response` |
| Multi-frame parse failure / truncated message | false | false | n/a | `malformed_response` |
| Decoded body > `max_decompressed_bytes` | false | false | n/a | `decompression_too_large` |

`UNKNOWN (2)` and `ALREADY_EXISTS (6)` were absent from this table until issue #171; they are listed now because the codec's status table covers the whole `0..16` range and the matrix is what that table is checked against. Both were already non-retryable in the shipped code — they fell off the end of its retryable list — so the rows record existing behaviour rather than change it. A `grpc-status` outside `0..16` has no row and is non-retryable, reported as `UNRECOGNIZED (<code>)`.

The `RESOURCE_EXHAUSTED` row is the most important non-obvious entry — its decode path is in `grpc-wire-protocol.md` §2.4, and the tests that cover it are listed in `grpc-wire-protocol.md` §7.

### 7.3 Application `ExportTransport` (ICP 0036)

A Provider built with `SdkBuilder::WithExportTransport` classifies nothing itself: the application's `ExportTransport::Send` answers with a `SendOutcome`, and `ExportTransportCodec` (`src/wire/custom/`) maps it. Failures carry `Error{Kind::Network, message}`; the transport's `message` becomes `last_error_message`.

| Case | `success` | `retryable` | `retry_after` | Counter |
|---|---|---|---|---|
| `Success`, `rejected` = 0 | true | n/a | n/a | (success) |
| `Success`, `rejected` > 0 | true | **false** (never retried) | n/a | `partial_success_rejection` |
| `Retryable` | false | true | the transport's `retry_after` if set, else jittered backoff | (counted on retry outcome) |
| `NonRetryable` | false | false | n/a | `non_retryable_failure` |
| `Send` throws (any type) | false | false | n/a | `non_retryable_failure`, error kind `InternalFailure` |
| Encoding came back empty (arena allocation failed) — `Send` not called | false | true | jittered backoff | (counted on retry outcome) |
| After `Cancel`, or past the shutdown deadline — `Send` not called | false | false | n/a | `non_retryable_failure` |

`connect_failure`, `response_too_large`, `malformed_response`, `decompression_too_large` and `transport_busy` never fire on this path. `batches_sent`, `batches_failed`, `retryable_failure_recovered` and `retry_budget_exhausted` mean what they mean for HTTP.

---

## 8. Init-failure taxonomy

`SdkBuilder::Build()` returns `microtel::Expected<std::shared_ptr<Provider>, ConfigError>` and validates eagerly. Every documented init failure maps to a `ConfigError::Kind`:

| Failure | `ConfigError::Kind` | `field` example |
|---|---|---|
| Unknown key in `microtel.toml`, strict mode | `UnknownKey` | `exporter.unknownsetting` |
| TOML syntax error | `FileParseFailure` | (empty) |
| Endpoint URL malformed | `EndpointMalformed` | `exporter.endpoint` |
| Endpoint scheme conflicts with explicit `protocol` | `ProtocolMismatch` | `exporter.protocol` |
| TLS CA bundle missing or unreadable | `TlsMaterialUnreadable` | `tls.ca_bundle` |
| Client cert / key path unreadable | `TlsMaterialUnreadable` | `tls.client_cert` |
| `insecure=true` while compiled with `MICROTEL_FORBID_INSECURE_TLS=ON` | `InsecureDisallowed` | `tls.insecure` |
| Incoherent batch options (zero queue size, batch larger than the queue, zero schedule delay) | `InvalidValue` | `sdk.max_export_batch_size` |
| A static request header that HTTP/2 forbids or that microtel sets for the resolved protocol, a static `authorization` header with `WithAuthProvider` ([ICP 0038](icps/0038-reject-reserved-request-headers.md)), or a static header value with CR, LF, NUL or edge whitespace (#412) | `InvalidValue` | `exporter.headers.host` |
| An environment variable that does not parse (e.g. `OTEL_EXPORTER_OTLP_TIMEOUT` not integer milliseconds) | `EnvParseFailure` | `OTEL_EXPORTER_OTLP_TIMEOUT` |
| Second call to `SdkBuilder::Build()` | `BuildAlreadyConsumed` | (empty) |

The exact `message` text each of these produces, and what to do about it, is collected in [`troubleshooting.md`](troubleshooting.md#sdkbuilderbuild-returns-an-error); this table stays the normative mapping.

An endpoint is validated after the sources are merged, so a malformed `OTEL_EXPORTER_OTLP_ENDPOINT` is reported like any other malformed endpoint: `EndpointMalformed` on field `exporter.endpoint`. Timeout values (`[timeouts]`, `OTEL_EXPORTER_OTLP_TIMEOUT`) are parsed but not range-checked in this release.

**Network preflight is not part of `Build()`.** `Build()` does not open sockets. Network reachability is validated by the `microtel-preflight` tool, `--preflight=connect` / `--preflight=export` ([`troubleshooting.md`](troubleshooting.md#microtel-preflight-exit-codes)) — never as a side effect of constructing a `Provider`.

---

## 9. Diagnostic surface

Three layers of visibility, in increasing detail.

### 9.1 `Provider::GetExporterHealth()`

Returns a structured snapshot. The shape is locked in `interfaces.md` against the `IDiagnosticsSink` interface; for v1 the snapshot includes:

- All counters in §3 by name, as `uint64_t`.
- Total batches sent, total batches failed.
- Current queue depth.
- Last-error timestamp and last-error short message (capped).
- Connection state (one of `Disconnected`, `Connecting`, `Connected`, `Reconnecting`, `Closed`).

**With an application `ExportTransport` (ICP 0036)** there is no connection to report, and the state means **"sends are succeeding"**, not "the peer is reachable": `Disconnected` until the first `Success`, `Connected` after one, `Reconnecting` after a failure that follows a success (and `Connected` again after the next success), `Closed` after `Shutdown`. `Connecting` never occurs. On a fire-and-forget link such as UDP, `Connected` says only that requests are leaving this host.

The snapshot is consistent at a moment in time but not transactionally consistent across counters — it is a read of `std::atomic<uint64_t>` values and a borrowed view into the last-error slot.

**One sink serves all three signals.** `SdkBuilder` hands the same `IDiagnosticsSink` to the trace, metric and log pipelines, so every counter here — `batches_sent` and `batches_failed` included — is a **cross-signal aggregate**. A failed metric export and a failed trace export both increment `batches_failed`; the snapshot does not say which signal lost a batch. Per-signal breakdown would need one counter set per signal, which is an ICP against `HealthSnapshot`. `last_error_message` is likewise last-writer-wins across signals.

For what to do when a counter moves or `last_error_message` is set, see [`troubleshooting.md`](troubleshooting.md#no-traces-arrive-reading-getexporterhealth).

### 9.2 Internal diagnostic log

Routed through the injected `LogSink` (§9.3) when one is installed, and to a minimal stderr fallback when none is — in every build configuration. **microtel itself never calls spdlog**: that route was considered, priced in issue #190, and declined in [ICP 0026](icps/0026-provider-setters.md) §6, because an undefined spdlog reference in `libmicrotel_common.a` would make the installed package unlinkable for every consumer. An application that wants spdlog installs `microtel_spdlog_bridge` as its sink, inside its own build. Levels:

- `error` — non-retryable failures, init failures, internal-failure recovery, `connect_failure` after reconnect-budget elapsed.
- `warn` — retryable failures, `partial_success_rejection`, `force_flush_timeout`, `shutdown_timeout`.
- `info` — `Build()` resolved-config dump (with secrets redacted per `configuration.md` §5), connect / disconnect transitions.
- `debug` — per-batch send / receive summary, per-stream lifecycle.
- `trace` — per-frame nghttp2 events (rare; primarily for development).

**Minimum level.** Emissions below the configured minimum are dropped before the sink is consulted — one relaxed atomic load, no lock. The default is `info`; it is seeded at `Build()` from `logging.level` / `MICROTEL_LOG_LEVEL` and retuned at runtime by `Provider::SetLogLevel` (ICP 0026 §6, `docs/configuration.md` §3.11). The filter is **process-wide**, not per-provider, because the emission point is a free function with no provider in scope: providers built from different profiles share it and the last writer wins.

**Rate limiting (LOCKED).** Diagnostic emissions for repeating events are rate-limited with a token-bucket limiter per `(level, reason)` pair. Defaults: 1 burst of 10, 1/sec sustained. The first occurrence of a new reason is always emitted; subsequent ones are suppressed but counted separately so operators can see "10 emitted, 4231 suppressed" in `GetExporterHealth()`.

### 9.3 Sink injection

Applications may redirect microtel's internal logs into their own logging system via `microtel::SetLogSink`:

```
microtel::SetLogSink([](microtel::LogLevel lvl, std::string_view msg) {
    my_app_logger.Log(static_cast<int>(lvl), msg);
});
```

Sink injection is available in both `MICROTEL_USE_SPDLOG=ON` and `=OFF` builds — and, per §9.2, it is now the only route microtel offers. The option gates the `microtel_spdlog_bridge` adapter, not anything inside `microtel_common`.

### 9.4 Never-recursive-export rule (LOCKED)

**microtel's internal diagnostic logs are never routed back through microtel's own OTLP exporter** in v1. Recursive export creates a failure loop where a broken exporter generates more telemetry it cannot ship, amplifying the original problem.

If an application wants its OTel-Logs pipeline to receive microtel's internal logs, it does so explicitly via `SetLogSink` pointing at a separate logger. v1 does not provide a built-in bridge.

---

## 10. What this document does not cover

- The exact wire-level byte sequences that produce each row in §7 — see `grpc-wire-protocol.md` for the gRPC side and the conformance tests for both.
- The retry timing algorithm (jitter formula, backoff multiplier) — pinned in M5; covered in `docs/sequences/retry-after-failure.md` once written.
- The format of the stderr fallback line — implementation detail of `src/common/log_sink.cpp`, and the only formatting microtel does.
- Per-method error annotations — see `interfaces.md`.
