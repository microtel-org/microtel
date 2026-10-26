// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#include "sdk/metric_gauge_storage.hpp"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <mutex>
#include <optional>
#include <utility>

namespace microtel::sdk
{

template <typename T>
void GaugeStorage<T>::Record(T value, AttributeSpan attrs)
{
    if (!std::isfinite(static_cast<double>(value)))
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
                .value = internal::MetricValue{value},
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
    it->second = value;
    if (exemplar.has_value())
    {
        m_exemplars.insert_or_assign(it->first, std::move(*exemplar));
    }
}

template <typename T>
internal::GaugeData GaugeStorage<T>::Collect(std::chrono::system_clock::time_point collection_time)
{
    const std::scoped_lock lock{m_mu};

    internal::GaugeData data;
    data.points.reserve(m_points.size());

    for (const auto& [key, value] : m_points)
    {
        internal::NumberPoint point;
        const auto pairs = key.Pairs();
        point.attributes.assign(pairs.begin(), pairs.end());
        point.time = collection_time;
        point.value = value;  // T (int64_t/double) selects the MetricValue alternative
        if (const auto ex_it = m_exemplars.find(key); ex_it != m_exemplars.end())
        {
            point.exemplars.push_back(ex_it->second);
        }
        data.points.push_back(std::move(point));
    }

    m_exemplars.clear();
    return data;
}

template class GaugeStorage<std::int64_t>;
template class GaugeStorage<double>;

}  // namespace microtel::sdk
