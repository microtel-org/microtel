// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "microtel/internal/exporter.hpp"  // ExportResult
#include "microtel/internal/log_batch.hpp"
#include "microtel/internal/log_exporter.hpp"
#include "microtel/status.hpp"

#include <chrono>

namespace microtel::testing
{

/// @brief Dumb mock for `microtel::internal::ILogExporter`.
///
/// Counts calls and returns configured results. Tests that inspect exported
/// batches use `FakeLogExporter` instead.
class MockLogExporter : public internal::ILogExporter
{
public:
    // --- Configurable returns ---
    internal::ExportResult export_result = internal::ExportResult::Success;
    microtel::Status force_flush_result = microtel::Status::Completed;
    microtel::Status shutdown_result = microtel::Status::Completed;

    // --- Recording ---
    int export_call_count = 0;
    int force_flush_call_count = 0;
    int shutdown_call_count = 0;
    /// The timeout each call was handed; -1 until the first call.
    std::chrono::milliseconds last_force_flush_timeout{-1};
    std::chrono::milliseconds last_shutdown_timeout{-1};

    // --- ILogExporter ---

    internal::ExportResult Export(internal::LogBatchHandle&& /*batch*/) noexcept override
    {
        ++export_call_count;
        return export_result;
    }

    [[nodiscard]] microtel::Status ForceFlush(std::chrono::milliseconds timeout) noexcept override
    {
        ++force_flush_call_count;
        last_force_flush_timeout = timeout;
        return force_flush_result;
    }

    [[nodiscard]] microtel::Status Shutdown(std::chrono::milliseconds timeout) noexcept override
    {
        ++shutdown_call_count;
        last_shutdown_timeout = timeout;
        return shutdown_result;
    }
};

}  // namespace microtel::testing
