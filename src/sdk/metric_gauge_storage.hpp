// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "microtel/attribute.hpp"
#include "microtel/internal/diagnostics_sink.hpp"
#include "microtel/internal/icurrent_span_source.hpp"
#include "microtel/internal/metric_batch.hpp"

#include "sdk/metric_attribute_set.hpp"

#include <chrono>
#include <mutex>
#include <unordered_map>

namespace microtel::sdk
{

/// @brief Live aggregation state for a Gauge instrument (synchronous Gauge /
/// ObservableGauge), per `docs/metrics-design.md` §8/§9.
///
/// Last-write-wins per attribute set behind a per-instrument mutex; `Collect()`
/// snapshots the latest values into `internal::GaugeData`. A gauge has no
/// temporality — it is always the most recent value. `T` is the measurement
/// value type (`std::int64_t` or `double`; the only instantiations defined in
/// the .cpp).
///
/// v1.2 scope note: same as `SumStorage` — cardinality limits and value
/// validation land in later increments; lookup materialises an `AttributeSet`
/// per `Record` (no-allocation heterogeneous probe is a follow-up).
///
/// @threadsafety Thread-safe — `Record` and `Collect` may be called concurrently.
template <typename T>
class GaugeStorage
{
public:
    /// @param diag Non-owning, borrowed diagnostics sink for overflow drop
    ///        accounting; null disables it. Lifetime: the owning provider
    ///        outlives this storage.
    explicit GaugeStorage(std::size_t max_cardinality = kDefaultMaxCardinality,
                          const internal::ICurrentSpanSource* span_source = nullptr,
                          internal::IDiagnosticsSink* diag = nullptr) noexcept
        : m_max_cardinality(max_cardinality), m_span_source(span_source), m_diag(diag)
    {
    }

    /// @brief Store `value` as the latest reading for `attrs` (hot path).
    void Record(T value, AttributeSpan attrs);

    /// @brief Snapshot the latest readings as `GaugeData`. Clears the exemplar
    /// window on every call (exemplars represent the current collection period).
    ///
    /// Every point's `time` is @p collection_time. A gauge carries no start
    /// time: OTLP leaves it optional for gauges.
    [[nodiscard]] internal::GaugeData Collect(
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
    std::size_t m_max_cardinality;
    const internal::ICurrentSpanSource* m_span_source;  ///< non-owning; null disables exemplars
    internal::IDiagnosticsSink* m_diag;  ///< non-owning; null disables drop accounting
    std::unordered_map<AttributeSet, T, AttributeSetHash> m_points;
    std::unordered_map<AttributeSet, internal::Exemplar, AttributeSetHash> m_exemplars;
};

}  // namespace microtel::sdk
