// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#include "exporter/otlp_metric_exporter.hpp"

#include "microtel/error.hpp"
#include "microtel/status.hpp"

#include <cstddef>
#include <cstdint>
#include <exception>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace microtel::exporter
{

constexpr auto kDestructorShutdownTimeout = std::chrono::seconds(5);

// Named in the recorded error when a failed result carries no `Error`.
constexpr std::string_view kFailureStage = "metric export failed at wire codec";

OtlpMetricExporter::OtlpMetricExporter(internal::IMetricEncoder* encoder,
                                       internal::IWireCodec* codec,
                                       OtlpMetricExporterConfig config,
                                       internal::IDiagnosticsSink* diag,
                                       internal::ISteadyClock* clock) noexcept
    : m_encoder(encoder),
      m_codec(codec),
      m_config(std::move(config)),
      m_diag(diag),
      m_retry(m_config.retry_policy, diag, clock),
      m_worker([this] { WorkerLoop(); })
{
}

OtlpMetricExporter::~OtlpMetricExporter() noexcept
{
    (void)Shutdown(kDestructorShutdownTimeout);
}

internal::ExportResult OtlpMetricExporter::Export(internal::MetricBatchHandle&& batch) noexcept
{
    // Read before the move: after `push_back` consumes the batch there is
    // nothing left to count.
    const auto metric_count = static_cast<std::uint64_t>(batch.Metrics().size());
    const std::scoped_lock lock{m_mu};
    if (m_shutdown.load(std::memory_order_relaxed))
    {
        RecordDropped(DropReason::PostShutdown, metric_count);
        return internal::ExportResult::AlreadyShutDown;
    }
    if (m_queue.size() >= m_config.max_queue_size)
    {
        RecordDropped(DropReason::QueueFull, metric_count);
        return internal::ExportResult::Dropped;
    }
    try
    {
        m_queue.push_back(std::move(batch));
    }
    // See OtlpExporter::Export — noexcept frame, so the catch must be wide
    // enough that nothing escapes.
    catch (const std::exception&)
    {
        RecordDropped(DropReason::QueueFull, metric_count);
        return internal::ExportResult::Dropped;
    }
    m_flush.Accept();
    // notify_all, not notify_one: flushes wait on m_cv too, and one wake that
    // reached a flush instead of the worker would leave the batch queued.
    m_cv.notify_all();
    return internal::ExportResult::Success;
}

microtel::Status OtlpMetricExporter::ForceFlush(std::chrono::milliseconds timeout) noexcept
{
    // Waits for the batches accepted before this call, not for an empty
    // queue: under sustained load the queue never empties (ICP 0041, #484).
    std::unique_lock lock{m_mu};
    return m_flush.Wait(lock, m_cv, timeout).status;
}

microtel::Status OtlpMetricExporter::Shutdown(std::chrono::milliseconds timeout) noexcept
{
    FlushTracker::Outcome outcome;
    {
        std::unique_lock lock{m_mu};
        if (m_shutdown.exchange(true, std::memory_order_relaxed))
        {
            return microtel::Status::AlreadyShutDown;
        }
        outcome = m_flush.WaitAfter(lock, m_cv, timeout, [this]() noexcept { m_retry.Abort(); });
    }
    if (!outcome.in_time && m_config.on_shutdown_timeout)
    {
        // Before the join: the worker may be inside a Send only this wakes.
        m_config.on_shutdown_timeout();
    }
    if (m_worker.joinable())
    {
        m_worker.join();
    }
    return outcome.status;
}

void OtlpMetricExporter::ProcessBatches(std::vector<internal::MetricBatchHandle>& batches,
                                        std::vector<bool>& delivered)
{
    std::vector<internal::EncodedPayload> payloads;
    payloads.reserve(batches.size());
    for (const auto& batch : batches)
    {
        payloads.push_back(m_encoder->Encode(batch));
    }
    const auto results = m_codec->SendAll(std::move(payloads), m_config.export_deadline);
    // The fan-out counts as attempt 0. Exactly one outcome per batch:
    // intermediate retryable failures are attempts, not failed batches.
    // `at()`: a codec returning more results than batches throws into
    // DrainQueue's catch rather than reading past the end.
    for (std::size_t i = 0; i < results.size(); ++i)
    {
        const auto& batch = batches.at(i);
        delivered.push_back(m_retry.Settle(
            results.at(i),
            [this, &batch]
            { return m_codec->Send(m_encoder->Encode(batch), m_config.export_deadline); },
            kFailureStage));
    }
}

void OtlpMetricExporter::RecordDropped(DropReason reason, std::uint64_t n) noexcept
{
    if (m_diag != nullptr)
    {
        m_diag->RecordDrop(reason, n);
    }
}

void OtlpMetricExporter::RecordDrainFailure(std::string_view what) noexcept
{
    if (m_diag == nullptr)
    {
        return;
    }
    // No `DropReason` names "the encoder threw", and adding one is an ICP
    // (`docs/interfaces.md` §3.5), so the failed-batch counter and the last
    // error carry it — which is what an operator reads out of
    // `GetExporterHealth()`.
    m_diag->RecordBatchFailed(
        Error{.kind = Error::Kind::InternalFailure, .message = std::string{what}, .os_errno = 0});
}

void OtlpMetricExporter::DrainQueue(std::unique_lock<std::mutex>& lock) noexcept
{
    while (!m_queue.empty())
    {
        std::vector<internal::MetricBatchHandle> batches;
        while (!m_queue.empty())
        {
            batches.push_back(std::move(m_queue.front()));
            m_queue.pop_front();
        }
        lock.unlock();
        std::vector<bool> delivered;
        try
        {
            ProcessBatches(batches, delivered);
        }
        // The batches are gone either way — the worker is `noexcept` and there
        // is nowhere to put them — but a swallowed failure that nothing counts
        // leaves GetExporterHealth() reporting a clean pipeline (issue #224).
        catch (const std::exception& e)
        {
            RecordDrainFailure(e.what());
        }
        lock.lock();
        m_flush.Complete(batches.size(), delivered);
        m_cv.notify_all();
    }
}

void OtlpMetricExporter::WorkerLoop() noexcept
{
    while (true)
    {
        std::unique_lock lock{m_mu};
        m_cv.wait(lock,
                  [this]
                  { return !m_queue.empty() || m_shutdown.load(std::memory_order_relaxed); });

        DrainQueue(lock);

        if (m_shutdown.load(std::memory_order_relaxed))
        {
            break;
        }
    }

    // The worker is gone and every batch was drained: wake any waiter.
    const std::scoped_lock lock{m_mu};
    m_cv.notify_all();
}

}  // namespace microtel::exporter
