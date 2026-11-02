// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#include "backend.hpp"

// Trace exporter headers (protocol-guarded)
#if defined(BENCH_BACKEND_OTELCPP_GRPC)
#include <opentelemetry/exporters/otlp/otlp_grpc_exporter_factory.h>
#include <opentelemetry/exporters/otlp/otlp_grpc_exporter_options.h>
#include <opentelemetry/exporters/otlp/otlp_grpc_log_record_exporter_factory.h>
#include <opentelemetry/exporters/otlp/otlp_grpc_log_record_exporter_options.h>
#include <opentelemetry/exporters/otlp/otlp_grpc_metric_exporter_factory.h>
#include <opentelemetry/exporters/otlp/otlp_grpc_metric_exporter_options.h>
#elif defined(BENCH_BACKEND_OTELCPP_HTTP)
#include <opentelemetry/exporters/otlp/otlp_http_exporter_factory.h>
#include <opentelemetry/exporters/otlp/otlp_http_exporter_options.h>
#include <opentelemetry/exporters/otlp/otlp_http_log_record_exporter_factory.h>
#include <opentelemetry/exporters/otlp/otlp_http_log_record_exporter_options.h>
#include <opentelemetry/exporters/otlp/otlp_http_metric_exporter_factory.h>
#include <opentelemetry/exporters/otlp/otlp_http_metric_exporter_options.h>
#else
static_assert(false, "BENCH_BACKEND_OTELCPP_GRPC or BENCH_BACKEND_OTELCPP_HTTP required");
#endif

// Trace SDK
#if defined(BENCH_OTELCPP_INMEMORY_SPANS)
#include <opentelemetry/exporters/memory/in_memory_span_exporter_factory.h>
#endif
#include <opentelemetry/sdk/common/global_log_handler.h>
#include <opentelemetry/sdk/trace/batch_span_processor_factory.h>
#include <opentelemetry/sdk/trace/batch_span_processor_options.h>
#include <opentelemetry/sdk/trace/tracer_provider.h>
#include <opentelemetry/trace/provider.h>
#include <opentelemetry/trace/scope.h>
#include <opentelemetry/trace/tracer.h>

// Logs SDK
#include <opentelemetry/logs/logger.h>
#include <opentelemetry/logs/severity.h>
#include <opentelemetry/sdk/logs/batch_log_record_processor_factory.h>
#include <opentelemetry/sdk/logs/batch_log_record_processor_options.h>
#include <opentelemetry/sdk/logs/exporter.h>
#include <opentelemetry/sdk/logs/logger_provider.h>

// Metrics SDK
#include <opentelemetry/context/context.h>
#include <opentelemetry/metrics/meter.h>
#include <opentelemetry/metrics/sync_instruments.h>
#include <opentelemetry/sdk/metrics/export/periodic_exporting_metric_reader_factory.h>
#include <opentelemetry/sdk/metrics/export/periodic_exporting_metric_reader_options.h>
#include <opentelemetry/sdk/metrics/meter_provider.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace bench
{

namespace
{

namespace otlp      = opentelemetry::exporter::otlp;
namespace sdktrace  = opentelemetry::sdk::trace;
namespace sdkmetrics = opentelemetry::sdk::metrics;
namespace sdklogs   = opentelemetry::sdk::logs;
namespace logs_api  = opentelemetry::logs;

constexpr const char* kLogBody = "bench log record";
namespace trace_api = opentelemetry::trace;
namespace metrics_api = opentelemetry::metrics;
namespace sdklog_internal = opentelemetry::sdk::common::internal_log;

// opentelemetry-cpp exposes no drop counter: a full BatchSpanProcessor queue
// only logs "BatchSpanProcessor queue is full - dropping span." per span
// (batch_span_processor.cc). This handler counts those messages and forwards
// every message to the handler it replaced, so logging is unchanged.
std::atomic<uint64_t> g_queue_full_drops{0};

class DropCountingLogHandler final : public sdklog_internal::LogHandler
{
public:
    explicit DropCountingLogHandler(opentelemetry::nostd::shared_ptr<sdklog_internal::LogHandler> next)
        : m_next(std::move(next))
    {
    }

    void Handle(sdklog_internal::LogLevel level,
                const char* file,
                int line,
                const char* msg,
                const opentelemetry::sdk::common::AttributeMap& attributes) noexcept override
    {
        if (msg != nullptr && std::strstr(msg, "dropping span") != nullptr)
        {
            g_queue_full_drops.fetch_add(1, std::memory_order_relaxed);
        }
        if (m_next)
        {
            m_next->Handle(level, file, line, msg, attributes);
        }
    }

private:
    opentelemetry::nostd::shared_ptr<sdklog_internal::LogHandler> m_next;
};

class OtelCppBackend final : public IBackend
{
public:
    OtelCppBackend() = default;

    OtelCppBackend(const OtelCppBackend&) = delete;
    OtelCppBackend& operator=(const OtelCppBackend&) = delete;
    OtelCppBackend(OtelCppBackend&&) = delete;
    OtelCppBackend& operator=(OtelCppBackend&&) = delete;

    ~OtelCppBackend() override = default;

    void Init(const BackendOptions& opts) override
    {
        InitTrace(opts);
        if (opts.metric_interval_ms > 0)
        {
            InitMetrics(opts);
        }
        if (opts.logs_enabled)
        {
            InitLogs(opts);
        }
    }

    void EmitSpan() override
    {
        auto span = m_tracer->StartSpan("bench.span");
        for (int i = 0; i < m_attrs_per_span; ++i)
        {
            span->SetAttribute(
                opentelemetry::nostd::string_view(m_attr_keys[i]),
                opentelemetry::nostd::string_view(m_attr_value));
        }
        span->End();
        m_emit_count.fetch_add(1, std::memory_order_relaxed);
    }

    void EmitRequest() override
    {
        auto parent = m_tracer->StartSpan("bench.request");
        {
            opentelemetry::trace::Scope scope(parent);
            auto child1 = m_tracer->StartSpan("bench.request.op1");
            child1->End();
            auto child2 = m_tracer->StartSpan("bench.request.op2");
            child2->End();
        }
        parent->End();
        m_emit_count.fetch_add(1, std::memory_order_relaxed);
    }

    void EmitRecord() override
    {
        if (!m_metric_provider)
        {
            return;
        }
        m_counter->Add(1);
        m_histogram->Record(1.0, opentelemetry::context::Context{});
        m_emit_count.fetch_add(1, std::memory_order_relaxed);
    }

    void EmitLog() override
    {
        if (!m_logger_provider)
        {
            return;
        }
        // Minimal record, matching the microtel backend: INFO severity and a
        // short string body. The SDK stamps the observed timestamp.
        m_logger->EmitLogRecord(logs_api::Severity::kInfo, kLogBody);
        m_emit_count.fetch_add(1, std::memory_order_relaxed);
    }

    [[nodiscard]] uint64_t ForceFlush() override
    {
        using Clock = std::chrono::steady_clock;
        const auto t0 = Clock::now();
        m_trace_provider->ForceFlush(std::chrono::microseconds(30'000'000));
        if (m_metric_provider)
        {
            m_metric_provider->ForceFlush(std::chrono::microseconds(30'000'000));
        }
        if (m_logger_provider)
        {
            m_logger_provider->ForceFlush(std::chrono::microseconds(30'000'000));
        }
        const auto t1 = Clock::now();
        return static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
    }

    void Shutdown() override
    {
        m_trace_provider->ForceFlush(std::chrono::microseconds(30'000'000));
        m_trace_provider->Shutdown();
        if (m_metric_provider)
        {
            m_metric_provider->ForceFlush(std::chrono::microseconds(30'000'000));
            m_metric_provider->Shutdown();
        }
        if (m_logger_provider)
        {
            m_logger_provider->ForceFlush(std::chrono::microseconds(30'000'000));
            m_logger_provider->Shutdown();
        }
    }

    [[nodiscard]] BackendStats Stats() const override
    {
        // opentelemetry-cpp does not expose drop counters via a stable public API;
        // queue-full drops are counted from its log (DropCountingLogHandler).
        // The other DroppedCounts fields stay 0.  See bench/docs/methodology.md.
        const uint64_t queue_full = g_queue_full_drops.load(std::memory_order_relaxed);
        return BackendStats{
            .spans_exported_total = m_emit_count.load(std::memory_order_relaxed),
            .spans_dropped        = {.queue_full = queue_full, .total = queue_full},
            .bytes_sent_total     = 0,
        };
    }

private:
    void InitTrace(const BackendOptions& opts)
    {
        std::unique_ptr<opentelemetry::sdk::trace::SpanExporter> exporter;

#if defined(BENCH_BACKEND_OTELCPP_GRPC)
        otlp::OtlpGrpcExporterOptions grpc_opts;
        grpc_opts.endpoint           = opts.endpoint;
        grpc_opts.use_ssl_credentials = false;
        exporter = otlp::OtlpGrpcExporterFactory::Create(grpc_opts);
#elif defined(BENCH_OTELCPP_INMEMORY_SPANS)
        // Parity study (bench/parity): the SDK's own SpanData recordable in
        // place of OtlpRecordable, so no protobuf is built on the calling
        // thread. Same BatchSpanProcessor, sampler and harness.
        // The exporter keeps finished spans in a bounded ring (the factory's
        // default size); nothing here reads them back.
        static_cast<void>(opts.endpoint);
        std::shared_ptr<opentelemetry::exporter::memory::InMemorySpanData> span_data;
        exporter = opentelemetry::exporter::memory::InMemorySpanExporterFactory::Create(span_data);
#elif defined(BENCH_BACKEND_OTELCPP_HTTP)
        otlp::OtlpHttpExporterOptions http_opts;
        http_opts.url = opts.endpoint + "/v1/traces";
        exporter      = otlp::OtlpHttpExporterFactory::Create(http_opts);
#endif

        if (!exporter)
        {
            throw std::runtime_error("opentelemetry-cpp trace exporter creation failed");
        }

        sdklog_internal::GlobalLogHandler::SetLogHandler(
            opentelemetry::nostd::shared_ptr<sdklog_internal::LogHandler>(
                new DropCountingLogHandler(sdklog_internal::GlobalLogHandler::GetLogHandler())));

        sdktrace::BatchSpanProcessorOptions proc_opts;
        // Parity study (bench/parity): opt-in larger queue, so a burst of
        // spans_per_sample does not overflow the SDK's default 2048.
        // NOLINTNEXTLINE(concurrency-mt-unsafe)
        if (const char* q = std::getenv("EMIT_OTELCPP_BSP_MAX_QUEUE_SIZE"); q != nullptr && *q != '\0')
        {
            proc_opts.max_queue_size = static_cast<std::size_t>(std::strtoull(q, nullptr, 10));
        }
        auto processor = sdktrace::BatchSpanProcessorFactory::Create(std::move(exporter),
                                                                     proc_opts);
        m_trace_provider = std::make_shared<sdktrace::TracerProvider>(std::move(processor));
        m_tracer         = m_trace_provider->GetTracer(opts.service_name, opts.service_version);

        m_attrs_per_span = opts.attributes_per_span;
        m_attr_keys.resize(static_cast<std::size_t>(opts.attributes_per_span));
        for (int i = 0; i < opts.attributes_per_span; ++i)
        {
            m_attr_keys[static_cast<std::size_t>(i)] = "bench.attr." + std::to_string(i);
        }
        m_attr_value = std::string(static_cast<std::size_t>(opts.attribute_value_bytes), 'x');
    }

    void InitMetrics(const BackendOptions& opts)
    {
        std::unique_ptr<opentelemetry::sdk::metrics::PushMetricExporter> exporter;

#if defined(BENCH_BACKEND_OTELCPP_GRPC)
        otlp::OtlpGrpcMetricExporterOptions metric_opts;
        metric_opts.endpoint           = opts.endpoint;
        metric_opts.use_ssl_credentials = false;
        exporter = otlp::OtlpGrpcMetricExporterFactory::Create(metric_opts);
#elif defined(BENCH_BACKEND_OTELCPP_HTTP)
        otlp::OtlpHttpMetricExporterOptions metric_opts;
        metric_opts.url = opts.endpoint + "/v1/metrics";
        exporter        = otlp::OtlpHttpMetricExporterFactory::Create(metric_opts);
#endif

        if (!exporter)
        {
            throw std::runtime_error("opentelemetry-cpp metric exporter creation failed");
        }

        sdkmetrics::PeriodicExportingMetricReaderOptions reader_opts;
        reader_opts.export_interval_millis =
            std::chrono::milliseconds(opts.metric_interval_ms);
        // timeout must be strictly less than interval (OTel spec); use half the interval
        reader_opts.export_timeout_millis =
            std::chrono::milliseconds(opts.metric_interval_ms / 2);
        auto reader = sdkmetrics::PeriodicExportingMetricReaderFactory::Create(
            std::move(exporter), reader_opts);

        m_metric_provider = std::make_shared<sdkmetrics::MeterProvider>();
        m_metric_provider->AddMetricReader(std::move(reader));

        auto meter    = m_metric_provider->GetMeter("bench", "0.0.0");
        m_counter     = meter->CreateUInt64Counter("bench.counter", "Bench counter", "1");
        m_histogram   = meter->CreateDoubleHistogram("bench.histogram", "Bench histogram", "ms");
    }

    void InitLogs(const BackendOptions& opts)
    {
        std::unique_ptr<sdklogs::LogRecordExporter> exporter;

#if defined(BENCH_BACKEND_OTELCPP_GRPC)
        otlp::OtlpGrpcLogRecordExporterOptions log_opts;
        log_opts.endpoint            = opts.endpoint;
        log_opts.use_ssl_credentials = false;
        exporter = otlp::OtlpGrpcLogRecordExporterFactory::Create(log_opts);
#elif defined(BENCH_BACKEND_OTELCPP_HTTP)
        otlp::OtlpHttpLogRecordExporterOptions log_opts;
        log_opts.url = opts.endpoint + "/v1/logs";
        exporter     = otlp::OtlpHttpLogRecordExporterFactory::Create(log_opts);
#endif

        if (!exporter)
        {
            throw std::runtime_error("opentelemetry-cpp log record exporter creation failed");
        }

        sdklogs::BatchLogRecordProcessorOptions proc_opts;
        auto processor = sdklogs::BatchLogRecordProcessorFactory::Create(std::move(exporter),
                                                                         proc_opts);
        m_logger_provider = std::make_shared<sdklogs::LoggerProvider>(std::move(processor));
        m_logger = m_logger_provider->GetLogger("bench", opts.service_name, opts.service_version);
    }

    // Trace members
    std::shared_ptr<sdktrace::TracerProvider>                       m_trace_provider;
    opentelemetry::nostd::shared_ptr<opentelemetry::trace::Tracer> m_tracer;
    std::atomic<uint64_t>                                           m_emit_count{0};
    int                                                             m_attrs_per_span{0};
    std::vector<std::string>                                        m_attr_keys;
    std::string                                                     m_attr_value;

    // Log members (null unless the logs workload is selected)
    std::shared_ptr<sdklogs::LoggerProvider>                                m_logger_provider;
    opentelemetry::nostd::shared_ptr<logs_api::Logger>                      m_logger;

    // Metric members (null when metric_interval_ms == 0)
    std::shared_ptr<sdkmetrics::MeterProvider>                              m_metric_provider;
    opentelemetry::nostd::shared_ptr<metrics_api::Counter<uint64_t>>       m_counter;
    opentelemetry::nostd::shared_ptr<metrics_api::Histogram<double>>       m_histogram;
};

}  // namespace

IBackend* CreateBackend()
{
    return new OtelCppBackend();  // NOLINT(cppcoreguidelines-owning-memory)
}

}  // namespace bench
