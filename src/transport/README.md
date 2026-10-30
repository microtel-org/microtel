# `src/transport/`

## What lives here

The HTTP/2 transport: an nghttp2 session over a plaintext or OpenSSL TLS
socket, driven by an epoll I/O loop on its own thread, with per-stream
in-flight state and reconnect after a drop. Both wire codecs sit on top of it.

The HTTP/1.1 transport for OTLP/HTTP receivers that do not speak HTTP/2
([ICP 0042](../../docs/icps/0042-http1-fallback-for-otlp-http.md)): serial
requests on one keep-alive connection, plaintext or TLS (ALPN `http/1.1`),
with the same reactor and connection setup.
It is also the seam where an HTTP/3 transport could drop in for v1.6+ without
changing `IWireCodec`.

| File | Contents |
|---|---|
| [`http2_transport.hpp`](http2_transport.hpp) | `Http2Transport`, the `ITransport` implementation |
| [`http1_transport.hpp`](http1_transport.hpp) | `Http1Transport`, the HTTP/1.1 `ITransport` (ICP 0042): serial requests, background reconnect after keep-alive ends, one stale-connection retry |
| [`event_log_limiter.hpp`](event_log_limiter.hpp) | How loudly `Http1Transport` logs its reconnects and retries: Info for the first of each, then at most one Info summary a minute, else Debug |
| [`epoll_reactor.hpp`](epoll_reactor.hpp) | `EpollReactor`, the `IReactor` implementation |
| [`nosignal_io.hpp`](nosignal_io.hpp) | `SendNoSignal` and the SIGPIPE-safe TLS `BIO` |
| [`connect_error.hpp`](connect_error.hpp) | The `Error` a failed TCP connect reports, from its errno |
| [`connection_setup.hpp`](connection_setup.hpp) | Connection setup shared by the transports: `ClaimConnectSlot`, `ParseEndpoint`, `TcpConnect`, `TlsConnect` (the TLS client handshake, ALPN list from the caller) and `SelectedAlpn` |
| [`http1_response_parser.hpp`](http1_response_parser.hpp) | `Http1ResponseParser`, the incremental HTTP/1.1 response parser for the fallback path ([ICP 0042](../../docs/icps/0042-http1-fallback-for-otlp-http.md) §3) |

## Owner

Track D — Transport, a foundational track in
[`docs/development.md`](../../docs/development.md) (it had to land before
tracks A, B, C and E could unblock).

## What it implements

- `internal::ITransport` (declared in [`include/microtel/internal/transport.hpp`](../../include/microtel/internal/transport.hpp))
- `internal::IReactor` (declared in [`include/microtel/internal/reactor.hpp`](../../include/microtel/internal/reactor.hpp))
- The connection state machine behind `ConnectionState` in
  [`provider.hpp`](../../include/microtel/provider.hpp): `Disconnected`,
  `Connecting`, `Connected`, `Reconnecting` (after a drop,
  [ICP 0018](../../docs/icps/0018-reconnect-after-drop.md)) and the terminal
  `Closed`.
- The I/O thread loop (`docs/threading-model.md` §2.3).

## Dependencies

- nghttp2 (system `libnghttp2.so`, ≥ 1.50)
- OpenSSL (system, ≥ 1.1.1)
- The RAII wrappers in [`src/common/raii/`](../common/raii/): `UniqueFd`,
  `SslCtx`, `SslSession`, `Nghttp2Session`, `BioMethod`. Per ICP 0003 §3.1,
  `SslCtx` is per-`Transport`, not process-shared.
- Test doubles: `fake_reactor.hpp` in [`tests/fakes/`](../../tests/fakes/)
  for the transport's unit tests. Code above the transport tests against
  `mock_transport.hpp` or `fake_transport.hpp`.

## Tests

- `tests/unit/transport/`: `http2_transport_test.cpp` against `FakeReactor`,
  `epoll_reactor_test.cpp` against a real epoll instance,
  `nosignal_io_test.cpp`, `connect_error_test.cpp`,
  `connection_setup_test.cpp` (the setup error paths),
  `event_log_limiter_test.cpp`, and
  `http1_response_parser_test.cpp` (every case fed whole and one byte at a
  time).
- [`tests/integration/transport/`](../../tests/integration/transport/) runs
  against a small in-process nghttp2 server over loopback:
  - `http2_connect_test.cpp`: connect, refused, drop, reconnect, HTTP/1.1 peer.
  - `http2_send_test.cpp`: request/response, concurrent `Send`, peer
    TCP reset, the frame-level peer behaviour from
    `docs/sequences/goaway-handling.md` and
    `docs/grpc-wire-protocol.md` §2.6 (`Send_PeerGoaway*`,
    `Send_PeerRstStream_*`, `GrpcResponse_Split*`), and the response
    memory caps of `docs/memory-model.md` §6
    (`Send_ResponseOverMaxResponseBytes_*`,
    `Send_TrailersOverMaxTrailerBytes_*`).
  - `http2_tls_connect_test.cpp`: TLS, ALPN, certificate handling.
  - `http1_transport_test.cpp`: `Http1Transport` against the scripted
    HTTP/1.1 peer in [`tests/helpers/fake_http1_peer.hpp`](../../tests/helpers/fake_http1_peer.hpp),
    plaintext and TLS: keep-alive and serial sends, chunked and
    close-delimited bodies, `Connection: close` and HTTP/1.0, oversized,
    malformed and truncated responses, a reset mid-response, the
    stale-connection retry, a failed reconnect, deadlines, cancel, the queue
    bound, and `HttpWireCodec` over it (status classification, header
    validation).
- `tests/fuzz/`: the response-path harnesses (`grpc_codec_fuzz.cpp`,
  `otlp_response_fuzz.cpp`, `response_decompression_fuzz.cpp`) sit above the
  transport but feed it the responses it bounds. `http1_response_fuzz.cpp`
  drives `Http1ResponseParser` directly.

## Style notes

- **`Send` is safe for concurrent callers** (LOCKED, relaxed by
  [ICP 0009](../../docs/icps/0009-transport-concurrent-send.md);
  `interfaces.md` §4.1). The trace, metric and log exporter workers share one
  transport, each through its own codec. The transport serialises submissions
  onto its single I/O thread through `m_pending_queue` under `m_pending_mu`, a
  leaf lock held only for a push or a drain. This note previously read
  "single-threaded `Send`… concurrent calls are a contract violation", which
  stopped being true in M12.
- **The bytes referenced by `RequestSpec.payload` are borrowed.** The
  caller (the wire codec) retains ownership and guarantees the buffer is
  stable until the per-request future completes (LOCKED —
  `memory-model.md` §3.3).
- **One transport == one socket == one nghttp2 session.** Reconnect is
  internal; clients see it only through `ConnectionState`.
- **Every socket write goes through `nosignal_io.hpp`**: `SendNoSignal` for
  plaintext, and the custom `BIO` it builds for TLS, which is why the TLS
  session is wired with `SSL_set_bio` and never `SSL_set_fd`. A bare `write`
  or `SSL_set_fd` here hands a peer the ability to kill the host process with
  `SIGPIPE` (issue #177, `threading-model.md` §7.1).
- **`SslCtx` is per-`Transport`** (ICP 0003 §3.1), not process-shared.
  v1.1 multi-`Provider` keeps the same shape: each `Provider` builds its
  own `Transport` with its own `SslCtx`.
- **No exceptions cross thread boundaries** (LOCKED —
  `error-model.md` §5). I/O thread internal failures are caught at the
  reactor's top level, recorded as `connect_failure`, and the loop continues.
  `Http2Transport::Create` and `EpollReactor::Create` are `noexcept` and turn
  allocation or thread-spawn failure into an `InternalFailure` error.
- **Fork survival is not handled here.** The `pthread_atfork` child handler is
  registered by the provider registry in
  [`src/sdk/provider_registry.cpp`](../sdk/provider_registry.cpp) and marks
  every live `Provider` as shut down, so the child never touches the
  transport's threads or locks (`docs/sequences/fork-survival.md`).
