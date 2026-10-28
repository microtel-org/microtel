// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "microtel/internal/clock.hpp"
#include "microtel/internal/diagnostics_sink.hpp"
#include "microtel/internal/metric_encoder.hpp"
#include "microtel/internal/metric_exporter.hpp"
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

namespace microtel::exporter
{

/// @brief Configuration for `OtlpMetricExporter`.
struct OtlpMetricExporterConfig
{
    /// @brief Maximum number of metric batches held in the worker queue.
    /// `Export` returns `Dropped` when the queue is at capacity.
    std::size_t max_queue_size = 256;
    /// @brief Per-export deadline passed to `IWireCodec::Send`.
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

/// @brief Metrics export pipeline — the metrics analogue of `OtlpExporter`.
///
/// Owns a worker thread. `Export` is non-blocking: it enqueues the batch and
/// returns immediately. The worker drains the queue, encodes each batch via
/// `IMetricEncoder`, and submits the encoded bytes to `IWireCodec::Send`.
///
/// The `IWireCodec` must be pointed at the OTLP metrics endpoint
/// (`/v1/metrics` for HTTP, metrics gRPC path for gRPC-over-HTTP/2).
///
/// A retryable failure is retried by `RetryEngine`, the engine the trace
/// exporter uses: same classification, backoff, budget and drop accounting.
///
/// **Dependencies (all non-owning):**
/// - `IMetricEncoder` — required.
/// - `IWireCodec` — required; must be connected before first `Export` call.
///
/// @threadsafety `Export` is thread-safe. `ForceFlush` and `Shutdown` are
///   caller-thread-safe and idempotent.
class OtlpMetricExporter final : public internal::IMetricExporter
{
public:
    /// @param diag non-owning diagnostics sink, or `nullptr` to disable drop
    ///        and batch accounting. Borrowed for the exporter's lifetime.
    /// @param clock non-owning steady clock for the retry budget, or `nullptr`
    ///        for `std::chrono::steady_clock`. Borrowed, as `diag` is.
    explicit OtlpMetricExporter(internal::IMetricEncoder* encoder,
                                internal::IWireCodec* codec,
                                OtlpMetricExporterConfig config = {},
                                internal::IDiagnosticsSink* diag = nullptr,
                                internal::ISteadyClock* clock = nullptr) noexcept;

    ~OtlpMetricExporter() noexcept override;

    OtlpMetricExporter(const OtlpMetricExporter&) = delete;
    OtlpMetricExporter& operator=(const OtlpMetricExporter&) = delete;
    OtlpMetricExporter(OtlpMetricExporter&&) = delete;
    OtlpMetricExporter& operator=(OtlpMetricExporter&&) = delete;

    /// @brief Enqueue a metric batch for export. Non-blocking.
    ///
    /// Returns `AlreadyShutDown` if `Shutdown` has been called, `Dropped` if
    /// the queue is at capacity, otherwise `Success`.
    [[nodiscard]] internal::ExportResult Export(
        internal::MetricBatchHandle&& batch) noexcept override;

    [[nodiscard]] microtel::Status ForceFlush(std::chrono::milliseconds timeout) noexcept override;

    [[nodiscard]] microtel::Status Shutdown(std::chrono::milliseconds timeout) noexcept override;

private:
    void WorkerLoop() noexcept;
    void DrainQueue(std::unique_lock<std::mutex>& lock) noexcept;
    /// @param delivered appended to, one entry per batch in order: whether it
    ///        was exported. Kept by the caller if this throws part-way.
    void ProcessBatches(std::vector<internal::MetricBatchHandle>& batches,
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

    internal::IMetricEncoder* m_encoder;
    internal::IWireCodec* m_codec;
    OtlpMetricExporterConfig m_config;
    internal::IDiagnosticsSink* m_diag;
    /// @brief Retry loop and final-outcome accounting. Before `m_worker`,
    ///        which uses it from the moment it starts.
    RetryEngine m_retry;

    std::deque<internal::MetricBatchHandle> m_queue;
    std::mutex m_mu;
    std::condition_variable m_cv;
    /// @brief What `ForceFlush` and `Shutdown` wait for. Guarded by `m_mu`.
    FlushTracker m_flush;
    std::atomic<bool> m_shutdown{false};
    std::thread m_worker;
};

}  // namespace microtel::exporter
