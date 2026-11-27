// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "microtel/internal/metric_batch.hpp"

#include <chrono>
#include <vector>

namespace microtel::internal
{

/// @brief What one `IMetricProducer::Collect` call is asked for (ICP 0045).
///
/// An aggregate, so callers name what they set —
/// `Collect({.selector = TemporalitySelector::Delta()})` — and leave the rest
/// at its default. A field added later carries a default, so neither the
/// existing calls nor `Collect`'s signature change.
struct CollectRequest
{
    /// @brief The temporality each instrument kind is collected with. Each
    ///        stream resolves its own, as `selector.For(kind)`.
    TemporalitySelector selector = TemporalitySelector::Cumulative();

    /// @brief The time after which this collection runs no further async
    ///        callback (ICP 0046). `time_point::max()`, the default, means no
    ///        deadline.
    ///
    /// Synchronous streams are always collected, and so is the first async
    /// stream. Each later async stream checks the deadline before its
    /// callback runs; once it has passed, that stream and every async stream
    /// after it are skipped for this collection, each counted once as
    /// `DropReason::MetricCallbackTimeout`. A skipped stream contributes no
    /// record and keeps its state. A callback is never interrupted: one that
    /// returns after the deadline keeps its measurements and is not counted.
    std::chrono::steady_clock::time_point callback_deadline =
        std::chrono::steady_clock::time_point::max();
};

/// @brief The pull source the `IMetricReader` collects from — the metrics SDK
/// core (meter registry + per-instrument aggregation state).
///
/// `Collect()` snapshots the current aggregation state into one
/// `MetricBatchHandle` per instrumentation scope and runs any registered
/// async-instrument callbacks. It is invoked on the reader thread
/// (`metrics-design.md` §4/§9), never on a caller hot path.
///
/// Not `noexcept`: it performs collection work and runs user callbacks. The
/// reader invokes it inside a `try`/`catch (const std::exception&)` boundary —
/// mirroring how `OtlpExporter::DrainQueue` wraps `FanOutAndProcess` — so a
/// throwing callback is caught and counted, not propagated.
///
/// @threadsafety Single-caller (the reader thread).
/// @see docs/metrics-design.md §5
class IMetricProducer
{
public:
    virtual ~IMetricProducer() noexcept = default;

    /// @brief Snapshot every stream, one `MetricBatchHandle` per scope.
    ///
    /// @param request What this collection is asked for. The default request
    ///        collects every kind cumulative.
    [[nodiscard]] virtual std::vector<MetricBatchHandle> Collect(
        const CollectRequest& request = {}) = 0;
};

}  // namespace microtel::internal
