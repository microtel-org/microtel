// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "microtel/attribute.hpp"
#include "microtel/internal/diagnostics_sink.hpp"
#include "microtel/internal/icurrent_span_source.hpp"
#include "microtel/internal/metric_batch.hpp"

#include "sdk/metric_attribute_set.hpp"

#include <chrono>
#include <cstdint>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace microtel::sdk
{

/// @brief One sign's bucket array for an exponential histogram point.
/// `counts[k]` is the count for absolute bucket index `offset + k`.
struct ExpBuckets
{
    std::int32_t offset = 0;
    std::vector<std::uint64_t> counts;
};

/// @brief Live exponential-histogram state for one attribute set. Scalars are
/// `T`-independent so the heavy logic lives outside the storage template.
struct ExpHistoPoint
{
    std::int32_t scale = 0;
    std::uint64_t zero_count = 0;
    std::uint64_t count = 0;
    double sum = 0.0;
    double min = 0.0;
    double max = 0.0;
    ExpBuckets positive;
    ExpBuckets negative;
    std::chrono::system_clock::time_point start;  ///< the series' first measurement
};

/// @brief Live aggregation state for a base-2 exponential-histogram instrument,
/// per `docs/metrics-design.md` §3.
///
/// Per attribute set: a `scale` (starting at `max_scale`), a zero-count, and
/// positive/negative bucket arrays. For value `v > 0` at scale `s` the bucket
/// index is `ceil(log2(v) * 2^s) - 1`. When a value would push a bucket array
/// past `max_buckets`, the point is **downscaled** (scale reduced by `k`,
/// indices floor-divided by `2^k`, buckets merged) until it fits. Negative
/// values go to the negative array (by magnitude); zeros to the zero count.
/// `Collect()` snapshots cumulative `internal::ExponentialHistogramData`.
/// `T` is the measurement value type (`std::int64_t` / `double`).
///
/// Known caveat: the mapping uses `std::log2`, which has the floating-point
/// boundary subtlety at exact powers of two shared with the OTel reference.
///
/// v1.2 scope note: cumulative-only; delta/reset, cardinality, and exemplars
/// land in their own increments.
///
/// @threadsafety Thread-safe — `Record` and `Collect` may be called concurrently.
template <typename T>
class ExponentialHistogramStorage
{
public:
    /// @param diag Non-owning, borrowed diagnostics sink for overflow drop
    ///        accounting; null disables it. Lifetime: the owning provider
    ///        outlives this storage.
    ExponentialHistogramStorage(std::int32_t max_scale,
                                std::int32_t max_buckets,
                                std::size_t max_cardinality = kDefaultMaxCardinality,
                                const internal::ICurrentSpanSource* span_source = nullptr,
                                internal::IDiagnosticsSink* diag = nullptr)
        : m_max_scale(max_scale),
          m_max_buckets(max_buckets),
          m_max_cardinality(max_cardinality),
          m_span_source(span_source),
          m_diag(diag)
    {
    }

    /// @brief Record `value` into the point for `attrs` (hot path).
    void Record(T value, AttributeSpan attrs);

    /// @brief Snapshot the buckets as `ExponentialHistogramData`. Cumulative
    /// (default) retains state; Delta reports the buckets accumulated since the
    /// previous collect and then clears the live state.
    ///
    /// Every point's `time` is @p collection_time. Its `start_time` is the
    /// series' first measurement (cumulative) or the previous collect's
    /// `collection_time`, the storage's creation on the first (delta).
    [[nodiscard]] internal::ExponentialHistogramData Collect(
        internal::AggregationTemporality temporality = internal::AggregationTemporality::Cumulative,
        std::chrono::system_clock::time_point collection_time = std::chrono::system_clock::now());

private:
    /// @brief Count one NaN / +-Inf measurement. No-op without a sink.
    ///        A helper rather than an inline guard so the hot-path `Record`
    ///        stays under the cognitive-complexity limit.
    void RecordNonFinite() const noexcept
    {
        if (m_diag != nullptr)
        {
            m_diag->RecordDrop(DropReason::NonFiniteValue);
        }
    }

    mutable std::mutex m_mu;
    std::int32_t m_max_scale;
    std::int32_t m_max_buckets;  ///< per-sign bucket-count cap (downscale trigger)
    std::size_t m_max_cardinality;
    const internal::ICurrentSpanSource* m_span_source;  ///< non-owning; null disables exemplars
    internal::IDiagnosticsSink* m_diag;  ///< non-owning; null disables drop accounting
    std::unordered_map<AttributeSet, ExpHistoPoint, AttributeSetHash> m_points;
    std::unordered_map<AttributeSet, internal::Exemplar, AttributeSetHash> m_exemplars;
    /// The previous collect's time; the storage's creation before the first.
    std::chrono::system_clock::time_point m_last_collect = std::chrono::system_clock::now();
};

}  // namespace microtel::sdk
