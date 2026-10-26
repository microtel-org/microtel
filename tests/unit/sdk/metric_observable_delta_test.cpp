// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0
//
// Observable sums under delta temporality (issue #450). The callback reports a
// cumulative total; a delta collection reports the change since the series'
// previous observation:
//  - a series' first observation is reported in full;
//  - a series a collection doesn't observe is forgotten, and is new when it
//    returns;
//  - a monotonic counter whose value goes down has reset, so the new value is
//    the delta; an up-down counter reports the (possibly negative) change.
// Cumulative collections report the observation unchanged.

#include "microtel/attribute.hpp"
#include "microtel/internal/metric_batch.hpp"

#include "sdk/metric_observable_instruments.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace mt = microtel;
namespace mts = microtel::sdk;
namespace mti = microtel::internal;

namespace
{

constexpr auto kDelta = mti::AggregationTemporality::Delta;
constexpr auto kCumulative = mti::AggregationTemporality::Cumulative;

std::vector<mt::KeyValue> Series()
{
    return {mt::KeyValue{.key = "pid", .value = std::int64_t{1001}}};
}

/// A callback that reports, per collection, the next scripted value for one
/// series, or nothing for a `nullopt` entry.
template <typename T>
class ScriptedCallback
{
public:
    explicit ScriptedCallback(std::vector<std::optional<T>> script) : m_script(std::move(script)) {}

    void operator()(mts::ObservableResult<T>& result)
    {
        const std::optional<T> next = m_script.at(m_next++);
        if (next.has_value())
        {
            result.Observe(*next, Series());
        }
    }

private:
    std::vector<std::optional<T>> m_script;
    std::size_t m_next = 0;
};

/// Collects @p stream once per script entry and returns each collection's
/// single value, or `nullopt` for a collection with no point.
template <typename T>
std::vector<std::optional<T>> CollectAll(mts::MetricStreamObservableSum<T>& stream,
                                         mti::AggregationTemporality temporality,
                                         std::size_t collections)
{
    std::vector<std::optional<T>> values;
    for (std::size_t i = 0; i < collections; ++i)
    {
        const auto record = stream.Collect(temporality);
        const auto& points = std::get<mti::SumData>(record.data).points;
        values.push_back(points.empty() ? std::nullopt
                                        : std::optional<T>{std::get<T>(points.at(0).value)});
    }
    return values;
}

template <typename T>
std::vector<std::optional<T>> CollectScript(bool monotonic,
                                            mti::AggregationTemporality temporality,
                                            std::vector<std::optional<T>> script)
{
    const std::size_t collections = script.size();
    mts::MetricStreamObservableSum<T> stream{
        "observed", "", "", monotonic, ScriptedCallback<T>{std::move(script)}};
    return CollectAll(stream, temporality, collections);
}

using I64 = std::optional<std::int64_t>;

TEST(ObservableDeltaTest, Counter_ReportsChangeSinceLastObservation)
{
    EXPECT_EQ(CollectScript<std::int64_t>(true, kDelta, {100, 110, 125}),
              (std::vector<I64>{100, 10, 15}));
}

TEST(ObservableDeltaTest, Counter_FirstObservationIsReportedInFull)
{
    EXPECT_EQ(CollectScript<std::int64_t>(true, kDelta, {50}), (std::vector<I64>{50}));
}

TEST(ObservableDeltaTest, Counter_UnobservedSeriesIsForgottenAndReturnsAsNew)
{
    EXPECT_EQ(CollectScript<std::int64_t>(true, kDelta, {50, std::nullopt, 20, 25}),
              (std::vector<I64>{50, std::nullopt, 20, 5}));
}

TEST(ObservableDeltaTest, Counter_DecreaseIsAResetAndReportsTheNewValue)
{
    EXPECT_EQ(CollectScript<std::int64_t>(true, kDelta, {100, 30, 45}),
              (std::vector<I64>{100, 30, 15}));
}

TEST(ObservableDeltaTest, CounterDouble_ReportsChangeSinceLastObservation)
{
    using F64 = std::optional<double>;
    EXPECT_EQ(CollectScript<double>(true, kDelta, {1.5, 4.0}), (std::vector<F64>{1.5, 2.5}));
}

TEST(ObservableDeltaTest, UpDownCounter_ReportsNegativeChange)
{
    EXPECT_EQ(CollectScript<std::int64_t>(false, kDelta, {10, 4, 7}),
              (std::vector<I64>{10, -6, 3}));
}

TEST(ObservableDeltaTest, Cumulative_ReportsTheObservationUnchanged)
{
    EXPECT_EQ(CollectScript<std::int64_t>(true, kCumulative, {100, 110, 30}),
              (std::vector<I64>{100, 110, 30}));
}

}  // namespace
