// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#include "sdk/metric_histogram_storage.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <utility>

namespace microtel::sdk
{

template <typename T>
void HistogramStorage<T>::Record(T value, AttributeSpan attrs)
{
    const auto observation = static_cast<double>(value);
    if (!std::isfinite(observation))
    {
        RecordNonFinite();
        return;
    }
    std::optional<internal::Exemplar> exemplar;
    if (m_span_source != nullptr)
    {
        const auto span = m_span_source->GetCurrentSpan();
        if (span.IsValid() && span.trace_flags.IsSampled())
        {
            exemplar = internal::Exemplar{
                .span_context = span,
                .time = std::chrono::system_clock::now(),
                .value = internal::MetricValue{observation},
                .filtered_attributes = {},
            };
        }
    }

    const std::scoped_lock lock{m_mu};
    AttributeSet key{attrs};
    auto it = m_points.find(key);
    if (it == m_points.end())
    {
        const bool needs_drop = (m_points.size() >= m_max_cardinality);
        if (needs_drop)
        {
            key = OverflowAttributeSet();
        }
        it = m_points.try_emplace(std::move(key)).first;
        if (needs_drop && m_diag != nullptr)
        {
            m_diag->RecordDrop(DropReason::CardinalityOverflow);
        }
    }
    Point& point = it->second;
    if (point.bucket_counts.empty())  // newly inserted entry (normal or first overflow use)
    {
        point.bucket_counts.assign(m_boundaries.size() + 1, 0);
        point.start = std::chrono::system_clock::now();
        point.min = observation;
        point.max = observation;
    }
    else
    {
        point.min = std::min(point.min, observation);
        point.max = std::max(point.max, observation);
    }

    // Upper-inclusive bucket: first boundary >= value, else the overflow bucket.
    const auto bound = std::ranges::lower_bound(m_boundaries, observation);
    const auto bucket = static_cast<std::size_t>(bound - m_boundaries.begin());
    ++point.bucket_counts[bucket];
    ++point.count;
    point.sum += observation;
    if (exemplar.has_value())
    {
        m_exemplars.insert_or_assign(it->first, std::move(*exemplar));
    }
}

template <typename T>
internal::HistogramData HistogramStorage<T>::Collect(
    internal::AggregationTemporality temporality,
    std::chrono::system_clock::time_point collection_time)
{
    const std::scoped_lock lock{m_mu};

    internal::HistogramData data;
    data.temporality = temporality;
    data.points.reserve(m_points.size());

    const bool delta = temporality == internal::AggregationTemporality::Delta;
    for (const auto& [key, point] : m_points)
    {
        internal::HistogramPoint out;
        const auto pairs = key.Pairs();
        out.attributes.assign(pairs.begin(), pairs.end());
        out.start_time = delta ? m_last_collect : point.start;
        out.time = collection_time;
        out.count = point.count;
        out.sum = point.sum;
        out.min = point.min;
        out.max = point.max;
        out.bucket_counts = point.bucket_counts;
        out.explicit_bounds = m_boundaries;
        if (const auto ex_it = m_exemplars.find(key); ex_it != m_exemplars.end())
        {
            out.exemplars.push_back(ex_it->second);
        }
        data.points.push_back(std::move(out));
    }

    m_exemplars.clear();
    m_last_collect = collection_time;
    if (delta)
    {
        m_points.clear();
    }
    return data;
}

template class HistogramStorage<std::int64_t>;
template class HistogramStorage<double>;

}  // namespace microtel::sdk
