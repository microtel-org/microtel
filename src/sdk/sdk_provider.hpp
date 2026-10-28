// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "microtel/internal/auth_provider.hpp"
#include "microtel/internal/diagnostics_sink.hpp"
#include "microtel/internal/exporter.hpp"
#include "microtel/internal/log_exporter.hpp"
#include "microtel/internal/log_record_processor.hpp"
#include "microtel/internal/metric_exporter.hpp"
#include "microtel/internal/otlp_encoder.hpp"
#include "microtel/internal/otlp_trace_decoder.hpp"
#include "microtel/internal/processor.hpp"
#include "microtel/internal/transport.hpp"
#include "microtel/internal/wire_codec.hpp"
#include "microtel/leaf_receiver.hpp"
#include "microtel/provider.hpp"
#include "microtel/resource.hpp"
#include "microtel/sampler.hpp"
#include "microtel/sdk_builder.hpp"

#include "sdk/diagnostics_counters.hpp"
#include "sdk/log_pipeline.hpp"
#include "sdk/metric_attribute_set.hpp"
#include "sdk/trace_pipeline.hpp"
#include "sdk/view_registry.hpp"
#include "wire/custom/export_transport_codec.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace microtel::sdk
{

// Forward-declared to keep implementation headers out of this header's transitive closure.
class SdkMeter;
class MetricProducer;
class PeriodicExportingMetricReader;
class BatchSpanProcessor;
class BatchLogRecordProcessor;
class SdkLeafReceiver;

/// @brief All owned objects passed to SdkProvider at construction.
///
/// Bundles the ten pipeline components so the constructor stays within the
/// seven-parameter tidy threshold. Declaration order matches the member
/// declaration order in SdkProvider (reverse-destruction semantics).
struct SdkProviderArgs
{
    /// @brief Diagnostics sink backing `GetExporterHealth()`. Created by
    /// `SdkBuilder::Build` before the exporters, which borrow it. Must be
    /// non-null.
    std::unique_ptr<DiagnosticsCounters> diagnostics;
    std::unique_ptr<internal::IOtlpEncoder> encoder;
    std::unique_ptr<internal::IAuthProvider> auth;
    /// @brief The HTTP/2 transport, or null with an application
    ///        `ExportTransport` (ICP 0036), when `export_channel` is set instead.
    std::unique_ptr<internal::ITransport> transport;
    /// @brief The application `ExportTransport`'s channel, or null for the
    ///        HTTP/2 transport. Exactly one of `transport` and this is set.
    std::unique_ptr<wire::ExportTransportChannel> export_channel;
    std::unique_ptr<internal::IWireCodec> codec;
    std::unique_ptr<internal::IExporter> exporter;
    /// @brief Borrowed, non-owning pointer to `processor` when it is a
    /// `BatchSpanProcessor`; null when the span pipeline does not batch.
    ///
    /// Set where the owning `unique_ptr` is assigned — `SdkBuilder::Build`,
    /// whose `BuildSpanProcessor` already returns the concrete type and erases
    /// it only at this boundary. `SetBatchOptions` needs the concrete type
    /// because batching knobs belong to the processors that batch, not to the
    /// `ISpanProcessor` contract (ICP 0026 §4). A setter that instead assumed
    /// `processor`'s dynamic type would be a latent trap.
    ///
    /// @note Declared **before** `processor` on purpose: designated
    ///       initializers must follow declaration order, and this lets a call
    ///       site write `.batch_span_processor = p.get(), .processor =
    ///       std::move(p)` without a second local to survive the move.
    BatchSpanProcessor* batch_span_processor{nullptr};
    /// @brief The span processor. It may outlive `exporter` — a tracer that
    ///        outlives the provider keeps it alive (issue #285) — so once
    ///        `Shutdown` has returned it must drop in `OnEnd` rather than
    ///        reach the exporter.
    std::unique_ptr<internal::ISpanProcessor> processor;
    std::shared_ptr<const Resource> resource;
    SamplerHandle sampler;
    SpanLimitOptions span_limits;
    internal::ConnectOptions connect_opts;
    /// @brief Optional wire codec for the metrics endpoint. Must outlive
    /// `metric_exporter` (i.e. declared before it in this struct so the
    /// SdkProvider member initializer list initializes it first).
    std::unique_ptr<internal::IWireCodec> metric_codec;
    /// @brief Optional metrics export pipeline. When non-null, a
    /// `PeriodicExportingMetricReader` is created on the first `GetMeter()` call.
    std::unique_ptr<internal::IMetricExporter> metric_exporter;
    /// @brief Background export interval for the metrics reader (default 30 s).
    std::chrono::milliseconds metric_interval{30'000};
    /// @brief Aggregation temporality preference for the metrics reader.
    microtel::TemporalityPreference metric_temporality{microtel::TemporalityPreference::Cumulative};
    /// @brief Per-instrument cardinality cap forwarded to every SdkMeter.
    std::size_t metric_max_cardinality{kDefaultMaxCardinality};
    /// @brief View configurations registered via `SdkBuilder::WithView()`.
    ViewRegistry view_registry;
    /// @brief Optional wire codec for the logs endpoint. Must outlive
    /// `log_exporter`.
    std::unique_ptr<internal::IWireCodec> log_codec;
    /// @brief Optional logs export pipeline. When non-null, the first
    /// `GetLogger()` builds a `BatchLogRecordProcessor` around it; when null,
    /// `GetLogger()` returns a no-op logger.
    std::unique_ptr<internal::ILogExporter> log_exporter;
    /// @brief Batch options for the log record processor (default BSP knobs).
    BatchOptions log_batch_opts;
    /// @brief The profile this provider is registered under (ICP 0027).
    ///
    /// Set by `SdkBuilder::Build` from `WithProfileName`; defaults to
    /// `microtel::kDefaultProfileName`, which is what every v1.0 program gets.
    std::string profile_name{kDefaultProfileName};
    /// @brief The leaf receiver's options (ICP 0034), or unset for none.
    ///
    /// The provider builds a live `SdkLeafReceiver` only when these are set
    /// with `enabled`, `leaf_decoder` is non-null, and the library was built
    /// with `MICROTEL_WITH_CONCENTRATOR`; otherwise `GetLeafReceiver` returns
    /// the no-op receiver. `SdkBuilder::Build` validates them first.
    std::optional<LeafReceiverOptions> leaf_receiver;
    /// @brief The OTLP decoder the leaf receiver owns. Ignored without
    /// `leaf_receiver`.
    std::unique_ptr<internal::IOtlpTraceDecoder> leaf_decoder;
};

/// @brief Production `Provider` wiring the full export pipeline.
///
/// Owns the pipeline end-to-end: encoder → transport → codec → exporter →
/// processor. `~SdkProvider` shuts the pipeline down and joins the span
/// processor's worker; members are then declared so that reverse-destruction
/// tears down the exporter, then the codec and transport, preserving the
/// happens-before chain required by TSAN and the threading model
/// (interfaces.md §6). The span processor, sampler and diagnostics sink are
/// shared with the tracers this provider hands out (`TracePipeline`), so they
/// go last — here, or later with the last tracer or span still holding them.
///
/// @threadsafety Thread-safe. All methods may be called from any thread.
class SdkProvider final : public microtel::Provider
{
public:
    explicit SdkProvider(SdkProviderArgs args) noexcept;

    ~SdkProvider() noexcept override;

    SdkProvider(const SdkProvider&) = delete;
    SdkProvider& operator=(const SdkProvider&) = delete;
    SdkProvider(SdkProvider&&) = delete;
    SdkProvider& operator=(SdkProvider&&) = delete;

    [[nodiscard]] std::shared_ptr<Tracer> GetTracer(std::string_view name,
                                                    std::string_view version = {}) override;

    [[nodiscard]] Expected<void, Error> Connect() override;

    [[nodiscard]] Status ForceFlush(std::chrono::milliseconds timeout) noexcept override;

    [[nodiscard]] Status Shutdown(std::chrono::milliseconds timeout) noexcept override;

    [[nodiscard]] HealthSnapshot GetExporterHealth() const noexcept override;

    /// @brief Acquire (or create) the `Meter` for one instrumentation scope.
    ///
    /// Lazily initialises the shared `MetricProducer` on the first call.
    /// Subsequent calls with the same `(name, version)` return the cached
    /// instance. `schema_url` is stored in the scope but does not affect
    /// caching in v1 (deferred to M12-hardening).
    [[nodiscard]] std::shared_ptr<microtel::Meter> GetMeter(
        std::string_view name,
        std::string_view version = {},
        std::string_view schema_url = {}) override;

    /// @brief Acquire (or create) the `Logger` for one instrumentation scope.
    ///
    /// Lazily builds the log pipeline (a `BatchLogRecordProcessor` around the
    /// configured log exporter) on the first call, and caches an `SdkLogger`
    /// per `(name, version)`. Returns a shared no-op logger when no log
    /// exporter is configured. Every `SdkLogger` shares the `LogPipeline`, so
    /// it may outlive this provider; its records then drop as `PostShutdown`
    /// (issue #417).
    [[nodiscard]] std::shared_ptr<microtel::Logger> GetLogger(
        std::string_view name, std::string_view version = {}) override;

    /// @brief The receiver built at construction: live when the concentrator
    ///        is compiled in and configured, the no-op otherwise (ICP 0034).
    ///        The same object on every call; `Shutdown` stops it.
    [[nodiscard]] std::shared_ptr<microtel::LeafReceiver> GetLeafReceiver() override;

    // ── Hot reload (ICP 0026) ──────────────────────────────────────────────
    // Contracts are on `microtel::Provider`; the notes here are about where
    // each one writes and under which lock.

    /// Two phases, never nested: retune the span processor; then, under
    /// `m_logger_mu`, store `m_log_batch_opts` (the seed a later `GetLogger`
    /// builds from) and read the borrowed log-processor pointer; release;
    /// then retune the log processor. Taking `m_logger_mu` and then the
    /// processor's own lock would nest two non-leaf locks, which
    /// `docs/threading-model.md` §4 rule 2 forbids — which is why the change
    /// is not atomic across the two pipelines.
    [[nodiscard]] Status SetBatchOptions(const BatchOptions& opts) noexcept override;

    /// Writes `m_metric_interval` under `m_meter_mu` and reads the borrowed
    /// reader pointer there, then calls `SetInterval` after releasing — the
    /// shape `MetricReaderPtr` exists for.
    [[nodiscard]] Status SetMetricInterval(std::chrono::milliseconds interval) noexcept override;

    /// No provider lock at all. `m_trace->sampler` is assigned once at
    /// construction and never reassigned, so the raw `ISampler*` every
    /// `SdkTracer` caches keeps pointing at the same live object; only its
    /// ratio moves.
    [[nodiscard]] Status SetSamplerRatio(double ratio) noexcept override;

    /// A thin forwarder to `internal::SetMinLogLevel`, so the operator surface
    /// is uniform across the four knobs even though this one is process-wide.
    [[nodiscard]] Status SetLogLevel(LogLevel level) noexcept override;

    /// @brief The profile name this provider is registered under.
    ///
    /// Immutable from construction until destruction, which is what lets the
    /// registry hold nothing but a pointer: the name is published to other
    /// threads by the release-store that puts this provider in a slot, and it
    /// outlives that slot's occupancy (`src/sdk/provider_registry.hpp`).
    ///
    /// @return a borrowed view of the provider's own storage, valid for the
    ///         provider's lifetime.
    [[nodiscard]] std::string_view ProfileName() const noexcept
    {
        return m_profile_name;
    }

    /// @brief Borrow the provider-owned diagnostics sink.
    ///
    /// Non-owning reference, valid for the provider's lifetime. The seam
    /// through which pipeline components record drops (wired in increment 26)
    /// and tests assert on health counters.
    [[nodiscard]] internal::IDiagnosticsSink& DiagnosticsSink() noexcept;

    /// @brief Mark this provider dead after `fork()`, from the child.
    ///
    /// Called by the `pthread_atfork` child handler. Only the forking thread
    /// survives `fork`, so every worker and I/O thread this provider owns is
    /// gone in the child while their mutexes may still be held by threads that
    /// no longer exist. Flipping the shutdown flag makes the API entry points
    /// that check it drop instead of touching that state
    /// (`docs/threading-model.md` §7).
    ///
    /// @note Runs in the child of `fork()`, where only async-signal-safe
    ///       operations are permitted: this does nothing but a relaxed-release
    ///       store to an atomic. It takes no lock and allocates nothing.
    void MarkForkedChild() noexcept;

private:
    /// @brief Borrowed pointer to the lazily-built metric reader, read under
    ///        `m_meter_mu`. Returns nullptr when no meter has been created.
    /// @note Exists so `ForceFlush`/`Shutdown` can observe the pointer without
    ///       holding the mutex across the call into the reader — taking
    ///       `m_meter_mu` and then the reader's own lock would nest two
    ///       non-leaf locks, which `docs/threading-model.md` §4 forbids.
    [[nodiscard]] PeriodicExportingMetricReader* MetricReaderPtr() noexcept;
    /// @brief Borrowed pointer to the lazily-built log processor, read under
    ///        `m_logger_mu`. Same rationale as `MetricReaderPtr`.
    [[nodiscard]] internal::ILogRecordProcessor* LogProcessorPtr() noexcept;
    /// @brief Store the batch seed and read back the concrete log processor,
    ///        both under one hold of `m_logger_mu`.
    ///
    /// Phase 2 of `SetBatchOptions`. One critical section rather than two so a
    /// concurrent `GetLogger` cannot build a processor from the *old* seed
    /// after this call has decided there was none to retune.
    ///
    /// @param opts the new seed, stored into `m_log_batch_opts`.
    /// @return borrowed, or nullptr when no logger has been created.
    [[nodiscard]] BatchLogRecordProcessor* SeedAndBorrowLogProcessor(
        const BatchOptions& opts) noexcept;
    /// @brief Drive every pipeline component's `ForceFlush` against one
    ///        shared deadline, each whatever the earlier ones returned, and
    ///        fold the worst status (ICP 0041). Split out of `ForceFlush` so
    ///        the timeout counter is recorded once for the whole call rather
    ///        than once per arm.
    [[nodiscard]] Status FlushPipeline(std::chrono::milliseconds timeout) noexcept;
    /// @brief Make the live leaf receiver refuse every later payload. A single
    ///        atomic store, so the fork child handler may call it too.
    void StopLeafReceiver() noexcept;
    /// @brief One Warn, the first time a signal an application
    ///        `ExportTransport` left off is asked for (ICP 0036 Decision 3).
    static void WarnSignalOff(std::atomic<bool>& warned, std::string_view message) noexcept;

public:
private:
    // The diagnostics sink, the sampler and the span processor, shared with
    // every SdkTracer and sampled SdkSpan this provider hands out so that a
    // tracer outliving the provider still has what it dereferences (issue
    // #285, `sdk/trace_pipeline.hpp`).
    //
    // shared_ptr, not unique_ptr: ownership is genuinely joint — the
    // `Provider::GetTracer` contract is that the tracer stays valid until both
    // it and the provider are gone, and either may go first. The refcount is
    // touched once per GetTracer and once per sampled span, where the span's
    // former Resource reference used to be, so the hot path pays no extra
    // atomic operation.
    //
    // Declared before every other member → destroyed last (when this is the
    // last reference), so the sink stays alive past any late RecordDrop from
    // the metric machinery (or any other pipeline component) during
    // reverse-order teardown — same reasoning as the m_metric_codec ordering
    // note below. The span processor is released here too, after the trace
    // exporter it borrows; ~SdkProvider joins its worker first, so it no
    // longer reaches the exporter by then.
    //
    // The sink is heap-allocated rather than held by value because the
    // eagerly-built exporters need a pointer to it at *their* construction
    // time, which is before this Provider exists. SdkBuilder::Build creates it,
    // hands the raw pointer to the exporters, and moves ownership in here.
    std::shared_ptr<TracePipeline> m_trace;
    /// Set by `Shutdown` before it tears anything down, so `GetMeter` and
    /// `GetLogger` stop building pipeline components afterwards. Without it
    /// either could construct a `PeriodicExportingMetricReader` or a
    /// `BatchLogRecordProcessor` — and spawn its thread — *after* `Shutdown`
    /// returned, contradicting `docs/threading-model.md` §6.2 and leaving the
    /// new thread joined only at destruction.
    ///
    /// Atomic because `Provider` is documented thread-safe: the check races
    /// a concurrent `Shutdown` on another thread. It narrows the window to
    /// the same one every other component here has (see `OtlpExporter::
    /// m_shutdown`); it does not eliminate it.
    std::atomic<bool> m_shut_down{false};
    // Encoder is stateless; no teardown order concern.
    std::unique_ptr<internal::IOtlpEncoder> m_encoder;
    std::unique_ptr<internal::IAuthProvider> m_auth;
    // Transport owns the I/O thread; must outlive all codecs and exporters.
    // Null with an application ExportTransport (ICP 0036).
    std::unique_ptr<internal::ITransport> m_transport;
    // The application ExportTransport, when there is one. It owns that
    // transport, which the codecs reach on the exporter workers, so it is
    // declared before them and destroyed after every worker is joined
    // (ICP 0036 Decision 2). Null for HTTP/2.
    std::unique_ptr<wire::ExportTransportChannel> m_export_channel;
    std::unique_ptr<internal::IWireCodec> m_codec;
    // Metric codec must outlive m_metric_exporter (which holds a raw pointer to it).
    std::unique_ptr<internal::IWireCodec> m_metric_codec;
    // Log codec must outlive m_log_exporter (which holds a raw pointer to it).
    std::unique_ptr<internal::IWireCodec> m_log_codec;
    // Trace exporter thread; must outlive codec and transport.
    std::unique_ptr<internal::IExporter> m_exporter;
    // Metric exporter thread; must outlive m_metric_reader.
    std::unique_ptr<internal::IMetricExporter> m_metric_exporter;
    // Log exporter thread. The log processor that borrows it may outlive it
    // (LogPipeline), so ~SdkProvider joins that processor's worker first.
    std::unique_ptr<internal::ILogExporter> m_log_exporter;
    // Guarded by m_meter_mu: the seed a later GetMeter builds the reader from,
    // and the value SetMetricInterval writes (ICP 0026 §4).
    std::chrono::milliseconds m_metric_interval;
    microtel::TemporalityPreference m_metric_temporality;
    std::size_t m_metric_max_cardinality;
    // Guarded by m_logger_mu: the seed a later GetLogger builds the processor
    // from, and the value SetBatchOptions writes.
    BatchOptions m_log_batch_opts;
    // Borrowed alias of m_trace->processor when it batches, else null.
    // Assigned once at construction and never reassigned, so it needs no lock
    // — the same reasoning m_trace->sampler rests on.
    BatchSpanProcessor* m_batch_span_processor;
    // Metric reader thread — declared last → destroyed first (before metric exporter).
    std::unique_ptr<PeriodicExportingMetricReader> m_metric_reader;
    // The log processor (its thread), span source and sink every SdkLogger
    // borrows — lazily created by the first GetLogger. Shared with each
    // logger, so a logger may outlive this provider (issue #417); the
    // aliasing pointer to the sink inside shares m_trace's ownership, so the
    // refcount is touched once per logger created, never per Emit. Released
    // before m_log_exporter; ~SdkProvider has shut the processor down and
    // joined its worker by then, so a late Emit never reaches the exporter.
    std::shared_ptr<LogPipeline> m_log_pipeline;
    // Borrowed alias of m_log_pipeline->processor, published by GetLogger under
    // m_logger_mu where the owning unique_ptr is assigned, and read under the
    // same lock. Unlike the span-side alias this one is written after
    // construction, so it is not lock-free.
    BatchLogRecordProcessor* m_log_batch_processor{nullptr};

    std::shared_ptr<const Resource> m_resource;
    SpanLimitOptions m_span_limits;
    internal::ConnectOptions m_connect_opts;

    std::shared_ptr<ViewRegistry> m_view_registry;

    // Metrics pipeline: lazily initialised on first GetMeter() call.
    std::mutex m_meter_mu;
    std::shared_ptr<MetricProducer> m_metric_producer;
    std::unordered_map<std::string, std::shared_ptr<SdkMeter>> m_meters;
    // What GetMeter returns when an application ExportTransport has metrics
    // off (ICP 0036 Decision 3). Built on first need, under m_meter_mu.
    std::shared_ptr<microtel::Meter> m_noop_meter;
    // One Warn each, the first time a switched-off signal is asked for.
    std::atomic<bool> m_warned_metrics_off{false};
    std::atomic<bool> m_warned_logs_off{false};

    // Logs pipeline: m_log_pipeline is lazily initialised on first GetLogger().
    std::mutex m_logger_mu;
    std::unordered_map<std::string, std::shared_ptr<microtel::Logger>> m_loggers;
    std::shared_ptr<microtel::Logger> m_noop_logger;

    // The leaf receiver (ICP 0034), built once at construction and never
    // reassigned, so it needs no lock. Shared with the application, which may
    // keep it past this provider; it holds m_trace, as a tracer does, so what
    // it borrows outlives it. m_sdk_leaf_receiver is a borrowed alias when it
    // is the live receiver, so Shutdown and the fork handler can stop it; null
    // for the no-op, and unused in a build without the concentrator.
    std::shared_ptr<microtel::LeafReceiver> m_leaf_receiver;
    [[maybe_unused]] SdkLeafReceiver* m_sdk_leaf_receiver{nullptr};

    // The registry's key for this provider. Assigned once at construction and
    // never reassigned, so it needs no lock — and the registry, which reads it
    // from other threads, needs no allocation. Destruction order is not a
    // concern: `~SdkProvider` releases the slot before any member is destroyed,
    // so nothing can be scanning this string by the time it goes.
    std::string m_profile_name;
};

}  // namespace microtel::sdk
