# Sequence: Connection Establishment

**Status:** M0 deliverable. Normative timeline for the first request after `Provider::Build()` returns.
**See also:** `architecture.md` §3.6 (Transport), `threading-model.md` §2.3 (I/O thread), `interfaces.md` §4.1 (`ITransport`).

---

## Participants

- **Caller thread** — the application thread that called `SdkBuilder::Build()`.
- **Exporter worker** — created by `BatchSpanProcessor`.
- **I/O thread** — created by `Transport::Connect`.
- **DNS** — system resolver via `getaddrinfo`.
- **Peer** — OTel collector or backend at the configured endpoint.

---

## Happy path

```
Caller          Exporter Worker    I/O Thread          DNS          Peer
  |                  |                  |                 |          |
  | Build()          |                  |                 |          |
  |--------+         |                  |                 |          |
  |        | construct Provider, Encoder,                 |          |
  |        | WireCodec, Transport (not yet connected)     |          |
  |<-------+         |                  |                 |          |
  |                  |                  |                 |          |
  | Provider::Connect()                  |                 |          |
  |---->  Transport::Connect(opts)       |                 |          |
  |              |                       |                 |          |
  |              |---- spawn I/O thread -+----- start ---->|          |
  |              |                       | getaddrinfo --->|          |
  |              |                       |<---- A/AAAA ----|          |
  |              |                       | TCP SYN ----------------> |
  |              |                       |<---- TCP SYNACK ----------|
  |              |                       | TCP ACK -----------------> |
  |              |                       |                            |
  |              |                       | TLS ClientHello (ALPN h2)->|
  |              |                       |<-- TLS ServerHello, cert --|
  |              |                       | TLS Finished ------------->|
  |              |                       |<-- ALPN selected: h2 ------|
  |              |                       |                            |
  |              |                       | HTTP/2 connection preface->|
  |              |                       | SETTINGS frame ----------->|
  |              |                       |<--------- SETTINGS --------|
  |              |                       | SETTINGS ACK ------------->|
  |              |                       |<--- SETTINGS ACK ----------|
  |              |                       |                            |
  |              | <-- ConnectionState ----                            |
  |              |     = Connected                                     |
  |<-- ok --|                                                          |
  |                  |                  |                 |          |
  | spawn exporter worker                |                 |          |
  +---->                                 |                 |          |
                     | wait on queue cv  |                 |          |
                     | (idle)            |                 |          |
```

---

## Annotations

**Step-by-step.**

1. `SdkBuilder::Build()` constructs the `Provider`, the encoder, the wire codec, and the transport — but **does not open a socket** (`error-model.md` §8: "Network preflight is not part of `Build()`"). The user's expectation that `Build` is a fast, non-network-touching operation is honoured.
2. The first network connection happens lazily on the first export attempt, or eagerly via `Provider::Connect()` if the caller wants to detect connectivity issues at startup. The diagram shows the eager path; the lazy path is identical from `Transport::Connect` onward.
3. The I/O thread is spawned by `Transport::Connect`. It immediately performs DNS resolution (synchronous `getaddrinfo`), then TCP, then TLS, then HTTP/2 preface and SETTINGS exchange.
4. ALPN negotiates `h2`. If the peer does not select `h2`, the transport returns `Error::Kind::Network` from `Connect` with a diagnostic explaining the ALPN failure.
5. Once SETTINGS ACKs are exchanged in both directions, the connection is `Connected` and ready for `Send` calls. The exporter worker is started by the SDK after `Connect` returns success.

**Timeouts** apply per `configuration.md` §3.4:

| Stage | Timeout |
|---|---|
| DNS + TCP | `connect` (default 10s) |
| TLS | `tls_handshake` (default 10s) |
| HTTP/2 SETTINGS exchange | included in `connect` |
| First export | `per_export` |
| Total retry budget | `retry_budget` |

A timeout in any stage produces `connect_failure` and triggers reconnect with backoff. (See `retry-after-failure.md` for the reconnect timing.)

**TLS material** (CA bundle, client cert, client key) is loaded once during `Connect` and held in `SslCtx` for the lifetime of the transport. Reconnect reuses `SslCtx`; only `SslSession` is reconstructed per connection.

**ALPN** offers `h2` only for OTLP/gRPC, and `h2,http/1.1` for OTLP/HTTP, which falls back to HTTP/1.1 on an `http/1.1` or empty answer ([ICP 0042](../icps/0042-http1-fallback-for-otlp-http.md)).

---

## Variant — connect failure

```
I/O Thread           DNS               Peer
  |                   |                  |
  | getaddrinfo --->  |                  |
  |<--- NXDOMAIN -----|                  |
  |                                      |
  | record connect_failure                |
  | wait backoff(attempt=1) ~ 1s ± jitter |
  | (loop: attempt=2, 4s ± jitter; etc.)  |
  |                                      |
  | until Transport::Connect timeout       |
  | or successful resolution               |
```

DNS failure, TCP refusal, and TLS handshake failure all map to the same recovery path: increment `connect_failure`, sleep with exponential backoff and jitter, retry up to the `connect` timeout. On final failure, `Transport::Connect` returns `Error::Kind::Network` and the I/O thread terminates.

**Protocol mismatch is the exception, and is not retried.** An ALPN answer that is not `h2`, or an HTTP/1.1 response to the plaintext connection preface, means the endpoint cannot ever carry this session: `Connect` returns `Error::Kind::Protocol` with a message naming the working alternatives, and the wire codecs mark the result non-retryable. See `docs/compatibility-matrix.md` §4 and issue #166.

For OTLP/HTTP, an HTTP/1.1 answer is not a mismatch but the fallback. It remains one for OTLP/gRPC, and so does any ALPN answer other than `h2`, `http/1.1` or none. The fallback happens inside the same `Connect`: `FallbackTransport::SwitchToHttp1` closes the HTTP/2 transport, which joins its I/O thread, destroys it, and only then creates the `Http1Transport`, so there is never more than one I/O thread.

`Build()` and `Provider::Connect` propagate this as `microtel::Expected<void, Error>` (alias — see ICP 0002). The application can react (retry with different config, exit with diagnostic, etc.).

---

## Edge cases captured by tests

Covered, in `tests/integration/transport/` (`http2_connect_test.cpp`, `http2_tls_connect_test.cpp`, and for the OTLP/HTTP fallback `fallback_transport_test.cpp`):

- Endpoint accepts TCP but the certificate is untrusted, or issued to another name.
- ALPN produces no agreement, or an answer microtel never offered.
- The plaintext peer answers the preface with an HTTP/1.1 response.
- The peer disappears under an established connection (`Reconnecting`), and the reconnect that follows.

Named in M0 and still uncovered:

- Endpoint hostname has no A/AAAA record.
- Peer SETTINGS frame includes `MAX_CONCURRENT_STREAMS=0`.
- Peer sends GOAWAY immediately after SETTINGS (mismatched expectations).
