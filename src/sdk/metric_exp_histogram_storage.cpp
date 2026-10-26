// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#include "sdk/metric_exp_histogram_storage.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

namespace microtel::sdk
{
namespace
{

/// floor(value / 2^by) for any signed value (by >= 1). Avoids signed `>>`.
std::int32_t FloorDivPow2(std::int32_t value, std::int32_t by)
{
    const auto divisor = static_cast<std::int32_t>(1U << static_cast<unsigned>(by));
    std::int32_t quotient = value / divisor;
    if (value % divisor != 0 && value < 0)
    {
        --quotient;
    }
    return quotient;
}

/// OTel base-2 bucket index for `abs_value > 0` at `scale`:
/// ceil(log2(value) * 2^scale) - 1.
std::int32_t MapIndex(double abs_value, std::int32_t scale)
{
    const double scale_factor = std::ldexp(1.0, scale);  // 2^scale
    return static_cast<std::int32_t>(std::ceil(std::log2(abs_value) * scale_factor)) - 1;
}

std::int32_t LastIndex(const ExpBuckets& buckets)
{
    return buckets.offset + static_cast<std::int32_t>(buckets.counts.size()) - 1;
}

std::size_t AsSize(std::int32_t value)
{
    return static_cast<std::size_t>(value);
}

/// Span (bucket count) needed to cover `buckets` plus `index` after merging
/// every index by 2^change.
std::int32_t MergedSpan(const ExpBuckets& buckets, std::int32_t index, std::int32_t change)
{
    const std::int32_t low = FloorDivPow2(std::min(buckets.offset, index), change);
    const std::int32_t high = FloorDivPow2(std::max(LastIndex(buckets), index), change);
    return high - low + 1;
}

/// Smallest downscale `change >= 1` that fits `buckets` plus `index` within
/// `max_buckets`.
std::int32_t RequiredDownscale(const ExpBuckets& buckets,
                               std::int32_t index,
                               std::int32_t max_buckets)
{
    std::int32_t change = 1;
    while (MergedSpan(buckets, index, change) > max_buckets)
    {
        ++change;
    }
    return change;
}

/// Merge buckets by floor-dividing every absolute index by 2^by.
void Rescale(ExpBuckets& buckets, std::int32_t by)
{
    if (buckets.counts.empty())
    {
        return;
    }
    const std::int32_t new_offset = FloorDivPow2(buckets.offset, by);
    const std::int32_t new_last = FloorDivPow2(LastIndex(buckets), by);
    const std::int32_t new_span = new_last - new_offset + 1;
    std::vector<std::uint64_t> merged(AsSize(new_span), 0);
    for (std::size_t k = 0; k < buckets.counts.size(); ++k)
    {
        if (buckets.counts[k] == 0)
        {
            continue;
        }
        const std::int32_t abs_index = buckets.offset + static_cast<std::int32_t>(k);
        const std::int32_t slot = FloorDivPow2(abs_index, by) - new_offset;
        merged[AsSize(slot)] += buckets.counts[k];
    }
    buckets.offset = new_offset;
    buckets.counts = std::move(merged);
}

/// Increment the bucket for `index`, growing the array (and shifting `offset`)
/// as needed. Caller guarantees the resulting span is within bounds.
void Place(ExpBuckets& buckets, std::int32_t index)
{
    if (buckets.counts.empty())
    {
        buckets.offset = index;
        buckets.counts.assign(1, 0);
    }
    else if (index < buckets.offset)
    {
        const std::int32_t prepend = buckets.offset - index;
        buckets.counts.insert(buckets.counts.begin(), AsSize(prepend), 0);
        buckets.offset = index;
    }
    else if (index > LastIndex(buckets))
    {
        const std::int32_t width = index - buckets.offset + 1;
        buckets.counts.resize(AsSize(width), 0);
    }
    const std::int32_t slot = index - buckets.offset;
    ++buckets.counts[AsSize(slot)];
}

/// Map `abs_value` to a bucket in the point, downscaling the whole point (both
/// signs) if the target array would exceed `max_buckets`.
void InsertValue(ExpHistoPoint& point, double abs_value, bool is_negative, std::int32_t max_buckets)
{
    std::int32_t index = MapIndex(abs_value, point.scale);
    ExpBuckets& target = is_negative ? point.negative : point.positive;

    if (!target.counts.empty() && MergedSpan(target, index, /*change=*/0) > max_buckets)
    {
        const std::int32_t change = RequiredDownscale(target, index, max_buckets);
        Rescale(point.positive, change);
        Rescale(point.negative, change);
        point.scale -= change;
        index = FloorDivPow2(index, change);  // consistent with the merge
    }
    Place(target, index);
}

}  // namespace

template <typename T>
void ExponentialHistogramStorage<T>::Record(T value, AttributeSpan attrs)
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
    ExpHistoPoint& point = it->second;
    if (point.count == 0)  // newly inserted entry (normal or first overflow use)
    {
        point.scale = m_max_scale;
        point.start = std::chrono::system_clock::now();
        point.min = observation;
        point.max = observation;
    }
    else
    {
        point.min = std::min(point.min, observation);
        point.max = std::max(point.max, observation);
    }
    ++point.count;
    point.sum += observation;

    if (observation == 0.0)
    {
        ++point.zero_count;
        return;
    }
    InsertValue(point, std::abs(observation), observation < 0.0, m_max_buckets);
    if (exemplar.has_value())
    {
        m_exemplars.insert_or_assign(it->first, std::move(*exemplar));
    }
}

template <typename T>
internal::ExponentialHistogramData ExponentialHistogramStorage<T>::Collect(
    internal::AggregationTemporality temporality,
    std::chrono::system_clock::time_point collection_time)
{
    const std::scoped_lock lock{m_mu};

    internal::ExponentialHistogramData data;
    data.temporality = temporality;
    data.points.reserve(m_points.size());

    const bool delta = temporality == internal::AggregationTemporality::Delta;
    for (const auto& [key, point] : m_points)
    {
        internal::ExponentialHistogramPoint out;
        const auto pairs = key.Pairs();
        out.attributes.assign(pairs.begin(), pairs.end());
        out.start_time = delta ? m_last_collect : point.start;
        out.time = collection_time;
        out.count = point.count;
        out.sum = point.sum;
        out.min = point.min;
        out.max = point.max;
        out.scale = point.scale;
        out.zero_count = point.zero_count;
        out.positive.offset = point.positive.offset;
        out.positive.bucket_counts = point.positive.counts;
        out.negative.offset = point.negative.offset;
        out.negative.bucket_counts = point.negative.counts;
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

template class ExponentialHistogramStorage<std::int64_t>;
template class ExponentialHistogramStorage<double>;

}  // namespace microtel::sdk
