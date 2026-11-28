# `tests/fakes/`

Test doubles that carry logic, for the cases where a mock isn't enough.

## What lives here

| Fake | Interface | Why a fake and not a mock |
|---|---|---|
| `fake_clock.hpp`                  | `internal::IClock`              | Tests advance time on demand, and `Now()` must answer consistently between advances. |
| `fake_steady_clock.hpp`           | `internal::ISteadyClock`        | Same. |
| `fake_transport.hpp`              | `internal::ITransport`          | Keeps every `RequestSpec` it receives and serves scripted `TransportResult`s in FIFO order, resolving each `Send` synchronously. |
| `fake_reactor.hpp`                | `internal::IReactor`            | Tests script event timelines; the fake dispatches them to registered callbacks deterministically. |
| `fake_wire_codec.hpp`             | `internal::IWireCodec`          | Serves scripted `WireResult`s in FIFO order, so retry-then-succeed and retry-until-exhausted can be driven from a queue, and keeps a copy of every payload so a test can decode what went on the wire. |
| `fake_diagnostics_sink.hpp`       | `internal::IDiagnosticsSink`    | Stores counters as plain `uint64_t` and exposes them for assertions. |
| `fake_auth_provider.hpp`          | `internal::IAuthProvider`       | Simulates the TTL cache: tests configure the cache lifetime and the miss/hit sequence. |
| `fake_resource_detector.hpp`      | `internal::IResourceDetector`   | Returns a configured `Resource`. Trivial, but logically distinct from a mock. |
| `fake_span_processor.hpp`         | `internal::ISpanProcessor`      | Stores received spans in a vector for inspection. |
| `fake_exporter.hpp`               | `internal::IExporter`           | Records batches in memory and exposes them. |
| `fake_log_record_processor.hpp`   | `internal::ILogRecordProcessor` | Records every emitted `LogRecord` with its scope, for `SdkLogger` tests. |
| `fake_log_exporter.hpp`           | `internal::ILogExporter`        | Captures every exported batch for assertions on record counts, scope grouping and `Resource`. |
| `fake_logger.hpp`                 | `microtel::Logger`              | Captures every emitted `LogRecord` for inspection. |
| `fake_span.hpp`                   | `microtel::Span`                | Records every mutation (attributes, events, links, statuses, ends) for the otelcpp shim tests. |
| `fake_tracer.hpp`                 | `microtel::Tracer`              | Records every `StartSpan` (name and options) and hands out a fresh recording `FakeSpan` per call. |
| `fake_provider.hpp`               | `microtel::Provider`            | Records tracer and meter acquisitions and flush/shutdown timeouts, and returns configured statuses. Hands out a `FakeLeafReceiver`. |
| `fake_leaf_receiver.hpp`          | `microtel::LeafReceiver`        | The no-op receiver `FakeProvider` returns (ICP 0034): answers `Disabled` and counts calls. |
| `fake_otlp_trace_decoder.hpp`     | `internal::IOtlpTraceDecoder`   | Serves a canned decode, applies `max_spans` to it, and throws `std::bad_alloc` on a chosen call to drive the receiver's out-of-memory path. |
| `fake_export_transport.hpp`       | `microtel::ExportTransport`     | Serves scripted `SendResult`s in FIFO order and keeps a copy of every request; optionally throws (std or not) or blocks each `Send`, past its deadline, until `Cancel` — the transport `Cancel` exists for (ICP 0036). Thread-safe, since a Provider with metrics and logs on calls it from three workers. |
| `fake_meter.hpp`                  | `microtel::Meter`               | Records instrument creations, returns recording sync instruments, and captures observable callbacks so tests can drive a collection cycle. |
| `failing_upb_alloc.hpp`           | upb's `upb_alloc_global`        | `ScopedFailingUpbAlloc` makes every upb allocation fail while it is alive, so the encoder's arena-creation failure path can be driven. Process-wide; single-threaded tests only. |

## Rules

- Logic is allowed. That is the difference between a fake and a mock.
- Fakes are deterministic: no real clocks, no real I/O, no sleeps. The
  test drives time, events and responses explicitly.
- Fakes don't start threads. The fake clock and reactor exist so tests
  can avoid real concurrency; a fake that needs its own thread means
  the design is wrong. (A fake may still be *called* from a production
  worker thread. `FakeWireCodec::send_call_count` is atomic for that
  reason.)
- Fakes are test-only. They live in the `microtel::testing` namespace
  and never appear in production headers.

## Naming

`Fake<InterfaceName>`, e.g. `FakeClock`, `FakeTransport`,
`FakeReactor`. Same namespace and naming scheme as `tests/mocks/`.

## Promoting a mock to a fake

A mock sometimes starts simple, then one test needs scripted behaviour,
then another needs more, and the mock picks up logic. When that
happens, promote it: don't grow logic on a mock. The class becomes a
fake and the file moves from `tests/mocks/` to `tests/fakes/`.

It never goes the other way. Once a test double has logic it stays a
fake, even if some tests configure it as a no-op.
