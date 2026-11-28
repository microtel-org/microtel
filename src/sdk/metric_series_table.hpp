// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "microtel/attribute.hpp"
#include "microtel/internal/diagnostics_sink.hpp"
#include "microtel/internal/metric_batch.hpp"

#include "sdk/metric_attribute_set.hpp"

#include <chrono>
#include <cstddef>
#include <mutex>
#include <optional>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

namespace microtel::sdk
{

/// @brief The series map every synchronous metric storage keeps: one `Point`
/// per attribute set, behind one per-instrument mutex, with the cardinality
/// limit and its overflow series (`docs/metrics-design.md` §9).
///
/// When a measurement names an attribute set the table has not seen and the
/// table already holds `max_cardinality` series, the measurement goes to the
/// overflow series (`OverflowAttributeSet()`, created on first use) and one
/// `DropReason::CardinalityOverflow` is counted. The overflow series itself
/// does not count against the limit, so a full table holds at most
/// `max_cardinality + 1` series.
///
/// The table knows nothing about aggregation: `Update` hands the caller the
/// series' `Point` under the lock, and `Collect` turns every `Point` into an
/// output point under the lock and then, for a delta collect, clears the table. `Point` must be
/// default-constructible; a new series starts as `Point{}`.
///
/// `Update` takes the borrowed `AttributeSpan` rather than a built
/// `AttributeSet`, so how a lookup finds its series is local to this class.
///
/// Rule of zero: owns a mutex, so it is neither copyable nor movable.
///
/// @threadsafety Thread-safe — `Update` and `Collect` may be called
///               concurrently. The callbacks run under the table's mutex and
///               must not re-enter the table.
template <typename Point>
class SeriesTable
{
public:
    /// @param max_cardinality the number of distinct attribute sets kept
    ///        before measurements fold into the overflow series.
    /// @param diag Non-owning, borrowed diagnostics sink for overflow drop
    ///        accounting; null disables it. Lifetime: the owning provider
    ///        outlives this table.
    SeriesTable(std::size_t max_cardinality, internal::IDiagnosticsSink* diag) noexcept
        : m_max_cardinality(max_cardinality), m_diag(diag)
    {
    }

    /// @brief Find or create the series for @p attrs, then call
    ///        `update(point, inserted)` under the lock.
    ///
    /// @p inserted is true when this call created the series' `Point`
    /// (a new attribute set, or the first use of the overflow series).
    template <typename Fn>
    void Update(AttributeSpan attrs, Fn&& update)
    {
        const std::scoped_lock lock{m_mu};
        AttributeSet key{attrs};
        auto it = m_points.find(key);
        bool inserted = false;
        if (it == m_points.end())
        {
            std::tie(it, inserted) = Insert(std::move(key));
        }
        std::forward<Fn>(update)(it->second, inserted);
    }

    /// @brief Append `visit(key, point, previous_collect)` to @p out for every
    ///        series under the lock, then record @p collection_time as the
    ///        previous collect and, when @p reset is set, drop every series.
    ///
    /// `previous_collect` is the previous `Collect`'s @p collection_time, or
    /// the table's creation before the first: the start of a delta window.
    template <typename Out, typename Visit>
    void Collect(std::chrono::system_clock::time_point collection_time,
                 bool reset,
                 std::vector<Out>& out,
                 Visit&& visit)
    {
        const std::scoped_lock lock{m_mu};
        out.reserve(out.size() + m_points.size());
        for (auto& [key, point] : m_points)
        {
            out.push_back(visit(key, point, m_last_collect));
        }
        m_last_collect = collection_time;
        if (reset)
        {
            m_points.clear();
        }
    }

private:
    using Points = std::unordered_map<AttributeSet, Point, AttributeSetHash>;

    /// @brief Insert a new series for @p key, or route it to the overflow
    ///        series when the table is full. Called with `m_mu` held.
    std::pair<typename Points::iterator, bool> Insert(AttributeSet key)
    {
        const bool needs_drop = (m_points.size() >= m_max_cardinality);
        if (needs_drop)
        {
            key = OverflowAttributeSet();
        }
        auto result = m_points.try_emplace(std::move(key));
        if (needs_drop && m_diag != nullptr)
        {
            m_diag->RecordDrop(DropReason::CardinalityOverflow);
        }
        return result;
    }

    std::mutex m_mu;
    std::size_t m_max_cardinality;
    internal::IDiagnosticsSink* m_diag;  ///< non-owning; null disables drop accounting
    Points m_points;
    /// The previous collect's time; the table's creation before the first.
    std::chrono::system_clock::time_point m_last_collect = std::chrono::system_clock::now();
};

/// @brief Move a series' exemplar, if it has one, into @p out and clear it:
///        an exemplar belongs to the collection window it was recorded in.
inline void TakeExemplar(std::optional<internal::Exemplar>& exemplar,
                         std::vector<internal::Exemplar>& out)
{
    if (exemplar.has_value())
    {
        out.push_back(std::move(*exemplar));
        exemplar.reset();
    }
}

}  // namespace microtel::sdk
