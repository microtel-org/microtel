# ICP 0042: HTTP/1.1 fallback for OTLP/HTTP

**Status:** Accepted — 2026-10-29. Milestone v1.2.6 (brought forward from
v1.3 on 2026-10-29); tracking issue #503. Amended 2026-10-30 (§3): the
auth-token claim corrected, and the stale-connection retry added.
**Affected interfaces / docs:**
- `docs/interfaces.md` §4.1 (`ITransport` Purpose) and §4.3 (`IWireCodec`
  Purpose, whose LOCKED "one interface" claim is unchanged): text below
- `docs/threading-model.md` §2.3, **Exactly one I/O thread per `Provider`**
  (LOCKED — cites `src/transport/http2_transport.hpp:m_io_thread`): the rule
  and its text stand, and only the citation changes (§4)
- `docs/sequences/connection-establishment.md`: the "ALPN preference is `h2`
  only" line and the "Protocol mismatch is the exception" paragraph. This is an
  M0 normative sequence doc, but neither line carries a LOCKED marker.
- `include/microtel/internal/transport.hpp`: the `ITransport` class Doxygen
- in the implementing PR: `docs/compatibility-matrix.md` §2 and §4,
  `docs/interop-matrix.md` §4, `docs/configuration.md` §3.3,
  `docs/troubleshooting.md`, `docs/architecture.md` §3.6, `docs/roadmap.md`
  §12 question 6, `README.md`, the examples' READMEs and
  `tests/conformance/README.md`. `git grep -n "HTTP/1.1"` finds every sentence
  that states the gap.
- code: new `src/transport/http1_transport.{hpp,cpp}`,
  `src/transport/http1_response_parser.{hpp,cpp}` and
  `src/transport/fallback_transport.{hpp,cpp}`; changes to
  `src/transport/http2_transport.{hpp,cpp}` (ALPN list, fallback signal),
  `src/sdk/sdk_builder.cpp` (`CreateTransport`, `WarnOnRiskyConfig`); new
  `src/wire/header_validation.{hpp,cpp}`, called from `BuildHeaders` in
  `src/wire/http/http_wire_codec.cpp` and `src/wire/grpc/grpc_wire_codec.cpp`
- tests: new `tests/fuzz/http1_response_fuzz.cpp` and
  `tests/integration/transport/http1_*_test.cpp`; header-validation tests in
  both codecs' unit tests and over each transport (§3); inverts
  `tests/conformance/http/plaintext_gap_test.cpp`

**Affected tracks:** D (Transport), plus the SDK builder's transport
construction, and the wire codecs' header assembly. There is no public API
change and no new configuration key. The encoder and the span hot path are not
touched.

## Summary

OTLP/HTTP falls back to HTTP/1.1 automatically when the endpoint cannot speak
HTTP/2. It sends serial requests on one keep-alive connection. OTLP/gRPC never
falls back.

## Motivation

microtel's transport is HTTP/2 only, so `http://` with OTLP/HTTP is h2c with
prior knowledge. Most plaintext OTLP/HTTP receivers serve HTTP/1.1 only. #166
closed this by decision: `Build()` warns, `Connect()` fails with a targeted
`Error::Kind::Protocol`, and `compatibility-matrix.md` §4 marks the
configuration unsupported. Roadmap §12 question 6 said to revisit it if
receivers turned up where neither alternative (`https://`, or OTLP/gRPC) is
available.

Those receivers exist. On 2026-10-29 these were measured locally with
`microtel-preflight --preflight=export`. All of them answer HTTP/1.1 to
microtel's preface:
- **The stock Collector `otlp` receiver on `:4318`** (contrib 0.160.0, the
  pinned image).
- **A plain Go `net/http` server.**
- **Coroot 1.29.0's OTLP/HTTP port `:8080`.** Coroot serves HTTPS only when
  `https_listen_address` and certificates are configured.

Upstream h2c support in the Collector was proposed (collector issue #10952)
and its PR closed without being merged. OTLP permits HTTP/1.1 for OTLP/HTTP,
and the most common quick-start, `http://localhost:4318`, is exactly the
configuration that fails.

## Decision

### 1. When to fall back

The fallback applies only when `ConnectOptions::protocol` is
`Protocol::Http`. The decision is made inside `Connect`, under the same
connect timeout, so callers see a `Connect` that succeeded.

- **Plaintext.** The existing signal, `Http2Transport::SniffHttp1Response`,
  detects an HTTP/1.1 response to the connection preface. Today it ends in
  `kHttp1PeerMessage`. With this ICP, `Http2Transport` exposes it as a
  transport-internal accessor, so nothing has to match message text. The
  transport closes that connection and reconnects with HTTP/1.1.
- **TLS.** For `Protocol::Http`, ALPN offers `h2,http/1.1` in that order. Two
  answers lead to HTTP/1.1:
  - The server selects `http/1.1`.
  - The server selects nothing: HTTP/2 over TLS requires ALPN `h2` (RFC 9113
    §3.2), so a server that negotiates nothing can only be spoken to in
    HTTP/1.1.

  Any other ALPN answer keeps today's `AlpnMismatchError`. The TLS
  connection is not handed over: the HTTP/1.1 path reconnects and repeats the
  handshake, once per `Provider`.
- **Remembered per transport.** Once a transport has fallen back, it stays on
  HTTP/1.1 for its lifetime, and reconnects go straight to HTTP/1.1. A
  receiver that later gains h2 is used over HTTP/2 after the next `Build()`.
  There is no configuration key to force either version.
- **One log line.** On fallback, `Info` is logged once per transport: "endpoint
  speaks HTTP/1.1 only; OTLP/HTTP continues over HTTP/1.1, one request at a
  time. For throughput use https:// (ALPN h2) or OTLP/gRPC."

### 2. Where it lives

- **`Http1Transport`** is the second `ITransport` that carries bytes. It is a
  new implementation in `src/transport/`, built on the existing
  `EpollReactor`, `UniqueFd`, `SslCtx`/`SslSession` and `nosignal_io`. It has
  its own I/O thread, started by `Http1Transport::Create`.
- **`FallbackTransport`** is a thin selector that the builder constructs for
  `Protocol::Http` only. It owns an `Http2Transport` and forwards to it. For
  `Protocol::Grpc`, `CreateTransport` builds a bare `Http2Transport` as today,
  so gRPC cannot fall back by construction.
- **At most one I/O thread exists at any time.** On the fallback signal,
  `FallbackTransport::SwitchToHttp1` runs three steps, strictly in order:
  1. `Http2Transport::Close`. It joins the I/O thread whatever its timeout
     ("joined either way", `Http2Transport::Close`), and fails what was queued
     or in flight with the existing retryable "transport closed".
  2. Destroy the `Http2Transport`.
  3. Only then `Http1Transport::Create`, which starts the new thread.

  From then on, `FallbackTransport` forwards to the `Http1Transport`.
- **What `Send` does during the switch.** The active transport is swapped under
  a `FallbackTransport` mutex. A `Send` that arrives between steps 1 and 3 fails
  retryable, like today's "already connecting", rather than block.
- **Why the ordering is achievable.** `Connect` runs on a caller thread
  (`Provider::Connect`, or an exporter worker on first use; `interfaces.md`
  §4.1), never on the I/O thread, so step 1 never joins the thread it runs on.
  [ICP 0019](0019-connect-on-the-io-thread.md), which would move `Connect`
  onto the I/O thread, is Draft. If it is ever accepted, it has to move the
  switch off that thread.
- **The rule over the deadline.** A wedged HTTP/2 I/O thread makes step 1 wait
  past the connect budget, as `Close` already does at shutdown. The rule takes
  priority.
- **No codec changes beyond header validation** (§3). `Http1Transport` hands
  up response headers in the
  shape `HttpWireCodec` already reads: field names lowercased, and the status
  as a synthesised `:status` entry. `HttpWireCodec` and its classification
  (`error-model.md` §7) are unchanged. Content-Encoding, gzip and the
  `max_decompressed_bytes` cap stay where they are.

### 3. HTTP/1.1 behaviour

**Requests**
- **Serial, on one keep-alive connection, with no pool and no pipelining.**
  `Send` stays safe for concurrent callers, as ICP 0009 requires. Submissions
  queue as they do today, bounded by `max_pending_requests` and reported as
  `transport_busy`, and the I/O thread writes the next request only once the
  previous response is complete.
- **`SendAll` fan-out becomes sequential.** A request whose deadline passes
  while it is queued fails as a retryable timeout, as it would today.
- **Throughput needs h2 or gRPC.** This is documented in
  `compatibility-matrix.md` §4 and `configuration.md` §3.3.
- **What a request looks like:**
  - The request line is `POST <:path> HTTP/1.1`.
  - `Host` comes from `:authority`.
  - `Content-Length` is always set. No chunked request bodies.
  - The other fields come from the spec, with pseudo-headers dropped. ICP 0038
    already rejects `host`, `connection`, `transfer-encoding`, `upgrade` and
    the rest at `Build()`.
- **Header validation at assembly, for both transports.**
  - **Why now:** ICP 0038 and #412 check static headers at `Build()`, and
    `CallbackAuthProvider` already rejects CR, LF and NUL in the token it
    produces at send time (#412). This check is defence in depth on HTTP/2 and
    the only guard on HTTP/1.1, where a CR or LF in any field is header
    injection.
  - **Where:** a new `wire::ValidateRequestHeaders` (`src/wire/header_validation.*`)
    rejects any field name or value containing CR, LF or NUL. Both codecs'
    `BuildHeaders` call it last, after the auth header is appended, so it covers
    every field, on HTTP/2 and on HTTP/1.1, and OTLP/gRPC as well.
  - **Failure:** the request fails as non-retryable through the existing
    `BuildHeaders` error path, before `ITransport::Send`. The message names the
    field and never its value, which may be a secret.
  - **Tests:**
    - unit tests in `http_wire_codec_test.cpp` and `grpc_wire_codec_test.cpp`:
      an auth provider returning a token with CR, LF or NUL gives a
      non-retryable result, and the mock transport's `Send` is never called;
    - one integration test per transport (`Http2Transport`, `Http1Transport`)
      driving `HttpWireCodec` against a fake peer, asserting the peer receives
      no request.
  - **The serialiser keeps no check of its own.** It relies on this one.

**Responses: a hand-written parser, `src/transport/http1_response_parser.*`**
- **What it reads:**
  - the status line;
  - header fields: total header bytes bounded by `max_trailer_bytes`; obs-fold
    rejected;
  - `1xx` interim responses, which are skipped;
  - bodies by `Content-Length` or chunked: chunk extensions ignored; trailers
    read and discarded under the same bound.
- **When to reject:** a response that carries both `Content-Length` and
  `Transfer-Encoding` (RFC 9112 §6.3), and a `Transfer-Encoding` other than
  `chunked`.
- **Connection close:** a body delimited by connection close, `Connection:
  close`, and an HTTP/1.0 status line all end keep-alive. The next request
  reconnects.
- **Stale-connection retry:** a request written on a reused keep-alive
  connection that closes before any response byte arrives (the peer's idle
  timeout racing the write) is retried once on a new connection, and the retry
  is counted. The peer may have processed the first copy, so the receiver can
  see the request twice; that is the duplicate OTLP's retry semantics already
  allow, as when any export is retried after its response was lost.
- **Size limit:** `max_response_bytes` bounds the body exactly as on HTTP/2,
  with `response_too_large` set.
- **Parser shape:** an incremental push parser with no allocation beyond the
  bounded buffers, so the fuzz target can drive it byte by byte.

**`Cancel` and deadlines**
- A queued request is simply removed.
- An in-flight request, or one past its deadline, closes the connection,
  because HTTP/1.1 has no stream reset. The next request reconnects under the
  existing backoff.

**Fuzz target:** `tests/fuzz/http1_response_fuzz.cpp` lands in the same PR as
the parser, with a seed corpus covering each case above.

### 4. Text for the documents

**`docs/interfaces.md` §4.1 Purpose**, replacing the first sentence:

> A connection to a single OTLP endpoint over HTTP/2, or over HTTP/1.1 for
> OTLP/HTTP when the endpoint cannot speak HTTP/2 (ICP 0042).

The implementations list adds `Http1Transport` and `FallbackTransport`. The
LOCKED `Send` concurrency line is unchanged: `Http1Transport` honours it by
serialising.

**`docs/interfaces.md` §4.3 Purpose:** "the OTLP-over-HTTP/2 wire shape"
becomes "the OTLP wire shape over HTTP/2, or HTTP/1.1 for OTLP/HTTP (ICP
0042)". The LOCKED "one interface" claim stands.

**`include/microtel/internal/transport.hpp`**, the `ITransport` Doxygen:
- "over HTTP/2" and "one nghttp2 session" take the same qualification as
  §4.1.
- The stale `Send is single-threaded — only the exporter worker calls it.
  (LOCKED.)` line is replaced by the ICP 0009 wording that `interfaces.md`
  carries.

**`docs/threading-model.md`, Exactly one I/O thread per `Provider`:** the
rule and its text stand unchanged. §2 is what keeps them true. Only the
citation changes. It keeps `src/transport/http2_transport.hpp:m_io_thread`
and `src/sdk/sdk_builder.cpp:Build`, and adds two files that the implementing PR
creates: `src/transport/http1_transport.hpp:m_io_thread` and
`src/transport/fallback_transport.cpp:SwitchToHttp1`. The implementing PR
updates the marker in the same commit that creates them, so `citation-check`
resolves it.

**`docs/sequences/connection-establishment.md`:**
- "ALPN preference is `h2` only — microtel does not negotiate HTTP/1.1
  fallback in v1" becomes:

  > ALPN offers `h2` only for OTLP/gRPC, and `h2,http/1.1` for OTLP/HTTP,
  > which falls back to HTTP/1.1 on an `http/1.1` or empty answer (ICP 0042).

- The "Protocol mismatch is the exception" paragraph gains:

  > For OTLP/HTTP, an HTTP/1.1 answer is not a mismatch but the fallback. It
  > remains one for OTLP/gRPC, and so does any ALPN answer other than `h2`,
  > `http/1.1` or none.

**The gap documents** state the opposite of the old text:
- `compatibility-matrix.md` §4 becomes "Plaintext OTLP/HTTP to an HTTP/1.1
  receiver", supported and serial. `interop-matrix.md` §4, `troubleshooting.md`
  and the README follow.
- `configuration.md` §3.3's warning (from #501) becomes a note that h2c is
  tried first and HTTP/1.1 is the fallback.
- Roadmap §12 question 6 is marked decided, pointing here.
- `WarnOnRiskyConfig` loses its plaintext branch. The `tls.insecure` warning
  stays.

**`tests/conformance/http/plaintext_gap_test.cpp`** is inverted, as its header
asks, into a positive delivery test against the plaintext `:4318` receiver. The
same suite gains one test that `grpc://` to an HTTP/1.1-only peer still fails
with `Error::Kind::Protocol`.

## Migration

- **Callers:** none. A plaintext or TLS OTLP/HTTP endpoint that failed with
  "peer answered the HTTP/2 preface with an HTTP/1.1 response", or with an
  ALPN `http/1.1`/`none` mismatch, now delivers. The `Build()` plaintext
  warning disappears. Throughput against those endpoints is one request at a
  time. Endpoints that already worked over h2 or h2c are unaffected.
- **Contributors:** request headers are validated once, in the codecs'
  `BuildHeaders`. A new header source goes in before that call, not after it.
- **Contributors:** a second transport implementation now carries bytes. A
  change to `ITransport` semantics, such as cancellation, deadlines or the
  response caps, lands in both `Http2Transport` and `Http1Transport`.
- **Ops:** an operator sees an `Info` line instead of a `Connect()` error.
  `kHttp1PeerMessage` stays for OTLP/gRPC. Its advice ("use https:// (ALPN
  h2) or OTLP/gRPC") is reworded for an endpoint that already is gRPC.

## Rationale & alternatives

- **A configuration key** (`http_version = 1.1`, or a scheme). This is
  explicit and saves one wasted connection, but it adds a setting to every
  configuration surface, and the error it avoids is already detected
  precisely. The fallback costs one extra connect per `Provider`.
- **`Upgrade: h2c`.** Receivers without h2c don't honour it either, and RFC
  9113 deprecates it.
- **A connection pool** to recover concurrency. More code and more connections
  for a path whose fast alternatives are a one-line change. It can follow if
  the serial path turns out to be a bottleneck in practice.
- **A third-party HTTP/1.1 parser** (llhttp and similar). It is a new runtime
  dependency (CLAUDE.md rule 12, which would need its own ICP), and the subset
  needed is small: one method, responses only.
- **Fallback inside `Http2Transport`**, with no selector. It mixes two
  protocols' state into one class and makes gRPC's exclusion a runtime check
  rather than a construction-time one. The selector costs one small forwarding
  class, a third `ITransport` implementation that carries no bytes itself.
- **Handing the TLS connection over** after an `http/1.1` ALPN answer. It saves
  one handshake per `Provider`, but moves an `SslSession` and socket between
  two I/O threads. Reconnecting is simpler.
- **Waiting for receivers to add h2c.** That is on Go's standard library since
  Go 1.24 (`http.Protocols.SetUnencryptedHTTP2`), and worth proposing upstream.
  It does not reach receivers already deployed.
- **Rough size:**
  - production code: about 600–900 lines in `src/transport/`;
  - tests: about 800–1,200 lines across integration, fuzz and conformance;
  - binary: a few tens of KB.
