// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#include "exporter/otlp_exporter.hpp"

#include "microtel/error.hpp"
#include "microtel/internal/batch.hpp"
#include "microtel/status.hpp"

#include "exporter/retry_engine.hpp"
#include "wire/encoder/trace_request_concat.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace microtel::exporter
{

OtlpExporter::OtlpExporter(internal::IOtlpEncoder* encoder,
                           internal::IWireCodec* codec,
                           OtlpExporterConfig config,
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

// Maximum time the destructor waits for the worker to drain on implicit shutdown.
constexpr auto kDestructorShutdownTimeout = std::chrono::seconds(5);

// Named in the recorded error when a failed result carries no `Error`.
constexpr std::string_view kFailureStage = "export failed at wire codec";

OtlpExporter::~OtlpExporter() noexcept
{
    (void)Shutdown(kDestructorShutdownTimeout);
}

internal::ExportResult OtlpExporter::Export(internal::BatchHandle&& batch) noexcept
{
    const internal::ExportResult result = [this, &batch]
    {
        const std::scoped_lock lock{m_mu};
        return EnqueueLocked(std::move(batch));
    }();
    if (result == internal::ExportResult::Success)
    {
        // notify_all, not notify_one: flushes wait on m_cv too, and one wake
        // that reached a flush instead of the worker would leave it queued.
        m_cv.notify_all();
    }
    return result;
}

void OtlpExporter::ExportGroup(std::vector<internal::BatchHandle>&& batches) noexcept
{
    std::vector<internal::BatchHandle> group = std::move(batches);
    bool queued = false;
    {
        // One lock for the whole group, so the worker cannot wake between two
        // of its batches and send the first alone (design §3.6.1).
        const std::scoped_lock lock{m_mu};
        for (auto& batch : group)
        {
            queued = (EnqueueLocked(std::move(batch)) == internal::ExportResult::Success) || queued;
        }
    }
    if (queued)
    {
        // notify_all, not notify_one: flushes wait on m_cv too, and one wake
        // that reached a flush instead of the worker would leave it queued.
        m_cv.notify_all();
    }
}

internal::ExportResult OtlpExporter::EnqueueLocked(internal::BatchHandle&& batch) noexcept
{
    // Read before the move: a rejected batch is still counted in spans, and
    // after `push_back` consumes it there is nothing left to count.
    const auto span_count = static_cast<std::uint64_t>(batch.Spans().size());
    if (m_shutdown.load(std::memory_order_relaxed))
    {
        RecordDropped(DropReason::PostShutdown, span_count);
        return internal::ExportResult::AlreadyShutDown;
    }
    // Both bounds: batches, and the spans they carry (issue #345). The span
    // budget is the one a fan-in drain meets, since it arrives as one small
    // batch per leaf.
    if (m_queue.size() >= m_config.max_queue_size ||
        m_queued_spans + span_count > m_config.max_queued_spans)
    {
        RecordDropped(DropReason::QueueFull, span_count);
        return internal::ExportResult::Dropped;
    }
    try
    {
        m_queue.push_back(std::move(batch));
        m_queued_spans += span_count;
        m_flush.Accept();
        PublishQueueDepth();
    }
    // std::exception, not std::bad_alloc: push_back can also throw
    // std::length_error, and Export is noexcept, so anything that escapes
    // terminates the host process rather than dropping one batch.
    catch (const std::exception&)
    {
        // Same loss as a full queue from the caller's side: the batch never
        // entered the pipeline because it would not fit.
        RecordDropped(DropReason::QueueFull, span_count);
        return internal::ExportResult::Dropped;
    }
    return internal::ExportResult::Success;
}

microtel::Status OtlpExporter::ForceFlush(std::chrono::milliseconds timeout) noexcept
{
    // Waits for the batches accepted before this call, not for an empty
    // queue: under sustained load the queue never empties (ICP 0041, #484).
    std::unique_lock lock{m_mu};
    return m_flush.Wait(lock, m_cv, timeout).status;
}

microtel::Status OtlpExporter::Shutdown(std::chrono::milliseconds timeout) noexcept
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

OtlpExporter::FlushProgress OtlpExporter::FlushProgressForTesting() noexcept
{
    const std::scoped_lock lock{m_mu};
    return FlushProgress{.processed = m_flush.Processed(), .waiting = m_flush.Waiting()};
}

void OtlpExporter::RecordDropped(DropReason reason, std::uint64_t n) noexcept
{
    if (m_diag != nullptr)
    {
        m_diag->RecordDrop(reason, n);
    }
}

void OtlpExporter::RecordDrainFailure(std::string_view what) noexcept
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

void OtlpExporter::PublishQueueDepth() noexcept
{
    if (m_diag != nullptr)
    {
        m_diag->SetQueueDepth(static_cast<std::uint64_t>(m_queue.size()));
    }
}

void OtlpExporter::DrainQueue(std::unique_lock<std::mutex>& lock) noexcept
{
    // Drain until the queue is empty. The lock is released while
    // FanOutAndProcess runs, so Export() can enqueue more batches mid-drain.
    // Each round reports its outcomes to m_flush, which is what a flush waits
    // on: the batches queued before it, not an empty queue (ICP 0041).
    while (!m_queue.empty())
    {
        std::vector<internal::BatchHandle> batches;
        while (!m_queue.empty())
        {
            batches.push_back(std::move(m_queue.front()));
            m_queue.pop_front();
        }
        // The whole queue was taken, so the whole budget is free again.
        m_queued_spans = 0;
        lock.unlock();
        std::vector<bool> delivered;
        try
        {
            FanOutAndProcess(batches, delivered);
        }
        // The batches are gone either way — the worker is `noexcept` and there
        // is nowhere to put them — but a swallowed failure that nothing counts
        // leaves GetExporterHealth() reporting a clean pipeline, against
        // error-model.md §5.1 (issue #224).
        catch (const std::exception& e)
        {
            RecordDrainFailure(e.what());
        }
        lock.lock();
        m_flush.Complete(batches.size(), delivered);
        m_cv.notify_all();
        PublishQueueDepth();
    }
}

internal::EncodedPayload OtlpExporter::EncodeRequest(
    const std::vector<internal::BatchHandle>& batches, std::size_t first, std::size_t count)
{
    std::vector<internal::EncodedPayload> parts;
    parts.reserve(count);
    for (std::size_t i = first; i < first + count; ++i)
    {
        parts.push_back(m_encoder->Encode(batches.at(i)));
    }
    return wire::ConcatenateTraceRequests(std::move(parts));
}

std::vector<OtlpExporter::RequestRange> OtlpExporter::GroupIntoRequests(
    const std::vector<internal::BatchHandle>& batches,
    const std::vector<internal::EncodedPayload>& parts) const
{
    std::vector<RequestRange> requests;
    std::size_t spans_in_request = 0;
    std::size_t bytes_in_request = 0;
    for (std::size_t i = 0; i < batches.size(); ++i)
    {
        const std::size_t spans = batches[i].Spans().size();
        const std::size_t bytes = parts.at(i).Size();
        const bool over_spans = spans_in_request + spans > m_config.max_spans_per_request;
        const bool over_bytes = m_config.max_request_bytes != 0 &&
                                bytes_in_request + bytes > m_config.max_request_bytes;
        if (requests.empty() || over_spans || over_bytes)
        {
            requests.push_back(RequestRange{.first = i, .count = 0});
            spans_in_request = 0;
            bytes_in_request = 0;
        }
        ++requests.back().count;
        spans_in_request += spans;
        bytes_in_request += bytes;
    }
    return requests;
}

void OtlpExporter::FanOutAndProcess(const std::vector<internal::BatchHandle>& batches,
                                    std::vector<bool>& delivered)
{
    if (batches.empty())
    {
        return;
    }

    // Encode each batch once, then split the drain into requests on those
    // encodings: consecutive batches while the running span count stays
    // within max_spans_per_request and, when set, the running encoded size
    // within max_request_bytes (ICP 0036). A request holds at least one
    // batch, so a batch over either limit goes alone and whole. The grouping
    // is fixed here for the whole drain, so a retry below re-encodes exactly
    // the group its first attempt carried.
    std::vector<internal::EncodedPayload> parts;
    parts.reserve(batches.size());
    for (const auto& batch : batches)
    {
        parts.push_back(m_encoder->Encode(batch));
    }
    const std::vector<RequestRange> requests = GroupIntoRequests(batches, parts);

    std::vector<internal::EncodedPayload> payloads;
    payloads.reserve(requests.size());
    for (const auto& request : requests)
    {
        std::vector<internal::EncodedPayload> group;
        group.reserve(request.count);
        for (std::size_t i = request.first; i < request.first + request.count; ++i)
        {
            group.push_back(std::move(parts.at(i)));
        }
        payloads.push_back(wire::ConcatenateTraceRequests(std::move(group)));
    }

    // Fan-out: all requests submitted concurrently; SendAll collapses N
    // sequential round trips into one (see ICP 0007).
    const auto results = m_codec->SendAll(std::move(payloads), m_config.export_deadline);

    // The fan-out counts as attempt 0. Exactly one outcome per request, and
    // intermediate retryable failures are attempts, not failed batches. A
    // retry re-encodes the whole request: bytes do not survive an attempt
    // (memory-model.md §3.1). `at()`: a codec returning more results than
    // requests throws into DrainQueue's catch rather than reading past the end.
    for (std::size_t i = 0; i < results.size(); ++i)
    {
        const RequestRange& request = requests.at(i);
        const bool sent = m_retry.Settle(
            results.at(i),
            [this, &batches, &request]
            {
                return m_codec->Send(EncodeRequest(batches, request.first, request.count),
                                     m_config.export_deadline);
            },
            kFailureStage,
            request.count);
        // Every batch in the request shares its outcome. Requests are
        // consecutive runs of `batches`, so this stays in batch order.
        delivered.insert(delivered.end(), request.count, sent);
    }
}

void OtlpExporter::WorkerLoop() noexcept
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
