# Migrating from opentelemetry-cpp

**Status:** M17 L6 deliverable. Validated against the working shim (M17 L1–L5,
`tests/integration/otelcpp_shim/wire_conformance_test.cpp`) — every claim below
is backed by a passing test, not aspiration.
**Companion:** [ICP 0014](icps/0014-otelcpp-shim-and-rule-13.md) (shim design
and distribution model), [ICP 0015](icps/0015-unrepresentable-attribute-policy.md)
(attribute-value policy), [ICP 0016](icps/0016-adapter-drop-accounting.md)
(shim-local diagnostics for adapter-level omissions), [ICP 0017](icps/0017-lazy-transport-connect.md)
(lazy transport connect), [`configuration.md`](configuration.md)
(env-var / `microtel.toml` reference), [`src/adapters/otelcpp/README.md`](../src/adapters/otelcpp/README.md)
(implementation-level detail).

## Who this is for

You have a codebase already instrumented against `opentelemetry-cpp` — call
sites doing `tracer->StartSpan(...)`, `meter->CreateUInt64Counter(...)`,
`logger->EmitLogRecord(...)` — and you want microtel's runtime and OTLP
exporter underneath it, without the gRPC / protobuf / abseil dependency tree
opentelemetry-cpp's SDK and exporters bring in. This is Tier 3 in
[`compatibility-matrix.md`](compatibility-matrix.md) §7 ("API-adapter compatibility"), **experimental**: the
supported subset is exactly what this document and `src/adapters/otelcpp/`
implement, no more.

**What does not change:** every call site that creates spans, records
metrics, or emits logs through the `opentelemetry-cpp` API. That is the whole
point — read on for the one thing that does change (how the provider is
constructed at startup).

## Quick start

<p align="center">
  <img alt="Using microtel through the opentelemetry-cpp shim: existing call sites keep calling the header-only opentelemetry-cpp API; startup.cpp builds a microtel Provider and calls RegisterGlobally, so the API's global providers resolve to the microtel shim, which exports through microtel over OTLP. The opentelemetry-cpp SDK, its exporters, gRPC, protobuf, abseil and libcurl are no longer linked." src="images/usage-otelcpp-shim.svg" width="860">
</p>

```cpp
// startup.cpp — the only file that changes.
#include "adapters/otelcpp/global_registration.hpp"
#include "microtel/sdk_builder.hpp"

auto provider = microtel::SdkBuilder()
                    .WithEndpoint("https://otel-collector.internal:4318")
                    .WithServiceName("my-service")
                    .Build();
if (!provider)
{
    // handle provider.error() — see error-model.md §8
}

microtel::adapters::otelcpp::RegisterGlobally(*provider);

// From here on, every existing opentelemetry-cpp call site — anywhere in the
// program, in any translation unit — routes to microtel with zero edits.
```

Everything after `RegisterGlobally` is unmodified `opentelemetry-cpp`
application code. `otel_trace::Provider::GetTracerProvider()`,
`otel_metrics::Provider::GetMeterProvider()`,
`otel_logs::Provider::GetLoggerProvider()` all now resolve to microtel.

If you only use one signal, `MakeTracerProvider` / `MakeMeterProvider` /
`MakeLoggerProvider` (declared alongside each signal's shim header) let you
register just that one via the signal's own `Provider::SetXProvider` call.

The connection to your collector is established lazily on the first export —
there is no separate connect step to call, matching `opentelemetry-cpp`'s own
OTLP exporters (ICP 0017). Call `(*provider)->Connect()` yourself only if you
want fail-fast-at-startup semantics instead of discovering a bad endpoint on
first export.

## Dependency-graph diff

This is the reason to migrate. Numbers are from ICP 0014, measured against
opentelemetry-cpp v1.19.0 and re-verified at v1.28.0.

| | opentelemetry-cpp (SDK + OTLP exporter) | microtel + shim |
|---|---|---|
| gRPC library | required for OTLP/gRPC | never linked (rule 13, `symbol-scan.sh`) |
| protobuf C++ runtime | required (SDK serialization) | never linked — wire encoding is upb |
| abseil | vendored inside the API headers under the *default* config (`WITH_STL=OFF`); 30 headers enter the include graph | **0** under `OPENTELEMETRY_STL_VERSION=2020`, which the shim pins |
| libcurl | required by the default HTTP exporter | never linked — transport is nghttp2 directly |
| Wire transport | gRPC-over-HTTP/2 or libcurl | nghttp2 directly, both protocols |

The abseil elimination is the one requiring your action: it only happens if
your **own** application code also builds against `OPENTELEMETRY_STL_VERSION=2020`
(see below) — otherwise your call sites still pull the vendored-abseil headers
even though the shim itself doesn't need them.

## Build-system changes

### 1. Remove opentelemetry-cpp's SDK and exporter targets

Your `CMakeLists.txt` currently links something like
`opentelemetry_trace`, `opentelemetry_metrics`, `opentelemetry_logs`,
`opentelemetry_exporter_otlp_http`, `opentelemetry_otlp_recordable`. Delete
all of it — the shim replaces the SDK+exporter stack entirely. Keep only the
**API** dependency (see below); nothing else about your build references
opentelemetry-cpp by name anymore.

### 2. Add microtel with the shim enabled

```cmake
include(FetchContent)
FetchContent_Declare(microtel
    GIT_REPOSITORY <your-microtel-remote>
    GIT_TAG        <pinned-tag>)
set(MICROTEL_BUILD_OTELCPP_SHIM ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(microtel)

target_link_libraries(your_app PRIVATE microtel_sdk microtel_otelcpp_shim)
```

`MICROTEL_BUILD_OTELCPP_SHIM=ON` makes microtel's own build fetch
opentelemetry-cpp's API headers (pinned at v1.28.0, `SOURCE_SUBDIR api/include`
so only the header-only API is populated — its SDK/exporter CMake project
never runs). This is a **build-time** dependency of the shim only; it adds
nothing to microtel's own runtime closure (rule 12).

### 3. Point your own translation units at the same otel-cpp configuration

**This is the step migrations most often get wrong, and getting it wrong is
silent.** `microtel_otelcpp_shim` sets `OPENTELEMETRY_STL_VERSION=2020` and
`OPENTELEMETRY_ABI_VERSION_NO=1` for its own compilation, but your
application's `.cpp` files that `#include <opentelemetry/...>` headers need
the identical two defines — otel-cpp's ABI is a function of them (ICP 0014
§"constraint"), and a mismatch produces link-incompatible symbols, not a
compile error. Link `microtel_otelcpp_shim` on **every** target that includes
an `opentelemetry/` header, not only the target that calls `RegisterGlobally`:

```cmake
target_link_libraries(your_instrumented_library PRIVATE microtel_otelcpp_shim)
```

If you get this wrong on ABI version specifically, you get a clear, actionable
`#error` instead of a mysterious link failure — `src/adapters/otelcpp/abi_guard.hpp`,
included from every shim header, fails the build by name
(`OPENTELEMETRY_ABI_VERSION_NO != 1`, or
`OPENTELEMETRY_HAVE_METRICS_BOUND_INSTRUMENTS_PREVIEW` defined) rather than
producing "cannot instantiate abstract class" forty lines into a template
instantiation. There is currently no equivalent guard for
`OPENTELEMETRY_STL_VERSION` mismatches specifically — those manifest as the
ABI-mismatch link failures ICP 0014 describes.

### 4. `src/` is on the public include path

`microtel_otelcpp_shim` exposes both microtel's public `include/microtel/`
headers and `src/` itself — the shim's own implementation headers
(`tracer_shim.hpp`, `meter_shim.hpp`, `logger_shim.hpp`,
`global_registration.hpp`, …) live under `src/adapters/otelcpp/` and include
each other by that path, so `src/` has to be reachable for a consumer linking
only `microtel_otelcpp_shim` to resolve them. No extra
`target_include_directories` call needed on your side — linking the target is
enough. (Note for anyone reading an old checkout: earlier versions of this
target exposed only `include/`, requiring a manual `-Isrc` workaround; that
gap is closed. `tests/unit/adapters/otelcpp_shim_public_include_test.cpp` is
the regression test.)

## API import changes

None, at call sites. Replace only:

| Was | Becomes |
|---|---|
| `#include <opentelemetry/sdk/trace/...>`, `.../exporters/otlp/...` (SDK/exporter construction headers) | `#include "adapters/otelcpp/global_registration.hpp"`, `#include "microtel/sdk_builder.hpp"` |
| Your own `TracerProvider`/`MeterProvider`/`LoggerProvider` construction code (whatever built your `BatchSpanProcessor`, `OtlpHttpExporter`, etc.) | `microtel::SdkBuilder{...}.Build()` + `RegisterGlobally(provider)` |
| `#include <opentelemetry/trace/tracer.h>`, `.../metrics/meter.h`, `.../logs/logger.h`, and all `opentelemetry::trace` / `::metrics` / `::logs` call sites | **unchanged** |

## Configuration: env vars and `microtel.toml`

microtel resolves `OTEL_*` env vars directly — see
[`configuration.md`](configuration.md) §3 for the complete, authoritative
per-setting table (endpoint, protocol, compression, headers, batch options,
span limits, sampler, and more, each with its `OTEL_*` name, its
`microtel.toml` key, and its `SdkBuilder` method). It is not reproduced here
to avoid the table drifting out of sync with itself in two places; if your
deployment already sets `OTEL_EXPORTER_OTLP_ENDPOINT`,
`OTEL_EXPORTER_OTLP_HEADERS`, `OTEL_RESOURCE_ATTRIBUTES`, etc., microtel reads
them with no changes on your part, precedence: code > env > `microtel.toml` >
defaults (`configuration.md` §1).

Three things worth knowing:

- **`OTEL_EXPORTER_OTLP_ENDPOINT=http://collector:4318` carries over** since
  v1.2.6. microtel tries HTTP/2 first and falls back to HTTP/1.1, which the
  receiver on `:4318` that opentelemetry-cpp was talking to serves. On
  HTTP/1.1 microtel sends one request at a time, so for throughput switch that
  deployment to `http://collector:4317` with `OTEL_EXPORTER_OTLP_PROTOCOL=grpc`,
  or to `https://collector:4318`. See
  [`compatibility-matrix.md`](compatibility-matrix.md) §4.
- Per-signal env vars for signals v1 doesn't yet resolve independently (e.g.
  `OTEL_EXPORTER_OTLP_METRICS_ENDPOINT` as distinct from the unsigned
  `OTEL_EXPORTER_OTLP_ENDPOINT`) are **ignored, not rejected**
  (`configuration.md` §2.2).
- There is no `microtel.toml` equivalent of opentelemetry-cpp's own YAML
  declarative-configuration file format. Translate your settings to the
  `SdkBuilder` calls or the env vars in the referenced table.

## Behavioral differences — read this before production

Every row is backed by a test. Where a row references ICP 0015, the shim's
governing principle is: **preserve or omit, never invent** — a converted
value is always either exact or absent, never a plausible-looking fabrication.

| Area | Behavior | Why | Reference |
|---|---|---|---|
| `uint64_t` attribute above `INT64_MAX` | Arrives as its **exact decimal digits**, as a string. Type changes; value doesn't. | ICP 0015 | `otelcpp_attribute_conversion_test.cpp` |
| `span<const uint64_t>` with any such element | **Every** element in the array renders as a decimal string, not just the offending one — arrays can't hold mixed types, and dropping one element would silently shift index correlation with a parallel attribute. | ICP 0015 | `otelcpp_attribute_conversion_test.cpp` |
| `span<const uint8_t>` (byte attributes) | Lowercase hex string, no separators. `microtel::AttributeValue` has no bytes variant. A span whose hex rendering (2 characters per byte) would exceed `ShimOptions::attribute_value_length_limit` (default 4096, i.e. over 2048 bytes) is **omitted**, never cut mid-byte, and counted in `GetShimDiagnostics().oversized_byte_attributes_omitted`. If you set a non-default `SpanLimitOptions::attribute_value_length_limit`, pass the same value to `RegisterGlobally(provider, {.attribute_value_length_limit = ...})`. On a metric, the measurement is kept and lands in the series without that attribute. | ICP 0015; [ICP 0033](icps/0033-shim-attribute-value-length-limit.md) | `otelcpp_attribute_conversion_test.cpp`, `src/adapters/otelcpp/README.md` |
| `uint64_t` **measurement** (Counter/Histogram) above `INT64_MAX` | **Omitted**, not wrapped to negative and not clamped. Unlike attributes, there is no degraded string form for a measurement — the instrument only accepts numbers. Counted via `microtel::adapters::otelcpp::GetShimDiagnostics().unrepresentable_measurements_omitted`, a shim-local counter — not `microtel::Provider`'s own diagnostics, which conversion failures above the SDK can't reach. | ICP 0015's principle, extended; ICP 0016 | `shim_diagnostics.hpp`, `otelcpp_meter_shim_test.cpp` |
| `LogRecord::SetEventId(id, name)` | The `name` half survives into `event_name`; the numeric `id` has no microtel field and is dropped. | `microtel::LogRecord` has no integer event-id slot | `log_record_shim.hpp`, `otelcpp_log_record_shim_test.cpp` |
| `TraceState` | **Round-trips**, via the serialised `tracestate` header value — both sides implement W3C §3.3 but keep private representations. An entry either survives exactly or, if the other side's parser rejects it, is absent. microtel gained the storage this needs in v1.1 (issue #208). | ICP 0015's principle; [ICP 0025](icps/0025-propagation-core.md) §1 | `otelcpp_context_conversion_test.cpp` |
| `schema_url` on `GetTracer` | **Dropped.** `microtel::Provider::GetTracer` has no such parameter. | — | `tracer_shim.hpp` |
| `schema_url` on `GetMeter` | **Passed through.** `microtel::Provider::GetMeter` does carry it. | Deliberate asymmetry — do not "fix" one to match the other; `microtel::Provider`'s own surface differs per signal. | `meter_shim.hpp` |
| `schema_url` / `attributes` on `GetLogger` | **Dropped.** `microtel::Provider::GetLogger` has neither parameter. | — | `logger_shim.hpp` |
| `GetLogger(logger_name, name, ...)` scope naming | If `name` is empty, the instrumentation-scope name resolves to `logger_name` — matching the reference opentelemetry-cpp SDK's own behavior (`if (name.empty()) name = logger_name;`), not a guess. Every ordinary single-argument call (`GetLogger("my.lib")`) depends on this. | Verified against vendored opentelemetry-cpp SDK source, `sdk/src/logs/logger_provider.cc` | `otelcpp_logger_shim_test.cpp` |
| Sync `Gauge` instrument | **Unavailable.** `Meter::CreateInt64Gauge`/`CreateDoubleGauge` are ABI v2; the shim pins ABI v1. Use `CreateInt64ObservableGauge`/`CreateDoubleObservableGauge` instead — those are ABI v1 and fully supported. | ABI v1 pin | `abi_guard.hpp` |
| `Counter<T>::Bind()` / bound instruments | **Unavailable.** Gated behind `OPENTELEMETRY_HAVE_METRICS_BOUND_INSTRUMENTS_PREVIEW`, which the shim's pinned config does not define. Building with that macro defined fails the build immediately with a named `#error` rather than a template-instantiation cascade. | Preview feature, not implemented | `abi_guard.hpp` |
| `Span::AddLink()` (post-creation link) | **Unavailable.** ABI v2. Links are only accepted at `StartSpan` time, via the `links` parameter — otel-cpp's own ABI-v1 surface. | ABI v1 pin | `otelcpp_tracer_shim_test.cpp` (`LinksForwardViaAddLinkWithConvertedAttributes`) |
| Default parent when otel's `parent` option is unset | Inherits the **currently active span** (`trace::Scope` / `WithActiveSpan`) if one exists on the thread, matching otel-cpp semantics and microtel's own native API, which has inherited the thread's current context since v1.1 ([ICP 0025](icps/0025-propagation-core.md) §3). | Precedence: explicit valid `SpanContext` > `is_root_span`-flagged `Context` > `Context`-carried span > active `Scope` > unset (root) | `otelcpp_tracer_shim_test.cpp` |
| Observable metric callback throws | **Contained at the shim boundary**, not propagated. otel-cpp does not require observer callbacks to be `noexcept`; microtel's collection path is. One throwing callback costs neither the process nor other callbacks' observations in the same collection cycle. | — | `otelcpp_meter_shim_test.cpp` (`ThrowingCallbackCostsNeitherTheProcessNorTheOtherCallbacks`) |
| Observable callback self-removal | A callback may call `RemoveCallback` on itself during its own invocation (e.g. after a terminal observation) without deadlocking. Removal takes effect from the *next* collection cycle, matching otel-cpp's own SDK guarantee. | — | `otelcpp_meter_shim_test.cpp` (`CallbackMayRemoveItselfDuringCollectionWithoutDeadlock`) |
| `TracerShim::CloseWithMicroseconds` | Shuts down the **entire microtel provider**, not just the tracer. otel-cpp puts flush/close on the tracer; microtel puts them on the provider. Same blast radius otel-cpp's own SDK has (closing one tracer shuts the shared processor pipeline). | — | `tracer_shim.hpp` |

## Validation

Every claim above and in the dependency-graph diff is exercised by an
automated test, not asserted in prose alone:

- `tests/unit/adapters/otelcpp_*_test.cpp` — one file per shim layer
  (attribute/context conversion, span, tracer, meter, logger, global
  registration), against fakes.
- `tests/integration/otelcpp_shim/wire_conformance_test.cpp` — the one that
  matters most for this document: a real `SdkBuilder`-built `Provider`, driven
  purely through the `opentelemetry-cpp` API via `RegisterGlobally`, over a
  real loopback HTTP/2 connection, with the captured request bytes decoded
  with upb and checked against exactly what the otel-cpp calls set — proving
  the whole chain end to end, not just that calls reach an in-memory fake.

Run `ctest --test-dir build -R otelcpp` after configuring with
`-DMICROTEL_BUILD_OTELCPP_SHIM=ON` to reproduce all of it locally.

## What this document does not cover

- **opentelemetry-python.** Out of scope for this shim; see the Python
  bindings milestone (M18, ICP 0013) for microtel's own Python story, which is
  not an opentelemetry-python adapter.
- **A Tier-1/Tier-2 compatibility matrix** against specific collector or
  backend versions — that's [`docs/interop-matrix.md`](interop-matrix.md).
- **Auto-instrumentation.** Both opentelemetry-cpp and microtel are manual
  instrumentation only in v1; microtel's auto-instrumentation is planned for
  v1.5 in the [roadmap](roadmap.md).
