# `src/sdk/`

## What lives here

The SDK behind the public API, built as `microtel_sdk`: `SdkBuilder` and the
`Provider` lifecycle (`ForceFlush`, `Shutdown`, the v1.1 setters), `Resource`
resolution, the samplers, the tracer and span, the span and log-record
processors, the metrics pipeline, and the multi-profile registry. It is the
largest directory in `src/`; the list below is grouped by signal.

## Owner

Track A — Trace SDK.

## What it implements

Provider and builder:

- `microtel::Provider` and `SdkBuilder` (declared in
  [`provider.hpp`](../../include/microtel/provider.hpp) and
  [`sdk_builder.hpp`](../../include/microtel/sdk_builder.hpp)), as
  `SdkProvider` in [`sdk_provider.cpp`](sdk_provider.cpp) and
  [`sdk_builder.cpp`](sdk_builder.cpp).
- The four hot-reload setters on `Provider`: `SetBatchOptions`,
  `SetMetricInterval`, `SetSamplerRatio`, `SetLogLevel`. They live in
  [`sdk_provider.cpp`](sdk_provider.cpp), with the concrete `SetOptions` /
  `SetInterval` they drive on the batch processors and the periodic reader.
- The multi-profile registry in
  [`provider_registry.{hpp,cpp}`](provider_registry.hpp), which also defines
  the public free function `microtel::GetProvider`. It is a fixed array of
  `kMaxProfiles = 8` atomic slots, claimed by `SdkBuilder::Build` after
  construction and released at the top of `~SdkProvider`. Every operation is
  lock-free and allocation-free, because the `pthread_atfork` child handler is
  registered here and walks the array, calling
  `SdkProvider::MarkForkedChild` on each live provider
  ([ICP 0027](../../docs/icps/0027-multi-profile-threading.md)).
- `internal::IDiagnosticsSink` as `DiagnosticsCounters`
  ([`diagnostics_counters.hpp`](diagnostics_counters.hpp)): atomic per-reason
  counters plus a mutex-guarded last-error slot, and the `HealthSnapshot`
  behind `GetExporterHealth()`.

Resource:

- `microtel::Resource` and the resource-merging pipeline (`docs/configuration.md` §3.2).
- `internal::IResourceDetector`: the `process` and `host` detectors in
  [`resource_detectors.cpp`](resource_detectors.cpp), behind the public
  `MakeProcessDetector` / `MakeHostDetector` factories. The composition
  (`docs/configuration.md` §3.2) that merges them with the config lives in
  [`resource_builder.cpp`](resource_builder.cpp), which also logs the
  resolved Resource once per `Build()` at `Info` (sorted, escaped, redacted, capped;
  issue #284). k8s and cloud detectors are future work.

Traces:

- `microtel::Tracer` and `microtel::Span` as `SdkTracer` and `SdkSpan`
  ([`sdk_tracer.cpp`](sdk_tracer.cpp), [`sdk_span.cpp`](sdk_span.cpp)).
  They reach the sampler, span processor and diagnostics sink through raw
  pointers kept alive by the provider's shared
  [`TracePipeline`](trace_pipeline.hpp), so a tracer and its spans may
  outlive the provider and drop as `PostShutdown` (issue #285).
- The unsampled-`Span` no-op singleton and the out-of-line
  `internal::SpanDeleter::operator()` in [`noop_span.cpp`](noop_span.cpp), per
  [ICP 0003 §3.2](../../docs/icps/0003-m0-deferred-decisions.md#32-unsampled-span-shape--unique_ptrspan-to-no-op-singleton).
- `internal::ISampler` realisations: `AlwaysOnSampler`, `AlwaysOffSampler`,
  `TraceIdRatioSampler` and `ParentBasedSampler` (all in
  [`sampler_factories.cpp`](sampler_factories.cpp)), plus the v1.1 rule
  combinators and chain composition modes `AttributeRuleSampler`,
  `SpanNameRuleSampler`, `SpanKindRuleSampler` and `ChainSampler` (in
  [`sampler_chains.cpp`](sampler_chains.cpp)). They all use the
  `DescriptionSlot` in [`sampler_description.hpp`](sampler_description.hpp)
  for the one thing `ISampler::TrySetRatio` makes hard: publishing a new
  `Description()` without invalidating a `string_view` already handed out
  ([ICP 0026](../../docs/icps/0026-provider-setters.md) §5).
- `internal::ISpanProcessor` realisations: `BatchSpanProcessor` and
  `SimpleSpanProcessor`.
- `internal::ICurrentSpanSource` as `CurrentSpanSource` in
  [`current_span_source.hpp`](current_span_source.hpp). It reads the API's
  thread-local context slot and applies the `trace_based` filter (valid and
  sampled, otherwise an invalid context). The `MetricProducer` owns one, and
  `SdkProvider::GetMeter` lends it to each `SdkMeter` (as
  `StorageOptions::span_source`, which turns on exemplar capture) only when
  the test-only `SdkProviderArgs::exemplar_capture_for_testing` is set:
  capture is off in v1.3 ([ICP 0043](../../docs/icps/0043-exemplar-capture-off-in-v1.3.md)); the provider's `LogPipeline` owns another for
  every `SdkLogger` (log/trace correlation). See issue #221 and
  [ICP 0025](../../docs/icps/0025-propagation-core.md) §3.

Metrics ([`docs/metrics-design.md`](../../docs/metrics-design.md)):

- `SdkMeter` and the instrument types (`meter_instruments.hpp`,
  `metric_observable_instruments.hpp`).
- Per-aggregation storage: `SumStorage`, `GaugeStorage`, `HistogramStorage`,
  `ExponentialHistogramStorage`, keyed by `AttributeSet`. Each composes a
  [`SeriesTable<Point>`](metric_series_table.hpp), which owns the mutex, the
  series map, the cardinality limit and its overflow series; a series'
  exemplar lives in its `Point`.
- `MetricProducer`, `ViewRegistry`, and the two readers,
  `PeriodicExportingMetricReader` and `SynchronousMetricReader`.

Concentrator ([`docs/leaf-concentrator-design.md`](../../docs/leaf-concentrator-design.md)
§3–§5, [ICP 0034](../../docs/icps/0034-leaf-receiver-api.md); experimental,
compiled only with `MICROTEL_WITH_CONCENTRATOR=ON`):

- `microtel::LeafReceiver` as `SdkLeafReceiver`
  ([`leaf_receiver.{hpp,cpp}`](leaf_receiver.hpp)): size limits, decode
  through `internal::IOtlpTraceDecoder`, all-or-nothing validation, leaf
  identity, the leaf's settings (static `leaves` plus the resolver's answer),
  time correction, root sampling and `BatchSpanProcessor::Enqueue`. The
  reserved wire attributes and the Resource merge are in
  [`leaf_resource.{hpp,cpp}`](leaf_resource.hpp); the bounded leaf table —
  settings, Resource, boot anchor and last sighting per leaf, with LRU and
  idle-timeout eviction on insert — and, apart from it, the bounded, expiring
  cache of leaves a rejecting receiver refuses (`UnknownLeafCache`, #343) in
  [`leaf_table.{hpp,cpp}`](leaf_table.hpp);
  the time modes' arithmetic and the boot-relative anchor in
  [`leaf_time.{hpp,cpp}`](leaf_time.hpp); and `Build()`'s checks of
  `LeafReceiverOptions` in [`leaf_options.{hpp,cpp}`](leaf_options.hpp). The
  `[concentrator]` TOML table and variables are parsed in `src/common/config/`.
  `SdkProvider` builds it at construction and stops it in `Shutdown`.
- [`noop_leaf_receiver.hpp`](noop_leaf_receiver.hpp): what `GetLeafReceiver`
  returns when the concentrator is off or not compiled in. Always built.
- [`span_limits.{hpp,cpp}`](span_limits.hpp): the span-limit helpers `SdkSpan`
  and the leaf receiver share (`TruncateStrings`, `ApplySpanLimits`,
  `AppendAttributes`).

Logs ([`docs/logs-design.md`](../../docs/logs-design.md)):

- `SdkLogger` and `NoopLogger`. `SdkLogger` reaches the log processor,
  current-span source and diagnostics sink through raw pointers kept alive by
  the provider's shared [`LogPipeline`](log_pipeline.hpp), so a logger may
  outlive the provider and drop as `PostShutdown` (issue #417).
- [`noop_meter.hpp`](noop_meter.hpp): `NoopMeter`, what `GetMeter` returns when
  an application `ExportTransport` has metrics off (ICP 0036 Decision 3); the
  metric-side twin of `NoopLogger`.
- `internal::ILogRecordProcessor` realisations: `BatchLogRecordProcessor` and
  `SimpleLogRecordProcessor`.

## Dependencies and test doubles

- `IExporter`, `IMetricExporter`, `ILogExporter`: mocks
  `mock_exporter.hpp`, `mock_metric_exporter.hpp`, `mock_log_exporter.hpp` in
  [`tests/mocks/`](../../tests/mocks/); fakes `fake_exporter.hpp` and
  `fake_log_exporter.hpp` in [`tests/fakes/`](../../tests/fakes/).
- `ISpanProcessor` and `ISampler`: `mock_span_processor.hpp`,
  `mock_sampler.hpp`, and `fake_span_processor.hpp`.
- `IDiagnosticsSink`: `fake_diagnostics_sink.hpp`.
- `IOtlpTraceDecoder` (the leaf receiver): `mock_otlp_trace_decoder.hpp` and
  `fake_otlp_trace_decoder.hpp`.
- `IResourceDetector`: `fake_resource_detector.hpp`.
- The `Config` value from [`src/common/config/`](../common/config/), built
  directly in tests; there is no interface.
- `SdkBuilder` pulls in the exporter, encoder, wire codec and transport
  directories to assemble the real pipeline.

The span queue in `BatchSpanProcessor` is a `std::deque` guarded by one mutex.
[ICP 0003 §3.3](../../docs/icps/0003-m0-deferred-decisions.md#33-mpsc-queue-shape--stays-deferred)
left the structure as an M3 implementation choice.

## Tests

- `tests/unit/sdk/`: one file per type. The samplers, processors, metric
  storages and readers, logger, registry and builder each have their own.
- `tests/unit/sdk/provider_registry_test.cpp` and
  `provider_registry_race_test.cpp` cover the registry below the line and
  `WithProfileName` / `GetProvider` above it, plus the TSAN hammer.
  `tests/unit/sdk/fork_safety_test.cpp` covers the child handler, including
  the multi-provider case the old single slot could not serve.
- `tests/unit/sdk/hot_reload_hammer_test.cpp` and `provider_setters_test.cpp`
  cover the setters; `tests/fuzz/provider_setters_fuzz.cpp` fuzzes their
  validation.
- `tests/integration/sdk/exemplar_wiring_test.cpp`: the two
  `ICurrentSpanSource` seams driven through a real `SdkProvider`.
- `tests/integration/sdk/`: `retry_budget_test.cpp`,
  `auth_failure_export_test.cpp` and `exporter_health_test.cpp` run the SDK
  end-to-end against fakes.
- `tests/conformance/`: against a real OpenTelemetry Collector (shared
  with `src/exporter/`).
- The concentrator: `tests/unit/sdk/leaf_receiver_test.cpp`,
  `leaf_table_test.cpp`, `leaf_time_test.cpp` and
  `leaf_receiver_builder_test.cpp`;
  `tests/integration/sdk/leaf_ingest_test.cpp` end to end through a recording
  codec; `tests/fuzz/leaf_ingest_fuzz.cpp`.

## Style notes

The public hot-path contract for `Tracer` and `Span` (`noexcept`, zero
allocation on the unsampled path, no I/O on the caller thread) is written down
in [`src/api/README.md`](../api/README.md); `SdkTracer`, `SdkSpan` and the
no-op span here are what have to honour it.

- **Worker thread is owned here** (`BatchSpanProcessor` per
  `docs/threading-model.md` §2.2). `Shutdown` joins it, and the destructor
  calls `Shutdown` with a small finite timeout if that has not happened yet.
- **`OnEnd` is `noexcept`**. It drops the record on a full queue and never
  throws.
- **Sampler hot path must not allocate** in the default case (LOCKED —
  `memory-model.md` §8.1, ICP 0003 §3.2). The chain combinators keep that
  promise by fixing everything at construction: the child vector is sized
  once, each child's dynamic type is resolved once, and the description is
  formatted once. `tests/unit/sdk/sampler_chain_alloc_test.cpp` counts
  allocations around `ShouldSample` to keep it that way. `TrySetRatio` keeps
  the same promise from the other side. The ratio sampler's decision is one
  relaxed atomic load, with the always-sample case folded into the threshold
  sentinel instead of a second, separately readable field.
- **A setter takes at most one non-leaf lock, and never across a call-out**
  (`docs/threading-model.md` §4 rule 2). That is why `SetBatchOptions` runs in
  two phases rather than one nested one, and why a composite sampler
  recomposes its description after forwarding to its delegates. The TSAN
  hammer in `tests/unit/sdk/hot_reload_hammer_test.cpp` checks it;
  `MICROTEL_HAMMER_SECONDS` extends its default budget for a local run.
- **A meter or instrument may outlive its provider.** The storage it writes
  into, the diagnostics sink and the `ICurrentSpanSource` all hang off the
  [`MetricProducer`](metric_producer.hpp), which the provider shares with
  every `SdkMeter` and synchronous instrument; the sink is held through the
  provider's `TracePipeline`, as for tracers. Measurements made after the
  provider is gone are kept but never collected (issue #259).
- **A logger may outlive its provider.** The log processor, the diagnostics
  sink and the `ICurrentSpanSource` hang off the
  [`LogPipeline`](log_pipeline.hpp), which the provider shares with every
  `SdkLogger`; the sink is held through the provider's `TracePipeline`.
  `~SdkProvider` shuts the processor down and joins its worker before the log
  exporter goes, so records emitted after the provider is gone drop as
  `PostShutdown` (issue #417).
- **Provider holds a `unique_ptr<SslCtx>` indirectly via `Transport`**
  (ICP 0003 §3.1), so there is no shared ownership of TLS state. There is one
  per transport, which means a process running several named profiles has one
  per profile.
- **The registry must stay lock-free**, everywhere, because the fork child
  handler walks it. A mutex held at `fork()` time by a thread that does not
  exist in the child would hang the child's re-`Build()`, the one recovery
  `docs/threading-model.md` §7 supports. A slot holds a bare `SdkProvider*` and
  nothing else. The profile name lives in the provider, so the handler never
  touches a `std::string` and the registry never allocates.
  `tests/unit/sdk/provider_registry_race_test.cpp` is the TSAN hammer that
  checks it.
