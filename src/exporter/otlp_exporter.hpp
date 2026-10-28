// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "microtel/internal/batch_group_exporter.hpp"
#include "microtel/internal/clock.hpp"
#include "microtel/internal/diagnostics_sink.hpp"
#include "microtel/internal/exporter.hpp"
#include "microtel/internal/otlp_encoder.hpp"
#include "microtel/internal/wire_codec.hpp"

#include "exporter/flush_tracker.hpp"
#include "exporter/retry_engine.hpp"
#include "exporter/retry_policy.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string_view>
#include <thread>
#include <vector>

namespace microtel::exporter
{

/// @brief How many full processor batches the trace exporter's queue holds.
inline constexpr std::size_t kQueuedFullBatches = 256;

/// @brief Default most spans in one export request: the default
/// `BatchOptions::max_export_batch_size`.
inline constexpr std::size_t kDefaultMaxSpansPerRequest = 512;

/// @brief The trace exporter's queue budget in spans, for a processor that
/// cuts batches of @p max_export_batch_size spans: `kQueuedFullBatches` of
/// them (issue #345).
///
/// The budget is in spans, not batches, because a processor drain becomes one
/// batch per `(Resource, scope)` group: the same drain is one batch from one
/// leaf and hundreds from hundreds of leaves. A bound in batches alone let a
/// many-leaf concentrator queue a small fraction of the spans a one-leaf one
/// could.
[[nodiscard]] constexpr std::size_t QueuedSpanBudget(std::size_t max_export_batch_size) noexcept
{
    return kQueuedFullBatches * max_export_batch_size;
}

/// @brief Configuration for `OtlpExporter`.
struct OtlpExporterConfig
{
    /// @brief Maximum number of batches held in the worker queue.
    /// `Export` returns `Dropped` when the queue is at capacity. Every batch
    /// the processor hands over carries at least one span, so at its default,
    /// equal to `max_queued_spans`, this never binds before the span budget.
    std::size_t max_queue_size = QueuedSpanBudget(kDefaultMaxSpansPerRequest);
    /// @brief Maximum number of spans, summed over the queued batches.
    /// A batch that would take the queue past it is refused like one that
    /// finds the queue full (`Dropped`, `queue_full` per span); the limit is
    /// a ceiling the queue may reach. `SdkBuilder` sets it to
    /// `QueuedSpanBudget(BatchOptions::max_export_batch_size)`.
    std::size_t max_queued_spans = QueuedSpanBudget(kDefaultMaxSpansPerRequest);
    /// @brief Per-export deadline passed to `IWireCodec::Send`.
    std::chrono::milliseconds export_deadline{std::chrono::seconds(10)};
    /// @brief Retry / backoff policy.
    RetryPolicyConfig retry_policy{};
    /// @brief Most spans one export request may carry when the worker joins
    /// several drained batches into one request
    /// (`docs/leaf-concentrator-design.md` §3.6.1). A batch is never split: a
    /// batch larger than this goes as a request of its own. `SdkBuilder` sets
    /// it to `BatchOptions::max_export_batch_size`.
    std::size_t max_spans_per_request = kDefaultMaxSpansPerRequest;
    /// @brief Most encoded bytes one joined request may carry; 0 for no cap
    /// (ICP 0036). A join stops before the batch whose encoding would take
    /// the request past it, and that batch starts the next request. A batch
    /// whose own encoding exceeds it still goes alone and whole. `SdkBuilder`
    /// sets it from `ExportTransportOptions::max_request_bytes` for an
    /// application transport, and leaves it 0 for HTTP and gRPC.
    std::size_t max_request_bytes = 0;
    /// @brief Called once from `Shutdown`, on the thread running it, when the
    /// drain wait expires, before the worker is joined; empty for none. The
    /// SDK sets it for an application `ExportTransport`, to call its `Cancel`
    /// and wake a `Send` the worker would otherwise be joined inside
    /// (ICP 0036 Decision 2). Must not block.
    std::function<void()> on_shutdown_timeout{};
};

/// @brief Protocol-agnostic OTLP export pipeline.
///
/// Owns a worker thread. `Export` is non-blocking — it enqueues the batch
/// and returns immediately. The worker drains the queue, encodes each batch
/// via `IOtlpEncoder`, and submits the encoded bytes to `IWireCodec::Send`.
///
/// A retryable failure is retried by `RetryEngine`, the engine the metric
/// and log exporters share (`RetryPolicyConfig`).
///
/// **Multi-Resource requests.** The batches the worker drains together are
/// sent as one request, up to `max_spans_per_request` spans, by concatenating
/// their encodings (`wire::ConcatenateTraceRequests`,
/// `docs/leaf-concentrator-design.md` §3.6.1). A request is sent, retried and
/// classified as one unit; its outcome is counted once per batch it carries.
/// `ExportGroup` queues all the batches of one processor drain under one lock,
/// so they are always drained together.
///
/// **Dependencies (all non-owning):**
/// - `IOtlpEncoder` — required.
/// - `IWireCodec` — required; must be connected before first `Export` call.
/// - `IDiagnosticsSink` — optional; used from M3-C onward.
/// - `ISteadyClock` — optional; used for the retry budget.
///
/// @threadsafety `Export` is thread-safe. `ForceFlush` and `Shutdown` are
///   caller-thread-safe and idempotent.
/// @see docs/interfaces.md §4.4
class OtlpExporter final : public internal::IExporter, public internal::IBatchGroupExporter
{
public:
    explicit OtlpExporter(internal::IOtlpEncoder* encoder,
                          internal::IWireCodec* codec,
                          OtlpExporterConfig config = {},
                          internal::IDiagnosticsSink* diag = nullptr,
                          internal::ISteadyClock* clock = nullptr) noexcept;

    ~OtlpExporter() noexcept override;

    OtlpExporter(const OtlpExporter&) = delete;
    OtlpExporter& operator=(const OtlpExporter&) = delete;
    OtlpExporter(OtlpExporter&&) = delete;
    OtlpExporter& operator=(OtlpExporter&&) = delete;

    [[nodiscard]] internal::ExportResult Export(internal::BatchHandle&& batch) noexcept override;

    void ExportGroup(std::vector<internal::BatchHandle>&& batches) noexcept override;

    [[nodiscard]] microtel::Status ForceFlush(std::chrono::milliseconds timeout) noexcept override;

    [[nodiscard]] microtel::Status Shutdown(std::chrono::milliseconds timeout) noexcept override;

private:
    /// @brief The batches `[first, first + count)` of one drain, sent as one
    ///        request.
    struct RequestRange
    {
        std::size_t first = 0;
        std::size_t count = 0;
    };

    void WorkerLoop() noexcept;
    void DrainQueue(std::unique_lock<std::mutex>& lock) noexcept;
    /// @param delivered appended to, one entry per batch in order: whether it
    ///        was exported. Kept by the caller if this throws part-way.
    void FanOutAndProcess(const std::vector<internal::BatchHandle>& batches,
                          std::vector<bool>& delivered);
    /// @brief Split one drain into requests by span count and, when set,
    ///        encoded size. @p parts are the batches' encodings, index for index.
    [[nodiscard]] std::vector<RequestRange> GroupIntoRequests(
        const std::vector<internal::BatchHandle>& batches,
        const std::vector<internal::EncodedPayload>& parts) const;
    /// @brief Queue one batch, or count why not. Caller must hold `m_mu`.
    [[nodiscard]] internal::ExportResult EnqueueLocked(internal::BatchHandle&& batch) noexcept;
    /// @brief Encode the batches `[first, first + count)` of @p batches and
    ///        join them into one request.
    [[nodiscard]] internal::EncodedPayload EncodeRequest(
        const std::vector<internal::BatchHandle>& batches, std::size_t first, std::size_t count);
    /// @brief Add `n` to the counter for `reason`. No-op when no sink was
    ///        supplied. Lock-free, so it is safe under `m_mu`.
    void RecordDropped(DropReason reason, std::uint64_t n) noexcept;
    /// @brief Account for a batch lost to an exception escaping
    ///        `FanOutAndProcess`. No-op when no sink was supplied.
    ///
    /// The worker is `noexcept` and holds nowhere to put the batch, so the
    /// loss is unavoidable — but it is recorded rather than swallowed, which
    /// is what `error-model.md` §5.1 requires of the worker's top-level catch
    /// (issue #224). Counted as a failed batch, not a `DropReason`: no
    /// existing reason names this, and adding one is an ICP
    /// (`docs/interfaces.md` §3.5).
    ///
    /// @param what the exception's `what()`. Borrowed; copied into the error.
    void RecordDrainFailure(std::string_view what) noexcept;
    /// @brief Publish the current queue depth. Caller must hold `m_mu`.
    void PublishQueueDepth() noexcept;

    internal::IOtlpEncoder* m_encoder;
    internal::IWireCodec* m_codec;
    OtlpExporterConfig m_config;
    // NOLINTNEXTLINE(clang-diagnostic-unused-private-field) — used from M3-C onward
    internal::IDiagnosticsSink* m_diag;
    /// @brief Retry loop and final-outcome accounting. Before `m_worker`,
    ///        which uses it from the moment it starts.
    RetryEngine m_retry;

    std::deque<internal::BatchHandle> m_queue;
    /// Summed span count of the batches in `m_queue`, held against
    /// `OtlpExporterConfig::max_queued_spans`. Guarded by `m_mu`.
    std::size_t m_queued_spans = 0;
    std::mutex m_mu;
    std::condition_variable m_cv;
    /// @brief What `ForceFlush` and `Shutdown` wait for. Guarded by `m_mu`.
    FlushTracker m_flush;
    std::atomic<bool> m_shutdown{false};
    std::thread m_worker;
};

}  // namespace microtel::exporter
