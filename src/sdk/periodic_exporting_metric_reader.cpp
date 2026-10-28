// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#include "sdk/periodic_exporting_metric_reader.hpp"

#include "microtel/internal/metric_batch.hpp"

#include <algorithm>
#include <chrono>
#include <exception>
#include <vector>

namespace microtel::sdk
{

PeriodicExportingMetricReader::PeriodicExportingMetricReader(
    internal::IMetricProducer& producer,
    internal::IMetricExporter& exporter,
    std::chrono::milliseconds interval,
    internal::AggregationTemporality temporality) noexcept
    : m_producer(producer), m_exporter(exporter), m_interval(interval), m_temporality(temporality)
{
    m_thread = std::thread{[this] { RunLoop(); }};
}

PeriodicExportingMetricReader::~PeriodicExportingMetricReader() noexcept
{
    if (!m_shut_down.load(std::memory_order_acquire))
    {
        (void)Shutdown(std::chrono::milliseconds{5'000});
    }
}

microtel::Status PeriodicExportingMetricReader::Collect(
    std::chrono::milliseconds /*timeout*/) noexcept
{
    if (m_shut_down.load(std::memory_order_acquire))
    {
        return microtel::Status::AlreadyShutDown;
    }
    return DoCollectExport();
}

microtel::Status PeriodicExportingMetricReader::ForceFlush(
    std::chrono::milliseconds timeout) noexcept
{
    if (m_shut_down.load(std::memory_order_acquire))
    {
        return microtel::Status::AlreadyShutDown;
    }
    // One collection, then a flush that covers it: the exporter waits for the
    // batches queued before its call, which end with this collection's
    // (ICP 0041). Both share the caller's one deadline.
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    const microtel::Status collected = DoCollectExport();
    const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
        deadline - std::chrono::steady_clock::now());
    const microtel::Status flushed =
        m_exporter.ForceFlush(std::max(left, std::chrono::milliseconds{0}));
    // A collection that could not be queued is a flush that did not export
    // what it was asked to.
    return collected == microtel::Status::Failed ? microtel::Status::Failed : flushed;
}

microtel::Status PeriodicExportingMetricReader::Shutdown(std::chrono::milliseconds timeout) noexcept
{
    if (m_shut_down.exchange(true, std::memory_order_acq_rel))
    {
        return microtel::Status::AlreadyShutDown;
    }
    {
        const std::scoped_lock lk{m_mu};
        m_wake = true;
    }
    m_cv.notify_all();
    if (m_thread.joinable())
    {
        m_thread.join();
    }
    return m_exporter.Shutdown(timeout);
}

void PeriodicExportingMetricReader::SetInterval(std::chrono::milliseconds interval) noexcept
{
    const std::scoped_lock lk{m_mu};
    m_interval = interval;
    // No m_wake, no notify: see the header. A retune is not a flush.
}

void PeriodicExportingMetricReader::RunLoop() noexcept
{
    while (!m_shut_down.load(std::memory_order_acquire))
    {
        std::unique_lock<std::mutex> lk{m_mu};
        m_cv.wait_for(lk, m_interval, [this] { return m_wake; });
        m_wake = false;
        lk.unlock();
        (void)DoCollectExport();
    }
}

namespace
{

/// @brief Holds the right to run one collect+export cycle.
///
/// Waits for any in-flight cycle, claims the slot, and releases it on scope
/// exit — including the early `return`s below, which is why this is a guard
/// rather than two bare flag assignments. `m_collect_mu` is only ever held
/// inside this type's constructor and destructor, so it never overlaps the
/// producer's or the exporter's locks.
class CollectSlot
{
public:
    CollectSlot(std::mutex& mu, std::condition_variable& cv, bool& busy) noexcept
        : m_mu(mu), m_cv(cv), m_busy(busy)
    {
        std::unique_lock lk{m_mu};
        m_cv.wait(lk, [this] { return !m_busy; });
        m_busy = true;
    }

    ~CollectSlot() noexcept
    {
        {
            const std::scoped_lock lk{m_mu};
            m_busy = false;
        }
        m_cv.notify_one();
    }

    CollectSlot(const CollectSlot&) = delete;
    CollectSlot& operator=(const CollectSlot&) = delete;
    CollectSlot(CollectSlot&&) = delete;
    CollectSlot& operator=(CollectSlot&&) = delete;

private:
    std::mutex& m_mu;
    std::condition_variable& m_cv;
    bool& m_busy;
};

}  // namespace

microtel::Status PeriodicExportingMetricReader::DoCollectExport() noexcept
{
    const CollectSlot slot{m_collect_mu, m_collect_cv, m_collecting};

    std::vector<internal::MetricBatchHandle> handles;
    try
    {
        handles = m_producer.Collect(m_temporality);
    }
    catch (const std::exception&)
    {
        return microtel::Status::Failed;
    }
    microtel::Status status = microtel::Status::Completed;
    for (auto& handle : handles)
    {
        const internal::ExportResult result = m_exporter.Export(std::move(handle));
        if (result == internal::ExportResult::Failure)
        {
            return microtel::Status::Failed;
        }
        // Refused (queue full, or shut down) and already counted as a drop by
        // the exporter. The rest of the collection is still offered.
        if (result != internal::ExportResult::Success)
        {
            status = microtel::Status::Failed;
        }
    }
    return status;
}

}  // namespace microtel::sdk
