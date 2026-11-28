// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#include "sdk/metric_sum_storage.hpp"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <optional>
#include <utility>

namespace microtel::sdk
{

template <typename T>
void SumStorage<T>::Add(T value, AttributeSpan attrs)
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
                    [&](Point& point, bool inserted)
                    {
                        if (inserted)
                        {
                            point.start = std::chrono::system_clock::now();
                        }
                        point.value += value;
                        if (exemplar.has_value())
                        {
                            point.exemplar = std::move(exemplar);
                        }
                    });
}

template <typename T>
internal::SumData SumStorage<T>::Collect(internal::AggregationTemporality temporality,
                                         std::chrono::system_clock::time_point collection_time)
{
    internal::SumData data;
    data.temporality = temporality;
    data.is_monotonic = m_monotonic;

    const bool delta = temporality == internal::AggregationTemporality::Delta;
    // Delta reports the increment since the last collect, so clear live state.
    m_series.Collect(collection_time,
                     delta,
                     data.points,
                     [&](const AttributeSet& key,
                         Point& series,
                         std::chrono::system_clock::time_point previous_collect)
                     {
                         internal::NumberPoint point;
                         const auto pairs = key.Pairs();
                         point.attributes.assign(pairs.begin(), pairs.end());
                         point.start_time = delta ? previous_collect : series.start;
                         point.time = collection_time;
                         // T (int64_t/double) selects the MetricValue alternative
                         point.value = series.value;
                         TakeExemplar(series.exemplar, point.exemplars);
                         return point;
                     });
    return data;
}

template class SumStorage<std::int64_t>;
template class SumStorage<double>;

}  // namespace microtel::sdk
