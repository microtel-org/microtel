# `examples/stack/` — the shared observability stack

One container stack that every example under `examples/` exports to:

```
example (microtel)  --OTLP/gRPC :4317-->  OTel Collector  --OTLP-->  Tempo  <--  Grafana
```

With it running, every example produces something you can look at. Start it
once, run any example, and the trace shows up in Grafana a few seconds later
with no configuration.

```bash
examples/stack/up.sh          # start, wait for all three, print the endpoints
# ... build and run examples ...
examples/stack/down.sh        # stop and remove everything
```

Then open <http://localhost:3000>. There is no login, because the stack turns
on anonymous admin access (see [Security](#security)). The home page is the
provisioned "microtel — recent traces" dashboard; click any row to get the
flame graph. **Explore → Tempo** shows the same data with a query box.

---

## What runs

| Service | Image (pinned) | Why |
|---|---|---|
| `otel-collector` | `otel/opentelemetry-collector-contrib:0.160.0` | What the examples export to. Same tag the conformance gate and the bench harness use (`ci/scripts/conformance.sh`, `bench/versions.lock`), so examples talk to the receiver the test suite is validated against. |
| `tempo` | `grafana/tempo:2.10.8` | Single-binary trace store behind the collector. |
| `grafana` | `grafana/grafana:13.2.2` | Reads Tempo. Datasource and dashboard are provisioned at boot. |

The tags are pinned so that an example that worked last month still works
today. Bumping one is a normal PR; re-run the verification below against the
new tag.

### Ports

| Host port | Service | Used by |
|---|---|---|
| `4317` | collector, OTLP/gRPC | every example; this is the default endpoint |
| `4318` | collector, OTLP/HTTP | nothing here today. It is plaintext HTTP/1.1, which microtel reaches through its HTTP/1.1 fallback, one request at a time; see [the note](#why-grpc-and-not-http) |
| `4319` | collector, OTLP/gRPC with no batching | [`examples/leaf`](../leaf/README.md#one-request-many-devices)'s fan-in check: each request is logged as it arrived, then forwarded to Tempo |
| `13133` | collector `health_check` | `up.sh` readiness polling |
| `3200` | Tempo HTTP API | Grafana's datasource, and the verification `curl` below |
| `3000` | Grafana | you |

Tempo's own OTLP receiver also listens on 4317, but only inside the compose
network. It isn't published, so it can't collide with the collector's port.

### Files

| File | What it is |
|---|---|
| `compose.yaml` | the three services, pinned, with the portability constraints commented inline |
| `collector-config.yaml` | receive OTLP on 4317/4318, batch, export to Tempo, log a line per batch; on 4319, log each request in full, unbatched, and export it to Tempo; and log every log record received on 4317/4318 in full (there is no log store; see [`examples/logs`](../logs/README.md)) |
| `tempo.yaml` | single-binary Tempo, tuned so a trace becomes searchable in about ten seconds instead of Tempo's default of several minutes |
| `grafana/provisioning/` | the Tempo datasource (uid `tempo`) and the dashboard provider |
| `grafana/dashboards/` | the starter dashboard: one TraceQL `{}` table |
| `compose-engine.sh` | engine + compose front-end detection, sourced by `up.sh` and `down.sh` |

---

## Container engine

Both `up.sh` and `down.sh` resolve the engine the same way the rest of the
repo does (`ci/scripts/conformance.sh`, `bench/driver/container.py`):

1. `MICROTEL_CONTAINER_ENGINE` if set (`podman` or `docker`), else
2. `podman` if it is installed, else
3. `docker`.

podman comes first because the project's reference dev host is Fedora. The
compose front-end is resolved as a separate step, since the engine doesn't
determine which compose command you have:

- **podman** → `podman-compose` if on `PATH`, else `podman compose`.
  (`podman compose` only shells out to `podman-compose` anyway, and calling it
  directly skips its "executing external compose provider" banner.)
- **docker** → `docker compose` (v2 plugin) if it works, else `docker-compose`.

Force the other engine on a host that has both:

```bash
MICROTEL_CONTAINER_ENGINE=docker examples/stack/up.sh
```

`compose.yaml` stays inside the intersection of what both front-ends do
reliably: no `healthcheck:` blocks (`up.sh` polls the health endpoints itself),
`depends_on` in short list form only, `:z` on every bind mount, and no named
volumes.

---

## Verifying it works

Every example prints the trace ID it emitted. Use it to check the whole path
end to end:

```bash
$ ./build/examples/microtel_example_basic_trace
trace_id: 532267361510131fac037e635454ce60
ForceFlush: Completed
batches_sent=1 batches_failed=0 rejected=0 queue_depth=0
Shutdown: Completed

$ curl -s http://localhost:3200/api/traces/532267361510131fac037e635454ce60 | head -c 200
```

On the reference host, a lookup by trace ID works about a second after the
example exits. Search (the dashboard and Explore) finds the trace about ten
seconds after that, once Tempo has cut the block and the querier has re-read
its blocklist. If the dashboard is empty, wait for one refresh before deciding
something is wrong.

Searching without a `start`/`end` defaults to a recent window:

```bash
curl -sG http://localhost:3200/api/search --data-urlencode 'q={}' --data-urlencode 'limit=5'
```

---

## Storage is ephemeral

There are no named volumes. Each container writes to its own writable layer,
so `down.sh` takes the collected traces, and Grafana's sqlite database, with
it. The next `up.sh` starts clean.

For a demo stack that is the better trade. It avoids the volume-ownership
problems rootless podman has with images that run as a non-root UID, and you
never get confused by a stale trace from last week. If you want traces to
survive a restart, add a volume for `/var/tempo`, and expect to `chown` it to
`10001:10001` first.

---

## Security

`compose.yaml` sets `GF_AUTH_ANONYMOUS_ENABLED=true` with
`GF_AUTH_ANONYMOUS_ORG_ROLE=Admin` and disables the login form. On a stack
that binds to localhost and holds nothing but example spans, a login screen
would only get in the way of seeing your first trace.

**Do not copy that block anywhere reachable from a network you do not own.**
The collector is likewise plaintext on 4317 and 4318 with no authentication.
For what a real deployment does instead, see
[`docs/auth-callback-recipes.md`](../../docs/auth-callback-recipes.md) and the
transport-security ledger in
[`docs/compatibility-matrix.md`](../../docs/compatibility-matrix.md) §3.

---

## Why gRPC and not HTTP

The collector's plaintext OTLP/HTTP receiver on 4318 serves HTTP/1.1 only. It
answers microtel's HTTP/2 preface with an HTTP/1.1 response, and microtel falls
back to HTTP/1.1 (since v1.2.6; see
[`docs/compatibility-matrix.md`](../../docs/compatibility-matrix.md) §4). That
works, but HTTP/1.1 is one request at a time. The examples therefore use
OTLP/gRPC on 4317, which is h2c by definition, or OTLP/HTTP over TLS, where
ALPN negotiates `h2` with the same collector. Port 4318 stays published, and
`http://localhost:4318` with `Protocol::Http` reaches it, but no example uses
it. Serving TLS from this
collector would mean mounting certificates into it, so
[`examples/tls/`](../tls/) runs its own collector on its own ports and leaves
this one alone.

---

## Troubleshooting

This section covers the containers. For microtel's own errors (a `Build()`
failure, a `Connect()` message, a health counter), see
[`docs/troubleshooting.md`](../../docs/troubleshooting.md).

**`up.sh` says a service never became ready.** Check the logs. The command to
get them is printed along with the error:

```bash
podman-compose -f examples/stack/compose.yaml -p microtel-stack logs
```

**Permission denied reading a config file (SELinux).** Every bind mount in
`compose.yaml` already carries `:z`, which relabels it for SELinux under
rootless podman on Fedora/RHEL and is accepted and ignored by docker. If you
add a mount, add `:z` to it too. A missing label looks like the container
exiting immediately with a permission error on a file that is plainly
world-readable on the host. `sudo ausearch -m avc -ts recent` confirms it.

**Port already in use.** 4317 is the usual one: a leftover conformance-gate
collector (`ci/scripts/conformance.sh` names its container
`microtel-conformance-<pid>`) or a bench sink. Find it with `podman ps` and
remove it.

**The example reports `batches_failed` and a connection error.** The stack is
not up, or something else owns 4317. `curl -s http://localhost:13133` should
return the collector's health JSON. The message in `last_error_message` is listed in
[`docs/troubleshooting.md`](../../docs/troubleshooting.md#providerconnect-fails).

**Traces reach the collector but not Grafana.** Split the path:
`podman-compose -f examples/stack/compose.yaml -p microtel-stack logs otel-collector`
shows a `debug` exporter line per batch. If those lines are there and Grafana
is empty, the problem is in the collector → Tempo hop and the example is fine.

**`down.sh` leaves containers behind** after an interrupted `up.sh`. Remove
them by project name:

```bash
podman ps -a --filter 'name=microtel-stack' -q | xargs -r podman rm -f
```
