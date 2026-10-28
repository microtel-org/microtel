// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "microtel/internal/clock.hpp"
#include "microtel/internal/diagnostics_sink.hpp"
#include "microtel/internal/log_batch.hpp"
#include "microtel/internal/log_encoder.hpp"
#include "microtel/internal/log_exporter.hpp"
#include "microtel/internal/wire_codec.hpp"
#include "microtel/provider.hpp"

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

/// @brief Configuration for `OtlpLogExporter`.
struct OtlpLogExporterConfig
{
    /// @brief Maximum number of log batches held in the worker queue.
    /// `Export` returns `Dropped` when the queue is at capacity.
    std::size_t max_queue_size = 256;
    /// @brief Per-export deadline passed to `IWireCodec::SendAll`.
    std::chrono::milliseconds export_deadline{std::chrono::seconds(10)};
    /// @brief Retry / backoff policy — the same engine and defaults as traces.
    RetryPolicyConfig retry_policy{};
    /// @brief Called once from `Shutdown`, on the thread running it, when the
    /// drain wait expires, before the worker is joined; empty for none. The
    /// SDK sets it for an application `ExportTransport`, to call its `Cancel`
    /// and wake a `Send` the worker would otherwise be joined inside
    /// (ICP 0036 Decision 2). Must not block.
    std::function<void()> on_shutdown_timeout{};
};

/// @brief Logs export pipeline — the logs analogue of `OtlpExporter`.
///
/// Owns a worker thread. `Export` is non-blocking: it enqueues the batch and
/// returns immediately. The worker drains the queue, encodes each batch via
/// `ILogEncoder`, and submits the encoded bytes to `IWireCodec::SendAll`.
///
/// The `IWireCodec` must be pointed at the OTLP logs endpoint (`/v1/logs` for
/// HTTP, the logs gRPC path for gRPC-over-HTTP/2).
///
/// A retryable failure is retried by `RetryEngine`, the engine the trace
/// exporter uses: same classification, backoff, budget and drop accounting.
///
/// **Dependencies (all non-owning):**
/// - `ILogEncoder` — required.
/// - `IWireCodec` — required; must be connected before the first `Export` call.
///
/// @threadsafety `Export` is thread-safe. `ForceFlush` and `Shutdown` are
///   caller-thread-safe and idempotent.
/// @see docs/logs-design.md §3
class OtlpLogExporter final : public internal::ILogExporter
{
public:
    /// @param diag non-owning diagnostics sink, or `nullptr` to disable drop
    ///        and batch accounting. Borrowed for the exporter's lifetime.
    /// @param clock non-owning steady clock for the retry budget, or `nullptr`
    ///        for `std::chrono::steady_clock`. Borrowed, as `diag` is.
    explicit OtlpLogExporter(internal::ILogEncoder* encoder,
                             internal::IWireCodec* codec,
                             OtlpLogExporterConfig config = {},
                             internal::IDiagnosticsSink* diag = nullptr,
                             internal::ISteadyClock* clock = nullptr) noexcept;

    ~OtlpLogExporter() noexcept override;

    OtlpLogExporter(const OtlpLogExporter&) = delete;
    OtlpLogExporter& operator=(const OtlpLogExporter&) = delete;
    OtlpLogExporter(OtlpLogExporter&&) = delete;
    OtlpLogExporter& operator=(OtlpLogExporter&&) = delete;

    /// @brief Enqueue a log batch for export. Non-blocking.
    ///
    /// Returns `AlreadyShutDown` if `Shutdown` has been called, `Dropped` if the
    /// queue is at capacity, otherwise `Success`.
    [[nodiscard]] internal::ExportResult Export(internal::LogBatchHandle&& batch) noexcept override;

    [[nodiscard]] microtel::Status ForceFlush(std::chrono::milliseconds timeout) noexcept override;

    [[nodiscard]] microtel::Status Shutdown(std::chrono::milliseconds timeout) noexcept override;

private:
    void WorkerLoop() noexcept;
    void DrainQueue(std::unique_lock<std::mutex>& lock) noexcept;
    /// @param delivered appended to, one entry per batch in order: whether it
    ///        was exported. Kept by the caller if this throws part-way.
    void ProcessBatches(std::vector<internal::LogBatchHandle>& batches,
                        std::vector<bool>& delivered);
    /// @brief Add `n` to the counter for `reason`. No-op without a sink.
    ///        Lock-free, so it is safe under `m_mu`.
    void RecordDropped(DropReason reason, std::uint64_t n) noexcept;
    /// @brief Account for a batch lost to an exception escaping
    ///        `ProcessBatches`. No-op without a sink.
    ///
    /// The worker is `noexcept` and holds nowhere to put the batch, so the
    /// loss is unavoidable — but it is recorded rather than swallowed, so
    /// `GetExporterHealth()` does not report a clean pipeline (issue #224).
    /// Counted as a failed batch, not a `DropReason`: no existing reason names
    /// this, and adding one is an ICP (`docs/interfaces.md` §3.5).
    ///
    /// @param what the exception's `what()`. Borrowed; copied into the error.
    void RecordDrainFailure(std::string_view what) noexcept;

    internal::ILogEncoder* m_encoder;
    internal::IWireCodec* m_codec;
    OtlpLogExporterConfig m_config;
    internal::IDiagnosticsSink* m_diag;
    /// @brief Retry loop and final-outcome accounting. Before `m_worker`,
    ///        which uses it from the moment it starts.
    RetryEngine m_retry;

    std::deque<internal::LogBatchHandle> m_queue;
    std::mutex m_mu;
    std::condition_variable m_cv;
    /// @brief What `ForceFlush` and `Shutdown` wait for. Guarded by `m_mu`.
    FlushTracker m_flush;
    std::atomic<bool> m_shutdown{false};
    std::thread m_worker;
};

}  // namespace microtel::exporter
