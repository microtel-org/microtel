// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "microtel/internal/metric_batch.hpp"

#include <chrono>

namespace microtel::sdk
{

/// @brief Type-erased interface for one metric stream — the (instrument, view)
/// pair that drives collection. Owned by `MetricProducer`.
///
/// Each concrete stream wraps one aggregation storage instance and knows the
/// instrument's name, description, and unit. `Collect()` snapshots the live
/// aggregation state into a `MetricRecord` with the requested temporality.
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
    [[nodiscard]] virtual internal::MetricRecord Collect(
        internal::AggregationTemporality temporality,
        std::chrono::system_clock::time_point collection_time) = 0;

    /// @brief `Collect(temporality, now)`.
    [[nodiscard]] internal::MetricRecord Collect(internal::AggregationTemporality temporality)
    {
        return Collect(temporality, std::chrono::system_clock::now());
    }
};

}  // namespace microtel::sdk
