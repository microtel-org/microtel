# `distributed_handoff`

Two processes, one trace.

A sender starts a `Client` span and uses the W3C propagators to write the
span's identity into an outgoing message. A receiver reads the headers back,
installs the extracted context as current, and starts a `Server` span. Because
of that install, the server span parents to the sender's span **without a
parent argument**. Both processes export to the same collector under different
service names, and Tempo joins their spans because the trace ID on the wire is
the trace ID in both.

This example builds on the previous two. [`sugar_tour/`](../sugar_tour/) shows
the call sites, and [`context_propagation/`](../context_propagation/) shows the
thread-local slot that makes implicit parenting work. This one puts a socket
in the middle and shows that the mechanism works the same way across it.

## Run it

You'll need two terminals. **Start the receiver first**, since it binds the
port and waits.

```bash
examples/stack/up.sh

cmake -S . -B build -DMICROTEL_BUILD_EXAMPLES=ON
cmake --build build --target microtel_example_handoff_receiver \
                    --target microtel_example_handoff_sender
```

```bash
# terminal 1
./build/examples/microtel_example_handoff_receiver
```
```bash
# terminal 2
./build/examples/microtel_example_handoff_sender
```

Both take the same two optional arguments:

```
<binary> [endpoint] [port]

  [endpoint]  OTLP/gRPC collector, default http://localhost:4317
  [port]      the loopback port the two halves meet on, default 9099
```

The sender retries the connection for five seconds, which gives you time to
switch terminals. The receiver handles exactly one message and exits, so the
example can show it flushing its trace and shutting down cleanly. A real
server would loop.

### What they print

Both binaries also end with a `view it:` hint giving the Grafana URL and a
`curl` command for the trace; those lines are left out below.

```
$ ./build/examples/microtel_example_handoff_sender
headers written:
  baggage: tenant=acme,order.priority=high
  traceparent: 00-6b6fbe127f904dffdd16cf9e16c823c0-add1851f35ba998f-01
  tracestate: microtel=e1
reply: 200 OK

sender trace_id: 6b6fbe127f904dffdd16cf9e16c823c0
sender span_id:  add1851f35ba998f   (the receiver's server span should name this as its parent)
ForceFlush: Completed
batches_sent=1 batches_failed=0 rejected=0 queue_depth=0
Shutdown: Completed
```

```
$ ./build/examples/microtel_example_handoff_receiver
receiver: waiting for one message on 127.0.0.1:9099
request: PLACE /orders/ord-20260919-0042
headers read:
  baggage: tenant=acme,order.priority=high
  traceparent: 00-6b6fbe127f904dffdd16cf9e16c823c0-add1851f35ba998f-01
  tracestate: microtel=e1
extracted: remote=true sampled=true tracestate="microtel=e1" baggage_entries=2

receiver trace_id:      6b6fbe127f904dffdd16cf9e16c823c0
receiver span_id:       3b3c3936ccd0042c
receiver parent span:   add1851f35ba998f   (the sender's client span)
ForceFlush: Completed
batches_sent=1 batches_failed=0 rejected=0 queue_depth=0
Shutdown: Completed
```

Three things to look for in that pair of outputs:

| sender | receiver | |
|---|---|---|
| `trace_id: 6b6fbe12…` | `trace_id: 6b6fbe12…` | equal, so it's one trace |
| `span_id: add1851f…` | `parent span: add1851f…` | the join |
| `traceparent: …-01` | `sampled=true` | the sampling decision crossed as well |

## Confirming it is really one trace

The ID is the same in both terminals, which is most of the proof. The rest is
checking that the backend agrees, by fetching a single trace that contains
spans from two different services:

```bash
$ curl -s http://localhost:3200/api/traces/6b6fbe127f904dffdd16cf9e16c823c0 | jq -r '
    .batches[] as $b
    | ($b.resource.attributes[] | select(.key=="service.name") | .value.stringValue) as $svc
    | $b.scopeSpans[].spans[]
    | "\($svc)\t\(.name)\t\(.kind)"'

microtel-handoff-sender      order.place     SPAN_KIND_CLIENT
microtel-handoff-receiver    order.receive   SPAN_KIND_SERVER
```

The two resource batches come back in whatever order Tempo stored them; only
the pairing matters.

In Grafana, open <http://localhost:3000>. The trace appears on the "microtel —
recent traces" dashboard within about ten seconds, and its flame graph shows
`order.receive` nested under `order.place`, with the service name (and colour)
changing between them. That change at the process boundary is what this
example is about.

TraceQL for either half:

```
{ resource.service.name =~ "microtel-handoff-.*" }
```

## What actually crosses

### 1. `traceparent`: the trace context

```cpp
microtel::SpanContext outgoing = client->GetContext();
outgoing.trace_state = outgoing.trace_state.Set("microtel", "e1");

microtel::W3CTraceContextPropagator{}.Inject(outgoing, setter);
```

`Inject` writes `traceparent`, plus `tracestate` if it isn't empty. If the
context is invalid it writes nothing at all, which is why the receiver checks
what it got.

The sender changes a local copy of the context, not the span itself.
`TraceState::Set` is copy-on-write: it builds a new list and returns a new
`TraceState`, leaving every other copy alone. This is what allows
`Span::GetContext()` to stay `noexcept` and return by value. `TraceState` keeps
its entries behind a `shared_ptr`, so copying a `SpanContext` costs a refcount
bump and no allocation
([ICP 0025](../../docs/icps/0025-propagation-core.md) §1).

Carrying a `tracestate` value across the hop is new in v1.1. Before that,
`TraceState` had no storage, `FromHeader` returned the empty state for every
input, and a microtel hop silently dropped a vendor's `tracestate` (issue
#208). The receiver prints what arrived so you can see it get through.

### 2. `baggage`: what the request carries

```cpp
const microtel::Baggage bag =
    microtel::Baggage{}.Set("tenant", "acme").Set("order.priority", "high");

microtel::W3CBaggagePropagator{}.Inject(bag, setter);
```

**Baggage lives on `Context`, never on `SpanContext`.** It belongs to the
request rather than to a span: a request carries baggage whether or not a span
is active, and baggage set inside a span has to outlive that span within the
enclosing scope. Putting it on `SpanContext` would also add a second growable
member to the `noexcept` by-value accessor, which is exactly the problem
`TraceState` sits behind a `shared_ptr` to avoid. ICP 0025 §2 records this
decision.

Baggage never influences sampling or parenting (ICP 0025 §3, contract 6). Its
purpose is to make a value known at the edge, here the tenant, available to
every service downstream. The receiver does the usual thing with it and
promotes one entry to a span attribute.

Every mutator is copy-on-write, so the chain above builds three immutable
values and keeps the last one. The limits are W3C's: 180 members, 4096 bytes
per member, and 8192 bytes in total. They're enforced against the canonical
serialised form, so baggage that fits once will always fit.

### 3. Nothing else

SDK state, resource attributes and configuration don't cross. A hop carries
the trace context and the baggage, and that's all.

## The two sides

### Inject: a `HeaderSetter`

```cpp
handoff::Headers headers;
const microtel::HeaderSetter setter = [&headers](std::string_view name, std::string_view value)
{ headers.insert_or_assign(std::string{name}, std::string{value}); };

microtel::W3CTraceContextPropagator{}.Inject(outgoing, setter);
microtel::W3CBaggagePropagator{}.Inject(bag, setter);
```

The propagators don't know anything about the carrier; they write through a
callback. The two propagators write separate sets of headers and don't
coordinate, so you can use either one or both. Both are stateless and
thread-safe, which is why the example constructs them inline instead of
keeping them around.

### Extract: a `HeaderGetter`, and then install

```cpp
const microtel::SpanContext remote = microtel::W3CTraceContextPropagator{}.Extract(getter);
const microtel::Baggage     bag    = microtel::W3CBaggagePropagator{}.Extract(getter);

const microtel::ScopedContext incoming{microtel::Context{remote, bag}};

const auto server = tracer.StartAsCurrentSpan("order.receive",
    {.kind = microtel::SpanKind::Server, .parent = {}, .start_time = {}, .attributes = {}});
```

**`.parent` is unset**, and that's the key line in the example. Installing the
extracted context and then starting a span with no parent is the same implicit
parenting you get inside one process. The parent is
`CurrentContext().active_span_context`, which in this case arrived over a
socket. Passing `.parent = remote` would also work and produce the same tree.
Installing is better because it also puts the baggage where the rest of the
handler can read it without passing it through every call.

`Extract` doesn't throw; on failure it returns an invalid `SpanContext`, since
a hot path has no error channel. With an invalid parent the server span becomes
an ordinary root, so the service still produces a trace, but it isn't joined
to the sender's. The receiver reports this on stderr:

```
receiver: no usable traceparent arrived — this span will start a new trace
          instead of joining the sender's.
```

To see it, run `printf 'PLACE /x\r\n\r\n' | nc localhost 9099` while the
receiver is waiting.

When extraction succeeds, the returned context has `remote` set to `true`. That
flag is how you tell a parent that came off the wire from a span in your own
process.

### Unsampled senders

If the sender's sampler drops its span, `traceparent` still goes out, with
real IDs and the sampled flag cleared, because `StartAsCurrentSpan` installs a
valid context on the drop path too. The receiver then joins a trace whose
sender half was never exported. That's correct W3C behaviour. For how the
handle and the current context differ on that path, see the unsampled section
of [`context_propagation/`](../context_propagation/).

## The plumbing, and why it is in its own file

`wire.hpp` holds the socket code and a text protocol with HTTP's
start-line-plus-headers shape and nothing more. Headers are kept in a sorted
map, so they go out in name order:

```
PLACE /orders/ord-20260919-0042 CRLF
baggage: tenant=acme,order.priority=high CRLF
traceparent: 00-<32 hex>-<16 hex>-01 CRLF
tracestate: microtel=e1 CRLF
CRLF
```

The protocol is intentionally plain, and it lives in its own file so that
`sender.cpp` and `receiver.cpp` contain only telemetry code. **Nothing in
`wire.hpp` includes a microtel header.** The propagators work through
`HeaderGetter` / `HeaderSetter` callbacks, so the carrier never needs to know
what it's carrying. You could replace `wire.hpp` with HTTP/2, gRPC metadata, a
Kafka record header or a JSON envelope, and neither `main` would change.

Two details in that file matter beyond this demo.

`handoff::Fd` is a move-only RAII owner for the file descriptor. The project
requires every resource to be RAII-owned (rule 5 in
[`CLAUDE.md`](../../CLAUDE.md)), example code included. microtel's own
`UniqueFd` lives in `src/common/raii/` and isn't public API, so an example
meant to be copied into your tree carries its own few lines rather than
reaching into `src/`.

Header names are matched exactly, without case folding. Both halves write them
the way the propagators do, in lowercase. A real HTTP carrier has to case-fold
in its `HeaderGetter`, because HTTP/1.1 field names are case-insensitive and
HTTP/2 requires lowercase on the wire.

## Baggage survives the span scope

The receiver reads the baggage from inside the server span's scope:

```cpp
const microtel::ScopedContext incoming{microtel::Context{remote, bag}};

const auto server = tracer.StartAsCurrentSpan("order.receive", {...});

if (const auto tenant = microtel::CurrentContext().baggage.Get("tenant"); tenant)
{
    server->SetAttribute("tenant.id", std::string{*tenant});   // <- here
}
```

This is the case that has to work for baggage to be useful. Baggage belongs
to the context, not to a span, so opening a span scope must not disturb it.
`StartAsCurrentSpan` carries the thread's baggage into the context it
installs, which lets the rest of the handler, and anything it calls, read the
same entries without passing them around.

That wasn't always true. Writing this example turned up a bug where
`StartAsCurrentSpan` installed a `Context` built from the span context alone,
so the thread's baggage was empty for the whole span scope and this read
returned `nullopt`. That was
[#283](https://github.com/microtel-org/microtel/issues/283), fixed in
`src/sdk/sdk_tracer.cpp` by passing `CurrentContext().baggage` into the
installed context. Until the fix landed, the example worked around it by doing
the read above the span, with a comment saying why.

## Exit codes

Both binaries return `0` on success, `1` if `Build()` fails, `2` if
`ForceFlush` did not complete, a batch failed, or the collector rejected spans via OTLP partial success, and `3` if the hand-off itself did not happen:
nothing accepted the connection, the peer hung up, or no reply was sent or
received. The receiver also returns `1` if it can't accept a connection on its
port (usually because something else is bound to it).

A failed hand-off on the sender side still produces a trace, with the client
span marked `Error`. That's the useful outcome: the failure is reported through
the API's normal channels, and you can see it in Grafana.
