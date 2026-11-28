// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "microtel/attribute.hpp"
#include "microtel/internal/diagnostics_sink.hpp"
#include "microtel/internal/icurrent_span_source.hpp"
#include "microtel/internal/metric_batch.hpp"

#include "sdk/metric_attribute_set.hpp"
#include "sdk/metric_series_table.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace microtel::sdk
{

/// @brief Drop accounting for negative histogram measurements (ICP 0047),
/// shared by `HistogramStorage` and `ExponentialHistogramStorage`.
///
/// Each `Drop` counts one `DropReason::NegativeHistogramValue` and may log a
/// `Warn` naming the instrument. The internal log has no per-`(level, reason)`
/// rate limiter yet (`docs/error-model.md` §9.2, `common/internal_log.hpp`),
/// so this limits itself as `SdkLeafReceiver::RecordOutOfMemory` does: the
/// 1st, 2nd, 4th, 8th ... drop is logged, with the running count.
///
/// @threadsafety Thread-safe — `Drop` may be called concurrently.
class NegativeValueDrops
{
public:
    /// @param instrument_name the instrument's name, for the `Warn`; it is
    ///        copied, with control characters escaped.
    explicit NegativeValueDrops(std::string_view instrument_name);

    /// @brief Count one dropped negative measurement and log it, rate-limited.
    /// @param diag Non-owning diagnostics sink; null skips the count.
    void Drop(internal::IDiagnosticsSink* diag) noexcept;

private:
    std::string m_instrument_name;  ///< escaped
    std::atomic<std::uint64_t> m_drops{0};
};

/// @brief Live aggregation state for an explicit-bucket Histogram instrument,
/// per `docs/metrics-design.md` §3.
///
/// A negative measurement is dropped before any aggregation or exemplar work
/// and counted as `NegativeHistogramValue` (ICP 0047); `-0.0` is recorded as
/// zero. Per attribute set, tracks count / sum / min / max and per-bucket counts
/// against a shared, immutable boundary ladder. Bucket semantics are
/// OTLP-standard upper-inclusive: a value `v` falls in bucket `i` = the
/// smallest `i` with `v <= bounds[i]`, or bucket `n` if `v > bounds[n-1]`
/// (so `bucket_counts.size() == bounds.size() + 1`). `Collect()` snapshots
/// cumulative `internal::HistogramData`. `T` is the measurement value type
/// (`std::int64_t` or `double`); values are bucketed as `double`.
///
/// v1.2 scope note: cumulative-only; exponential histograms, delta/reset,
/// cardinality limits, and exemplars land in their own increments. Lookup
/// materialises an `AttributeSet` per `Record` (no-alloc probe is a follow-up).
///
/// @threadsafety Thread-safe — `Record` and `Collect` may be called concurrently.
template <typename T>
class HistogramStorage
{
public:
    /// @param diag Non-owning, borrowed diagnostics sink for overflow drop
    ///        accounting; null disables it. Lifetime: the owning provider
    ///        outlives this storage.
    /// @param instrument_name names the instrument in the negative-value
    ///        `Warn`; copied.
    explicit HistogramStorage(std::vector<double> boundaries,
                              std::size_t max_cardinality = kDefaultMaxCardinality,
                              const internal::ICurrentSpanSource* span_source = nullptr,
                              internal::IDiagnosticsSink* diag = nullptr,
                              std::string_view instrument_name = {})
        : m_boundaries(std::move(boundaries)),
          m_span_source(span_source),
          m_diag(diag),
          m_negative_drops(instrument_name),
          m_series(max_cardinality, diag)
    {
    }

    /// @brief Record `value` into the point for `attrs` (hot path).
    void Record(T value, AttributeSpan attrs);

    /// @brief Snapshot the buckets as `HistogramData`. Cumulative (default)
    /// retains state; Delta reports the buckets accumulated since the previous
    /// collect and then clears the live state.
    ///
    /// Every point's `time` is @p collection_time. Its `start_time` is the
    /// series' first measurement (cumulative) or the previous collect's
    /// `collection_time`, the storage's creation on the first (delta).
    [[nodiscard]] internal::HistogramData Collect(
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

    /// @brief Whether @p observation is dropped before any aggregation or
    ///        exemplar work, counting the drop: NaN / +-Inf, or below zero
    ///        (ICP 0047; `-0.0` is not). One helper for both checks keeps the
    ///        hot-path `Record` under the cognitive-complexity limit.
    [[nodiscard]] bool DroppedBeforeAggregation(double observation) noexcept;

    /// @brief One series: count, sum, min, max and bucket counts, its first
    ///        measurement, and the latest exemplar of the current window.
    struct Point
    {
        std::uint64_t count = 0;
        double sum = 0.0;
        double min = 0.0;
        double max = 0.0;
        std::vector<std::uint64_t> bucket_counts;
        std::chrono::system_clock::time_point start;  ///< the series' first measurement
        /// The latest exemplar of the current collection window.
        std::optional<internal::Exemplar> exemplar;
    };

    /// @brief Fold @p observation into @p point, initialising it first if the
    ///        series is new. Called under the series table's lock.
    void Aggregate(Point& point, double observation) const;

    std::vector<double> m_boundaries;                   ///< shared, immutable upper bounds (sorted)
    const internal::ICurrentSpanSource* m_span_source;  ///< non-owning; null disables exemplars
    internal::IDiagnosticsSink* m_diag;  ///< non-owning; null disables drop accounting
    NegativeValueDrops m_negative_drops;
    SeriesTable<Point> m_series;
};

}  // namespace microtel::sdk
