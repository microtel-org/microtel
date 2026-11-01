# `tls/` — TLS, a custom CA, mTLS, and the one place OTLP/HTTP works

```cpp
builder.WithEndpoint("https://localhost:5327")
    .WithProtocol(microtel::Protocol::Grpc)
    .WithTls({.insecure = false,
              .ca_bundle = "examples/tls/certs/ca.crt",
              .client_cert = {},        // mTLS: the client's certificate
              .client_key = {},         // mTLS: its key
              .sni_override = {}});     // empty: derived from the endpoint host
```

An empty `ca_bundle` means the system trust store is used; the peer is still
verified. `insecure = true` is the only way to skip verification. It logs a
warning at `Build()`, and in a library configured with
`MICROTEL_FORBID_INSECURE_TLS=ON` it makes `Build()` fail instead.

---

## Running it

The example needs a TLS receiver and a CA to pin, so it brings both as an
opt-in overlay that runs next to the shared stack without changing it.

```bash
examples/stack/up.sh          # the shared stack, so accepted traces reach Grafana
examples/tls/up-tls.sh        # generates certs, then starts the TLS collector

cmake -S . -B build -DMICROTEL_BUILD_EXAMPLES=ON && cmake --build build
./build/examples/microtel_example_tls      # from the repository root

examples/tls/down-tls.sh      # tears down only the overlay
```

Arguments, all optional:
`[grpc-endpoint] [http-endpoint] [mtls-endpoint] [cert-dir]`, defaulting to
`https://localhost:5327`, `https://localhost:5328`, `https://localhost:5337`
and `examples/tls/certs`. The default cert directory is relative, so run the
binary from the repository root or pass the directory explicitly.

### What the overlay is

| File | What it is |
|---|---|
| `gen-certs.sh` | a throwaway CA, a server certificate for `localhost`, and a client certificate for mTLS |
| `tls-collector-config.yaml` | three receivers (TLS gRPC on 5327, TLS HTTP on 5328, mTLS gRPC on 5337) that forward what they accept to the shared stack's collector |
| `tls-compose.yaml` | that collector as its own compose project, `microtel-tls-example` |
| `up-tls.sh` / `down-tls.sh` | start and stop it, using the shared stack's engine detection (`../stack/compose-engine.sh`) |

Nothing in `examples/stack/` is touched. The overlay is a separate compose
project on separate ports (5327/5328/5337, and 15134 for health), so every
other example keeps exporting to 4317 while it runs. `certs/` is gitignored;
`up-tls.sh` generates it if it is missing, and `MICROTEL_CERT_FORCE=1`
regenerates it. If the shared stack isn't running, `up-tls.sh` says so and the
accepted traces stop at the overlay collector's `debug` exporter.

### The certificates

`gen-certs.sh` makes P-256 keys, the same as `ci/scripts/conformance.sh`, so
the example uses the same kind of material as the conformance gate. The server
certificate carries `CN=localhost` and also
`subjectAltName = DNS:localhost, IP:127.0.0.1`. You need the SAN: microtel
verifies the hostname (ICP [0022](../../docs/icps/0022-tls-peer-verification.md)),
and a certificate with only a CN is rejected. Every file is `chmod 0644`
because the collector reads them as uid 10001, and a key the container can't
open is the most common reason this example fails to start.

None of it is secret. It authenticates a demo collector on localhost.

---

## The four phases

| Phase | Configuration | Outcome |
|---|---|---|
| 1 | gRPC, **no** `ca_bundle` | `Connect()` fails: `TLS certificate verification failed: unable to get local issuer certificate` |
| 2 | gRPC, `ca_bundle` pinned | connects, `batches_sent=1` |
| 3 | **HTTP**, `ca_bundle` pinned | connects, `batches_sent=1` |
| 4 | gRPC, `ca_bundle` + client certificate, against the mTLS port | connects, `batches_sent=1` |

### Sample run

Trimmed: the run also prints the cert directory up front, and each phase
prints its `ca_bundle`, `ForceFlush` and `Shutdown` lines.

```
=== phase 1: gRPC over TLS, no ca_bundle (expected to fail) ===
  endpoint: https://localhost:5327
  ca_bundle: (system trust)
  Connect() failed: TLS certificate verification failed: unable to get local issuer certificate
  ForceFlush: Completed
  health: connection_state=Disconnected batches_sent=0 batches_failed=1 rejected=0 ConnectFailure=5
  last_error: TLS certificate verification failed: unable to get local issuer certificate

=== phase 2: gRPC over TLS, ca_bundle pinned ===
  Connect(): ok
  trace_id: 445f7a8ae25442693518191f30f5fba3
  health: connection_state=Connected batches_sent=1 batches_failed=0 rejected=0 ConnectFailure=0

=== phase 3: OTLP/HTTP over TLS, ca_bundle pinned ===
  endpoint: https://localhost:5328
  Connect(): ok
  trace_id: fc1b795e0dfed01b2f42d2fd40e128f1
  health: connection_state=Connected batches_sent=1 batches_failed=0 rejected=0 ConnectFailure=0

=== phase 4: gRPC over mTLS, client certificate presented ===
  endpoint: https://localhost:5337
  Connect(): ok
  trace_id: e4ccd18543e15497e147f66819f66946
  health: connection_state=Connected batches_sent=1 batches_failed=0 rejected=0 ConnectFailure=0
```

---

## The failure without `ca_bundle`

Phase 1 is the check that would catch verification quietly not happening. A
client that accepts any certificate passes every other phase in this example.
The overlay's CA is self-signed and isn't in any trust store on the machine, so
with `ca_bundle` empty the peer is checked against system trust and refused at
`Connect()`, before any span is on the wire:

```
TLS certificate verification failed: unable to get local issuer certificate
```

The same message ends up in `HealthSnapshot::last_error_message`, and the
`ConnectFailure` drop counter goes up with each attempt.

This has actually gone wrong before. Until ICP 0022, `ca_bundle` was loaded
into a trust store that `SSL_VERIFY_NONE` never consulted, and a client in that
state would happily connect to a server its configured CA did not vouch for.

There are three ways to make phase 1 succeed. From most to least sensible: pin
the CA (phase 2), install the CA in the system trust store, or set
`insecure = true` and accept that verification is off.

---

## OTLP/HTTP over HTTP/2

Phase 3 is the configuration in which microtel uses OTLP/HTTP over HTTP/2
against a stock collector: over TLS, ALPN negotiates `h2`. The collector's
plaintext OTLP/HTTP receiver serves HTTP/1.1 only. microtel reaches it too,
since v1.2.6, by falling back to HTTP/1.1, but HTTP/1.1 sends one request at a
time. Full detail is in
[`docs/compatibility-matrix.md`](../../docs/compatibility-matrix.md) §4.

---

## The TLS 1.2 floor

microtel calls `SSL_CTX_set_min_proto_version(TLS1_2_VERSION)` itself instead
of inheriting the linked OpenSSL's floor. A receiver limited to TLS 1.0 or 1.1
fails the handshake on every build, and **there is no option to lower the
floor** (`docs/compatibility-matrix.md` §3, issue #216). TLS 1.3 is used when
the receiver offers it. The pinned collector does, which explains how the mTLS
rejection below looks.

---

## mTLS, and the variant that fails

Phase 4 presents `client.crt` / `client.key` to the receiver on 5337, which is
configured with `client_ca_file`. The phase is skipped automatically if the
client certificate or key is missing.

To see the negative case, the mTLS port without a client certificate, change
one argument:

```bash
./build/examples/microtel_example_tls https://localhost:5337 https://localhost:5328 https://localhost:5337
```

Phase 2 then dials the mTLS port with the CA pinned but no client certificate,
and reports:

```
  Connect() failed: nghttp2 recv failed during SETTINGS exchange
  health: connection_state=Disconnected batches_sent=0 batches_failed=1 rejected=0 ConnectFailure=5
```

The message doesn't mention certificates. Here is why. Under
TLS 1.3 the client finishes its side of the handshake before the server has
validated the client certificate. The server's refusal therefore arrives later,
as the connection closing, and microtel reports it at the layer where it
noticed: the HTTP/2 SETTINGS exchange. The connection is refused either way.
Phase 1 reads differently because there verification failed on the client's
own side, where microtel knows the exact reason. When mTLS is misconfigured,
the collector's log states the cause plainly:

```bash
podman-compose -f examples/tls/tls-compose.yaml -p microtel-tls-example logs
```

---

## Where this is validated properly

This example is a demonstration. The assertions live in
`tests/conformance/{http,grpc}/tls_test.cpp` (`TlsCustomCa`,
`UntrustedCaFails`, `MutualTls`, `MutualTlsWithoutClientCertFails`,
`SniOverride`), which run against a collector started by
`ci/scripts/conformance.sh`. The transport-security ledger, including what is
unsupported (TLS 1.0/1.1 receivers, HTTP proxies, in-process SigV4), is in
[`docs/compatibility-matrix.md`](../../docs/compatibility-matrix.md) §3.
