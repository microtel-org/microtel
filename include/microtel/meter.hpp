// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "microtel/attribute.hpp"

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace microtel
{

/// OTel-specified default max scale for base-2 exponential histograms (scale 20).
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

/// @brief Abstract monotonic Sum instrument.
///
/// Obtained via `Meter::CreateCounter<T>()`. Hot-path `Add()` is `noexcept`.
///
/// @tparam T Value type: `std::int64_t` or `double`.
template <typename T>
class Counter
{
public:
    Counter() noexcept = default;
    virtual ~Counter() noexcept = default;

    Counter(const Counter&) = delete;
    Counter& operator=(const Counter&) = delete;
    Counter(Counter&&) = delete;
    Counter& operator=(Counter&&) = delete;

    /// @brief Record a measurement. Values below zero are silently dropped.
    virtual void Add(T value, AttributeSpan attrs) noexcept = 0;
};

/// @brief Abstract non-monotonic Sum instrument.
///
/// Obtained via `Meter::CreateUpDownCounter<T>()`. Accepts negative values.
///
/// @tparam T Value type: `std::int64_t` or `double`.
template <typename T>
class UpDownCounter
{
public:
    UpDownCounter() noexcept = default;
    virtual ~UpDownCounter() noexcept = default;

    UpDownCounter(const UpDownCounter&) = delete;
    UpDownCounter& operator=(const UpDownCounter&) = delete;
    UpDownCounter(UpDownCounter&&) = delete;
    UpDownCounter& operator=(UpDownCounter&&) = delete;

    /// @brief Record a measurement (may be negative).
    virtual void Add(T value, AttributeSpan attrs) noexcept = 0;
};

/// @brief Abstract synchronous Gauge instrument (last-write-wins).
///
/// Obtained via `Meter::CreateGauge<T>()`.
///
/// @tparam T Value type: `std::int64_t` or `double`.
template <typename T>
class Gauge
{
public:
    Gauge() noexcept = default;
    virtual ~Gauge() noexcept = default;

    Gauge(const Gauge&) = delete;
    Gauge& operator=(const Gauge&) = delete;
    Gauge(Gauge&&) = delete;
    Gauge& operator=(Gauge&&) = delete;

    /// @brief Record an observation; last call wins per attribute set.
    virtual void Record(T value, AttributeSpan attrs) noexcept = 0;
};

/// @brief Abstract explicit-bucket Histogram instrument.
///
/// Obtained via `Meter::CreateHistogram<T>()`.
///
/// @tparam T Value type: `std::int64_t` or `double`.
template <typename T>
class Histogram
{
public:
    Histogram() noexcept = default;
    virtual ~Histogram() noexcept = default;

    Histogram(const Histogram&) = delete;
    Histogram& operator=(const Histogram&) = delete;
    Histogram(Histogram&&) = delete;
    Histogram& operator=(Histogram&&) = delete;

    /// @brief Record an observation into the histogram.
    ///
    /// `value` must be non-negative. A negative value is dropped and counted
    /// as `NegativeHistogramValue`.
    virtual void Record(T value, AttributeSpan attrs) noexcept = 0;
};

/// @brief Abstract base-2 exponential Histogram instrument.
///
/// Obtained via `Meter::CreateExponentialHistogram<T>()`. Hot-path `Record()`
/// is `noexcept`. The storage automatically downscales when a bucket array
/// would exceed @p max_buckets (set at creation time).
///
/// @tparam T Value type: `std::int64_t` or `double`.
template <typename T>
class ExponentialHistogram
{
public:
    ExponentialHistogram() noexcept = default;
    virtual ~ExponentialHistogram() noexcept = default;

    ExponentialHistogram(const ExponentialHistogram&) = delete;
    ExponentialHistogram& operator=(const ExponentialHistogram&) = delete;
    ExponentialHistogram(ExponentialHistogram&&) = delete;
    ExponentialHistogram& operator=(ExponentialHistogram&&) = delete;

    /// @brief Record an observation into the exponential histogram.
    ///
    /// `value` must be non-negative. A negative value is dropped and counted
    /// as `NegativeHistogramValue`.
    virtual void Record(T value, AttributeSpan attrs) noexcept = 0;
};

// ── Observable instrument support ─────────────────────────────────────────────

/// @brief Observer API passed to an observable instrument callback.
///
/// Implementations call `Observe()` once per distinct attribute set per cycle.
///
/// @tparam T Value type: `std::int64_t` or `double`.
template <typename T>
class ObservableResult
{
public:
    ObservableResult() noexcept = default;
    virtual ~ObservableResult() noexcept = default;

    ObservableResult(const ObservableResult&) = delete;
    ObservableResult& operator=(const ObservableResult&) = delete;
    ObservableResult(ObservableResult&&) = delete;
    ObservableResult& operator=(ObservableResult&&) = delete;

    /// @brief Report one observation for @p attrs. Last call wins per attribute set.
    virtual void Observe(T value, AttributeSpan attrs) = 0;
};

/// @brief Signature of an observable instrument callback.
///
/// The function is invoked once per collection cycle. The @p result reference
/// is valid only for the duration of the call.
///
/// @tparam T Value type: `std::int64_t` or `double`.
template <typename T>
using ObservableCallback = std::function<void(ObservableResult<T>&)>;

/// @brief RAII handle for a registered ObservableCounter.
///
/// Move-only. In M12 this is an empty token; v1.2 will add deregistration.
///
/// @tparam T Value type: `std::int64_t` or `double`.
template <typename T>
class ObservableCounter
{
public:
    ObservableCounter() noexcept = default;
    ~ObservableCounter() noexcept = default;

    ObservableCounter(const ObservableCounter&) = delete;
    ObservableCounter& operator=(const ObservableCounter&) = delete;
    ObservableCounter(ObservableCounter&&) noexcept = default;
    ObservableCounter& operator=(ObservableCounter&&) noexcept = default;
};

/// @brief RAII handle for a registered ObservableUpDownCounter.
///
/// Move-only. In M12 this is an empty token; v1.2 will add deregistration.
///
/// @tparam T Value type: `std::int64_t` or `double`.
template <typename T>
class ObservableUpDownCounter
{
public:
    ObservableUpDownCounter() noexcept = default;
    ~ObservableUpDownCounter() noexcept = default;

    ObservableUpDownCounter(const ObservableUpDownCounter&) = delete;
    ObservableUpDownCounter& operator=(const ObservableUpDownCounter&) = delete;
    ObservableUpDownCounter(ObservableUpDownCounter&&) noexcept = default;
    ObservableUpDownCounter& operator=(ObservableUpDownCounter&&) noexcept = default;
};

/// @brief RAII handle for a registered ObservableGauge.
///
/// Move-only. In M12 this is an empty token; v1.2 will add deregistration.
///
/// @tparam T Value type: `std::int64_t` or `double`.
template <typename T>
class ObservableGauge
{
public:
    ObservableGauge() noexcept = default;
    ~ObservableGauge() noexcept = default;

    ObservableGauge(const ObservableGauge&) = delete;
    ObservableGauge& operator=(const ObservableGauge&) = delete;
    ObservableGauge(ObservableGauge&&) noexcept = default;
    ObservableGauge& operator=(ObservableGauge&&) noexcept = default;
};

/// @brief Factory for metric instruments bound to one instrumentation scope.
///
/// Obtained via `Provider::GetMeter(name, version)`. Each `Create*<T>()` call
/// registers a metric stream with the shared producer and returns a
/// `shared_ptr` to an abstract instrument whose `Add()`/`Record()` methods
/// route directly to the underlying storage.
///
/// Supported value types: `std::int64_t` and `double`. Other types produce a
/// link error via the un-defined primary template specialization.
///
/// @threadsafety Thread-safe.
class Meter
{
public:
    Meter() noexcept = default;
    virtual ~Meter() noexcept = default;

    Meter(const Meter&) = delete;
    Meter& operator=(const Meter&) = delete;
    Meter(Meter&&) = delete;
    Meter& operator=(Meter&&) = delete;

    // ── Synchronous instruments ────────────────────────────────────────────

    /// @brief Create a monotonic Sum instrument (Counter).
    template <typename T>
    std::shared_ptr<Counter<T>> CreateCounter(std::string name,
                                              std::string description = {},
                                              std::string unit = {});

    /// @brief Create a non-monotonic Sum instrument (UpDownCounter).
    template <typename T>
    std::shared_ptr<UpDownCounter<T>> CreateUpDownCounter(std::string name,
                                                          std::string description = {},
                                                          std::string unit = {});

    /// @brief Create a synchronous last-write-wins Gauge instrument.
    template <typename T>
    std::shared_ptr<Gauge<T>> CreateGauge(std::string name,
                                          std::string description = {},
                                          std::string unit = {});

    /// @brief Create a Histogram with explicit bucket @p boundaries.
    template <typename T>
    std::shared_ptr<Histogram<T>> CreateHistogram(std::string name,
                                                  std::string description,
                                                  std::string unit,
                                                  std::vector<double> boundaries);

    /// @brief Create a Histogram with the OTel default 15-boundary ladder.
    template <typename T>
    std::shared_ptr<Histogram<T>> CreateHistogram(std::string name,
                                                  std::string description = {},
                                                  std::string unit = {})
    {
        return CreateHistogram<T>(std::move(name),
                                  std::move(description),
                                  std::move(unit),
                                  std::vector<double>(kDefaultHistogramBoundaries.begin(),
                                                      kDefaultHistogramBoundaries.end()));
    }

    /// @brief Create a base-2 exponential Histogram with explicit scale/bucket limits.
    ///
    /// @param max_scale   Starting scale (≤ 20). Storage downscales automatically
    ///                    when a bucket array would exceed @p max_buckets.
    /// @param max_buckets Per-side bucket count cap (downscale trigger).
    template <typename T>
    std::shared_ptr<ExponentialHistogram<T>> CreateExponentialHistogram(std::string name,
                                                                        std::string description,
                                                                        std::string unit,
                                                                        std::int32_t max_scale,
                                                                        std::int32_t max_buckets);

    /// @brief Create an exponential Histogram with OTel defaults (scale 20, 160 buckets/side).
    template <typename T>
    std::shared_ptr<ExponentialHistogram<T>> CreateExponentialHistogram(
        std::string name, std::string description = {}, std::string unit = {})
    {
        return CreateExponentialHistogram<T>(std::move(name),
                                             std::move(description),
                                             std::move(unit),
                                             kDefaultExpHistogramMaxScale,
                                             kDefaultExpHistogramMaxBuckets);
    }

    // ── Asynchronous (observable) instruments ──────────────────────────────

    /// @brief Create an observable monotonic Sum instrument.
    template <typename T>
    ObservableCounter<T> CreateObservableCounter(std::string name,
                                                 std::string description,
                                                 std::string unit,
                                                 ObservableCallback<T> callback);

    /// @brief Create an observable non-monotonic Sum instrument.
    template <typename T>
    ObservableUpDownCounter<T> CreateObservableUpDownCounter(std::string name,
                                                             std::string description,
                                                             std::string unit,
                                                             ObservableCallback<T> callback);

    /// @brief Create an observable last-write-wins Gauge instrument.
    template <typename T>
    ObservableGauge<T> CreateObservableGauge(std::string name,
                                             std::string description,
                                             std::string unit,
                                             ObservableCallback<T> callback);

private:
    virtual std::shared_ptr<Counter<std::int64_t>> DoCreateCounterI64(std::string name,
                                                                      std::string description,
                                                                      std::string unit) = 0;
    virtual std::shared_ptr<Counter<double>> DoCreateCounterDouble(std::string name,
                                                                   std::string description,
                                                                   std::string unit) = 0;

    virtual std::shared_ptr<UpDownCounter<std::int64_t>> DoCreateUpDownCounterI64(
        std::string name, std::string description, std::string unit) = 0;
    virtual std::shared_ptr<UpDownCounter<double>> DoCreateUpDownCounterDouble(
        std::string name, std::string description, std::string unit) = 0;

    virtual std::shared_ptr<Gauge<std::int64_t>> DoCreateGaugeI64(std::string name,
                                                                  std::string description,
                                                                  std::string unit) = 0;
    virtual std::shared_ptr<Gauge<double>> DoCreateGaugeDouble(std::string name,
                                                               std::string description,
                                                               std::string unit) = 0;

    virtual std::shared_ptr<Histogram<std::int64_t>> DoCreateHistogramI64(
        std::string name,
        std::string description,
        std::string unit,
        std::vector<double> boundaries) = 0;
    virtual std::shared_ptr<Histogram<double>> DoCreateHistogramDouble(
        std::string name,
        std::string description,
        std::string unit,
        std::vector<double> boundaries) = 0;

    virtual std::shared_ptr<ExponentialHistogram<std::int64_t>> DoCreateExponentialHistogramI64(
        std::string name,
        std::string description,
        std::string unit,
        std::int32_t max_scale,
        std::int32_t max_buckets) = 0;
    virtual std::shared_ptr<ExponentialHistogram<double>> DoCreateExponentialHistogramDouble(
        std::string name,
        std::string description,
        std::string unit,
        std::int32_t max_scale,
        std::int32_t max_buckets) = 0;

    virtual ObservableCounter<std::int64_t> DoCreateObservableCounterI64(
        std::string name,
        std::string description,
        std::string unit,
        ObservableCallback<std::int64_t> callback) = 0;
    virtual ObservableCounter<double> DoCreateObservableCounterDouble(
        std::string name,
        std::string description,
        std::string unit,
        ObservableCallback<double> callback) = 0;

    virtual ObservableUpDownCounter<std::int64_t> DoCreateObservableUpDownCounterI64(
        std::string name,
        std::string description,
        std::string unit,
        ObservableCallback<std::int64_t> callback) = 0;
    virtual ObservableUpDownCounter<double> DoCreateObservableUpDownCounterDouble(
        std::string name,
        std::string description,
        std::string unit,
        ObservableCallback<double> callback) = 0;

    virtual ObservableGauge<std::int64_t> DoCreateObservableGaugeI64(
        std::string name,
        std::string description,
        std::string unit,
        ObservableCallback<std::int64_t> callback) = 0;
    virtual ObservableGauge<double> DoCreateObservableGaugeDouble(
        std::string name,
        std::string description,
        std::string unit,
        ObservableCallback<double> callback) = 0;
};

// ── Explicit specializations — Counter ────────────────────────────────────────

template <>
inline std::shared_ptr<Counter<std::int64_t>> Meter::CreateCounter<std::int64_t>(
    std::string name, std::string description, std::string unit)
{
    return DoCreateCounterI64(std::move(name), std::move(description), std::move(unit));
}

template <>
inline std::shared_ptr<Counter<double>> Meter::CreateCounter<double>(std::string name,
                                                                     std::string description,
                                                                     std::string unit)
{
    return DoCreateCounterDouble(std::move(name), std::move(description), std::move(unit));
}

// ── Explicit specializations — UpDownCounter ──────────────────────────────────

template <>
inline std::shared_ptr<UpDownCounter<std::int64_t>> Meter::CreateUpDownCounter<std::int64_t>(
    std::string name, std::string description, std::string unit)
{
    return DoCreateUpDownCounterI64(std::move(name), std::move(description), std::move(unit));
}

template <>
inline std::shared_ptr<UpDownCounter<double>> Meter::CreateUpDownCounter<double>(
    std::string name, std::string description, std::string unit)
{
    return DoCreateUpDownCounterDouble(std::move(name), std::move(description), std::move(unit));
}

// ── Explicit specializations — Gauge ──────────────────────────────────────────

template <>
inline std::shared_ptr<Gauge<std::int64_t>> Meter::CreateGauge<std::int64_t>(
    std::string name, std::string description, std::string unit)
{
    return DoCreateGaugeI64(std::move(name), std::move(description), std::move(unit));
}

template <>
inline std::shared_ptr<Gauge<double>> Meter::CreateGauge<double>(std::string name,
                                                                 std::string description,
                                                                 std::string unit)
{
    return DoCreateGaugeDouble(std::move(name), std::move(description), std::move(unit));
}

// ── Explicit specializations — Histogram (4-arg) ──────────────────────────────

template <>
inline std::shared_ptr<Histogram<std::int64_t>> Meter::CreateHistogram<std::int64_t>(
    std::string name, std::string description, std::string unit, std::vector<double> boundaries)
{
    return DoCreateHistogramI64(
        std::move(name), std::move(description), std::move(unit), std::move(boundaries));
}

template <>
inline std::shared_ptr<Histogram<double>> Meter::CreateHistogram<double>(
    std::string name, std::string description, std::string unit, std::vector<double> boundaries)
{
    return DoCreateHistogramDouble(
        std::move(name), std::move(description), std::move(unit), std::move(boundaries));
}

// ── Explicit specializations — ObservableCounter ──────────────────────────────

template <>
inline ObservableCounter<std::int64_t> Meter::CreateObservableCounter<std::int64_t>(
    std::string name,
    std::string description,
    std::string unit,
    ObservableCallback<std::int64_t> callback)
{
    return DoCreateObservableCounterI64(
        std::move(name), std::move(description), std::move(unit), std::move(callback));
}

template <>
inline ObservableCounter<double> Meter::CreateObservableCounter<double>(
    std::string name,
    std::string description,
    std::string unit,
    ObservableCallback<double> callback)
{
    return DoCreateObservableCounterDouble(
        std::move(name), std::move(description), std::move(unit), std::move(callback));
}

// ── Explicit specializations — ObservableUpDownCounter ────────────────────────

template <>
inline ObservableUpDownCounter<std::int64_t> Meter::CreateObservableUpDownCounter<std::int64_t>(
    std::string name,
    std::string description,
    std::string unit,
    ObservableCallback<std::int64_t> callback)
{
    return DoCreateObservableUpDownCounterI64(
        std::move(name), std::move(description), std::move(unit), std::move(callback));
}

template <>
inline ObservableUpDownCounter<double> Meter::CreateObservableUpDownCounter<double>(
    std::string name,
    std::string description,
    std::string unit,
    ObservableCallback<double> callback)
{
    return DoCreateObservableUpDownCounterDouble(
        std::move(name), std::move(description), std::move(unit), std::move(callback));
}

// ── Explicit specializations — ObservableGauge ────────────────────────────────

template <>
inline ObservableGauge<std::int64_t> Meter::CreateObservableGauge<std::int64_t>(
    std::string name,
    std::string description,
    std::string unit,
    ObservableCallback<std::int64_t> callback)
{
    return DoCreateObservableGaugeI64(
        std::move(name), std::move(description), std::move(unit), std::move(callback));
}

template <>
inline ObservableGauge<double> Meter::CreateObservableGauge<double>(
    std::string name,
    std::string description,
    std::string unit,
    ObservableCallback<double> callback)
{
    return DoCreateObservableGaugeDouble(
        std::move(name), std::move(description), std::move(unit), std::move(callback));
}

// ── Explicit specializations — ExponentialHistogram (5-arg) ───────────────────

template <>
inline std::shared_ptr<ExponentialHistogram<std::int64_t>>
Meter::CreateExponentialHistogram<std::int64_t>(std::string name,
                                                std::string description,
                                                std::string unit,
                                                std::int32_t max_scale,
                                                std::int32_t max_buckets)
{
    return DoCreateExponentialHistogramI64(
        std::move(name), std::move(description), std::move(unit), max_scale, max_buckets);
}

template <>
inline std::shared_ptr<ExponentialHistogram<double>> Meter::CreateExponentialHistogram<double>(
    std::string name,
    std::string description,
    std::string unit,
    std::int32_t max_scale,
    std::int32_t max_buckets)
{
    return DoCreateExponentialHistogramDouble(
        std::move(name), std::move(description), std::move(unit), max_scale, max_buckets);
}

}  // namespace microtel
