// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "microtel/internal/diagnostics_sink.hpp"
#include "microtel/internal/icurrent_span_source.hpp"
#include "microtel/internal/metric_batch.hpp"

#include "sdk/metric_attribute_set.hpp"
#include "sdk/metric_exp_histogram_storage.hpp"
#include "sdk/metric_gauge_storage.hpp"
#include "sdk/metric_histogram_storage.hpp"
#include "sdk/metric_stream.hpp"
#include "sdk/metric_sum_storage.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace microtel::sdk
{

/// @brief Optional storage configuration shared by every MetricStream ctor.
///
/// Folds the storage-tuning parameters (cardinality cap, exemplar span source,
/// diagnostics sink) into one aggregate so the stream ctors stay within the
/// 7-parameter limit once the diagnostics sink is threaded through (M12
/// increment 26). All members default to the "no extra behaviour" value, so a
/// default-constructed `StorageOptions{}` reproduces the previous defaults.
struct StorageOptions
{
    /// @brief Maximum distinct attribute sets before overflow folding.
    std::size_t max_cardinality = kDefaultMaxCardinality;
    /// @brief Non-owning; null disables exemplar capture.
    const internal::ICurrentSpanSource* span_source = nullptr;
    /// @brief Non-owning; null disables overflow drop accounting. Lifetime:
    ///        the owning provider outlives the stream.
    internal::IDiagnosticsSink* diag = nullptr;
};

/// @brief IMetricStream for a Sum instrument (Counter / UpDownCounter).
///
/// Owns a `SumStorage<T>` and exposes it via `Storage()` for hot-path
/// `Add()` calls from the instrument handle on the caller thread. `Collect()`
/// assembles a `MetricRecord` containing a `SumData`.
///
/// @tparam T Measurement value type (`std::int64_t` or `double`).
template <typename T>
class MetricStreamSum : public IMetricStream
{
public:
    MetricStreamSum(std::string name,
                    std::string description,
                    std::string unit,
                    bool monotonic,
                    StorageOptions options = {})
        : m_name(std::move(name)),
          m_description(std::move(description)),
          m_unit(std::move(unit)),
          m_storage(monotonic, options.max_cardinality, options.span_source, options.diag)
    {
    }

    MetricStreamSum(const MetricStreamSum&) = delete;
    MetricStreamSum& operator=(const MetricStreamSum&) = delete;
    MetricStreamSum(MetricStreamSum&&) = delete;
    MetricStreamSum& operator=(MetricStreamSum&&) = delete;
    ~MetricStreamSum() noexcept override = default;

    /// @brief Snapshot the storage into a MetricRecord.
    using IMetricStream::Collect;

    [[nodiscard]] internal::MetricRecord Collect(
        internal::AggregationTemporality temporality,
        std::chrono::system_clock::time_point collection_time) override
    {
        return internal::MetricRecord{
            .name = m_name,
            .description = m_description,
            .unit = m_unit,
            .data = m_storage.Collect(temporality, collection_time),
        };
    }

    /// @brief Non-owning reference to the live storage for hot-path `Add()`.
    [[nodiscard]] SumStorage<T>& Storage() noexcept
    {
        return m_storage;
    }

private:
    std::string m_name;
    std::string m_description;
    std::string m_unit;
    SumStorage<T> m_storage;
};

/// @brief IMetricStream for a Gauge instrument (synchronous Gauge /
/// ObservableGauge). Owns a `GaugeStorage<T>`; `Storage()` exposes it for
/// `Record()` calls. `Collect()` assembles a `MetricRecord` with `GaugeData`.
///
/// @tparam T Measurement value type (`std::int64_t` or `double`).
template <typename T>
class MetricStreamGauge : public IMetricStream
{
public:
    MetricStreamGauge(std::string name,
                      std::string description,
                      std::string unit,
                      StorageOptions options = {})
        : m_name(std::move(name)),
          m_description(std::move(description)),
          m_unit(std::move(unit)),
          m_storage(options.max_cardinality, options.span_source, options.diag)
    {
    }

    MetricStreamGauge(const MetricStreamGauge&) = delete;
    MetricStreamGauge& operator=(const MetricStreamGauge&) = delete;
    MetricStreamGauge(MetricStreamGauge&&) = delete;
    MetricStreamGauge& operator=(MetricStreamGauge&&) = delete;
    ~MetricStreamGauge() noexcept override = default;

    using IMetricStream::Collect;

    [[nodiscard]] internal::MetricRecord Collect(
        internal::AggregationTemporality /*temporality*/,
        std::chrono::system_clock::time_point collection_time) override
    {
        return internal::MetricRecord{
            .name = m_name,
            .description = m_description,
            .unit = m_unit,
            .data = m_storage.Collect(collection_time),
        };
    }

    [[nodiscard]] GaugeStorage<T>& Storage() noexcept
    {
        return m_storage;
    }

private:
    std::string m_name;
    std::string m_description;
    std::string m_unit;
    GaugeStorage<T> m_storage;
};

/// @brief IMetricStream for an explicit-bucket Histogram instrument.
/// Owns a `HistogramStorage<T>`; `Storage()` exposes it for `Record()` calls.
///
/// @tparam T Measurement value type (`std::int64_t` or `double`).
template <typename T>
class MetricStreamHistogram : public IMetricStream
{
public:
    MetricStreamHistogram(std::string name,
                          std::string description,
                          std::string unit,
                          std::vector<double> boundaries,
                          StorageOptions options = {})
        : m_name(std::move(name)),
          m_description(std::move(description)),
          m_unit(std::move(unit)),
          m_storage(
              std::move(boundaries), options.max_cardinality, options.span_source, options.diag)
    {
    }

    MetricStreamHistogram(const MetricStreamHistogram&) = delete;
    MetricStreamHistogram& operator=(const MetricStreamHistogram&) = delete;
    MetricStreamHistogram(MetricStreamHistogram&&) = delete;
    MetricStreamHistogram& operator=(MetricStreamHistogram&&) = delete;
    ~MetricStreamHistogram() noexcept override = default;

    using IMetricStream::Collect;

    [[nodiscard]] internal::MetricRecord Collect(
        internal::AggregationTemporality temporality,
        std::chrono::system_clock::time_point collection_time) override
    {
        return internal::MetricRecord{
            .name = m_name,
            .description = m_description,
            .unit = m_unit,
            .data = m_storage.Collect(temporality, collection_time),
        };
    }

    [[nodiscard]] HistogramStorage<T>& Storage() noexcept
    {
        return m_storage;
    }

private:
    std::string m_name;
    std::string m_description;
    std::string m_unit;
    HistogramStorage<T> m_storage;
};

/// @brief IMetricStream for a base-2 exponential Histogram instrument.
/// Owns an `ExponentialHistogramStorage<T>`; `Storage()` exposes it for
/// `Record()` calls.
///
/// @tparam T Measurement value type (`std::int64_t` or `double`).
template <typename T>
class MetricStreamExpHistogram : public IMetricStream
{
public:
    MetricStreamExpHistogram(std::string name,
                             std::string description,
                             std::string unit,
                             std::int32_t max_scale,
                             std::int32_t max_buckets,
                             StorageOptions options = {})
        : m_name(std::move(name)),
          m_description(std::move(description)),
          m_unit(std::move(unit)),
          m_storage(
              max_scale, max_buckets, options.max_cardinality, options.span_source, options.diag)
    {
    }

    MetricStreamExpHistogram(const MetricStreamExpHistogram&) = delete;
    MetricStreamExpHistogram& operator=(const MetricStreamExpHistogram&) = delete;
    MetricStreamExpHistogram(MetricStreamExpHistogram&&) = delete;
    MetricStreamExpHistogram& operator=(MetricStreamExpHistogram&&) = delete;
    ~MetricStreamExpHistogram() noexcept override = default;

    using IMetricStream::Collect;

    [[nodiscard]] internal::MetricRecord Collect(
        internal::AggregationTemporality temporality,
        std::chrono::system_clock::time_point collection_time) override
    {
        return internal::MetricRecord{
            .name = m_name,
            .description = m_description,
            .unit = m_unit,
            .data = m_storage.Collect(temporality, collection_time),
        };
    }

    [[nodiscard]] ExponentialHistogramStorage<T>& Storage() noexcept
    {
        return m_storage;
    }

private:
    std::string m_name;
    std::string m_description;
    std::string m_unit;
    ExponentialHistogramStorage<T> m_storage;
};

}  // namespace microtel::sdk
