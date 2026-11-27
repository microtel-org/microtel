// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0
//
// Behavioural tests for SynchronousMetricReader — the IMetricReader impl
// that drives a synchronous collect + export cycle on demand.
//
// Contract under test:
//  - Collect() calls IMetricProducer::Collect() once per invocation.
//  - Each MetricBatchHandle returned by the producer is forwarded to
//    IMetricExporter::Export() in a separate call.
//  - An empty producer results in zero Export() calls.
//  - Collect() returns Status::Completed when all exports succeed.
//  - Collect() returns Status::Failed when any export returns Failure.
//  - ForceFlush() delegates to IMetricExporter::ForceFlush().
//  - Shutdown() delegates to IMetricExporter::Shutdown().
//  - Collect() / ForceFlush() after Shutdown() return AlreadyShutDown.

#include "sdk/synchronous_metric_reader.hpp"

#include "microtel/internal/metric_batch.hpp"
#include "microtel/internal/metric_exporter.hpp"
#include "microtel/internal/metric_producer.hpp"
#include "microtel/resource.hpp"
#include "microtel/status.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace mts = microtel::sdk;
namespace mti = microtel::internal;
namespace mt = microtel;

using namespace std::chrono_literals;

namespace
{

// ── Fakes ────────────────────────────────────────────────────────────────────

class FakeMetricProducer : public mti::IMetricProducer
{
public:
    explicit FakeMetricProducer(std::vector<mti::MetricBatchHandle> handles = {}) noexcept
        : m_handles(std::move(handles))
    {
    }

    [[nodiscard]] std::vector<mti::MetricBatchHandle> Collect(
        const mti::CollectRequest& request = {}) override
    {
        ++m_collect_count;
        m_last_selector = request.selector;
        return std::move(m_handles);
    }

    [[nodiscard]] int CollectCount() const noexcept
    {
        return m_collect_count;
    }

    /// The selector of the last `Collect` request.
    [[nodiscard]] mti::TemporalitySelector LastSelector() const noexcept
    {
        return m_last_selector;
    }

private:
    std::vector<mti::MetricBatchHandle> m_handles;
    int m_collect_count = 0;
    // Not Cumulative(), so a test can tell a recorded request from the initial value.
    mti::TemporalitySelector m_last_selector{mti::AggregationTemporality::Delta};
};

class FakeMetricExporter : public mti::IMetricExporter
{
public:
    explicit FakeMetricExporter(
        mti::ExportResult export_result = mti::ExportResult::Success) noexcept
        : m_export_result(export_result)
    {
    }

    // NOLINTNEXTLINE(cppcoreguidelines-rvalue-reference-param-not-moved)
    [[nodiscard]] mti::ExportResult Export(mti::MetricBatchHandle&& /*batch*/) noexcept override
    {
        ++m_export_count;
        return m_export_result;
    }

    [[nodiscard]] mt::Status ForceFlush(std::chrono::milliseconds /*timeout*/) noexcept override
    {
        ++m_flush_count;
        return m_flush_result;
    }

    [[nodiscard]] mt::Status Shutdown(std::chrono::milliseconds /*timeout*/) noexcept override
    {
        ++m_shutdown_count;
        return m_shutdown_result;
    }

    [[nodiscard]] int ExportCount() const noexcept
    {
        return m_export_count;
    }
    [[nodiscard]] int FlushCount() const noexcept
    {
        return m_flush_count;
    }
    [[nodiscard]] int ShutdownCount() const noexcept
    {
        return m_shutdown_count;
    }

    void SetExportResult(mti::ExportResult r) noexcept
    {
        m_export_result = r;
    }
    void SetFlushResult(mt::Status s) noexcept
    {
        m_flush_result = s;
    }
    void SetShutdownResult(mt::Status s) noexcept
    {
        m_shutdown_result = s;
    }

private:
    mti::ExportResult m_export_result;
    mt::Status m_flush_result = mt::Status::Completed;
    mt::Status m_shutdown_result = mt::Status::Completed;
    int m_export_count = 0;
    int m_flush_count = 0;
    int m_shutdown_count = 0;
};

// ── Helpers ───────────────────────────────────────────────────────────────────

mti::MetricBatchHandle MakeHandle(std::string scope_name = "lib")
{
    return mti::MetricBatchHandle{
        /*metrics=*/{},
        std::make_shared<const mt::Resource>(),
        mti::InstrumentationScope{.name = std::move(scope_name), .version = "1.0"},
    };
}

}  // namespace

// ── Collect — happy path ──────────────────────────────────────────────────────

TEST(SynchronousMetricReaderTest, CollectCallsProducerOncePerInvocation)
{
    FakeMetricProducer producer;
    FakeMetricExporter exporter;
    mts::SynchronousMetricReader reader{producer, exporter};

    EXPECT_EQ(reader.Collect(100ms), mt::Status::Completed);
    EXPECT_EQ(producer.CollectCount(), 1);

    EXPECT_EQ(reader.Collect(100ms), mt::Status::Completed);
    EXPECT_EQ(producer.CollectCount(), 2);
}

TEST(SynchronousMetricReaderTest, CollectSendsTheDefaultRequest)
{
    // ICP 0045: the synchronous reader has no preference of its own, so it
    // collects with the default request — every kind cumulative.
    FakeMetricProducer producer;
    FakeMetricExporter exporter;
    mts::SynchronousMetricReader reader{producer, exporter};

    EXPECT_EQ(reader.Collect(100ms), mt::Status::Completed);
    EXPECT_EQ(producer.LastSelector(), mti::TemporalitySelector::Cumulative());
}

TEST(SynchronousMetricReaderTest, EmptyProducerExportsNothing)
{
    FakeMetricProducer producer;
    FakeMetricExporter exporter;
    mts::SynchronousMetricReader reader{producer, exporter};

    EXPECT_EQ(reader.Collect(100ms), mt::Status::Completed);
    EXPECT_EQ(exporter.ExportCount(), 0);
}

TEST(SynchronousMetricReaderTest, OneHandleYieldsOneExportCall)
{
    std::vector<mti::MetricBatchHandle> handles;
    handles.push_back(MakeHandle("lib-a"));
    FakeMetricProducer producer{std::move(handles)};
    FakeMetricExporter exporter;
    mts::SynchronousMetricReader reader{producer, exporter};

    EXPECT_EQ(reader.Collect(100ms), mt::Status::Completed);
    EXPECT_EQ(exporter.ExportCount(), 1);
}

TEST(SynchronousMetricReaderTest, TwoHandlesYieldTwoExportCalls)
{
    std::vector<mti::MetricBatchHandle> handles;
    handles.push_back(MakeHandle("lib-a"));
    handles.push_back(MakeHandle("lib-b"));
    FakeMetricProducer producer{std::move(handles)};
    FakeMetricExporter exporter;
    mts::SynchronousMetricReader reader{producer, exporter};

    EXPECT_EQ(reader.Collect(100ms), mt::Status::Completed);
    EXPECT_EQ(exporter.ExportCount(), 2);
}

TEST(SynchronousMetricReaderTest, CollectReturnsCompletedWhenAllExportsSucceed)
{
    std::vector<mti::MetricBatchHandle> handles;
    handles.push_back(MakeHandle());
    FakeMetricProducer producer{std::move(handles)};
    FakeMetricExporter exporter{mti::ExportResult::Success};
    mts::SynchronousMetricReader reader{producer, exporter};

    EXPECT_EQ(reader.Collect(100ms), mt::Status::Completed);
}

TEST(SynchronousMetricReaderTest, CollectReturnsFailedWhenExporterFails)
{
    std::vector<mti::MetricBatchHandle> handles;
    handles.push_back(MakeHandle());
    FakeMetricProducer producer{std::move(handles)};
    FakeMetricExporter exporter{mti::ExportResult::Failure};
    mts::SynchronousMetricReader reader{producer, exporter};

    EXPECT_EQ(reader.Collect(100ms), mt::Status::Failed);
}

// ── ForceFlush / Shutdown ─────────────────────────────────────────────────────

TEST(SynchronousMetricReaderTest, ForceFlushDelegatesToExporter)
{
    FakeMetricProducer producer;
    FakeMetricExporter exporter;
    mts::SynchronousMetricReader reader{producer, exporter};

    EXPECT_EQ(reader.ForceFlush(100ms), mt::Status::Completed);
    EXPECT_EQ(exporter.FlushCount(), 1);
}

TEST(SynchronousMetricReaderTest, ForceFlushPropagatesExporterResult)
{
    FakeMetricProducer producer;
    FakeMetricExporter exporter;
    exporter.SetFlushResult(mt::Status::TimedOut);
    mts::SynchronousMetricReader reader{producer, exporter};

    EXPECT_EQ(reader.ForceFlush(100ms), mt::Status::TimedOut);
}

TEST(SynchronousMetricReaderTest, ShutdownDelegatesToExporter)
{
    FakeMetricProducer producer;
    FakeMetricExporter exporter;
    mts::SynchronousMetricReader reader{producer, exporter};

    EXPECT_EQ(reader.Shutdown(100ms), mt::Status::Completed);
    EXPECT_EQ(exporter.ShutdownCount(), 1);
}

TEST(SynchronousMetricReaderTest, ShutdownPropagatesExporterResult)
{
    FakeMetricProducer producer;
    FakeMetricExporter exporter;
    exporter.SetShutdownResult(mt::Status::Failed);
    mts::SynchronousMetricReader reader{producer, exporter};

    EXPECT_EQ(reader.Shutdown(100ms), mt::Status::Failed);
}

// ── Post-shutdown guards ──────────────────────────────────────────────────────

TEST(SynchronousMetricReaderTest, CollectAfterShutdownReturnsAlreadyShutDown)
{
    FakeMetricProducer producer;
    FakeMetricExporter exporter;
    mts::SynchronousMetricReader reader{producer, exporter};

    (void)reader.Shutdown(100ms);
    EXPECT_EQ(reader.Collect(100ms), mt::Status::AlreadyShutDown);
    // producer and exporter must not be called again
    EXPECT_EQ(producer.CollectCount(), 0);
}

TEST(SynchronousMetricReaderTest, ForceFlushAfterShutdownReturnsAlreadyShutDown)
{
    FakeMetricProducer producer;
    FakeMetricExporter exporter;
    mts::SynchronousMetricReader reader{producer, exporter};

    (void)reader.Shutdown(100ms);
    EXPECT_EQ(reader.ForceFlush(100ms), mt::Status::AlreadyShutDown);
    EXPECT_EQ(exporter.FlushCount(), 0);
}

TEST(SynchronousMetricReaderTest, DoubleShutdownReturnsAlreadyShutDown)
{
    FakeMetricProducer producer;
    FakeMetricExporter exporter;
    mts::SynchronousMetricReader reader{producer, exporter};

    EXPECT_EQ(reader.Shutdown(100ms), mt::Status::Completed);
    EXPECT_EQ(reader.Shutdown(100ms), mt::Status::AlreadyShutDown);
    EXPECT_EQ(exporter.ShutdownCount(), 1);  // called only once
}
