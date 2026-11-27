// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "microtel/internal/metric_batch.hpp"
#include "microtel/internal/metric_exporter.hpp"
#include "microtel/internal/metric_producer.hpp"
#include "microtel/internal/metric_reader.hpp"
#include "microtel/status.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace microtel::sdk
{

/// @brief Push-model `IMetricReader` that drives collect+export on a background
/// thread at a configurable interval (default 60 s per metrics-design.md §5).
///
/// Three call paths trigger a synchronous collect+export cycle:
///  - Background thread fires at each `interval` tick.
///  - `Collect(timeout)` — manual, caller-thread invocation.
///  - `ForceFlush(timeout)` — immediate caller-thread cycle, then delegates
///    to the exporter's `ForceFlush`.
///
/// `Shutdown(timeout)` wakes the background thread for a final cycle, waits
/// for it to exit, then delegates to the exporter's `Shutdown`.
///
/// Concurrent calls to `Collect`, `ForceFlush`, and the background thread are
/// serialized so that one interval's deltas are never split across two
/// exports. The serialization is a `m_collecting` flag guarded by
/// `m_collect_mu` rather than the mutex being held for the whole cycle:
/// `MetricProducer::Collect` and `IMetricExporter::Export` each take their own
/// lock, so holding `m_collect_mu` across them nested two non-leaf locks —
/// which `docs/threading-model.md` §4 marks LOCKED against. The flag gives the
/// same mutual exclusion while `m_collect_mu` stays a leaf.
///
/// `final` is load-bearing, not decoration. The destructor calls `Shutdown()`,
/// a virtual — in a base class that would statically bind to this class's
/// override while a derived object's own `Shutdown` was skipped, because the
/// derived part is already destroyed by the time the base destructor runs.
/// Sealing the class makes the call unambiguously correct rather than
/// correct-until-someone-subclasses-it. A future reader that needs different
/// shutdown behaviour implements `IMetricReader` directly.
///
/// @threadsafety Thread-safe.
/// @noexcept All public methods; background thread entry point.
class PeriodicExportingMetricReader final : public internal::IMetricReader
{
public:
    /// Default export interval per metrics-design.md §5.
    static constexpr std::chrono::milliseconds kDefaultInterval{60'000};

    /// @brief Construct the reader and start the background export thread.
    ///
    /// @param producer     Source of metric snapshots; must outlive this reader.
    /// @param exporter     Sink for exported batches; must outlive this reader.
    /// @param interval     Background export period; defaults to 60 s.
    /// @param selector     Each instrument kind's temporality, passed to
    ///                     `IMetricProducer::Collect()` in every cycle's
    ///                     `CollectRequest` (ICP 0045).
    PeriodicExportingMetricReader(internal::IMetricProducer& producer,
                                  internal::IMetricExporter& exporter,
                                  std::chrono::milliseconds interval = kDefaultInterval,
                                  internal::TemporalitySelector selector =
                                      internal::TemporalitySelector::Cumulative()) noexcept;

    PeriodicExportingMetricReader(const PeriodicExportingMetricReader&) = delete;
    PeriodicExportingMetricReader& operator=(const PeriodicExportingMetricReader&) = delete;
    PeriodicExportingMetricReader(PeriodicExportingMetricReader&&) = delete;
    PeriodicExportingMetricReader& operator=(PeriodicExportingMetricReader&&) = delete;

    /// @brief Shut down the reader if not already shut down, then join the
    /// background thread. Blocks for at most one export cycle.
    ~PeriodicExportingMetricReader() noexcept override;

    /// @brief Synchronously collect a snapshot and export each batch.
    ///
    /// Serialized against the background thread's own export cycle.
    /// Returns `AlreadyShutDown` immediately if `Shutdown()` was called.
    [[nodiscard]] microtel::Status Collect(std::chrono::milliseconds timeout) noexcept override;

    /// @brief Synchronously collect + export, then delegate `ForceFlush` to
    /// the exporter. Returns `AlreadyShutDown` if already shut down.
    [[nodiscard]] microtel::Status ForceFlush(std::chrono::milliseconds timeout) noexcept override;

    /// @brief Wake the background thread for a final export, join it, then
    /// delegate `Shutdown` to the exporter. Idempotent.
    [[nodiscard]] microtel::Status Shutdown(std::chrono::milliseconds timeout) noexcept override;

    /// @brief Retune the background export period while the reader runs
    /// (ICP 0026).
    ///
    /// Takes `m_mu`, assigns `m_interval`, releases. **`m_wake` is
    /// deliberately not set**: waking the reader would force an immediate
    /// collect+export cycle, and "export at a different cadence" is not
    /// "export now" — `ForceFlush` already means the second. The wait already
    /// in flight keeps the deadline it fixed at entry, so a shortened interval
    /// costs at most one old-length tick before it applies.
    ///
    /// Validation is the caller's: `SdkProvider::SetMetricInterval` rejects a
    /// non-positive interval, which would otherwise turn `wait_for` into a
    /// spin.
    ///
    /// @threadsafety Thread-safe.
    /// @noexcept
    void SetInterval(std::chrono::milliseconds interval) noexcept;

private:
    /// Background thread entry point — loops until `m_shut_down` is set.
    void RunLoop() noexcept;

    /// Execute one collect+export cycle. Guarded by `m_collect_mu`.
    /// @returns `Status::Completed` or `Status::Failed`.
    microtel::Status DoCollectExport() noexcept;

    internal::IMetricProducer& m_producer;
    internal::IMetricExporter& m_exporter;
    // Guarded by m_mu (ICP 0026). RunLoop's read is already inside the
    // unique_lock it waits under; SetInterval is the writer. m_selector,
    // m_producer and m_exporter stay immutable after construction.
    std::chrono::milliseconds m_interval;
    internal::TemporalitySelector m_selector;

    std::atomic<bool> m_shut_down{false};

    std::mutex m_mu;               // guards m_wake and m_interval
    std::condition_variable m_cv;  // signalled on wake or shutdown
    bool m_wake{false};            // early-wakeup flag, guarded by m_mu

    // Serializes DoCollectExport across threads. Held only to claim or release
    // the cycle — never across a call into the producer or the exporter.
    std::mutex m_collect_mu;
    std::condition_variable m_collect_cv;
    bool m_collecting{false};  // guarded by m_collect_mu

    std::thread m_thread;  // started last; joined in Shutdown/dtor
};

}  // namespace microtel::sdk
