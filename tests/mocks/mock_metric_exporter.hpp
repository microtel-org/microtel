// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "microtel/internal/metric_batch.hpp"
#include "microtel/internal/metric_exporter.hpp"
#include "microtel/status.hpp"

#include <atomic>
#include <chrono>
#include <mutex>
#include <utility>
#include <vector>

namespace microtel::testing
{

/// @brief Dumb mock for `microtel::internal::IMetricExporter`.
///
/// Records call counts atomically (worker thread writes; test thread reads
/// after ForceFlush/Shutdown). All return values are configurable.
class MockMetricExporter : public internal::IMetricExporter
{
public:
    internal::ExportResult export_result{internal::ExportResult::Success};
    microtel::Status flush_result{microtel::Status::Completed};
    microtel::Status shutdown_result{microtel::Status::Completed};

    std::atomic<int> export_call_count{0};
    std::atomic<int> flush_call_count{0};
    std::atomic<int> shutdown_call_count{0};

    /// The scope of every exported batch, in order. Guarded by `scopes_mu`:
    /// the reader thread and a `ForceFlush` caller both export.
    std::mutex scopes_mu;
    std::vector<internal::InstrumentationScope> exported_scopes;
    /// Every exported batch, in order. Guarded by `scopes_mu`.
    std::vector<internal::MetricBatchHandle> exported_batches;

    [[nodiscard]] internal::ExportResult Export(
        internal::MetricBatchHandle&& batch) noexcept override
    {
        ++export_call_count;
        const std::scoped_lock lk{scopes_mu};
        exported_scopes.push_back(batch.Scope());
        exported_batches.push_back(std::move(batch));
        return export_result;
    }

    [[nodiscard]] microtel::Status ForceFlush(
        std::chrono::milliseconds /*timeout*/) noexcept override
    {
        ++flush_call_count;
        return flush_result;
    }

    [[nodiscard]] microtel::Status Shutdown(std::chrono::milliseconds /*timeout*/) noexcept override
    {
        ++shutdown_call_count;
        return shutdown_result;
    }
};

}  // namespace microtel::testing
