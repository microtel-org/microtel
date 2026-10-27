// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0
//
// Unit tests for SdkProvider: ForceFlush two-stage pipeline behaviour.

#include "sdk/sdk_provider.hpp"

#include "microtel/internal/sampler.hpp"
#include "microtel/provider.hpp"
#include "microtel/resource.hpp"
#include "microtel/sampler.hpp"
#include "microtel/sdk_builder.hpp"
#include "microtel/span.hpp"
#include "microtel/status.hpp"
#include "microtel/tracer.hpp"

#include "fakes/fake_exporter.hpp"
#include "fakes/fake_span_processor.hpp"
#include "helpers/thread_ids.hpp"
#include "mocks/mock_exporter.hpp"
#include "mocks/mock_log_exporter.hpp"
#include "mocks/mock_metric_exporter.hpp"
#include "mocks/mock_span_processor.hpp"
#include "mocks/mock_transport.hpp"
#include "sdk/batch_span_processor.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <thread>
#include <utility>
#include <vector>

namespace mt = microtel;
namespace mts = microtel::sdk;
namespace mtm = microtel::testing;

namespace
{

constexpr auto kTimeout = std::chrono::milliseconds(500);

// Build an SdkProvider with mock processor, exporter, and transport.
// Returns the provider; raw non-owning pointers to the mocks are written
// through the out-params so callers can observe call counts after the call.
std::unique_ptr<mts::SdkProvider> MakeProvider(mtm::MockSpanProcessor** out_proc,
                                               mtm::MockExporter** out_exp,
                                               mtm::MockTransport** out_transport)
{
    auto proc = std::make_unique<mtm::MockSpanProcessor>();
    auto exp = std::make_unique<mtm::MockExporter>();
    auto transport = std::make_unique<mtm::MockTransport>();

    *out_proc = proc.get();
    *out_exp = exp.get();
    *out_transport = transport.get();

    return std::make_unique<mts::SdkProvider>(mts::SdkProviderArgs{
        .diagnostics = std::make_unique<mts::DiagnosticsCounters>(),
        .encoder = nullptr,
        .auth = nullptr,
        .transport = std::move(transport),
        .codec = nullptr,
        .exporter = std::move(exp),
        .processor = std::move(proc),
        .resource = std::make_shared<mt::Resource>(),
        .sampler = mt::MakeAlwaysOnSampler(),
        .span_limits = {},
        .connect_opts = {},
    });
}

// Variants that actually wire a log / metric exporter. Without one, GetLogger
// returns the noop logger and GetMeter never builds a reader, so a
// post-shutdown test against the plain fixture would pass while proving
// nothing.
std::unique_ptr<mts::SdkProvider> MakeProviderWithLogExporter(mtm::MockSpanProcessor** out_proc,
                                                              mtm::MockExporter** out_exp,
                                                              mtm::MockTransport** out_transport)
{
    auto proc = std::make_unique<mtm::MockSpanProcessor>();
    auto exp = std::make_unique<mtm::MockExporter>();
    auto transport = std::make_unique<mtm::MockTransport>();
    *out_proc = proc.get();
    *out_exp = exp.get();
    *out_transport = transport.get();

    return std::make_unique<mts::SdkProvider>(mts::SdkProviderArgs{
        .diagnostics = std::make_unique<mts::DiagnosticsCounters>(),
        .encoder = nullptr,
        .auth = nullptr,
        .transport = std::move(transport),
        .codec = nullptr,
        .exporter = std::move(exp),
        .processor = std::move(proc),
        .resource = std::make_shared<mt::Resource>(),
        .sampler = mt::MakeAlwaysOnSampler(),
        .span_limits = {},
        .connect_opts = {},
        .log_exporter = std::make_unique<mtm::MockLogExporter>(),
    });
}

std::unique_ptr<mts::SdkProvider> MakeProviderWithMetricExporter(mtm::MockSpanProcessor** out_proc,
                                                                 mtm::MockExporter** out_exp,
                                                                 mtm::MockTransport** out_transport)
{
    auto proc = std::make_unique<mtm::MockSpanProcessor>();
    auto exp = std::make_unique<mtm::MockExporter>();
    auto transport = std::make_unique<mtm::MockTransport>();
    *out_proc = proc.get();
    *out_exp = exp.get();
    *out_transport = transport.get();

    return std::make_unique<mts::SdkProvider>(mts::SdkProviderArgs{
        .diagnostics = std::make_unique<mts::DiagnosticsCounters>(),
        .encoder = nullptr,
        .auth = nullptr,
        .transport = std::move(transport),
        .codec = nullptr,
        .exporter = std::move(exp),
        .processor = std::move(proc),
        .resource = std::make_shared<mt::Resource>(),
        .sampler = mt::MakeAlwaysOnSampler(),
        .span_limits = {},
        .connect_opts = {},
        .metric_exporter = std::make_unique<mtm::MockMetricExporter>(),
    });
}

/// How long a slow stage takes, against `kTimeout`.
constexpr auto kStageDelay = std::chrono::milliseconds(300);

/// A provider whose span processor is a fake that can take time, with the
/// trace exporter, log exporter and transport as mocks the test can inspect.
struct StagedProvider
{
    mtm::FakeSpanProcessor* processor = nullptr;
    mtm::MockExporter* exporter = nullptr;
    mtm::MockLogExporter* log_exporter = nullptr;
    mtm::MockTransport* transport = nullptr;
    std::unique_ptr<mts::SdkProvider> provider;
};

StagedProvider MakeStagedProvider()
{
    auto proc = std::make_unique<mtm::FakeSpanProcessor>();
    auto exp = std::make_unique<mtm::MockExporter>();
    auto log_exp = std::make_unique<mtm::MockLogExporter>();
    auto transport = std::make_unique<mtm::MockTransport>();
    StagedProvider s{.processor = proc.get(),
                     .exporter = exp.get(),
                     .log_exporter = log_exp.get(),
                     .transport = transport.get(),
                     .provider = nullptr};
    s.provider = std::make_unique<mts::SdkProvider>(mts::SdkProviderArgs{
        .diagnostics = std::make_unique<mts::DiagnosticsCounters>(),
        .encoder = nullptr,
        .auth = nullptr,
        .transport = std::move(transport),
        .codec = nullptr,
        .exporter = std::move(exp),
        .processor = std::move(proc),
        .resource = std::make_shared<mt::Resource>(),
        .sampler = mt::MakeAlwaysOnSampler(),
        .span_limits = {},
        .connect_opts = {},
        .log_exporter = std::move(log_exp),
    });
    return s;
}

}  // namespace

// ---------------------------------------------------------------------------
// ForceFlush — two-stage pipeline
// ---------------------------------------------------------------------------

// Both processor and exporter ForceFlush must be called on the happy path.
TEST(SdkProviderTest, ForceFlush_CallsBothProcessorAndExporter)
{
    mtm::MockSpanProcessor* proc = nullptr;   // NOLINT(misc-const-correctness)
    mtm::MockExporter* exp = nullptr;         // NOLINT(misc-const-correctness)
    mtm::MockTransport* transport = nullptr;  // NOLINT(misc-const-correctness)
    auto provider = MakeProvider(&proc, &exp, &transport);

    const auto status = provider->ForceFlush(kTimeout);

    EXPECT_EQ(status, mt::Status::Completed);
    EXPECT_EQ(proc->force_flush_call_count, 1);
    EXPECT_EQ(exp->force_flush_call_count, 1);
}

// ---------------------------------------------------------------------------
// One deadline, every stage, the worst status (ICP 0041, #486)
// ---------------------------------------------------------------------------

// A stage that fails or times out no longer stops the stages after it: the
// trace exporter and the log exporter are still flushed, and the caller sees
// the worst status.
TEST(SdkProviderTest, ForceFlush_ProcessorTimedOut_EveryLaterStageStillRuns)
{
    auto s = MakeStagedProvider();
    s.processor->force_flush_result = mt::Status::TimedOut;

    EXPECT_EQ(s.provider->ForceFlush(kTimeout), mt::Status::TimedOut);
    EXPECT_EQ(s.exporter->force_flush_call_count, 1);
    EXPECT_EQ(s.log_exporter->force_flush_call_count, 1);
}

TEST(SdkProviderTest, ForceFlush_FoldsTheWorstStatus)
{
    auto s = MakeStagedProvider();
    s.exporter->force_flush_result = mt::Status::Failed;
    s.log_exporter->force_flush_result = mt::Status::TimedOut;

    EXPECT_EQ(s.provider->ForceFlush(kTimeout), mt::Status::Failed);
    EXPECT_EQ(s.log_exporter->force_flush_call_count, 1);
}

TEST(SdkProviderTest, ForceFlush_StagesShareOneDeadline)
{
    auto s = MakeStagedProvider();
    s.processor->force_flush_delay = kStageDelay;

    ASSERT_EQ(s.provider->ForceFlush(kTimeout), mt::Status::Completed);
    EXPECT_LE(s.exporter->last_force_flush_timeout, kTimeout - kStageDelay);
    EXPECT_LE(s.log_exporter->last_force_flush_timeout, kTimeout - kStageDelay);
}

TEST(SdkProviderTest, ForceFlush_PastTheDeadline_LaterStagesStillRunWithNoTime)
{
    auto s = MakeStagedProvider();
    s.processor->force_flush_delay = kTimeout + kStageDelay;

    (void)s.provider->ForceFlush(kTimeout);
    EXPECT_EQ(s.exporter->force_flush_call_count, 1);
    EXPECT_EQ(s.exporter->last_force_flush_timeout, std::chrono::milliseconds{0});
    EXPECT_EQ(s.log_exporter->last_force_flush_timeout, std::chrono::milliseconds{0});
}

// With every stage running, a ForceFlush after Shutdown would otherwise reach
// exporters whose workers are already joined and wait out the timeout.
TEST(SdkProviderTest, ForceFlush_AfterShutdown_ReturnsAlreadyShutDownAndFlushesNothing)
{
    auto s = MakeStagedProvider();
    ASSERT_EQ(s.provider->Shutdown(kTimeout), mt::Status::Completed);

    EXPECT_EQ(s.provider->ForceFlush(kTimeout), mt::Status::AlreadyShutDown);
    EXPECT_EQ(s.processor->force_flush_call_count, 0);
    EXPECT_EQ(s.exporter->force_flush_call_count, 0);
}

TEST(SdkProviderTest, Shutdown_StagesShareOneDeadline)
{
    auto s = MakeStagedProvider();
    s.processor->shutdown_delay = kStageDelay;

    ASSERT_EQ(s.provider->Shutdown(kTimeout), mt::Status::Completed);
    EXPECT_LE(s.log_exporter->last_shutdown_timeout, kTimeout - kStageDelay);
    EXPECT_LE(s.exporter->last_shutdown_timeout, kTimeout - kStageDelay);
    EXPECT_LE(s.transport->last_close_timeout, kTimeout - kStageDelay);
}

// ---------------------------------------------------------------------------
// GetExporterHealth — diagnostics counters
// ---------------------------------------------------------------------------

// Drops recorded on the provider-owned diagnostics sink surface in the
// health snapshot; connection state is still read live from the transport.
TEST(SdkProviderTest, GetExporterHealthReflectsRecordedDrops)
{
    mtm::MockSpanProcessor* proc = nullptr;   // NOLINT(misc-const-correctness)
    mtm::MockExporter* exp = nullptr;         // NOLINT(misc-const-correctness)
    mtm::MockTransport* transport = nullptr;  // NOLINT(misc-const-correctness)
    auto provider = MakeProvider(&proc, &exp, &transport);

    constexpr std::uint64_t kOverflowDrops = 4;
    provider->DiagnosticsSink().RecordDrop(mt::DropReason::QueueFull);
    provider->DiagnosticsSink().RecordDrop(mt::DropReason::CardinalityOverflow, kOverflowDrops);

    const mt::HealthSnapshot health = provider->GetExporterHealth();

    EXPECT_EQ(health.drop_counters[static_cast<std::size_t>(mt::DropReason::QueueFull)], 1U);
    EXPECT_EQ(health.drop_counters[static_cast<std::size_t>(mt::DropReason::CardinalityOverflow)],
              kOverflowDrops);
    EXPECT_EQ(health.connection_state, mt::ConnectionState::Connected);
}

// If the exporter times out the caller receives TimedOut.
TEST(SdkProviderTest, ForceFlush_ExporterTimedOut_ReturnTimedOut)
{
    mtm::MockSpanProcessor* proc = nullptr;  // NOLINT(misc-const-correctness)
    mtm::MockExporter* exp = nullptr;
    mtm::MockTransport* transport = nullptr;  // NOLINT(misc-const-correctness)
    auto provider = MakeProvider(&proc, &exp, &transport);

    exp->force_flush_result = mt::Status::TimedOut;

    const auto status = provider->ForceFlush(kTimeout);

    EXPECT_EQ(status, mt::Status::TimedOut);
    EXPECT_EQ(proc->force_flush_call_count, 1);
    EXPECT_EQ(exp->force_flush_call_count, 1);
}

// ---------------------------------------------------------------------------
// Drop accounting for lifecycle outcomes — issue #169. The Provider is the
// only layer that sees one user-visible ForceFlush / Shutdown / Connect call;
// counting inside the processor and the exporter arms as well would report
// two or three drops for one timed-out call.
// ---------------------------------------------------------------------------

static std::uint64_t DropCount(const mt::HealthSnapshot& health, mt::DropReason reason)
{
    return health.drop_counters.at(static_cast<std::size_t>(reason));
}

TEST(SdkProviderTest, Diagnostics_ForceFlushTimedOut_CountsOncePerCall)
{
    mtm::MockSpanProcessor* proc = nullptr;   // NOLINT(misc-const-correctness)
    mtm::MockExporter* exp = nullptr;         // NOLINT(misc-const-correctness)
    mtm::MockTransport* transport = nullptr;  // NOLINT(misc-const-correctness)
    auto provider = MakeProvider(&proc, &exp, &transport);

    exp->force_flush_result = mt::Status::TimedOut;

    ASSERT_EQ(provider->ForceFlush(kTimeout), mt::Status::TimedOut);

    const mt::HealthSnapshot health = provider->GetExporterHealth();
    EXPECT_EQ(DropCount(health, mt::DropReason::ForceFlushTimeout), 1U);
    EXPECT_EQ(DropCount(health, mt::DropReason::ShutdownTimeout), 0U);
}

TEST(SdkProviderTest, Diagnostics_ForceFlushCompleted_CountsNothing)
{
    mtm::MockSpanProcessor* proc = nullptr;   // NOLINT(misc-const-correctness)
    mtm::MockExporter* exp = nullptr;         // NOLINT(misc-const-correctness)
    mtm::MockTransport* transport = nullptr;  // NOLINT(misc-const-correctness)
    auto provider = MakeProvider(&proc, &exp, &transport);

    ASSERT_EQ(provider->ForceFlush(kTimeout), mt::Status::Completed);

    EXPECT_EQ(DropCount(provider->GetExporterHealth(), mt::DropReason::ForceFlushTimeout), 0U);
}

TEST(SdkProviderTest, Diagnostics_ShutdownTimedOut_CountsOncePerCall)
{
    mtm::MockSpanProcessor* proc = nullptr;   // NOLINT(misc-const-correctness)
    mtm::MockExporter* exp = nullptr;         // NOLINT(misc-const-correctness)
    mtm::MockTransport* transport = nullptr;  // NOLINT(misc-const-correctness)
    auto provider = MakeProvider(&proc, &exp, &transport);

    // Two components time out; the user made one Shutdown call, so the
    // counter must read 1, not 2.
    proc->shutdown_result = mt::Status::TimedOut;
    transport->close_result = mt::Status::TimedOut;

    ASSERT_EQ(provider->Shutdown(kTimeout), mt::Status::TimedOut);

    const mt::HealthSnapshot health = provider->GetExporterHealth();
    EXPECT_EQ(DropCount(health, mt::DropReason::ShutdownTimeout), 1U);
    EXPECT_EQ(DropCount(health, mt::DropReason::ForceFlushTimeout), 0U);
}

TEST(SdkProviderTest, Diagnostics_ConnectFailed_CountsConnectFailure)
{
    mtm::MockSpanProcessor* proc = nullptr;   // NOLINT(misc-const-correctness)
    mtm::MockExporter* exp = nullptr;         // NOLINT(misc-const-correctness)
    mtm::MockTransport* transport = nullptr;  // NOLINT(misc-const-correctness)
    auto provider = MakeProvider(&proc, &exp, &transport);

    transport->connect_result =
        mt::make_unexpected(mt::Error{.kind = mt::Error::Kind::Network, .message = "refused"});

    EXPECT_FALSE(provider->Connect().has_value());

    EXPECT_EQ(DropCount(provider->GetExporterHealth(), mt::DropReason::ConnectFailure), 1U);
}

TEST(SdkProviderTest, Diagnostics_ConnectSucceeded_CountsNothing)
{
    mtm::MockSpanProcessor* proc = nullptr;   // NOLINT(misc-const-correctness)
    mtm::MockExporter* exp = nullptr;         // NOLINT(misc-const-correctness)
    mtm::MockTransport* transport = nullptr;  // NOLINT(misc-const-correctness)
    auto provider = MakeProvider(&proc, &exp, &transport);

    EXPECT_TRUE(provider->Connect().has_value());

    EXPECT_EQ(DropCount(provider->GetExporterHealth(), mt::DropReason::ConnectFailure), 0U);
}

// MakeProvider reports its mocks through T** out-params, so these locals must
// be non-const pointers even in tests that only read the mock afterwards.
// NOLINTBEGIN(misc-const-correctness)
// ---------------------------------------------------------------------------
// Shutdown status aggregation.
//
// Provider::Shutdown drove six components and returned only the span
// processor's status, discarding the other five with (void). A transport or
// exporter that timed out was invisible to the caller — which made
// Http2Transport::Close's timeout unobservable even after it was honoured.
// ---------------------------------------------------------------------------

TEST(SdkProviderTest, Shutdown_TransportTimedOut_IsReportedToTheCaller)
{
    mtm::MockSpanProcessor* proc = nullptr;
    mtm::MockExporter* exp = nullptr;
    mtm::MockTransport* transport = nullptr;
    auto provider = MakeProvider(&proc, &exp, &transport);
    ASSERT_NE(transport, nullptr);

    ASSERT_NE(proc, nullptr);
    ASSERT_NE(exp, nullptr);
    transport->close_result = mt::Status::TimedOut;

    EXPECT_EQ(provider->Shutdown(std::chrono::milliseconds(10)), mt::Status::TimedOut);
    // The timeout must not stop the rest of the teardown.
    EXPECT_GT(proc->shutdown_call_count, 0);
    EXPECT_GT(exp->shutdown_call_count, 0);
}

TEST(SdkProviderTest, Shutdown_AllComponentsComplete_ReportsCompleted)
{
    mtm::MockSpanProcessor* proc = nullptr;
    mtm::MockExporter* exp = nullptr;
    mtm::MockTransport* transport = nullptr;
    auto provider = MakeProvider(&proc, &exp, &transport);
    ASSERT_NE(proc, nullptr);
    ASSERT_NE(exp, nullptr);
    ASSERT_NE(transport, nullptr);

    EXPECT_EQ(provider->Shutdown(std::chrono::milliseconds(10)), mt::Status::Completed);
    EXPECT_GT(proc->shutdown_call_count, 0);
    EXPECT_GT(exp->shutdown_call_count, 0);
    EXPECT_GT(transport->close_call_count, 0);
}

TEST(SdkProviderTest, Shutdown_TransportStillClosesWhenProcessorTimedOut)
{
    mtm::MockSpanProcessor* proc = nullptr;
    mtm::MockExporter* exp = nullptr;
    mtm::MockTransport* transport = nullptr;
    auto provider = MakeProvider(&proc, &exp, &transport);
    ASSERT_NE(proc, nullptr);
    ASSERT_NE(transport, nullptr);

    ASSERT_NE(exp, nullptr);
    proc->shutdown_result = mt::Status::TimedOut;

    EXPECT_EQ(provider->Shutdown(std::chrono::milliseconds(10)), mt::Status::TimedOut);
    EXPECT_GT(exp->shutdown_call_count, 0);
    // A partial teardown would leak the I/O thread and socket, so an early
    // timeout must not short-circuit the components after it.
    EXPECT_GT(transport->close_call_count, 0);
}

// ---------------------------------------------------------------------------
// Post-shutdown component construction.
//
// GetMeter and GetLogger had no shutdown check, so either could build a
// PeriodicExportingMetricReader or a BatchLogRecordProcessor -- and spawn its
// thread -- after Shutdown() returned. threading-model.md §6.2 says no further
// records are accepted after Shutdown; the new thread was also joined only at
// destruction. Threads started is the assertion because the thread is the bug.
// ---------------------------------------------------------------------------

TEST(SdkProviderTest, GetLoggerAfterShutdown_SpawnsNoThread)
{
    mtm::MockSpanProcessor* proc = nullptr;
    mtm::MockExporter* exp = nullptr;
    mtm::MockTransport* transport = nullptr;
    auto provider = MakeProviderWithLogExporter(&proc, &exp, &transport);

    ASSERT_EQ(provider->Shutdown(std::chrono::milliseconds(50)), mt::Status::Completed);

    const auto before = mtm::ThreadIds();
    ASSERT_FALSE(before.empty());
    auto logger = provider->GetLogger("after", "1.0");
    EXPECT_EQ(mtm::ThreadsStartedSince(before), 0U);
    EXPECT_NE(logger, nullptr);
}

TEST(SdkProviderTest, GetMeterAfterShutdown_SpawnsNoThread)
{
    mtm::MockSpanProcessor* proc = nullptr;
    mtm::MockExporter* exp = nullptr;
    mtm::MockTransport* transport = nullptr;
    auto provider = MakeProviderWithMetricExporter(&proc, &exp, &transport);

    ASSERT_EQ(provider->Shutdown(std::chrono::milliseconds(50)), mt::Status::Completed);

    const auto before = mtm::ThreadIds();
    ASSERT_FALSE(before.empty());
    auto meter = provider->GetMeter("after", "1.0");
    EXPECT_EQ(mtm::ThreadsStartedSince(before), 0U);
    EXPECT_NE(meter, nullptr);
}

// The guard must not break the ordinary path.
TEST(SdkProviderTest, GetLoggerBeforeShutdown_StillBuildsThePipeline)
{
    mtm::MockSpanProcessor* proc = nullptr;
    mtm::MockExporter* exp = nullptr;
    mtm::MockTransport* transport = nullptr;
    auto provider = MakeProviderWithLogExporter(&proc, &exp, &transport);

    const auto before = mtm::ThreadIds();
    auto logger = provider->GetLogger("before", "1.0");
    EXPECT_NE(logger, nullptr);
    // The processor's worker thread is expected here.
    EXPECT_GT(mtm::ThreadsStartedSince(before), 0U);
}

// ---------------------------------------------------------------------------
// Instrumentation scope round-trip (issue #167 / ICP 0023)
// ---------------------------------------------------------------------------

// End-to-end inside the SDK: Provider::GetTracer -> SdkTracer -> SdkSpan ->
// a real BatchSpanProcessor -> a capturing exporter. Before ICP 0023 the
// exported batch carried the scope the processor was constructed with -- in
// production, the service name out of builder config -- and the name and
// version passed to GetTracer reached nothing.
TEST(SdkProviderTest, ExportedBatchCarriesTheTracerScope)
{
    auto exporter = std::make_unique<mtm::FakeExporter>();
    auto* const captured = exporter.get();

    mt::BatchOptions opts;
    opts.schedule_delay = std::chrono::hours(1);  // only ForceFlush drains

    auto provider = std::make_unique<mts::SdkProvider>(mts::SdkProviderArgs{
        .diagnostics = std::make_unique<mts::DiagnosticsCounters>(),
        .encoder = nullptr,
        .auth = nullptr,
        .transport = std::make_unique<mtm::MockTransport>(),
        .codec = nullptr,
        .exporter = std::move(exporter),
        .processor = std::make_unique<mts::BatchSpanProcessor>(
            captured, std::make_shared<const mt::Resource>(), opts),
        .resource = std::make_shared<mt::Resource>(),
        .sampler = mt::MakeAlwaysOnSampler(),
        .span_limits = {},
        .connect_opts = {},
    });

    {
        const auto tracer = provider->GetTracer("scope.x", "1.2");
        auto span = tracer->StartSpan("op");
        span->End();
    }

    ASSERT_EQ(provider->ForceFlush(kTimeout), mt::Status::Completed);

    ASSERT_EQ(captured->received_batches.size(), std::size_t{1});
    EXPECT_EQ(captured->received_batches[0].Scope().name, "scope.x");
    EXPECT_EQ(captured->received_batches[0].Scope().version, "1.2");
}

// ---------------------------------------------------------------------------
// Tracer outlives its Provider — issue #285. `Provider::GetTracer` documents
// the returned tracer as owned jointly with the provider, valid until both
// are gone. The provider's destructor shuts the pipeline down, so a tracer
// used afterwards must drop every span as `PostShutdown` rather than reach
// into freed provider state.
// ---------------------------------------------------------------------------

namespace
{

/// A provider on the real batching processor, so post-shutdown drops are
/// counted exactly where production counts them. @p out_diag is the provider's
/// diagnostics sink; reading it after the provider is gone is valid only
/// because a live tracer keeps the trace pipeline — sink included — alive,
/// which is the property under test.
std::unique_ptr<mts::SdkProvider> MakeBatchingProvider(mts::DiagnosticsCounters** out_diag)
{
    auto diagnostics = std::make_unique<mts::DiagnosticsCounters>();
    *out_diag = diagnostics.get();
    auto exporter = std::make_unique<mtm::FakeExporter>();
    auto processor =
        std::make_unique<mts::BatchSpanProcessor>(exporter.get(),
                                                  std::make_shared<const mt::Resource>(),
                                                  mt::BatchOptions{},
                                                  mt::MemoryLimitOptions{}.max_record_bytes,
                                                  mt::MemoryLimitOptions{}.max_total_queue_bytes,
                                                  diagnostics.get());
    return std::make_unique<mts::SdkProvider>(mts::SdkProviderArgs{
        .diagnostics = std::move(diagnostics),
        .encoder = nullptr,
        .auth = nullptr,
        .transport = std::make_unique<mtm::MockTransport>(),
        .codec = nullptr,
        .exporter = std::move(exporter),
        .batch_span_processor = processor.get(),
        .processor = std::move(processor),
        .resource = std::make_shared<mt::Resource>(),
        .sampler = mt::MakeAlwaysOnSampler(),
        .span_limits = {},
        .connect_opts = {},
    });
}

std::uint64_t PostShutdownDrops(const mts::DiagnosticsCounters& diag)
{
    return DropCount(diag.Snapshot(), mt::DropReason::PostShutdown);
}

}  // namespace

TEST(SdkProviderTest, TracerOutlivesProvider_SpansAreDroppedAndCounted)
{
    mts::DiagnosticsCounters* diag = nullptr;
    auto provider = MakeBatchingProvider(&diag);
    const std::shared_ptr<mt::Tracer> tracer = provider->GetTracer("outlives", "1.0");

    provider.reset();

    constexpr int kLateSpans = 3;
    for (int i = 0; i < kLateSpans; ++i)
    {
        auto span = tracer->StartSpan("late");
        span->SetAttribute("k", std::int64_t{1});
        span->End();
    }
    EXPECT_EQ(PostShutdownDrops(*diag), static_cast<std::uint64_t>(kLateSpans));
}

TEST(SdkProviderTest, TracerOutlivesProvider_AcrossExplicitShutdown)
{
    mts::DiagnosticsCounters* diag = nullptr;
    auto provider = MakeBatchingProvider(&diag);
    const std::shared_ptr<mt::Tracer> tracer = provider->GetTracer("outlives", "1.0");
    // Started while the pipeline is live, ended after the provider is gone:
    // the span must not borrow anything the provider frees either.
    auto in_flight = tracer->StartSpan("in-flight");

    ASSERT_EQ(provider->Shutdown(kTimeout), mt::Status::Completed);
    tracer->StartSpan("after-shutdown")->End();
    EXPECT_EQ(PostShutdownDrops(*diag), 1U);

    provider.reset();
    in_flight->End();
    tracer->StartSpan("after-destruction")->End();
    EXPECT_EQ(PostShutdownDrops(*diag), 3U);
}

// A zero-timeout Shutdown returns before the batch worker has exited, so the
// worker may still be exporting the queued span. The tracer then keeps the
// processor alive past the exporter; destruction must still wait for the
// worker before the exporter goes, or it exports into freed memory (ASAN).
TEST(SdkProviderTest, TracerOutlivesProvider_AfterTimedOutShutdown_WorkerIsJoined)
{
    mts::DiagnosticsCounters* diag = nullptr;
    auto provider = MakeBatchingProvider(&diag);
    const std::shared_ptr<mt::Tracer> tracer = provider->GetTracer("outlives", "1.0");
    tracer->StartSpan("queued")->End();

    EXPECT_EQ(provider->Shutdown(std::chrono::milliseconds{0}), mt::Status::TimedOut);
    provider.reset();

    tracer->StartSpan("late")->End();
    EXPECT_EQ(PostShutdownDrops(*diag), 1U);
}

namespace
{

/// One tracing thread's body: signal once it is tracing, then keep going.
void TraceInALoop(const std::shared_ptr<mt::Tracer>& tracer, std::atomic<int>& tracing)
{
    constexpr int kSpansPerThread = 2000;
    tracer->StartSpan("first")->End();
    tracing.fetch_add(1);
    for (int n = 0; n < kSpansPerThread; ++n)
    {
        tracer->StartSpan("hot")->End();
    }
}

}  // namespace

// Tracing threads race the provider's destruction, and the last tracer — on
// one of those threads — frees the pipeline. Meaningful under TSAN and ASAN:
// nothing may be torn down under a thread still using it.
TEST(SdkProviderTest, TracersOutliveProvider_ConcurrentDestructionIsSafe)
{
    constexpr int kThreads = 8;
    mts::DiagnosticsCounters* diag = nullptr;
    auto provider = MakeBatchingProvider(&diag);

    std::atomic<int> tracing{0};
    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int i = 0; i < kThreads; ++i)
    {
        threads.emplace_back(TraceInALoop, provider->GetTracer("racer"), std::ref(tracing));
    }
    // Destroy only once every thread is tracing, so the teardown lands
    // mid-loop rather than before the threads have started.
    while (tracing.load() < kThreads)
    {
        std::this_thread::yield();
    }
    provider.reset();
    for (auto& thread : threads)
    {
        thread.join();
    }
    SUCCEED();
}

// NOLINTEND(misc-const-correctness)
