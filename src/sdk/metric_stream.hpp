// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "microtel/internal/metric_batch.hpp"
#include "microtel/view.hpp"

#include <chrono>

namespace microtel::sdk
{

/// @brief Type-erased interface for one metric stream — the (instrument, view)
/// pair that drives collection. Owned by `MetricProducer`.
///
/// Each concrete stream wraps one aggregation storage instance and knows the
/// instrument's name, description, unit and `InstrumentKind`. `Collect()`
/// snapshots the live aggregation state into a `MetricRecord`, with the
/// temporality the selector gives the stream's own kind (ICP 0045).
///
/// @threadsafety Implementations must be safe for `Collect()` to be called
/// from the reader thread while concurrent hot-path calls are in flight.
class IMetricStream
{
public:
    IMetricStream() noexcept = default;
    IMetricStream(const IMetricStream&) = delete;
    IMetricStream& operator=(const IMetricStream&) = delete;
    IMetricStream(IMetricStream&&) = delete;
    IMetricStream& operator=(IMetricStream&&) = delete;
    virtual ~IMetricStream() noexcept = default;

    /// @brief Snapshot the stream's aggregation state, stamping every point
    ///        with @p collection_time. `MetricProducer` passes one time to
    ///        every stream of a collection.
    ///
    /// @param selector The collection's per-kind temporalities. The stream
    ///        collects with `selector.For(kind)` for its own kind; a gauge
    ///        has no temporality and ignores it.
    [[nodiscard]] virtual internal::MetricRecord Collect(
        const internal::TemporalitySelector& selector,
        std::chrono::system_clock::time_point collection_time) = 0;

    /// @brief Whether collecting this stream runs a user callback: true for an
    ///        observable (async) stream, false for a synchronous one. Only
    ///        these are held to a collection's callback deadline (ICP 0046).
    [[nodiscard]] virtual bool RunsCallback() const noexcept
    {
        return false;
    }

    /// @brief `Collect(selector, now)`.
    [[nodiscard]] internal::MetricRecord Collect(const internal::TemporalitySelector& selector)
    {
        return Collect(selector, std::chrono::system_clock::now());
    }
};

/// @brief Whether a Sum stream of @p kind is monotonic: a Counter or an
///        ObservableCounter. An up-down counter is not.
[[nodiscard]] constexpr bool IsMonotonicSum(InstrumentKind kind) noexcept
{
    return kind == InstrumentKind::Counter || kind == InstrumentKind::ObservableCounter;
}

}  // namespace microtel::sdk
