// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0
//
// Acceptance proof for the two `ICurrentSpanSource` seams wired in v1.1
// packet 2.3b (issue #221, ICP 0025 §3 "The exemplar payoff").
//
// Metrics exemplars and log trace-correlation shipped structurally complete and
// inert: every `StorageOptions` site in `src/sdk/sdk_meter.cpp` omitted
// `.span_source`, and `SdkProvider::GetLogger` passed a literal `nullptr` for
// the correlation seam. Both are now fed by the thread-local current-context
// slot. This test drives the real pipeline — SdkProvider → SdkMeter →
// SumStorage/HistogramStorage → PeriodicExportingMetricReader → exporter — and
// asserts the exported exemplars carry the *actual* span and trace ids of the
// span that was current when the measurement was recorded.
//
// Contract under test:
//  - A measurement recorded inside `StartAsCurrentSpan` produces an exemplar
//    whose span_context matches the enclosing span exactly.
//  - A measurement recorded with no span active produces no exemplar.
//  - A measurement recorded under an unsampled span produces no exemplar
//    (the `trace_based` filter of `docs/metrics-design.md` §7).
//  - A log record emitted inside `StartAsCurrentSpan` is stamped with that
//    span's trace id / span id.

#include "microtel/context.hpp"
#include "microtel/internal/log_batch.hpp"
#include "microtel/internal/metric_batch.hpp"
#include "microtel/internal/metric_exporter.hpp"
#include "microtel/internal/processor.hpp"
#include "microtel/log_record.hpp"
#include "microtel/logger.hpp"
#include "microtel/meter.hpp"
#include "microtel/resource.hpp"
#include "microtel/sampler.hpp"
#include "microtel/sdk_builder.hpp"
#include "microtel/span.hpp"
#include "microtel/status.hpp"
#include "microtel/tracer.hpp"

#include "fakes/fake_log_exporter.hpp"
#include "mocks/mock_exporter.hpp"
#include "mocks/mock_transport.hpp"
#include "sdk/diagnostics_counters.hpp"
#include "sdk/sdk_provider.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

namespace mt = microtel;
namespace mti = microtel::internal;
namespace mts = microtel::sdk;
namespace mtm = microtel::testing;

using namespace std::chrono_literals;

namespace
{

/// Background collection is driven only by the explicit `ForceFlush` in these
/// tests; an interval this long keeps the reader's own loop out of the way.
constexpr std::chrono::milliseconds kNoBackgroundCollect{60 * 60 * 1000};
constexpr std::chrono::milliseconds kFlushTimeout{2000};

/// Capturing `IMetricExporter`. Keeps every batch so the test can walk the
/// collected points; the mutex is for the reader thread, which may in principle
/// export concurrently with the flushing test thread.
class CapturingMetricExporter : public mti::IMetricExporter
{
public:
    [[nodiscard]] mti::ExportResult Export(mti::MetricBatchHandle&& batch) noexcept override
    {
        const std::scoped_lock lock{m_mu};
        m_batches.push_back(std::move(batch));
        return mti::ExportResult::Success;
    }

    [[nodiscard]] mt::Status ForceFlush(std::chrono::milliseconds /*timeout*/) noexcept override
    {
        return mt::Status::Completed;
    }

    [[nodiscard]] mt::Status Shutdown(std::chrono::milliseconds /*timeout*/) noexcept override
    {
        return mt::Status::Completed;
    }

    /// Every exemplar attached to a point of the named metric, across all
    /// captured batches and every aggregation shape.
    [[nodiscard]] std::vector<mti::Exemplar> ExemplarsFor(const std::string& metric_name) const
    {
        std::vector<mti::Exemplar> found;
        const std::scoped_lock lock{m_mu};
        for (const auto& batch : m_batches)
        {
            CollectFromBatch(batch, metric_name, found);
        }
        return found;
    }

private:
    /// Appends the exemplars of every stream in @p batch named @p metric_name.
    static void CollectFromBatch(const mti::MetricBatchHandle& batch,
                                 const std::string& metric_name,
                                 std::vector<mti::Exemplar>& out)
    {
        for (const auto& record : batch.Metrics())
        {
            if (record.name == metric_name)
            {
                CollectExemplars(record.data, out);
            }
        }
    }

    /// Appends the exemplars of every point in @p data, whichever aggregation
    /// shape it holds.
    static void CollectExemplars(const mti::MetricData& data, std::vector<mti::Exemplar>& out)
    {
        std::visit(
            [&out](const auto& aggregation)
            {
                for (const auto& point : aggregation.points)
                {
                    out.insert(out.end(), point.exemplars.begin(), point.exemplars.end());
                }
            },
            data);
    }

    mutable std::mutex m_mu;
    std::vector<mti::MetricBatchHandle> m_batches;
};

/// Stateless `ISpanProcessor`. The span pipeline is not what this file asserts
/// on, and a call-counting mock would itself be the data race in the
/// concurrency test below.
class NoopSpanProcessor : public mti::ISpanProcessor
{
public:
    void OnStart(mt::Span& /*span*/, const mt::Context& /*parent*/) noexcept override {}

    // NOLINTNEXTLINE(cppcoreguidelines-rvalue-reference-param-not-moved)
    void OnEnd(mti::SpanRecord&& /*record*/,
               const mti::InstrumentationScope& /*scope*/) noexcept override
    {
    }

    [[nodiscard]] mt::Status ForceFlush(std::chrono::milliseconds /*timeout*/) noexcept override
    {
        return mt::Status::Completed;
    }

    [[nodiscard]] mt::Status Shutdown(std::chrono::milliseconds /*timeout*/) noexcept override
    {
        return mt::Status::Completed;
    }
};

/// Owns the provider plus the borrowed exporter pointers the assertions read.
///
/// Exemplar capture is off in v1.3 (ICP 0043), so the fixture turns it on
/// through the internal test hook unless @p exemplar_capture is false.
struct PipelineFixture
{
    CapturingMetricExporter* metrics = nullptr;
    mtm::FakeLogExporter* logs = nullptr;
    std::shared_ptr<mts::SdkProvider> provider;

    explicit PipelineFixture(bool exemplar_capture = true)
    {
        auto metric_exporter = std::make_unique<CapturingMetricExporter>();
        auto log_exporter = std::make_unique<mtm::FakeLogExporter>();
        metrics = metric_exporter.get();
        logs = log_exporter.get();

        provider = std::make_shared<mts::SdkProvider>(mts::SdkProviderArgs{
            .diagnostics = std::make_unique<mts::DiagnosticsCounters>(),
            .encoder = nullptr,
            .auth = nullptr,
            .transport = std::make_unique<mtm::MockTransport>(),
            .codec = nullptr,
            .exporter = std::make_unique<mtm::MockExporter>(),
            .processor = std::make_unique<NoopSpanProcessor>(),
            .resource = std::make_shared<mt::Resource>(),
            .sampler = mt::MakeAlwaysOnSampler(),
            .span_limits = {},
            .connect_opts = {},
            .metric_codec = nullptr,
            .metric_exporter = std::move(metric_exporter),
            .metric_interval = kNoBackgroundCollect,
            .exemplar_capture_for_testing = exemplar_capture,
            .log_codec = nullptr,
            .log_exporter = std::move(log_exporter),
        });
    }

    void Flush() const
    {
        ASSERT_EQ(provider->ForceFlush(kFlushTimeout), mt::Status::Completed);
    }
};

/// Opens a span scope and records one measurement inside it.
void RecordOneInsideSpan(const std::shared_ptr<mt::Tracer>& tracer,
                         const std::shared_ptr<mt::Counter<std::int64_t>>& counter)
{
    const auto scoped = tracer->StartAsCurrentSpan("work");
    counter->Add(1, {});
}

/// Opens a span scope and records one measurement inside it, over and over,
/// until @p stop is set. The body of each recorder thread in the concurrency
/// test below.
///
/// The first record is unconditional. The flush loop the test runs alongside
/// these threads completes in a couple of milliseconds, so a thread first
/// scheduled after `stop` is set would otherwise contribute nothing at all,
/// and the test's closing assertion needs at least one record to exist.
void RecordUntilStopped(const std::shared_ptr<mt::Tracer>& tracer,
                        const std::shared_ptr<mt::Counter<std::int64_t>>& counter,
                        const std::atomic<bool>& stop)
{
    RecordOneInsideSpan(tracer, counter);
    while (!stop.load(std::memory_order_relaxed))
    {
        RecordOneInsideSpan(tracer, counter);
    }
}

}  // namespace

// ---------------------------------------------------------------------------
// Metrics exemplars — the acceptance proof
// ---------------------------------------------------------------------------

TEST(ExemplarWiringTest, CounterRecordedInsideASpan_ExemplarCarriesThatSpansIds)
{
    const PipelineFixture f;
    const auto meter = f.provider->GetMeter("exemplar.lib");
    const auto counter = meter->CreateCounter<std::int64_t>("requests", "", "1");
    const auto tracer = f.provider->GetTracer("exemplar.lib");

    std::optional<mt::SpanContext> span_ctx;
    {
        const auto scoped = tracer->StartAsCurrentSpan("handle-request");
        span_ctx = scoped->GetContext();
        counter->Add(1, {});
    }
    f.Flush();

    const auto exemplars = f.metrics->ExemplarsFor("requests");
    ASSERT_EQ(exemplars.size(), 1U);
    ASSERT_TRUE(span_ctx.has_value());
    EXPECT_TRUE(exemplars[0].span_context.IsValid());
    EXPECT_EQ(exemplars[0].span_context.trace_id.AsBytes(), span_ctx->trace_id.AsBytes());
    EXPECT_EQ(exemplars[0].span_context.span_id.AsBytes(), span_ctx->span_id.AsBytes());
}

TEST(ExemplarWiringTest, HistogramRecordedInsideASpan_ExemplarCarriesThatSpansIds)
{
    const PipelineFixture f;
    const auto meter = f.provider->GetMeter("exemplar.lib");
    const auto hist = meter->CreateHistogram<double>("latency", "", "ms");
    const auto tracer = f.provider->GetTracer("exemplar.lib");

    std::optional<mt::SpanContext> span_ctx;
    {
        const auto scoped = tracer->StartAsCurrentSpan("handle-request");
        span_ctx = scoped->GetContext();
        hist->Record(12.5, {});
    }
    f.Flush();

    ASSERT_TRUE(span_ctx.has_value());
    const auto exemplars = f.metrics->ExemplarsFor("latency");
    ASSERT_EQ(exemplars.size(), 1U);
    EXPECT_EQ(exemplars[0].span_context.trace_id.AsBytes(), span_ctx->trace_id.AsBytes());
    EXPECT_EQ(exemplars[0].span_context.span_id.AsBytes(), span_ctx->span_id.AsBytes());
}

TEST(ExemplarWiringTest, CaptureOffByDefault_CounterInsideSampledSpan_HasNoExemplar)
{
    const PipelineFixture f{/*exemplar_capture=*/false};
    const auto meter = f.provider->GetMeter("exemplar.lib");
    const auto counter = meter->CreateCounter<std::int64_t>("requests", "", "1");
    const auto tracer = f.provider->GetTracer("exemplar.lib");
    {
        const auto scoped = tracer->StartAsCurrentSpan("handle-request");
        ASSERT_TRUE(mt::CurrentContext().active_span_context.trace_flags.IsSampled());
        counter->Add(1, {});
    }
    f.Flush();

    EXPECT_FALSE(f.provider->ExemplarCaptureEnabled());
    EXPECT_TRUE(f.metrics->ExemplarsFor("requests").empty());
}

TEST(ExemplarWiringTest, SdkBuilderProvider_CapturesNoExemplars)
{
    auto built = mt::SdkBuilder().WithEndpoint("https://localhost:4318").Build();
    ASSERT_TRUE(built.has_value());
    const auto* const sdk_provider = dynamic_cast<const mts::SdkProvider*>(built->get());
    ASSERT_NE(sdk_provider, nullptr);

    EXPECT_FALSE(sdk_provider->ExemplarCaptureEnabled());
}

TEST(ExemplarWiringTest, CounterRecordedWithNoSpanActive_HasNoExemplar)
{
    const PipelineFixture f;
    const auto meter = f.provider->GetMeter("exemplar.lib");
    const auto counter = meter->CreateCounter<std::int64_t>("requests", "", "1");

    counter->Add(1, {});
    f.Flush();

    EXPECT_TRUE(f.metrics->ExemplarsFor("requests").empty());
}

TEST(ExemplarWiringTest, CounterRecordedUnderAnUnsampledSpan_HasNoExemplar)
{
    auto metric_exporter = std::make_unique<CapturingMetricExporter>();
    auto* const metrics = metric_exporter.get();
    const auto provider = std::make_shared<mts::SdkProvider>(mts::SdkProviderArgs{
        .diagnostics = std::make_unique<mts::DiagnosticsCounters>(),
        .encoder = nullptr,
        .auth = nullptr,
        .transport = std::make_unique<mtm::MockTransport>(),
        .codec = nullptr,
        .exporter = std::make_unique<mtm::MockExporter>(),
        .processor = std::make_unique<NoopSpanProcessor>(),
        .resource = std::make_shared<mt::Resource>(),
        .sampler = mt::MakeAlwaysOffSampler(),
        .span_limits = {},
        .connect_opts = {},
        .metric_codec = nullptr,
        .metric_exporter = std::move(metric_exporter),
        .metric_interval = kNoBackgroundCollect,
        .exemplar_capture_for_testing = true,
    });

    const auto meter = provider->GetMeter("exemplar.lib");
    const auto counter = meter->CreateCounter<std::int64_t>("requests", "", "1");
    const auto tracer = provider->GetTracer("exemplar.lib");
    {
        const auto scoped = tracer->StartAsCurrentSpan("dropped");
        ASSERT_FALSE(mt::CurrentContext().active_span_context.trace_flags.IsSampled());
        counter->Add(1, {});
    }
    ASSERT_EQ(provider->ForceFlush(kFlushTimeout), mt::Status::Completed);

    EXPECT_TRUE(metrics->ExemplarsFor("requests").empty());
}

// Recorders race collection: four threads each open a span scope and record
// into the same instrument while the test thread repeatedly forces a collect.
// Under TSAN (-DMICROTEL_SANITIZER=tsan) this covers the whole exemplar path —
// the thread-local slot read in CurrentSpanSource, the storage mutex, and the
// reader's snapshot — at once.
TEST(ExemplarWiringTest, ConcurrentRecordingAndCollection_IsRaceFree)
{
    constexpr int kRecorders = 4;
    constexpr int kFlushes = 20;

    const PipelineFixture f;
    const auto meter = f.provider->GetMeter("exemplar.lib");
    const auto counter = meter->CreateCounter<std::int64_t>("requests", "", "1");
    const auto tracer = f.provider->GetTracer("exemplar.lib");

    std::atomic<bool> stop{false};
    std::vector<std::thread> recorders;
    recorders.reserve(kRecorders);
    for (int i = 0; i < kRecorders; ++i)
    {
        recorders.emplace_back([&stop, &tracer, &counter]
                               { RecordUntilStopped(tracer, counter, stop); });
    }

    for (int i = 0; i < kFlushes; ++i)
    {
        EXPECT_EQ(f.provider->ForceFlush(kFlushTimeout), mt::Status::Completed);
    }
    stop.store(true, std::memory_order_relaxed);
    for (auto& recorder : recorders)
    {
        recorder.join();
    }

    // One collection that provably follows every record. The 20 flushes above
    // take a couple of milliseconds in total, so under load they can all
    // complete before a recorder thread is first scheduled — each then
    // collects an empty reservoir, and the records made afterwards are never
    // collected at all (#269). The joins order every Add before this flush.
    // The concurrent window the test exists to exercise is the loop above,
    // which is unchanged.
    EXPECT_EQ(f.provider->ForceFlush(kFlushTimeout), mt::Status::Completed);

    EXPECT_FALSE(f.metrics->ExemplarsFor("requests").empty());
}

// ---------------------------------------------------------------------------
// Log trace-correlation — the second seam (sdk_provider.cpp GetLogger)
// ---------------------------------------------------------------------------

TEST(ExemplarWiringTest, LogEmittedInsideASpan_IsStampedWithThatSpansIds)
{
    const PipelineFixture f;
    const auto logger = f.provider->GetLogger("exemplar.lib");
    const auto tracer = f.provider->GetTracer("exemplar.lib");

    std::optional<mt::SpanContext> span_ctx;
    {
        const auto scoped = tracer->StartAsCurrentSpan("handle-request");
        span_ctx = scoped->GetContext();
        logger->Emit(mt::LogRecord{.body = std::string{"hello"}});
    }
    f.Flush();

    ASSERT_TRUE(span_ctx.has_value());
    ASSERT_EQ(f.logs->exported.size(), 1U);
    const auto records = f.logs->exported[0].Records();
    ASSERT_EQ(records.size(), 1U);
    EXPECT_EQ(records[0].trace_id.AsBytes(), span_ctx->trace_id.AsBytes());
    EXPECT_EQ(records[0].span_id.AsBytes(), span_ctx->span_id.AsBytes());
}

TEST(ExemplarWiringTest, LogEmittedWithNoSpanActive_HasNoTraceIds)
{
    const PipelineFixture f;
    const auto logger = f.provider->GetLogger("exemplar.lib");

    logger->Emit(mt::LogRecord{.body = std::string{"hello"}});
    f.Flush();

    ASSERT_EQ(f.logs->exported.size(), 1U);
    const auto records = f.logs->exported[0].Records();
    ASSERT_EQ(records.size(), 1U);
    EXPECT_FALSE(records[0].trace_id.IsValid());
}
