# `auth_bearer/`: a collector that requires a token

There are two ways to attach a credential, and this example shows both of
them working and both of them failing:

```cpp
// static: zero-allocation StaticHeadersAuthProvider, no user code per batch
builder.WithHeaders({{.key = "authorization", .value = "Bearer …"}});

// rotating: CallbackAuthProvider, called per export batch, memoised for cache_ttl
builder.WithAuthProvider([]() -> microtel::Expected<std::string, microtel::Error>
                         { return cached_header_value; },
                         std::chrono::milliseconds{0});
```

Either way, the value is the complete header value with the scheme included.
The codecs write it verbatim and prepend nothing, so pass `"Bearer <token>"`
and not just `"<token>"`.

---

## Running it

This example needs a receiver that actually checks the token, so it brings its
own as an opt-in overlay. The shared stack is left alone.

```bash
examples/stack/up.sh              # the shared stack, so accepted traces reach Grafana
examples/auth_bearer/up-auth.sh   # the guarded collector on :5317

cmake -S . -B build -DMICROTEL_BUILD_EXAMPLES=ON && cmake --build build
./build/examples/microtel_example_auth_bearer

examples/auth_bearer/down-auth.sh # tears down only the overlay
```

Arguments: `[endpoint]` (default `http://localhost:5317`) and `[token]`
(default `microtel-example-token`). Pass a different token and phases 3 and 4
fail the same way phase 1 does.

### What the overlay is

| File | What it is |
|---|---|
| `auth-collector-config.yaml` | a collector whose OTLP/gRPC receiver on 5317 is guarded by the `bearertokenauth` extension, forwarding what it accepts to the shared stack's collector |
| `auth-compose.yaml` | that collector as its own compose project, `microtel-auth-example` |
| `up-auth.sh` / `down-auth.sh` | start and stop it, using the shared stack's engine detection (`../stack/compose-engine.sh`) |

Nothing in `examples/stack/` is touched. The overlay is a second collector on
its own ports (5317, plus 15133 for health) in its own compose project, so
every other example keeps exporting to 4317 while this one runs. Accepted
batches are forwarded to `stack-host:4317`, which is the host's own port
reached through an `extra_hosts: host-gateway` entry. That's how a trace that
passed authentication still ends up in Tempo and Grafana.

The `bearertokenauth` extension ships in the pinned
`otel/opentelemetry-collector-contrib:0.160.0` image, the same image and tag
the conformance gate uses (`tests/conformance/collector/config.yaml` configures
the same extension). There's no second image and nothing is unpinned.

The token is a demo credential committed to a public repository. It
authenticates nothing.

---

## The four phases

| Phase | Configuration | Outcome |
|---|---|---|
| 1 | `WithHeaders`, wrong token | the collector rejects it: `UNAUTHENTICATED (16)`, `batches_failed=1`, `NonRetryableFailure=1` |
| 2 | `WithAuthProvider`, callback returns an error | the header can't be built, so the batch is dropped and never sent unauthenticated; `last_error_message` is prefixed `authorization header unavailable:` |
| 3 | `WithHeaders`, right token | `batches_sent=1` |
| 4 | `WithAuthProvider`, right token | `batches_sent=1` |

### Sample run

```
endpoint: http://localhost:5317
start the auth overlay with: examples/auth_bearer/up-auth.sh

=== phase 1: WithHeaders, wrong token ===
  trace_id: c67c5804f3880532e9ac96088136f672
  ForceFlush: Completed
  health: connection_state=Connected batches_sent=0 batches_failed=1 rejected=0 NonRetryableFailure=1 ConnectFailure=0
  last_error: UNAUTHENTICATED (16): provided authorization does not match expected scheme or token
  Shutdown: Completed

=== phase 2: WithAuthProvider, callback fails ===
  trace_id: 734aa87b2418898e0bc7d024de98b765
  ForceFlush: Completed
  health: connection_state=Connected batches_sent=0 batches_failed=1 rejected=0 NonRetryableFailure=1 ConnectFailure=0
  last_error: authorization header unavailable: token endpoint unreachable
  Shutdown: Completed

=== phase 3: WithHeaders, right token ===
  trace_id: f47a77ad80857e8c11c8b9488d99016b
  ForceFlush: Completed
  health: connection_state=Connected batches_sent=1 batches_failed=0 rejected=0 NonRetryableFailure=0 ConnectFailure=0
  Shutdown: Completed
```

Phase 4 prints the same as phase 3 with a different trace ID, and the run ends
with a pointer to Grafana. The real end-to-end check is to look the trace IDs
up in Tempo afterwards. Phases 3 and 4 resolve; phases 1 and 2 don't:

```bash
curl -s -o /dev/null -w '%{http_code}\n' http://localhost:3200/api/traces/f47a77ad80857e8c11c8b9488d99016b   # 200
curl -s -o /dev/null -w '%{http_code}\n' http://localhost:3200/api/traces/c67c5804f3880532e9ac96088136f672   # 404
```

---

## How each failure behaves

A wrong token costs the batch, and the spans in it are gone. The receiver's
`UNAUTHENTICATED` is non-retryable (an identical retry would get an identical
rejection), so the exporter records one `batches_failed` and one
`NonRetryableFailure` and moves on. `Connect()` still succeeds, because the
credential travels on the export request and not on the handshake. A token
problem never looks like a connectivity problem.

A failing callback also costs the batch, and nothing goes to the wire. This
was deliberate hardening in v1.1. Before [issue
#250](https://github.com/microtel-org/microtel/issues/250), a callback error sent
the batch *unauthenticated*. Now `BuildHeaders` fails as a unit, the batch is
dropped, and the message reaches `HealthSnapshot::last_error_message` prefixed
`authorization header unavailable:`. A throwing callback costs the same one
batch: the exception is caught at the provider boundary and converted to
`Error::Kind::InternalFailure`
([#251](https://github.com/microtel-org/microtel/issues/251)). **Prefer
returning `make_unexpected` to throwing.** A returned error keeps your
`Error::Kind` all the way to the snapshot, whereas a throw shows up as
`InternalFailure` whatever actually went wrong.

Errors aren't cached. If your token endpoint is down, the callback runs once
per batch with no damping, so do your backoff inside your own refresher and
keep it out of the callback.

---

## Before writing a real callback

Read [`docs/auth-callback-recipes.md`](../../docs/auth-callback-recipes.md) §2
first. Four facts decide what a callback may do, and the signature shows none
of them:

1. It runs on an exporter worker thread, inside `IWireCodec::Send`.
2. One auth provider serves every pipeline. Traces, metrics and logs share it.
3. It runs under the provider's mutex, so a slow callback stalls all three.
4. Nothing bounds it with a timeout. If it blocks forever the pipeline blocks
   forever, the queue fills, and spans drop with `QueueFull`.

So the callback must do **no network I/O and take no unbounded lock**. It
should amount to a mutex acquisition and a string copy. Fetch tokens on a
thread you own, and have the callback hand out whatever that thread last
fetched. Phase 4 is written that way: it returns a string that's already in
hand.

`cache_ttl` memoises your callback and knows nothing about token expiry. When
you own a cache (and you should), pass zero so yours is the only one. That
costs one mutex acquisition and one string copy per batch, and saves you from
reasoning about which of two caches is stale.

The recipes document has the full OAuth2 client-credentials shape (a refresh
thread, a synchronous first fetch, and the wiring). It also explains why
`AuthCallback` can't express AWS SigV4: SigV4 signs a canonical request that
includes the body and needs a per-request `x-amz-date`, so sign at a sidecar
instead.

---

## Transport security is a separate question

The overlay's guarded port is plaintext on purpose. gRPC is h2c by definition,
so microtel reaches it without TLS, and any failure here has to be an auth
failure. A real deployment sends the token inside TLS. See [`tls/`](../tls/),
which brings its own certificates, and
[`docs/compatibility-matrix.md`](../../docs/compatibility-matrix.md) §3 for the
transport-security ledger.
