// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "microtel/view.hpp"

#include "sdk/meter_instruments.hpp"
#include "sdk/metric_observable_instruments.hpp"
#include "sdk/metric_producer.hpp"
#include "sdk/metric_stream_impls.hpp"

#include <array>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace microtel::sdk
{

/// OTel-specified default max scale for base-2 exponential histograms.
inline constexpr std::int32_t kDefaultExpHistogramMaxScale = 20;
/// OTel-specified default per-side bucket count cap for exponential histograms.
inline constexpr std::int32_t kDefaultExpHistogramMaxBuckets = 160;

/// OTel-specified default explicit-bucket boundaries (15 boundaries → 16 buckets).
inline constexpr std::array<double, 15> kDefaultHistogramBoundaries = {0.0,
                                                                       5.0,
                                                                       10.0,
                                                                       25.0,
                                                                       50.0,
                                                                       75.0,
                                                                       100.0,
                                                                       250.0,
                                                                       500.0,
                                                                       750.0,
                                                                       1000.0,
                                                                       2500.0,
                                                                       5000.0,
                                                                       7500.0,
                                                                       10000.0};

/// @brief Factory for synchronous metric instruments.
///
/// Obtained via `Provider::GetMeter(name, version)`. Each `Create*<T>()` call
/// allocates a `MetricStream*<T>`, registers it with the shared
/// `MetricProducer` under this Meter's `InstrumentationScope`, and returns a
/// lightweight handle whose `Add()`/`Record()` methods route straight to the
/// underlying storage.
///
/// Instrument handles hold non-owning raw pointers into heap storage owned by
/// the producer. The caller must not use a handle after the `MetricProducer`
/// it was created from has been destroyed.
class Meter
{
public:
    /// @brief Construct a Meter bound to @p scope and @p producer.
    explicit Meter(internal::InstrumentationScope scope,
                   std::shared_ptr<MetricProducer> producer) noexcept
        : m_scope(std::move(scope)), m_producer(std::move(producer))
    {
    }

    Meter(const Meter&) = delete;
    Meter& operator=(const Meter&) = delete;
    Meter(Meter&&) = delete;
    Meter& operator=(Meter&&) = delete;
    ~Meter() noexcept = default;

    // ── Synchronous instruments ───────────────────────────────────────────

    /// @brief Create a monotonic Sum instrument (Counter).
    template <typename T>
    Counter<T> CreateCounter(std::string name, std::string description, std::string unit)
    {
        auto stream = std::make_unique<MetricStreamSum<T>>(
            std::move(name), std::move(description), std::move(unit), InstrumentKind::Counter);
        SumStorage<T>& storage = stream->Storage();
        m_producer->AddStream(m_scope, std::move(stream));
        return Counter<T>{&storage};
    }

    /// @brief Create a non-monotonic Sum instrument (UpDownCounter).
    template <typename T>
    UpDownCounter<T> CreateUpDownCounter(std::string name,
                                         std::string description,
                                         std::string unit)
    {
        auto stream = std::make_unique<MetricStreamSum<T>>(std::move(name),
                                                           std::move(description),
                                                           std::move(unit),
                                                           InstrumentKind::UpDownCounter);
        SumStorage<T>& storage = stream->Storage();
        m_producer->AddStream(m_scope, std::move(stream));
        return UpDownCounter<T>{&storage};
    }

    /// @brief Create a synchronous Gauge instrument (last-write-wins).
    template <typename T>
    Gauge<T> CreateGauge(std::string name, std::string description, std::string unit)
    {
        auto stream = std::make_unique<MetricStreamGauge<T>>(
            std::move(name), std::move(description), std::move(unit));
        GaugeStorage<T>& storage = stream->Storage();
        m_producer->AddStream(m_scope, std::move(stream));
        return Gauge<T>{&storage};
    }

    /// @brief Create a Histogram with explicit bucket @p boundaries.
    template <typename T>
    Histogram<T> CreateHistogram(std::string name,
                                 std::string description,
                                 std::string unit,
                                 std::vector<double> boundaries)
    {
        auto stream = std::make_unique<MetricStreamHistogram<T>>(
            std::move(name), std::move(description), std::move(unit), std::move(boundaries));
        HistogramStorage<T>& storage = stream->Storage();
        m_producer->AddStream(m_scope, std::move(stream));
        return Histogram<T>{&storage};
    }

    /// @brief Create a Histogram with the OTel default 15-boundary ladder.
    template <typename T>
    Histogram<T> CreateHistogram(std::string name, std::string description, std::string unit)
    {
        return CreateHistogram<T>(std::move(name),
                                  std::move(description),
                                  std::move(unit),
                                  std::vector<double>(kDefaultHistogramBoundaries.begin(),
                                                      kDefaultHistogramBoundaries.end()));
    }

    /// @brief Create a base-2 exponential Histogram with explicit scale/bucket limits.
    template <typename T>
    ExponentialHistogram<T> CreateExponentialHistogram(std::string name,
                                                       std::string description,
                                                       std::string unit,
                                                       std::int32_t max_scale,
                                                       std::int32_t max_buckets)
    {
        auto stream = std::make_unique<MetricStreamExpHistogram<T>>(
            std::move(name), std::move(description), std::move(unit), max_scale, max_buckets);
        ExponentialHistogramStorage<T>& storage = stream->Storage();
        m_producer->AddStream(m_scope, std::move(stream));
        return ExponentialHistogram<T>{&storage};
    }

    /// @brief Create an exponential Histogram with OTel defaults (scale 20, 160 buckets/side).
    template <typename T>
    ExponentialHistogram<T> CreateExponentialHistogram(std::string name,
                                                       std::string description,
                                                       std::string unit)
    {
        return CreateExponentialHistogram<T>(std::move(name),
                                             std::move(description),
                                             std::move(unit),
                                             kDefaultExpHistogramMaxScale,
                                             kDefaultExpHistogramMaxBuckets);
    }

    // ── Observable instruments ────────────────────────────────────────────

    /// @brief Create a monotonic observable Sum instrument (ObservableCounter).
    template <typename T>
    ObservableCounter<T> CreateObservableCounter(std::string name,
                                                 std::string description,
                                                 std::string unit,
                                                 ObservableCallback<T> callback)
    {
        m_producer->AddStream(
            m_scope,
            std::make_unique<MetricStreamObservableSum<T>>(std::move(name),
                                                           std::move(description),
                                                           std::move(unit),
                                                           InstrumentKind::ObservableCounter,
                                                           std::move(callback)));
        return ObservableCounter<T>{};
    }

    /// @brief Create a non-monotonic observable Sum instrument
    /// (ObservableUpDownCounter).
    template <typename T>
    ObservableUpDownCounter<T> CreateObservableUpDownCounter(std::string name,
                                                             std::string description,
                                                             std::string unit,
                                                             ObservableCallback<T> callback)
    {
        m_producer->AddStream(
            m_scope,
            std::make_unique<MetricStreamObservableSum<T>>(std::move(name),
                                                           std::move(description),
                                                           std::move(unit),
                                                           InstrumentKind::ObservableUpDownCounter,
                                                           std::move(callback)));
        return ObservableUpDownCounter<T>{};
    }

    /// @brief Create an observable Gauge instrument (ObservableGauge).
    template <typename T>
    ObservableGauge<T> CreateObservableGauge(std::string name,
                                             std::string description,
                                             std::string unit,
                                             ObservableCallback<T> callback)
    {
        m_producer->AddStream(
            m_scope,
            std::make_unique<MetricStreamObservableGauge<T>>(
                std::move(name), std::move(description), std::move(unit), std::move(callback)));
        return ObservableGauge<T>{};
    }

private:
    internal::InstrumentationScope m_scope;
    std::shared_ptr<MetricProducer> m_producer;
};

}  // namespace microtel::sdk
