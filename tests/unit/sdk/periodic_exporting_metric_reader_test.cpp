// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0
//
// Behavioural tests for PeriodicExportingMetricReader — the IMetricReader impl
// that drives collect+export on a background thread at a configurable interval.
//
// Contract under test:
//  - Collect() immediately does one synchronous collect+export cycle.
//  - ForceFlush() immediately does one synchronous collect+export cycle.
//  - Background thread fires at the configured interval.
//  - Shutdown() stops the background thread and delegates to the exporter.
//  - Collect() / ForceFlush() after Shutdown() return AlreadyShutDown.
//  - Double Shutdown() returns AlreadyShutDown on the second call.

#include "sdk/periodic_exporting_metric_reader.hpp"

#include "microtel/internal/metric_batch.hpp"
#include "microtel/internal/metric_exporter.hpp"
#include "microtel/internal/metric_producer.hpp"
#include "microtel/resource.hpp"
#include "microtel/status.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace mts = microtel::sdk;
namespace mti = microtel::internal;
namespace mt = microtel;

using namespace std::chrono_literals;

namespace
{

// ── Thread-safe fakes ─────────────────────────────────────────────────────────
// Counters are atomic so the background thread and test thread can read/write
// concurrently without a data race.

class FakeMetricProducer : public mti::IMetricProducer
{
public:
    explicit FakeMetricProducer(std::vector<mti::MetricBatchHandle> handles = {}) noexcept
        : m_handles(std::move(handles))
    {
    }

    [[nodiscard]] std::vector<mti::MetricBatchHandle> Collect(
        mti::AggregationTemporality temporality = mti::AggregationTemporality::Cumulative) override
    {
        m_collect_count.fetch_add(1, std::memory_order_relaxed);
        m_last_temporality.store(static_cast<int>(temporality), std::memory_order_relaxed);
        return std::move(m_handles);
    }

    [[nodiscard]] int CollectCount() const noexcept
    {
        return m_collect_count.load(std::memory_order_relaxed);
    }

    [[nodiscard]] mti::AggregationTemporality LastTemporality() const noexcept
    {
        return static_cast<mti::AggregationTemporality>(
            m_last_temporality.load(std::memory_order_relaxed));
    }

private:
    std::vector<mti::MetricBatchHandle> m_handles;
    std::atomic<int> m_collect_count{0};
    std::atomic<int> m_last_temporality{static_cast<int>(mti::AggregationTemporality::Cumulative)};
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
        m_export_count.fetch_add(1, std::memory_order_relaxed);
        return m_export_result;
    }

    [[nodiscard]] mt::Status ForceFlush(std::chrono::milliseconds /*timeout*/) noexcept override
    {
        m_flush_count.fetch_add(1, std::memory_order_relaxed);
        return m_flush_result;
    }

    [[nodiscard]] mt::Status Shutdown(std::chrono::milliseconds /*timeout*/) noexcept override
    {
        m_shutdown_count.fetch_add(1, std::memory_order_relaxed);
        return m_shutdown_result;
    }

    [[nodiscard]] int ExportCount() const noexcept
    {
        return m_export_count.load(std::memory_order_relaxed);
    }
    [[nodiscard]] int FlushCount() const noexcept
    {
        return m_flush_count.load(std::memory_order_relaxed);
    }
    [[nodiscard]] int ShutdownCount() const noexcept
    {
        return m_shutdown_count.load(std::memory_order_relaxed);
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
    mt::Status m_flush_result{mt::Status::Completed};
    mt::Status m_shutdown_result{mt::Status::Completed};
    std::atomic<int> m_export_count{0};
    std::atomic<int> m_flush_count{0};
    std::atomic<int> m_shutdown_count{0};
};

}  // namespace

// ── Collect — happy path ──────────────────────────────────────────────────────

TEST(PeriodicExportingMetricReaderTest, CollectCallsProducerOnce)
{
    FakeMetricProducer producer;
    FakeMetricExporter exporter;
    mts::PeriodicExportingMetricReader reader{producer, exporter, 60'000ms};

    EXPECT_EQ(reader.Collect(100ms), mt::Status::Completed);
    // Background thread has not fired yet (60s interval).
    EXPECT_GE(producer.CollectCount(), 1);  // at least 1 from Collect()
}

TEST(PeriodicExportingMetricReaderTest, CollectReturnsCompleted)
{
    FakeMetricProducer producer;
    FakeMetricExporter exporter;
    mts::PeriodicExportingMetricReader reader{producer, exporter, 60'000ms};

    EXPECT_EQ(reader.Collect(100ms), mt::Status::Completed);
}

TEST(PeriodicExportingMetricReaderTest, CollectReturnsFailedWhenExportFails)
{
    std::vector<mti::MetricBatchHandle> handles;
    handles.push_back(mti::MetricBatchHandle{
        {},
        std::make_shared<const mt::Resource>(),
        mti::InstrumentationScope{.name = "lib", .version = "1.0"},
    });
    FakeMetricProducer producer{std::move(handles)};
    FakeMetricExporter exporter{mti::ExportResult::Failure};
    mts::PeriodicExportingMetricReader reader{producer, exporter, 60'000ms};

    EXPECT_EQ(reader.Collect(100ms), mt::Status::Failed);
}

// ── ForceFlush ────────────────────────────────────────────────────────────────

// ICP 0041: the flush covers its own collection, so a collection the exporter
// refused to queue fails it — the exporter is still flushed.
TEST(PeriodicExportingMetricReaderTest, ForceFlushReturnsFailedWhenItsCollectionIsDropped)
{
    std::vector<mti::MetricBatchHandle> handles;
    handles.push_back(mti::MetricBatchHandle{
        {},
        std::make_shared<const mt::Resource>(),
        mti::InstrumentationScope{.name = "lib", .version = "1.0"},
    });
    FakeMetricProducer producer{std::move(handles)};
    FakeMetricExporter exporter{mti::ExportResult::Dropped};
    mts::PeriodicExportingMetricReader reader{producer, exporter, 60'000ms};

    EXPECT_EQ(reader.ForceFlush(100ms), mt::Status::Failed);
    EXPECT_EQ(exporter.FlushCount(), 1);
}

TEST(PeriodicExportingMetricReaderTest, ForceFlushReturnsTheExportersStatus)
{
    FakeMetricProducer producer;
    FakeMetricExporter exporter;
    exporter.SetFlushResult(mt::Status::Failed);
    mts::PeriodicExportingMetricReader reader{producer, exporter, 60'000ms};

    EXPECT_EQ(reader.ForceFlush(100ms), mt::Status::Failed);
}

TEST(PeriodicExportingMetricReaderTest, ForceFlushCallsProducerAndExporter)
{
    FakeMetricProducer producer;
    FakeMetricExporter exporter;
    mts::PeriodicExportingMetricReader reader{producer, exporter, 60'000ms};

    EXPECT_EQ(reader.ForceFlush(100ms), mt::Status::Completed);
    EXPECT_GE(producer.CollectCount(), 1);
    // ForceFlush also delegates to exporter.ForceFlush
    EXPECT_GE(exporter.FlushCount(), 1);
}

TEST(PeriodicExportingMetricReaderTest, ForceFlushPropagatesExporterResult)
{
    FakeMetricProducer producer;
    FakeMetricExporter exporter;
    exporter.SetFlushResult(mt::Status::TimedOut);
    mts::PeriodicExportingMetricReader reader{producer, exporter, 60'000ms};

    EXPECT_EQ(reader.ForceFlush(100ms), mt::Status::TimedOut);
}

// ── Shutdown ──────────────────────────────────────────────────────────────────

TEST(PeriodicExportingMetricReaderTest, ShutdownDelegatesToExporter)
{
    FakeMetricProducer producer;
    FakeMetricExporter exporter;
    mts::PeriodicExportingMetricReader reader{producer, exporter, 60'000ms};

    EXPECT_EQ(reader.Shutdown(100ms), mt::Status::Completed);
    EXPECT_EQ(exporter.ShutdownCount(), 1);
}

TEST(PeriodicExportingMetricReaderTest, ShutdownPropagatesExporterResult)
{
    FakeMetricProducer producer;
    FakeMetricExporter exporter;
    exporter.SetShutdownResult(mt::Status::Failed);
    mts::PeriodicExportingMetricReader reader{producer, exporter, 60'000ms};

    EXPECT_EQ(reader.Shutdown(100ms), mt::Status::Failed);
}

TEST(PeriodicExportingMetricReaderTest, DoubleShutdownReturnsAlreadyShutDown)
{
    FakeMetricProducer producer;
    FakeMetricExporter exporter;
    mts::PeriodicExportingMetricReader reader{producer, exporter, 60'000ms};

    EXPECT_EQ(reader.Shutdown(100ms), mt::Status::Completed);
    EXPECT_EQ(reader.Shutdown(100ms), mt::Status::AlreadyShutDown);
    EXPECT_EQ(exporter.ShutdownCount(), 1);  // called only once
}

// ── Post-shutdown guards ──────────────────────────────────────────────────────

TEST(PeriodicExportingMetricReaderTest, CollectAfterShutdownReturnsAlreadyShutDown)
{
    FakeMetricProducer producer;
    FakeMetricExporter exporter;
    mts::PeriodicExportingMetricReader reader{producer, exporter, 60'000ms};

    (void)reader.Shutdown(100ms);
    EXPECT_EQ(reader.Collect(100ms), mt::Status::AlreadyShutDown);
}

TEST(PeriodicExportingMetricReaderTest, ForceFlushAfterShutdownReturnsAlreadyShutDown)
{
    FakeMetricProducer producer;
    FakeMetricExporter exporter;
    mts::PeriodicExportingMetricReader reader{producer, exporter, 60'000ms};

    (void)reader.Shutdown(100ms);
    EXPECT_EQ(reader.ForceFlush(100ms), mt::Status::AlreadyShutDown);
}

// ── Background thread fires at interval ──────────────────────────────────────

TEST(PeriodicExportingMetricReaderTest, BackgroundThreadFiresAtConfiguredInterval)
{
    FakeMetricProducer producer;
    FakeMetricExporter exporter;
    // Very short interval so the background thread fires quickly in CI.
    // NOLINTNEXTLINE(misc-const-correctness) — Shutdown() is non-const; cf18 false positive
    mts::PeriodicExportingMetricReader reader{producer, exporter, 5ms};

    // Wait long enough for at least 3 background cycles.
    std::this_thread::sleep_for(50ms);
    (void)reader.Shutdown(500ms);

    EXPECT_GE(producer.CollectCount(), 3);
}

// ── Destructor shuts down cleanly ─────────────────────────────────────────────

TEST(PeriodicExportingMetricReaderTest, DestructorShutdownDoesNotCrash)
{
    FakeMetricProducer producer;
    FakeMetricExporter exporter;
    {
        // NOLINTNEXTLINE(misc-const-correctness) — dtor is non-const; cf18 false positive
        mts::PeriodicExportingMetricReader reader{producer, exporter, 60'000ms};
        // Destructor runs here — must not crash or block indefinitely.
    }
}

// ── Temporality threading ─────────────────────────────────────────────────────

TEST(PeriodicExportingMetricReaderTest, Collect_DefaultTemporality_PassesCumulativeToProducer)
{
    FakeMetricProducer producer;
    FakeMetricExporter exporter;
    mts::PeriodicExportingMetricReader reader{
        producer, exporter, 60'000ms, mti::AggregationTemporality::Cumulative};

    EXPECT_EQ(reader.Collect(100ms), mt::Status::Completed);
    EXPECT_EQ(producer.LastTemporality(), mti::AggregationTemporality::Cumulative);
}

TEST(PeriodicExportingMetricReaderTest, Collect_WithDeltaTemporality_PassesDeltaToProducer)
{
    FakeMetricProducer producer;
    FakeMetricExporter exporter;
    mts::PeriodicExportingMetricReader reader{
        producer, exporter, 60'000ms, mti::AggregationTemporality::Delta};

    EXPECT_EQ(reader.Collect(100ms), mt::Status::Completed);
    EXPECT_EQ(producer.LastTemporality(), mti::AggregationTemporality::Delta);
}

// ── Serialization after the nested-lock fix ──────────────────────────────────
//
// DoCollectExport used to hold m_collect_mu for the whole cycle, which nested
// it over MetricProducer's lock and the exporter's lock — the thing
// threading-model.md §4 marks LOCKED against. The mutex is now held only to
// claim and release the cycle. These tests pin the property that made the
// mutex worth having in the first place: one interval's deltas must never be
// split across two overlapping exports.

namespace
{

/// Producer that reports whether two collect+export cycles ever overlap.
class OverlapDetectingProducer : public mti::IMetricProducer
{
public:
    [[nodiscard]] std::vector<mti::MetricBatchHandle> Collect(
        mti::AggregationTemporality /*temporality*/ =
            mti::AggregationTemporality::Cumulative) override
    {
        if (m_inside.exchange(true, std::memory_order_acq_rel))
        {
            m_overlapped.store(true, std::memory_order_relaxed);
        }
        // Widen the window so an unserialized caller reliably collides.
        std::this_thread::sleep_for(2ms);
        m_inside.store(false, std::memory_order_release);
        m_cycles.fetch_add(1, std::memory_order_relaxed);
        return {};
    }

    [[nodiscard]] bool Overlapped() const noexcept
    {
        return m_overlapped.load(std::memory_order_relaxed);
    }
    [[nodiscard]] int Cycles() const noexcept
    {
        return m_cycles.load(std::memory_order_relaxed);
    }

private:
    std::atomic<bool> m_inside{false};
    std::atomic<bool> m_overlapped{false};
    std::atomic<int> m_cycles{0};
};

constexpr int kOverlapThreads = 8;
constexpr int kCallsPerThread = 10;

void CollectRepeatedly(mts::PeriodicExportingMetricReader& reader)
{
    for (int i = 0; i < kCallsPerThread; ++i)
    {
        (void)reader.Collect(100ms);
    }
}

}  // namespace

TEST(PeriodicExportingMetricReaderTest, ConcurrentCollectsNeverOverlap)
{
    OverlapDetectingProducer producer;
    FakeMetricExporter exporter;
    // Long interval: the background thread must not contribute cycles, so any
    // overlap is attributable to the concurrent callers below.
    mts::PeriodicExportingMetricReader reader{producer, exporter, 1h};

    std::vector<std::thread> threads;
    threads.reserve(kOverlapThreads);
    for (int i = 0; i < kOverlapThreads; ++i)
    {
        threads.emplace_back(CollectRepeatedly, std::ref(reader));
    }
    for (auto& t : threads)
    {
        t.join();
    }

    EXPECT_FALSE(producer.Overlapped()) << "two collect+export cycles ran concurrently";
    EXPECT_EQ(producer.Cycles(), kOverlapThreads * kCallsPerThread);
    ASSERT_EQ(reader.Shutdown(1s), mt::Status::Completed);
}

TEST(PeriodicExportingMetricReaderTest, CollectAndForceFlushDoNotOverlap)
{
    OverlapDetectingProducer producer;
    FakeMetricExporter exporter;
    mts::PeriodicExportingMetricReader reader{producer, exporter, 1h};

    std::thread collector(CollectRepeatedly, std::ref(reader));
    for (int i = 0; i < kCallsPerThread; ++i)
    {
        (void)reader.ForceFlush(100ms);
    }
    collector.join();

    EXPECT_FALSE(producer.Overlapped());
    ASSERT_EQ(reader.Shutdown(1s), mt::Status::Completed);
}

// ── SetInterval — ICP 0026 hot reload ────────────────────────────────────────

TEST(PeriodicExportingMetricReaderTest, SetIntervalDoesNotForceACollection)
{
    FakeMetricProducer producer;
    FakeMetricExporter exporter;
    mts::PeriodicExportingMetricReader reader{producer, exporter, 1h};

    reader.SetInterval(1h);
    std::this_thread::sleep_for(150ms);

    // m_wake is deliberately not set: retuning the cadence is not "export now"
    // (ICP 0026 §4). Nothing has asked for a cycle, so none has run.
    EXPECT_EQ(producer.CollectCount(), 0);
    ASSERT_EQ(reader.Shutdown(1s), mt::Status::Completed);
}

TEST(PeriodicExportingMetricReaderTest, SetIntervalShortensTheCadence)
{
    FakeMetricProducer producer;
    FakeMetricExporter exporter;
    mts::PeriodicExportingMetricReader reader{producer, exporter, 200ms};

    reader.SetInterval(2ms);

    // The wait already in flight keeps its 200 ms deadline, so the new cadence
    // costs at most one old-length tick. Well inside the deadline below, 2 ms
    // ticks reach a count the 200 ms one could not.
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (producer.CollectCount() < 30 && std::chrono::steady_clock::now() < deadline)
    {
        std::this_thread::sleep_for(2ms);
    }
    EXPECT_GE(producer.CollectCount(), 30);
    ASSERT_EQ(reader.Shutdown(1s), mt::Status::Completed);
}

TEST(PeriodicExportingMetricReaderTest, SetIntervalLengthensAfterAtMostOneOldTick)
{
    FakeMetricProducer producer;
    FakeMetricExporter exporter;
    mts::PeriodicExportingMetricReader reader{producer, exporter, 20ms};

    std::this_thread::sleep_for(60ms);  // let the short cadence establish
    reader.SetInterval(1h);
    const int baseline = producer.CollectCount();

    // At 20 ms this window would be worth a dozen cycles; the lengthened
    // interval costs at most the tick already in flight, plus the cycle that
    // may have been running when the setter returned.
    std::this_thread::sleep_for(400ms);
    EXPECT_LE(producer.CollectCount() - baseline, 2);

    ASSERT_EQ(reader.Shutdown(1s), mt::Status::Completed);
}
