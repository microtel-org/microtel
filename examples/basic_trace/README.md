# `basic_trace`

The smallest end-to-end microtel program. The other examples copy its layout
and conventions.

It builds a `Provider` with `SdkBuilder`, opens the OTLP/gRPC connection and
emits one request trace: a `Server` parent span with two `Internal` children,
with attributes, an event and a status. Then it prints the trace ID, calls
`ForceFlush`, prints `GetExporterHealth()`, and calls `Shutdown`.

## Run it

Start the shared stack, then run the binary with no arguments. Its default
endpoint is the stack's collector.

```bash
examples/stack/up.sh

cmake -S . -B build -DMICROTEL_BUILD_EXAMPLES=ON
cmake --build build --target microtel_example_basic_trace

./build/examples/microtel_example_basic_trace
```

```
trace_id: 532267361510131fac037e635454ce60
ForceFlush: Completed
batches_sent=1 batches_failed=0 rejected=0 queue_depth=0
Shutdown: Completed
```

Open <http://localhost:3000>. The **microtel — recent traces** dashboard is the
home page, and the trace shows up there within about ten seconds. You can also
ask Tempo for it directly with the printed ID:

```bash
curl -s http://localhost:3200/api/traces/532267361510131fac037e635454ce60
```

To use a different endpoint, pass it as `argv[1]`:

```bash
./build/examples/microtel_example_basic_trace http://collector.internal:4317
```

## What to notice in the code

The trace ID is printed. `EmitRequestTrace` returns
`SpanContext::trace_id.ToHex()`. Without it you can't tell "the export failed"
apart from "I'm looking at the wrong time range", which is why every example in
this tree prints one.

`StartSpanOptions` is initialised with every field (`.kind`, `.parent`,
`.start_time`, `.attributes`). A partial designated initialiser trips
`-Wmissing-field-initializers` under the `-Werror` the examples build with.

String attribute values are spelled `std::string{...}`. `microtel::AttributeValue`
is a `std::variant` whose first alternative is `bool`. A C++20 standard library
converts a bare string literal to the `std::string` alternative (clang 22 and
GCC 15 both do), but libraries that predate the P0608 fix to `variant`'s
converting constructor bind a `const char*` to `bool` and silently record
`true`. The explicit `std::string` removes the question.

`Connect()` is optional, and the example calls it anyway. The connection is
otherwise established lazily on the first export; calling it up front turns a
misconfigured endpoint into an immediate message. The example warns and
carries on, so the full lifecycle still runs with nothing listening, and
`ForceFlush`'s status and `GetExporterHealth()` then report the failure. That's
what those calls are for.

The program exits 0 only when `ForceFlush` returns `Completed` **and**
`batches_failed` is 0 **and** the collector rejected no span via OTLP partial
success (the `PartialSuccessRejection` drop counter is 0), 1 if `Build()`
fails, and 2 otherwise. `Completed` means the queue drained, which is also true
when the collector rejected the batch, and a partial-success rejection counts
the batch as sent, not failed. Against a collector with no traces pipeline, the flush completes but
the batch fails, and the program exits 2:

```
trace_id: 5829b14f3c5a78b31296077438aa6d0d
ForceFlush: Completed
batches_sent=0 batches_failed=1 rejected=0 queue_depth=0
last_error: UNIMPLEMENTED (12): unknown service opentelemetry.proto.collector.trace.v1.TraceService
Shutdown: Completed
```

The `Status` switch is exhaustive. It includes `InvalidArgument` and
`Unsupported`, which only the setters return and `ForceFlush` never does. With
no `default:` label, `-Wswitch` flags the next enumerator someone adds.

## Finding this trace among others

Backends group traces by service name, which is set at build time:

```cpp
.WithServiceName("microtel-basic-example")
.WithServiceVersion("1.0.0")
```

In Grafana's Explore → Tempo, that becomes the TraceQL filter:

```
{ resource.service.name = "microtel-basic-example" }
```

To tell two runs apart, change the string and rebuild. There's no
`--service-name` flag on purpose: a real service names itself once at startup,
and an example that took the name from the command line would teach the wrong
habit. Deployment-time overrides go through `OTEL_SERVICE_NAME`; see
[`docs/configuration.md`](../../docs/configuration.md) §3.

## Protocol

The example uses `Protocol::Grpc` against a plaintext `http://` endpoint:
gRPC is h2c by definition. Plaintext OTLP/HTTP to the collector's `:4318`
works too, but the collector serves HTTP/1.1 there, so microtel falls back to
HTTP/1.1 and sends one request at a time (see
[`docs/compatibility-matrix.md`](../../docs/compatibility-matrix.md) §4 and the
note in [`examples/README.md`](../README.md)). For OTLP/HTTP over HTTP/2, point
the example at an `https://` endpoint and add
`.WithProtocol(microtel::Protocol::Http)`.
