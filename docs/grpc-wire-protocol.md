# gRPC Wire Protocol on nghttp2 — Implementation Notes

**Status:** M0 deliverable. Implementation notes for the OTLP/gRPC codec under `src/wire/grpc/`. Companion to `interfaces.md` (the `IWireCodec` contract) and `error-model.md` §7.2 (the gRPC retry classification matrix).
**Source of truth for rationale:** this document; the retry matrix is `error-model.md` §7.2.
**Audience:** the contributor or AI agent picking up Track C (OTLP/gRPC wire) at M3 onward.

This document is the specification of microtel's OTLP/gRPC wire behaviour. It pins the exact codec state machine, byte-level edge cases, the `RetryInfo` decode path, and the malformed-server failure modes that production gRPC traffic actually exhibits.

---

## 1. Goals and non-goals

### 1.1 Goals

Implement the **gRPC wire protocol** sufficient for OTLP unary RPCs, on top of nghttp2 — without linking the gRPC library or any of its transitive dependencies.

The supported endpoint is exactly:

```
POST /opentelemetry.proto.collector.trace.v1.TraceService/Export
```

Plus, in v1.2+, the metrics and logs analogues (same shape).

### 1.2 Explicit non-goals

Not implemented, and not needed for OTLP unary:

- **Server-streaming, client-streaming, bidi-streaming RPCs.** OTLP is unary-only. The codec rejects any response that delivers more than one DATA-frame message before END_STREAM as malformed.
- **Service config / retry policy JSON.** Retry policy is microtel's, not negotiated.
- **Name resolution / xDS / load balancing.** One endpoint per `ITransport`.
- **Health checking service. Server reflection. Channel-state machinery beyond "open or reconnecting."**
- **Interceptors / middleware.** Auth is handled by `IAuthProvider`; nothing else interposes.
- **Compression negotiation beyond `gzip`.** No `deflate`, no `snappy` in v1.

These are not arrangements; they are constraints on the codec implementation. Any code that assumes one of these features should fail review.

---

## 2. Wire shape

### 2.1 Request HEADERS frame

Sent on stream open. Required pseudo-headers and headers:

| Header | Value |
|---|---|
| `:method` | `POST` |
| `:scheme` | `https` (or `http` if `insecure=true`) |
| `:authority` | endpoint authority (host:port) |
| `:path` | `/opentelemetry.proto.collector.trace.v1.TraceService/Export` |
| `te` | `trailers` |
| `content-type` | `application/grpc+proto` |
| `user-agent` | `microtel-cpp/<version>` (or `microtel-python/<version>`) |
| `grpc-encoding` | `gzip` if request compression on; absent otherwise |
| `grpc-accept-encoding` | `gzip`, always — the codec decodes `CF = 0x01` responses (§5.2) |
| `grpc-timeout` | absent in v1 — microtel manages its own timeouts and uses `RST_STREAM` on local timeout (see §2.5) |

Plus any user-configured static headers (`exporter.headers` in `microtel.toml`, `WithHeaders`, `OTEL_EXPORTER_OTLP_HEADERS`) and the `authorization` header from `IAuthProvider`.

**Header rules.** Header values are HPACK-encoded by nghttp2; microtel constructs them as `std::string_view` plus copy where nghttp2 needs an owning view. The codec appends the static headers unchanged, and nghttp2 sends them unchanged apart from lowercasing the names. The names are checked once, at `SdkBuilder::Build()` ([ICP 0038](icps/0038-reject-reserved-request-headers.md)). It rejects pseudo-headers and other non-token names, the connection-specific headers HTTP/2 forbids (`connection`, `keep-alive`, `proxy-connection`, `transfer-encoding`, `upgrade`, `te`), `host`, the names in the table above that microtel sets itself (`content-type`, `user-agent`, `grpc-encoding`, `grpc-accept-encoding`), and `content-length`, which microtel does not send and which a fixed value would get wrong for every batch but one. It also rejects a static `authorization` when `IAuthProvider` supplies one, and a static value that RFC 9113 §8.2.1 makes malformed (CR, LF or NUL anywhere, a space or tab at either end). The auth callback's value gets the same value check on every export; a bad one fails that batch before anything is sent (#412). Oversized headers are caught by HTTP/2's own limits (`SETTINGS_MAX_HEADER_LIST_SIZE`).

`:authority` is always built from the configured endpoint. A user-supplied `host` header fails `Build()`.

### 2.2 Request DATA frames — gRPC framing

Per gRPC spec, each gRPC message is prefixed with a 5-byte header:

```
+--------+--------+--------+--------+--------+
|  CF    |       message length (BE)         |
+--------+--------+--------+--------+--------+
| message bytes (length bytes)               |
+--------------------------------------------+
```

- `CF` — compression flag. `0x00` means uncompressed; `0x01` means compressed per `grpc-encoding`. Other values are illegal in v1 (`0x02..0xFF` reserved per gRPC spec).
- `length` — big-endian uint32, **excludes** the 5-byte prefix.

**Unary OTLP requests carry exactly one message.** The codec writes the 5-byte prefix followed by the encoded protobuf bytes from `EncodedPayload` (`memory-model.md` §3.2).

Compression: if `grpc-encoding: gzip` is set, the protobuf bytes are gzip-compressed *before* the 5-byte prefix is computed. The compression flag is `0x01` and the length is the compressed length. (LOCKED)

DATA frames are sent with `END_STREAM=1` on the final frame so the server sees a complete message followed by an empty trailers section after the response.

### 2.3 Response DATA frames — parsing

The codec receives an arbitrary stream of DATA bytes from nghttp2. **The parser must not assume a gRPC message corresponds to a single HTTP/2 DATA frame.** (LOCKED — cites `src/wire/grpc/grpc_wire_codec.cpp:DecodeResponseFrame`)

Three concrete cases the parser handles:

1. **Whole message in one frame.** Common case. The 5-byte prefix and the message bytes arrive together.
2. **Message split across frames.** The prefix may arrive in one frame, the body in the next; the body itself may be split. Parser maintains a small ring of unconsumed bytes between callbacks.
3. **Multiple messages in one frame.** The codec rejects this for OTLP unary as malformed (§3.5 below). The parser still tracks length-prefixed boundaries to detect it cleanly.

The parser is a small state machine over four states:

```
WAIT_PREFIX -> READ_PREFIX(0..5) -> READ_BODY(0..length) -> COMPLETE
                       ^                                       |
                       |-----[next message — REJECT]------------
```

Bytes beyond `COMPLETE` cause `malformed_response`. Receiving `END_STREAM` in `WAIT_PREFIX` (i.e., DATA-empty) leads to either trailer-only handling (§2.4) if the response was indeed empty, or `malformed_response` otherwise.

### 2.4 Trailer HEADERS frame — status

After the (sole) DATA frame's `END_STREAM=1` signal — or as a trailer-only response — the server sends a HEADERS frame with the gRPC status:

| Trailer | Meaning |
|---|---|
| `grpc-status` | decimal status code, `"0"` = OK |
| `grpc-message` | optional URL-encoded human-readable message |
| `grpc-status-details-bin` | optional base64-encoded `google.rpc.Status` protobuf (used for `RetryInfo`) |

The codec inspects `grpc-status` first. If `"0"`, the response is success — proceed to parse the OTLP message body for partial-success. If non-zero, the response is failure — classify per `error-model.md` §7.2.

#### `grpc-status-details-bin` decoding

Specifically for `RESOURCE_EXHAUSTED (8)`, the codec attempts to decode `grpc-status-details-bin`:

1. Base64-decode the value (URL-safe, no padding required per gRPC spec).
2. Parse as `google.rpc.Status` via upb.
3. Iterate `details[]`. Each entry is `google.protobuf.Any`; the codec inspects `type_url` and decodes `RetryInfo` (`type_url == "type.googleapis.com/google.rpc.RetryInfo"`) explicitly.
4. If `RetryInfo` is present and has `retry_delay`, the codec returns `retryable=true, retry_after=<delay>`.
5. If `RetryInfo` is absent, the codec returns `retryable=false`. (LOCKED — cites `src/wire/grpc/grpc_wire_codec.cpp:ResourceExhaustedWithoutRetryInfo`)
6. Other `Any` types are not interpreted in v1; their presence is logged at `debug` for diagnostics.

`google.rpc.Status` and `RetryInfo` are vendored under `proto/` and generated under `gen/` alongside the OTLP protos. Updating the vendored `googleapis` set is an ICP.

### 2.5 Trailer-only responses

A **trailer-only** response is one where the server sends final status in the *initial* HEADERS frame, with `END_STREAM=1` and no DATA frames. Common in proxy-heavy environments where intermediaries terminate the stream early.

The codec recognises trailer-only by:

1. The first HEADERS frame has `END_STREAM=1`.
2. The frame contains `grpc-status` (and optionally `grpc-message`, `grpc-status-details-bin`).

When detected, the codec processes the headers as a trailer (§2.4) and skips the DATA-parsing path entirely.

If the first HEADERS frame has `END_STREAM=1` but **lacks** `grpc-status`, see §3.6 below — the codec falls back to HTTP-status-based classification.

### 2.6 Cancellation — `RST_STREAM`

The codec sends `RST_STREAM` with `error_code = CANCEL (0x8)` on:

- **Per-export timeout elapsed.** The exporter's per-export deadline fires before the stream completes.
- **Shutdown observed.** The exporter is in `Draining` and the stream cannot complete within the remaining budget.

`RST_STREAM` is the I/O thread's responsibility — the exporter signals a per-request cancel; the I/O thread submits the frame to nghttp2.

The cancelled stream's request record is completed with `Error::Kind::Cancelled` and the appropriate counter (`shutdown_timeout` or none if the request was simply timing-out under a retry budget that's also expiring).

---

## 3. Codec state machine

States and transitions for one in-flight request:

```
                 +---------------+
                 |   IDLE        |
                 +-------+-------+
                         | Send(payload)
                         v
                 +---------------+
                 |   SUBMITTED   |  (request enqueued to ITransport)
                 +-------+-------+
                         | I/O thread opens stream
                         v
                 +---------------+
                 |   STREAMING   |  (HEADERS sent; DATA in flight)
                 +-------+-------+
                         | END_STREAM ack on send
                         v
                 +---------------+
       +---------+   AWAIT_RESP  +-----+
       |         +-------+-------+     | trailer-only HEADERS
       |                 |             |
       | DATA arrives    | first       v
       |                 | HEADERS    +---------------+
       v                 v            |  STATUS_ONLY  |
+---------------+ +---------------+   +-------+-------+
| READ_BODY     | | READ_INITIAL  |           |
+-------+-------+ +-------+-------+           |
        | END_STREAM      |                   |
        v                 v                   v
+---------------+ +---------------+    +---------------+
|  AWAIT_TRAIL  | |  READ_BODY    |    |   COMPLETE    |
+-------+-------+ +---------------+    +-------+-------+
        | trailer HEADERS                       |
        v                                       |
+---------------+                               |
|  COMPLETE     |<-------------------------------+
+---------------+
```

**Timeouts** can fire in any state and transition to `CANCELLED` → `COMPLETE` with an error. `RST_STREAM` from the peer transitions any state to `COMPLETE` with `Error::Kind::Network` or `::Cancelled` depending on the peer error code.

**Memory cleanup.** On `COMPLETE`, the response buffer is reset, the in-flight request record is freed, the per-stream parser state is destroyed.

**Implementation note (non-normative) — distinguishing initial-vs-trailing HEADERS in the nghttp2 callback model.** nghttp2 surfaces every received header through a single `on_header_callback`. To bucket headers correctly into the state machine above (initial HEADERS feed `READ_INITIAL` / `STATUS_ONLY`; trailing HEADERS feed `AWAIT_TRAIL`), the codec inspects `frame->headers.cat`:

| `frame->headers.cat` | Meaning on a client stream |
|---|---|
| `NGHTTP2_HCAT_RESPONSE` | First HEADERS frame on this stream — the initial response headers. If `END_STREAM=1` and `grpc-status` is present, this is a trailer-only response. |
| `NGHTTP2_HCAT_HEADERS`  | Any subsequent HEADERS frame — for OTLP unary, these are trailers. |

The trailer-only recognition rule from §2.5 (END_STREAM on the first HEADERS combined with `grpc-status` present) is unchanged; `headers.cat` is the nghttp2-specific way to spot "this is the first HEADERS frame on a client stream." Validated against the OpenTelemetry Collector during the M1 spike.

---

## 4. Status interpretation

Pinned in `error-model.md` §7.2 (the matrix). This document does not duplicate the table; the rules below cover the specifics that affect implementation.

### 4.1 Status priority

When both an HTTP `:status` and `grpc-status` are present, the codec uses `grpc-status` for retry classification, as the gRPC HTTP/2 protocol requires. (LOCKED — cites `src/wire/grpc/grpc_wire_codec.cpp:ClassifyResponse`) The HTTP `:status` is captured for diagnostics only.

### 4.2 Missing `grpc-status` (malformed servers / proxies)

When the response — including a trailer-only — lacks `grpc-status`, the codec falls back to the HTTP `:status`:

- `429`, `502`, `503`, `504` → retryable with jittered backoff.
- Anything else → `malformed_response` and the batch is dropped non-retryably.

This case is common in proxy-heavy environments. The codec emits a `warn`-level diagnostic on the first occurrence per `(connection)` so operators can see "your proxy is terminating gRPC streams without trailers."

### 4.3 Conflicting / impossible combinations

| Observed | Treatment |
|---|---|
| HTTP `:status` 200, `grpc-status` non-zero | Failure per `grpc-status` (LOCKED). |
| HTTP `:status` 4xx/5xx, `grpc-status` 0 | Treat as success per `grpc-status`. Diagnostic emitted because this configuration is unusual. |
| Multiple DATA messages before `END_STREAM` | `malformed_response`. |
| `END_STREAM=1` with no trailer HEADERS frame and no initial-with-status | `malformed_response`. |
| `grpc-status` in DATA payload (not in trailers) | `malformed_response`. |

### 4.4 `grpc-message` handling

`grpc-message` is URL-encoded UTF-8. The codec percent-decodes (no DOS protection needed — it's bounded by `max_trailer_bytes`) and stores the decoded message in `WireResult::error.message`, capped per the `BoundedString` rule in `interfaces.md` §3.

**Format.** `WireResult::error.message` is `"<NAME> (<code>): <decoded grpc-message>"`, e.g. `UNAUTHENTICATED (16): provided authorization does not match expected scheme or token`. With no `grpc-message` the suffix is omitted entirely — `UNAUTHENTICATED (16)` — rather than leaving a dangling separator. A `grpc-status` outside `0..16` renders as `UNRECOGNIZED (<code>)` and is non-retryable: the codec will not classify what it does not recognise. The name and the number are both always present; the name is what an operator reads, the number is what they grep for.

Percent-decoding accepts either hex case. A malformed escape — anything that is not `%` followed by two hex digits, including a `%` at the end of the value — passes through verbatim rather than failing the decode, because the field is human-readable text that may legitimately contain a bare `%`.

The decoded message is also what fills `WireResult::response_excerpt` on the gRPC path, mirroring the body excerpt the OTLP/HTTP codec builds.

The status names and their retryability live in one table, `src/wire/grpc/grpc_status.cpp`, which is the codec's only source of both; `error-model.md` §7.2 is the matrix it is checked against.

### 4.5 `RetryInfo` precedence

If both `grpc-message` and `grpc-status-details-bin` carry information about retry timing, `RetryInfo.retry_delay` wins over any inline `grpc-message` content. The latter is human-readable; the former is machine-readable; the codec acts on the machine-readable signal.

---

## 5. Compression

### 5.1 Request compression

Configured by `[exporter] compression = "gzip"`. Default off, for a low-CPU profile (`configuration.md` §3.3).

When on:

1. The codec wraps `EncodedPayload.bytes()` in a gzip stream.
2. The 5-byte prefix is computed from the **compressed** length, with `CF = 0x01`.
3. `grpc-encoding: gzip` is set in the request HEADERS.

The compression buffer is bounded by `max_record_bytes` upstream (the input is itself bounded), so the compressed output is bounded by a small overhead factor. Decompression-bomb protection on the response side is `max_decompressed_bytes`.

### 5.2 Response compression

Advertised via `grpc-accept-encoding: gzip`. If the server returns `grpc-encoding: gzip`, each DATA-frame message carries `CF = 0x01` and the codec decompresses before parsing. Decompression is bounded by `max_decompressed_bytes`; on overflow, the codec returns `decompression_too_large` per the matrix.

Mixed messages (some compressed, some not) are permitted by gRPC but unused for OTLP unary; the codec handles the per-message flag correctly anyway.

**Implementation status: implemented.** `GrpcWireCodec` sends
`grpc-accept-encoding: gzip` unconditionally and validates the response frame
before parsing it — `CF = 0x01` is inflated under `max_decompressed_bytes`,
`CF > 0x01` and any declared length that disagrees with the bytes present are
`malformed_response`, and an overflow is `decompression_too_large`.

The advertisement and the decoding landed together, as issue #161 required —
but the pairing turned out not to be what made the gap harmless. Measured
against the pinned collector (`otel/opentelemetry-collector-contrib:0.160.0`),
a response comes back `grpc-encoding: gzip` with `CF = 0x01` whenever the
*request* carried `grpc-encoding: gzip`, whether or not `grpc-accept-encoding`
was ever sent — grpc-go picks the response compressor from the request's
encoding. So any deployment running OTLP/gRPC with `compression = "gzip"` was
already receiving compressed responses and discarding their partial-success
bodies. Fixed by the PR closing #161.

### 5.3 Distinct from HTTP `Content-Encoding`

`grpc-encoding` is per-message and lives at the gRPC layer. HTTP `Content-Encoding` is per-stream and would be applied by HTTP/2 transport layer. **microtel never sets HTTP `Content-Encoding` for gRPC requests** (LOCKED — gRPC compression is gRPC-layer).

---

## 6. HTTP/2 settings interactions

### 6.1 Settings exchanged at connect

microtel sends:

| Setting | Value |
|---|---|
| `SETTINGS_MAX_CONCURRENT_STREAMS` | 100 (default; configurable) |
| `SETTINGS_INITIAL_WINDOW_SIZE` | 1 MiB (default; configurable) |
| `SETTINGS_HEADER_TABLE_SIZE` | nghttp2 default |
| `SETTINGS_ENABLE_PUSH` | 0 (we are a client; push not supported) |
| `SETTINGS_MAX_FRAME_SIZE` | nghttp2 default |
| `SETTINGS_MAX_HEADER_LIST_SIZE` | 64 KiB (matches `max_trailer_bytes`) |

Server settings are honoured directly by nghttp2; the codec does not need to enforce them.

### 6.2 GOAWAY mid-batch

When the server sends `GOAWAY`:

1. Streams with IDs ≤ `last-stream-id` (the GOAWAY-permitted last) continue to completion.
2. Streams with IDs > `last-stream-id` are cancelled by nghttp2 with `REFUSED_STREAM`.
3. The codec receives a stream-level error for cancelled streams; the request record is completed with `retryable=true` and a small jittered backoff.
4. The transport observes the GOAWAY and triggers reconnect (driven by the reactor; new socket, new session).

The full sequence is `docs/sequences/goaway-handling.md`.

### 6.3 Flow control

For OTLP unary, request payloads fit comfortably under typical `INITIAL_WINDOW_SIZE`. If a payload exceeds the per-stream window, nghttp2 fragments DATA frames as needed and waits for `WINDOW_UPDATE`. The codec is unaware of this; it submits a single DATA frame and lets nghttp2 chunk.

The codec's per-export timeout covers the case where flow control stalls the request indefinitely.

---

## 7. Test surfaces

The codec is tested at three levels.

### 7.1 Unit tests — `tests/unit/wire/grpc/`

Each parser state, each status-classification entry in §4, each compression branch. Mocks `IAuthProvider`, `ITransport`. Uses `FakeTransport` to script byte-level responses.

### 7.2 Wire tests — `tests/grpc-wire/`

Byte-level validation against captured responses from real grpc-server and the OpenTelemetry Collector. Each test fixture is a single recorded HTTP/2 byte stream; the codec is run against it; the test asserts the resulting `WireResult` matches expected.

Required corpus entries (M9 hardening milestone):

- Trailer-only response with `grpc-status: 0`.
- Trailer-only response with `grpc-status: 14` (UNAVAILABLE).
- Trailer-only response without `grpc-status` (malformed).
- Multi-DATA-frame response with a single message split mid-prefix.
- Multi-DATA-frame response with a single message split mid-body.
- Response with `RESOURCE_EXHAUSTED` and inline `RetryInfo` in `grpc-status-details-bin`.
- Response with `RESOURCE_EXHAUSTED` and no `RetryInfo`.
- Response with `partial_success` in DATA and `grpc-status: 0` in trailers.
- Response with conflicting HTTP `:status: 503` and `grpc-status: 0`.
- GOAWAY mid-stream during DATA.
- RST_STREAM with `INTERNAL_ERROR (0x2)` from peer.

### 7.3 Conformance tests — `tests/conformance/`

End-to-end against a real OpenTelemetry Collector container. Validates the codec produces payloads accepted by upstream and parses their responses correctly. Run on every PR.

### 7.4 Fuzzing — `tests/fuzz/grpc_codec_fuzz.cpp`

libFuzzer harness over the response-parser entry point. Inputs: arbitrary byte sequences claiming to be HTTP/2 frames containing gRPC payloads. Goal: zero crashes, zero ASAN/UBSAN findings, bounded memory growth even on adversarial inputs.

Was a v1.0 release gate. Every PR builds the fuzz harnesses and replays any committed crash inputs, of which there are none yet (`corpus-check` in `ci.yml`); fuzzing runs weekly (`fuzz.yml`). See `ci-architecture.md`.

---

## 8. Cross-references to OTLP/HTTP

The HTTP and gRPC codecs share:

- `IOtlpEncoder` output (the encoded protobuf bytes). Identical.
- The `IWireCodec` interface contract (`interfaces.md` §4.3).
- The retry-budget and jittered-backoff logic, which lives in the exporter, not the codecs.
- `IAuthProvider`, `IDiagnosticsSink`, `ITransport`.

They differ in:

- Path, headers, framing.
- Retry classification (the matrices in `error-model.md` §7.1 vs §7.2).
- Compression layer (`Content-Encoding` for HTTP vs `grpc-encoding` for gRPC).
- Status source (HTTP `:status` for the HTTP codec, `grpc-status` trailer for the gRPC codec, with `:status` fallback when missing).

A single test plan exercises the shared contract; the protocol-specific tests cover the wire-shape divergence.

---

## 9. Known interop hazards

Real-world gRPC traffic encounters proxies, service meshes, and load balancers that don't always implement the gRPC trailer correctly. Five hazards we know about, with mitigation.

| Hazard | Cause | Mitigation |
|---|---|---|
| Trailer-only responses without `grpc-status` | nginx/envoy aborting upstream connection | §4.2 fallback to HTTP `:status`; `warn` diagnostic on first occurrence |
| `Connection: close` on HTTP/2 | non-compliant intermediary | nghttp2 surfaces as connection error; reconnect with backoff |
| HTTP/1.1 `Upgrade: h2c` proxy stripping `te: trailers` | corporate proxies | explicit failure with diagnostic; not retried |
| Transparent gzip middlebox compressing already-compressed gRPC | rare, broken middleboxes | decompression error → `malformed_response` |
| Keep-alive PING flood DoS-protection blocking microtel | mTLS + paranoid LB | use nghttp2's PING-rate limits; document tuning if needed |

These hazards inform the §7 test corpus. Each hazard has at least one fixture or test that demonstrates microtel handles it without crashing or hanging.

---

## 10. What this document does not cover

- The gRPC-side encoding of OTLP **request** payloads — that's the encoder's job (`interfaces.md` §4.2).
- HTTP/2 transport details (TLS, ALPN, reconnect) — that's the transport's job (`architecture.md` §3.6, `interfaces.md` §4.1).
- The decision to treat `RESOURCE_EXHAUSTED` without `RetryInfo` as non-retryable — recorded in `error-model.md` §7.2.
- Implementation details of the upb code generation (`gen/` directory contents) — that's the encoder's local concern.
