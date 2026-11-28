// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#include "sdk/metric_gauge_storage.hpp"

#include <chrono>
#include <cmath>
#include <cstdint>
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
    m_series.Update(attrs,
                    [&](Point& point, bool /*inserted*/)
                    {
                        point.value = value;
                        if (exemplar.has_value())
                        {
                            point.exemplar = std::move(exemplar);
                        }
                    });
}

template <typename T>
internal::GaugeData GaugeStorage<T>::Collect(std::chrono::system_clock::time_point collection_time)
{
    internal::GaugeData data;
    // A gauge has no temporality: its series are kept across collects.
    m_series.Collect(collection_time,
                     /*reset=*/false,
                     data.points,
                     [&](const AttributeSet& key,
                         Point& series,
                         std::chrono::system_clock::time_point /*previous_collect*/)
                     {
                         internal::NumberPoint point;
                         const auto pairs = key.Pairs();
                         point.attributes.assign(pairs.begin(), pairs.end());
                         point.time = collection_time;
                         // T (int64_t/double) selects the MetricValue alternative
                         point.value = series.value;
                         TakeExemplar(series.exemplar, point.exemplars);
                         return point;
                     });
    return data;
}

template class GaugeStorage<std::int64_t>;
template class GaugeStorage<double>;

}  // namespace microtel::sdk
