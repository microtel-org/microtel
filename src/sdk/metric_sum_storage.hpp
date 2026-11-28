// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "microtel/attribute.hpp"
#include "microtel/internal/diagnostics_sink.hpp"
#include "microtel/internal/icurrent_span_source.hpp"
#include "microtel/internal/metric_batch.hpp"

#include "sdk/metric_attribute_set.hpp"
#include "sdk/metric_series_table.hpp"

#include <chrono>
#include <optional>

namespace microtel::sdk
{

/// @brief Live aggregation state for a Sum instrument (Counter /
/// UpDownCounter / ObservableCounter), per `docs/metrics-design.md` §1/§9.
///
/// Accumulates a running total per attribute set behind a per-instrument mutex;
/// `Collect()` snapshots the totals into the OTLP-shaped `internal::SumData`.
/// `T` is the measurement value type — `std::int64_t` or `double` (the only
/// instantiations defined in the .cpp).
///
/// v1.2 scope note: temporality is cumulative (retain on collect); delta/reset,
/// cardinality limits, and value validation land in their own increments.
/// Lookup currently materialises an `AttributeSet` per `Add`; the no-allocation
/// heterogeneous probe (via `AttributeSet::HashOf`/`MatchesSpan`) is a
/// benchmark-driven follow-up.
///
/// @threadsafety Thread-safe — `Add` and `Collect` may be called concurrently.
template <typename T>
class SumStorage
{
public:
    /// @param diag Non-owning, borrowed diagnostics sink for overflow drop
    ///        accounting; null disables it. Lifetime: the owning provider
    ///        outlives this storage.
    explicit SumStorage(bool monotonic,
                        std::size_t max_cardinality = kDefaultMaxCardinality,
                        const internal::ICurrentSpanSource* span_source = nullptr,
                        internal::IDiagnosticsSink* diag = nullptr) noexcept
        : m_monotonic(monotonic),
          m_span_source(span_source),
          m_diag(diag),
          m_series(max_cardinality, diag)
    {
    }

    /// @brief Accumulate `value` into the point for `attrs` (hot path).
    void Add(T value, AttributeSpan attrs);

    /// @brief Snapshot the running totals as `SumData`.
    ///
    /// Cumulative (default) retains state across collects. Delta reports the
    /// totals accumulated since the previous collect and then clears the live
    /// state (so series with no new measurements are simply absent next cycle).
    ///
    /// Every point's `time` is @p collection_time. Its `start_time` is the
    /// series' first measurement (cumulative) or the previous collect's
    /// `collection_time`, the storage's creation on the first (delta).
    [[nodiscard]] internal::SumData Collect(
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

    /// @brief One series: its running total, its first measurement, and the
    ///        latest exemplar of the current collection window.
    struct Point
    {
        T value{};
        std::chrono::system_clock::time_point start;
        std::optional<internal::Exemplar> exemplar;
    };

    bool m_monotonic;
    const internal::ICurrentSpanSource* m_span_source;  ///< non-owning; null disables exemplars
    internal::IDiagnosticsSink* m_diag;  ///< non-owning; null disables drop accounting
    SeriesTable<Point> m_series;
};

}  // namespace microtel::sdk
