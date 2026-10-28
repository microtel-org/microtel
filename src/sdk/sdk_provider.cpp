// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#include "sdk/sdk_provider.hpp"

#include "microtel/internal/batch.hpp"
#include "microtel/internal/metric_batch.hpp"
#include "microtel/internal/sampler.hpp"
#include "microtel/log_sink.hpp"
#include "microtel/provider.hpp"
#include "microtel/status.hpp"
#include "microtel/tracer.hpp"

#include "common/config/config_validator.hpp"
#include "common/internal_log.hpp"
#include "sdk/batch_log_record_processor.hpp"
#include "sdk/batch_span_processor.hpp"
#ifdef MICROTEL_WITH_CONCENTRATOR
#include "sdk/leaf_receiver.hpp"
#endif
#include "sdk/metric_producer.hpp"
#include "sdk/noop_leaf_receiver.hpp"
#include "sdk/noop_logger.hpp"
#include "sdk/noop_meter.hpp"
#include "sdk/periodic_exporting_metric_reader.hpp"
#include "sdk/provider_registry.hpp"
#include "sdk/sdk_logger.hpp"
#include "sdk/sdk_meter.hpp"
#include "sdk/sdk_tracer.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>

namespace microtel::sdk
{

namespace
{

constexpr auto kProviderDestructorTimeout = std::chrono::milliseconds(5000);

#ifdef MICROTEL_WITH_CONCENTRATOR
/// The live receiver, when the args ask for one and it can be built.
[[nodiscard]] std::shared_ptr<SdkLeafReceiver> MakeLeafReceiver(
    SdkProviderArgs& args, const std::shared_ptr<TracePipeline>& trace, BatchSpanProcessor* bsp)
{
    if (!args.leaf_receiver.has_value() || !args.leaf_receiver->enabled ||
        args.leaf_decoder == nullptr)
    {
        return nullptr;
    }
    return std::make_shared<SdkLeafReceiver>(std::move(*args.leaf_receiver),
                                             LeafReceiverDeps{
                                                 .owner = trace,
                                                 .sampler = trace->sampler.Get(),
                                                 .processor = trace->processor.get(),
                                                 .batch_processor = bsp,
                                                 .diagnostics = trace->diagnostics.get(),
                                                 .decoder = std::move(args.leaf_decoder),
                                                 .span_limits = args.span_limits,
                                             });
}
#endif

[[nodiscard]] internal::AggregationTemporality ToAggregationTemporality(
    microtel::TemporalityPreference pref) noexcept
{
    switch (pref)
    {
        case microtel::TemporalityPreference::Delta:
        case microtel::TemporalityPreference::LowMemory:
            // LowMemory uses delta for all instruments; per-kind mapping deferred to v1.3.
            return internal::AggregationTemporality::Delta;
        case microtel::TemporalityPreference::Cumulative:
        default:
            return internal::AggregationTemporality::Cumulative;
    }
}

}  // namespace

SdkProvider::SdkProvider(SdkProviderArgs args) noexcept
    : m_trace(std::make_shared<TracePipeline>(TracePipeline{
          .diagnostics = std::move(args.diagnostics),
          .sampler = std::move(args.sampler),
          .processor = std::move(args.processor),
      })),
      m_encoder(std::move(args.encoder)),
      m_auth(std::move(args.auth)),
      m_transport(std::move(args.transport)),
      m_export_channel(std::move(args.export_channel)),
      m_codec(std::move(args.codec)),
      m_metric_codec(std::move(args.metric_codec)),
      m_log_codec(std::move(args.log_codec)),
      m_exporter(std::move(args.exporter)),
      m_metric_exporter(std::move(args.metric_exporter)),
      m_log_exporter(std::move(args.log_exporter)),
      m_metric_interval(args.metric_interval),
      m_metric_temporality(args.metric_temporality),
      m_metric_max_cardinality(args.metric_max_cardinality),
      m_log_batch_opts(args.log_batch_opts),
      m_batch_span_processor(args.batch_span_processor),
      m_resource(std::move(args.resource)),
      m_span_limits(args.span_limits),
      m_connect_opts(std::move(args.connect_opts)),
      m_view_registry(std::make_shared<ViewRegistry>(std::move(args.view_registry))),
      m_noop_logger(std::make_shared<NoopLogger>()),
      m_profile_name(std::move(args.profile_name))
{
#ifdef MICROTEL_WITH_CONCENTRATOR
    if (auto live = MakeLeafReceiver(args, m_trace, m_batch_span_processor); live != nullptr)
    {
        m_sdk_leaf_receiver = live.get();
        m_leaf_receiver = std::move(live);
    }
#endif
    if (m_leaf_receiver == nullptr)
    {
        m_leaf_receiver = std::make_shared<NoopLeafReceiver>();
    }
    // Registration is `SdkBuilder::Build`'s, not this constructor's: it can
    // fail on a duplicate name or a full registry, and this constructor is
    // `noexcept` with no way to say so (ICP 0027 §2).
}

SdkProvider::~SdkProvider() noexcept
{
    // Before `Shutdown`, and before any member is destroyed. That ordering is
    // what makes the fork child handler safe: a slot is emptied before its
    // provider begins tearing down, so a handler running in the child sees
    // either a provider that is entirely intact or nothing at all. A provider
    // that never registered — a duplicate-name `Build`, or a direct
    // construction — passes through this as a no-op.
    DeregisterProvider(this);
    (void)Shutdown(kProviderDestructorTimeout);
    // The span processor is shared with the tracers (TracePipeline), so it may
    // be destroyed after the exporter below. Its worker is the one part of it
    // that can still be inside the exporter: Shutdown only waits for it up to
    // a timeout, and a caller's earlier Shutdown may have used a shorter one.
    if (m_batch_span_processor != nullptr)
    {
        m_batch_span_processor->JoinWorker();
    }
    // Likewise the log processor, shared with the loggers (LogPipeline,
    // issue #417). No lock: nothing else may touch a provider being destroyed.
    if (m_log_batch_processor != nullptr)
    {
        m_log_batch_processor->JoinWorker();
    }
}

void SdkProvider::MarkForkedChild() noexcept
{
    m_shut_down.store(true, std::memory_order_release);
    StopLeafReceiver();
}

void SdkProvider::StopLeafReceiver() noexcept
{
#ifdef MICROTEL_WITH_CONCENTRATOR
    if (m_sdk_leaf_receiver != nullptr)
    {
        m_sdk_leaf_receiver->MarkShutDown();
    }
#endif
}

void SdkProvider::WarnSignalOff(std::atomic<bool>& warned, std::string_view message) noexcept
{
    if (!warned.exchange(true, std::memory_order_relaxed))
    {
        internal::LogImpl(LogLevel::Warn, message);
    }
}

std::shared_ptr<microtel::LeafReceiver> SdkProvider::GetLeafReceiver()
{
    return m_leaf_receiver;
}

std::shared_ptr<Tracer> SdkProvider::GetTracer(std::string_view name, std::string_view version)
{
    // The tracer shares m_trace, so it — and every span it starts — keeps the
    // sampler, processor and sink alive past this provider (issue #285).
    return std::make_shared<SdkTracer>(
        m_trace->sampler.Get(),
        m_trace->processor.get(),
        m_trace,
        internal::InstrumentationScope{.name = std::string{name}, .version = std::string{version}},
        m_span_limits,
        m_trace->diagnostics.get());
}

Expected<void, Error> SdkProvider::Connect()
{
    if (m_transport == nullptr)
    {
        // An application ExportTransport: there is nothing to connect, and
        // the link is the application's (ICP 0036 Decision 1).
        return {};
    }
    auto result = m_transport->Connect(m_connect_opts);
    if (!result)
    {
        // The eager path. The codecs' lazy EnsureConnected covers the other
        // one; a given connect attempt runs through exactly one of the two,
        // so the counter never double-counts a single failure.
        m_trace->diagnostics->RecordDrop(DropReason::ConnectFailure);
    }
    return result;
}

namespace
{

/// @brief Combine two lifecycle statuses, worst-outcome-wins: `Shutdown`
///        and, since ICP 0041, `ForceFlush`.
///
/// `Provider::Shutdown` promises a structured status (CLAUDE.md rule 17), but
/// it drove six components and reported only the span processor's, discarding
/// the rest with `(void)`. A transport or exporter that timed out was
/// invisible to the caller — which made `Close`'s timeout unobservable even
/// once it was honoured.
[[nodiscard]] Status WorseOf(Status a, Status b) noexcept
{
    // Failed is the strongest signal, then TimedOut. AlreadyShutDown only
    // survives if nothing else had anything to report.
    if (a == Status::Failed || b == Status::Failed)
    {
        return Status::Failed;
    }
    if (a == Status::TimedOut || b == Status::TimedOut)
    {
        return Status::TimedOut;
    }
    if (a == Status::Completed || b == Status::Completed)
    {
        return Status::Completed;
    }
    return a;
}

/// @brief The time left until @p deadline, never negative.
///
/// `ForceFlush` and `Shutdown` hand each stage this rather than the caller's
/// whole timeout, so the call as a whole is bounded by that one timeout
/// (ICP 0041). A stage reached after the deadline gets 0 ms: it is still
/// signalled, just not waited for.
[[nodiscard]] std::chrono::milliseconds Remaining(
    std::chrono::steady_clock::time_point deadline) noexcept
{
    const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
        deadline - std::chrono::steady_clock::now());
    return std::max(left, std::chrono::milliseconds{0});
}

}  // namespace

Status SdkProvider::ForceFlush(std::chrono::milliseconds timeout) noexcept
{
    // Every stage runs now, so without this a flush after Shutdown would reach
    // exporters whose workers are joined and wait out the whole timeout.
    if (m_shut_down.load(std::memory_order_acquire))
    {
        return Status::AlreadyShutDown;
    }
    const Status status = FlushPipeline(timeout);
    if (status == Status::TimedOut)
    {
        // Recorded here and nowhere else: the processor and exporter arms
        // below can each time out, but the user made one ForceFlush call and
        // must see one drop.
        m_trace->diagnostics->RecordDrop(DropReason::ForceFlushTimeout);
    }
    return status;
}

Status SdkProvider::FlushPipeline(std::chrono::milliseconds timeout) noexcept
{
    // Two-stage flush per signal: drain the processor queue into the exporter
    // queue first, then drain the exporter queue (the actual sends). Every
    // stage runs whatever the ones before it returned, against one shared
    // deadline, and the statuses fold worst-first (ICP 0041, #486): a slow
    // trace stage must not stop the logs being flushed.
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    Status status = m_trace->processor->ForceFlush(Remaining(deadline));
    status = WorseOf(status, m_exporter->ForceFlush(Remaining(deadline)));
    // Metric reader ForceFlush: collect a snapshot then flush the exporter.
    // Read through the accessor: GetMeter publishes this pointer from another
    // thread under m_meter_mu, and Provider is documented thread-safe.
    if (auto* const reader = MetricReaderPtr(); reader != nullptr)
    {
        status = WorseOf(status, reader->ForceFlush(Remaining(deadline)));
    }
    // Log pipeline: drain the processor queue into the exporter, then flush it.
    if (auto* const processor = LogProcessorPtr(); processor != nullptr)
    {
        status = WorseOf(status, processor->ForceFlush(Remaining(deadline)));
    }
    if (m_log_exporter != nullptr)
    {
        status = WorseOf(status, m_log_exporter->ForceFlush(Remaining(deadline)));
    }
    return status;
}

PeriodicExportingMetricReader* SdkProvider::MetricReaderPtr() noexcept
{
    const std::scoped_lock lk{m_meter_mu};
    return m_metric_reader.get();
}

internal::ILogRecordProcessor* SdkProvider::LogProcessorPtr() noexcept
{
    const std::scoped_lock lk{m_logger_mu};
    return m_log_pipeline != nullptr ? m_log_pipeline->processor.get() : nullptr;
}

BatchLogRecordProcessor* SdkProvider::SeedAndBorrowLogProcessor(const BatchOptions& opts) noexcept
{
    const std::scoped_lock lk{m_logger_mu};
    m_log_batch_opts = opts;
    return m_log_batch_processor;
}

Status SdkProvider::Shutdown(std::chrono::milliseconds timeout) noexcept
{
    // Set before tearing anything down so a concurrent GetMeter/GetLogger
    // stops building pipeline components (and spawning their threads).
    m_shut_down.store(true, std::memory_order_release);
    // One deadline for the whole sequence: each component is handed the time
    // remaining, not the caller's whole timeout (ICP 0041).
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    if (m_export_channel != nullptr)
    {
        // Every Send from here on gets a deadline no later than this one
        // (ICP 0036 Decision 2).
        m_export_channel->BeginShutdown(timeout);
    }
    // Before the processor: a payload that arrives now is refused whole as
    // ShutDown, not half-enqueued into a processor that is draining.
    StopLeafReceiver();

    Status status = m_trace->processor->Shutdown(Remaining(deadline));
    // Every component below still runs even if an earlier one timed out: a
    // partial teardown would leak threads and sockets. Their statuses are
    // folded in rather than discarded.
    // Metric reader shutdown (also shuts down the metric exporter internally).
    if (auto* const reader = MetricReaderPtr(); reader != nullptr)
    {
        status = WorseOf(status, reader->Shutdown(Remaining(deadline)));
    }
    else if (m_metric_exporter != nullptr)
    {
        status = WorseOf(status, m_metric_exporter->Shutdown(Remaining(deadline)));
    }
    // Log pipeline: stop the processor (halts emits to the exporter), then the
    // exporter, before the shared transport is closed.
    if (auto* const processor = LogProcessorPtr(); processor != nullptr)
    {
        status = WorseOf(status, processor->Shutdown(Remaining(deadline)));
    }
    if (m_log_exporter != nullptr)
    {
        status = WorseOf(status, m_log_exporter->Shutdown(Remaining(deadline)));
    }
    status = WorseOf(status, m_exporter->Shutdown(Remaining(deadline)));
    if (m_transport != nullptr)
    {
        status = WorseOf(status, m_transport->Close(Remaining(deadline)));
    }
    if (m_export_channel != nullptr)
    {
        m_export_channel->MarkClosed();
    }
    if (status == Status::TimedOut)
    {
        // One user-visible Shutdown call, one drop — however many of the six
        // components ran out of time.
        m_trace->diagnostics->RecordDrop(DropReason::ShutdownTimeout);
    }
    return status;
}

HealthSnapshot SdkProvider::GetExporterHealth() const noexcept
{
    HealthSnapshot health = m_trace->diagnostics->Snapshot();
    // Connection state is read live from the transport; the sink's
    // SetConnectionState channel is wired up in increment 26. With an
    // application ExportTransport it is the channel's "sends are succeeding"
    // state (ICP 0036, docs/error-model.md §9.1).
    health.connection_state =
        m_transport != nullptr ? m_transport->GetState() : m_export_channel->State();
    return health;
}

internal::IDiagnosticsSink& SdkProvider::DiagnosticsSink() noexcept
{
    return *m_trace->diagnostics;
}

std::shared_ptr<microtel::Meter> SdkProvider::GetMeter(std::string_view name,
                                                       std::string_view version,
                                                       std::string_view /*schema_url*/)
{
    // Read before taking m_meter_mu, not after: in a forked child that mutex
    // may be held by a thread that no longer exists, so locking first would
    // deadlock before the flag was ever consulted. GetLogger already does this.
    const bool shut_down = m_shut_down.load(std::memory_order_acquire);
    if (m_export_channel != nullptr && m_metric_exporter == nullptr)
    {
        WarnSignalOff(m_warned_metrics_off,
                      "export transport: metrics are off in ExportTransportOptions; GetMeter "
                      "returns a no-op meter");
        const std::scoped_lock lk{m_meter_mu};
        if (!m_noop_meter)
        {
            m_noop_meter = std::make_shared<NoopMeter>();
        }
        return m_noop_meter;
    }
    const std::scoped_lock lk{m_meter_mu};
    if (!m_metric_producer)
    {
        // The aliasing constructor shares m_trace's ownership, so the sink
        // lives while any meter or instrument does (issue #259) — the same
        // way it does for tracers (issue #285).
        m_metric_producer = std::make_shared<MetricProducer>(
            m_resource,
            std::shared_ptr<internal::IDiagnosticsSink>{m_trace, m_trace->diagnostics.get()});
        // Same reasoning as GetLogger: no new reader thread after Shutdown.
        // The meter itself is still returned so callers do not have to
        // null-check, but nothing collects from it. There is no NoopMeter to
        // hand back instead -- see the PR note.
        if (m_metric_exporter != nullptr && !shut_down)
        {
            m_metric_reader = std::make_unique<PeriodicExportingMetricReader>(
                *m_metric_producer,
                *m_metric_exporter,
                m_metric_interval,
                ToAggregationTemporality(m_metric_temporality));
        }
    }
    std::string key;
    key.reserve(name.size() + 1 + version.size());
    key.append(name);
    key += '\0';
    key.append(version);
    auto& entry = m_meters[key];
    if (!entry)
    {
        entry = std::make_shared<SdkMeter>(
            internal::InstrumentationScope{.name = std::string{name},
                                           .version = std::string{version}},
            m_metric_producer,
            m_metric_max_cardinality,
            m_metric_producer->Diagnostics(),
            m_view_registry,
            m_metric_producer->SpanSource());
    }
    return entry;
}

std::shared_ptr<microtel::Logger> SdkProvider::GetLogger(std::string_view name,
                                                         std::string_view version)
{
    // After Shutdown the pipeline is gone; building a BatchLogRecordProcessor
    // here would spawn a worker thread that nothing joins until destruction,
    // and its records could never be exported anyway (threading-model.md §6.2).
    if (m_log_exporter == nullptr && m_export_channel != nullptr)
    {
        WarnSignalOff(m_warned_logs_off,
                      "export transport: logs are off in ExportTransportOptions; GetLogger "
                      "returns a no-op logger");
    }
    if (m_log_exporter == nullptr || m_shut_down.load(std::memory_order_acquire))
    {
        return m_noop_logger;
    }
    const std::scoped_lock lk{m_logger_mu};
    BatchLogRecordProcessor* batch_processor = m_log_batch_processor;
    if (!m_log_pipeline)
    {
        auto pipeline = std::make_shared<LogPipeline>();
        // The aliasing constructor shares m_trace's ownership, so the sink
        // lives while any logger does (issue #417) — as for tracers (#285).
        pipeline->diagnostics =
            std::shared_ptr<internal::IDiagnosticsSink>{m_trace, m_trace->diagnostics.get()};
        auto built = std::make_unique<BatchLogRecordProcessor>(
            m_log_exporter.get(), m_resource, m_log_batch_opts, pipeline->diagnostics.get());
        batch_processor = built.get();
        pipeline->processor = std::move(built);
        m_log_pipeline = std::move(pipeline);
    }
    std::string key;
    key.reserve(name.size() + 1 + version.size());
    key.append(name);
    key += '\0';
    key.append(version);
    // Published here, under m_logger_mu, where the owning unique_ptr is
    // assigned — the borrowed pointer SetBatchOptions retunes (ICP 0026 §4).
    m_log_batch_processor = batch_processor;
    auto& entry = m_loggers[key];
    if (!entry)
    {
        // The logger shares m_log_pipeline, so it keeps the processor, span
        // source and sink alive past this provider (issue #417).
        entry = std::make_shared<SdkLogger>(
            m_log_pipeline->processor.get(),
            internal::InstrumentationScope{.name = std::string{name},
                                           .version = std::string{version}},
            &m_log_pipeline->span_source,  // trace-correlation seam (ICP 0025 §3)
            m_log_pipeline->diagnostics.get(),
            LogLimitOptions{},
            m_log_pipeline);
    }
    return entry;
}

// ---------------------------------------------------------------------------
// Hot reload — ICP 0026
//
// Every one of the four reads m_shut_down before it takes any mutex, which is
// the pattern GetMeter and GetLogger already use: in a forked child a mutex
// may be held by a thread that no longer exists, so locking first would
// deadlock before the flag was ever consulted (docs/threading-model.md §7).
//
// Validation runs before the support check, so an operator's bad value is
// reported as InvalidArgument whatever pipelines this provider happens to own
// — a value that is wrong is wrong everywhere, and answering Unsupported for
// it would hide the typo behind a deployment detail.
// ---------------------------------------------------------------------------

namespace
{

/// The setters' rejection channel. `Status` carries which *kind* of rejection;
/// which field and what range go here, at Warn — which is also what makes
/// `SetLogLevel` self-consistent, a rejected setter call being an internal log
/// like any other.
void WarnRejected(std::string_view detail) noexcept
{
    internal::LogImpl(LogLevel::Warn, detail);
}

/// Room for the prefix plus the longest `config::BatchOptionsFault` message;
/// `snprintf` truncates rather than overflows if a message ever outgrows it.
constexpr std::size_t kBatchRejectionBufferSize = 128;

/// `WarnRejected` for a `BatchOptions` fault, composed on the stack so the
/// `noexcept` setter never allocates to report a rejection.
void WarnBatchRejected(const config::BatchOptionsFault& fault) noexcept
{
    std::array<char, kBatchRejectionBufferSize> buf{};
    const int written = std::snprintf(buf.data(),
                                      buf.size(),
                                      "SetBatchOptions rejected: %.*s",
                                      static_cast<int>(fault.message.size()),
                                      fault.message.data());
    const auto len = std::min(static_cast<std::size_t>(std::max(written, 0)), buf.size() - 1);
    WarnRejected(std::string_view{buf.data(), len});
}

}  // namespace

Status SdkProvider::SetBatchOptions(const BatchOptions& opts) noexcept
{
    if (m_shut_down.load(std::memory_order_acquire))
    {
        return Status::AlreadyShutDown;
    }
    // The rule set `SdkBuilder::Build` applies through `config::Validate`
    // (issue #267): a value is accepted here exactly when it would build.
    if (const auto fault = config::CheckBatchOptions(opts))
    {
        WarnBatchRejected(*fault);
        return Status::InvalidArgument;
    }
    if (m_batch_span_processor == nullptr)
    {
        // A provider whose span pipeline does not batch has no batching knobs
        // to retune, and half-applying the change to the log side would be
        // worse than declining it. Unreachable from SdkBuilder today, which
        // always builds a BatchSpanProcessor (ICP 0026 Discrepancy 4).
        return Status::Unsupported;
    }

    // Phase 1 — the span pipeline. No provider lock: the pointer is fixed at
    // construction and the processor takes its own m_mu.
    m_batch_span_processor->SetOptions(opts);

    // Phase 2 — the log pipeline. The seed and the borrowed pointer come out
    // of one hold of m_logger_mu; the retune happens after it is released, so
    // this provider never holds m_logger_mu and the processor's m_mu at once.
    // The consequence, stated rather than hidden: the change is not atomic
    // across the two pipelines.
    if (auto* const log_processor = SeedAndBorrowLogProcessor(opts); log_processor != nullptr)
    {
        log_processor->SetOptions(opts);
    }
    return Status::Completed;
}

Status SdkProvider::SetMetricInterval(std::chrono::milliseconds interval) noexcept
{
    if (m_shut_down.load(std::memory_order_acquire))
    {
        return Status::AlreadyShutDown;
    }
    if (interval.count() <= 0)
    {
        WarnRejected("SetMetricInterval rejected: interval must be greater than zero");
        return Status::InvalidArgument;
    }
    if (m_metric_exporter == nullptr)
    {
        return Status::Unsupported;
    }

    PeriodicExportingMetricReader* reader = nullptr;
    {
        // Stored even when no reader exists yet: the first GetMeter builds one
        // from this value, so a retune before any meter is created still takes
        // effect (ICP 0026 §1).
        const std::scoped_lock lk{m_meter_mu};
        m_metric_interval = interval;
        reader = m_metric_reader.get();
    }
    if (reader != nullptr)
    {
        reader->SetInterval(interval);
    }
    return Status::Completed;
}

Status SdkProvider::SetSamplerRatio(double ratio) noexcept
{
    if (m_shut_down.load(std::memory_order_acquire))
    {
        return Status::AlreadyShutDown;
    }
    if (std::isnan(ratio) || ratio < 0.0 || ratio > 1.0)
    {
        // The factory clamps and maps NaN to 0.0; the setter rejects. At build
        // time a clamp is a documented convenience, but at reload time a
        // caller asking for 1.5 has a bug in their administrative surface, and
        // silently clamping hides it from the operator watching the return
        // value (ICP 0026 Decision 3).
        WarnRejected("SetSamplerRatio rejected: ratio must be a number in [0.0, 1.0]");
        return Status::InvalidArgument;
    }

    if (auto* const sampler = m_trace->sampler.Get();
        sampler == nullptr || !sampler->TrySetRatio(ratio))
    {
        WarnRejected(
            "SetSamplerRatio: the configured sampler has no ratio to retune - nothing changed");
        return Status::Unsupported;
    }
    return Status::Completed;
}

Status SdkProvider::SetLogLevel(LogLevel level) noexcept
{
    if (m_shut_down.load(std::memory_order_acquire))
    {
        return Status::AlreadyShutDown;
    }
    if (!internal::SetMinLogLevel(level))
    {
        WarnRejected("SetLogLevel rejected: not a declared LogLevel enumerator");
        return Status::InvalidArgument;
    }
    return Status::Completed;
}

}  // namespace microtel::sdk
