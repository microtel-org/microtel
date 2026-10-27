// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0
//
// Data-point timestamps (issue #448), per the OTel metrics SDK "Start
// timestamps" rules:
//  - time = the collection time, shared by every point of one collection.
//  - Delta: start = the previous collection's time, or the instrument's
//    creation time on its first collection, so successive windows chain with
//    no gap.
//  - Cumulative, synchronous: start = the series' first measurement, constant
//    across collections.
//  - Cumulative, asynchronous: start = the instrument's creation time if the
//    series was observed in the first collection, else the collection before
//    its first observation.
//  - Gauges carry time only; OTLP leaves a gauge's start time optional.

#include "microtel/attribute.hpp"
#include "microtel/internal/metric_batch.hpp"
#include "microtel/resource.hpp"

#include "sdk/metric_exp_histogram_storage.hpp"
#include "sdk/metric_gauge_storage.hpp"
#include "sdk/metric_histogram_storage.hpp"
#include "sdk/metric_observable_instruments.hpp"
#include "sdk/metric_producer.hpp"
#include "sdk/metric_stream_impls.hpp"
#include "sdk/metric_sum_storage.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
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

using Clock = std::chrono::system_clock;
using TimePoint = Clock::time_point;

constexpr auto kCumulative = mti::AggregationTemporality::Cumulative;
constexpr auto kDelta = mti::AggregationTemporality::Delta;

std::vector<mt::KeyValue> SeriesA()
{
    return {mt::KeyValue{.key = "series", .value = std::string{"a"}}};
}

std::vector<mt::KeyValue> SeriesB()
{
    return {mt::KeyValue{.key = "series", .value = std::string{"b"}}};
}

/// Three collection times after anything a test records, a second apart.
struct Collections
{
    TimePoint first = Clock::now() + std::chrono::hours(1);
    TimePoint second = first + std::chrono::seconds(1);
    TimePoint third = second + std::chrono::seconds(1);
};

/// The window [before, after] that a `Clock::now()` taken in between lies in.
struct Window
{
    TimePoint before;
    TimePoint after;
};

void ExpectWithin(TimePoint t, const Window& w)
{
    EXPECT_GE(t, w.before);
    EXPECT_LE(t, w.after);
}

/// The point whose attributes are @p attrs; fails the test if absent.
template <typename Point>
const Point& PointFor(const std::vector<Point>& points, const std::vector<mt::KeyValue>& attrs)
{
    for (const Point& p : points)
    {
        if (p.attributes.size() == attrs.size() && p.attributes[0].value == attrs[0].value)
        {
            return p;
        }
    }
    ADD_FAILURE() << "no point for the requested attribute set";
    static const Point kMissing{};
    return kMissing;
}

// ── Sum ──────────────────────────────────────────────────────────────────────

TEST(MetricTimestampsTest, SumCumulative_StartIsFirstMeasurement_TimeIsEachCollection)
{
    mts::SumStorage<std::int64_t> storage{/*monotonic=*/true};
    const Collections c;

    Window measured{.before = Clock::now(), .after = {}};
    storage.Add(1, SeriesA());
    measured.after = Clock::now();

    const auto d1 = storage.Collect(kCumulative, c.first);
    ASSERT_EQ(d1.points.size(), 1U);
    ExpectWithin(d1.points[0].start_time, measured);
    EXPECT_EQ(d1.points[0].time, c.first);
    const TimePoint start = d1.points[0].start_time;

    storage.Add(1, SeriesA());
    const auto d2 = storage.Collect(kCumulative, c.second);
    ASSERT_EQ(d2.points.size(), 1U);
    EXPECT_EQ(d2.points[0].start_time, start);
    EXPECT_EQ(d2.points[0].time, c.second);

    const auto d3 = storage.Collect(kCumulative, c.third);
    ASSERT_EQ(d3.points.size(), 1U);
    EXPECT_EQ(d3.points[0].start_time, start);
    EXPECT_EQ(d3.points[0].time, c.third);
}

TEST(MetricTimestampsTest, SumCumulative_LateSeriesStartsAtItsOwnFirstMeasurement)
{
    mts::SumStorage<std::int64_t> storage{/*monotonic=*/true};
    const Collections c;
    storage.Add(1, SeriesA());
    const auto d1 = storage.Collect(kCumulative, c.first);
    ASSERT_EQ(d1.points.size(), 1U);
    const TimePoint start_a = d1.points[0].start_time;

    Window first_b{.before = Clock::now(), .after = {}};
    storage.Add(1, SeriesB());
    first_b.after = Clock::now();

    const auto d2 = storage.Collect(kCumulative, c.second);
    ASSERT_EQ(d2.points.size(), 2U);
    EXPECT_EQ(PointFor(d2.points, SeriesA()).start_time, start_a);
    ExpectWithin(PointFor(d2.points, SeriesB()).start_time, first_b);
}

// Unlike an observable series, a synchronous one isn't absent from a
// collection just because nothing was measured in its interval: storage still
// reports it, so it keeps its start.
TEST(MetricTimestampsTest, SumCumulative_QuietSeriesKeepsItsStart)
{
    mts::SumStorage<std::int64_t> storage{/*monotonic=*/true};
    const Collections c;
    storage.Add(1, SeriesA());
    storage.Add(1, SeriesB());
    const auto d1 = storage.Collect(kCumulative, c.first);
    ASSERT_EQ(d1.points.size(), 2U);
    const TimePoint start_b = PointFor(d1.points, SeriesB()).start_time;

    storage.Add(1, SeriesA());
    (void)storage.Collect(kCumulative, c.second);
    storage.Add(1, SeriesA());
    const auto d3 = storage.Collect(kCumulative, c.third);

    ASSERT_EQ(d3.points.size(), 2U);
    EXPECT_EQ(PointFor(d3.points, SeriesB()).start_time, start_b);
    EXPECT_EQ(std::get<std::int64_t>(PointFor(d3.points, SeriesB()).value), 1);
}

TEST(MetricTimestampsTest, SumDelta_WindowsChainFromCreation)
{
    Window created{.before = Clock::now(), .after = {}};
    mts::SumStorage<std::int64_t> storage{/*monotonic=*/true};
    created.after = Clock::now();
    const Collections c;

    storage.Add(1, SeriesA());
    const auto d1 = storage.Collect(kDelta, c.first);
    ASSERT_EQ(d1.points.size(), 1U);
    ExpectWithin(d1.points[0].start_time, created);
    EXPECT_EQ(d1.points[0].time, c.first);

    storage.Add(1, SeriesA());
    const auto d2 = storage.Collect(kDelta, c.second);
    ASSERT_EQ(d2.points.size(), 1U);
    EXPECT_EQ(d2.points[0].start_time, c.first);
    EXPECT_EQ(d2.points[0].time, c.second);

    // A series new in the third window still starts at the previous collection:
    // every delta point of an instrument shares one start.
    storage.Add(1, SeriesA());
    storage.Add(1, SeriesB());
    const auto d3 = storage.Collect(kDelta, c.third);
    ASSERT_EQ(d3.points.size(), 2U);
    for (const auto& p : d3.points)
    {
        EXPECT_EQ(p.start_time, c.second);
        EXPECT_EQ(p.time, c.third);
    }
}

TEST(MetricTimestampsTest, SumDelta_EmptyWindowStillAdvancesTheStart)
{
    mts::SumStorage<std::int64_t> storage{/*monotonic=*/true};
    const Collections c;
    storage.Add(1, SeriesA());
    static_cast<void>(storage.Collect(kDelta, c.first));
    EXPECT_TRUE(storage.Collect(kDelta, c.second).points.empty());

    storage.Add(1, SeriesA());
    const auto d3 = storage.Collect(kDelta, c.third);
    ASSERT_EQ(d3.points.size(), 1U);
    EXPECT_EQ(d3.points[0].start_time, c.second);
}

// ── Histogram ────────────────────────────────────────────────────────────────

TEST(MetricTimestampsTest, HistogramCumulative_StartIsFirstMeasurement_TimeIsEachCollection)
{
    mts::HistogramStorage<double> storage{{1.0, 10.0}};
    const Collections c;

    Window measured{.before = Clock::now(), .after = {}};
    storage.Record(2.0, SeriesA());
    measured.after = Clock::now();

    const auto d1 = storage.Collect(kCumulative, c.first);
    ASSERT_EQ(d1.points.size(), 1U);
    ExpectWithin(d1.points[0].start_time, measured);
    EXPECT_EQ(d1.points[0].time, c.first);
    const TimePoint start = d1.points[0].start_time;

    storage.Record(3.0, SeriesA());
    const auto d2 = storage.Collect(kCumulative, c.second);
    const auto d3 = storage.Collect(kCumulative, c.third);
    ASSERT_EQ(d2.points.size(), 1U);
    ASSERT_EQ(d3.points.size(), 1U);
    EXPECT_EQ(d2.points[0].start_time, start);
    EXPECT_EQ(d2.points[0].time, c.second);
    EXPECT_EQ(d3.points[0].start_time, start);
    EXPECT_EQ(d3.points[0].time, c.third);
}

TEST(MetricTimestampsTest, HistogramDelta_WindowsChainFromCreation)
{
    Window created{.before = Clock::now(), .after = {}};
    mts::HistogramStorage<double> storage{{1.0, 10.0}};
    created.after = Clock::now();
    const Collections c;

    storage.Record(2.0, SeriesA());
    const auto d1 = storage.Collect(kDelta, c.first);
    storage.Record(2.0, SeriesA());
    const auto d2 = storage.Collect(kDelta, c.second);
    storage.Record(2.0, SeriesA());
    const auto d3 = storage.Collect(kDelta, c.third);

    ASSERT_EQ(d1.points.size(), 1U);
    ASSERT_EQ(d2.points.size(), 1U);
    ASSERT_EQ(d3.points.size(), 1U);
    ExpectWithin(d1.points[0].start_time, created);
    EXPECT_EQ(d1.points[0].time, c.first);
    EXPECT_EQ(d2.points[0].start_time, c.first);
    EXPECT_EQ(d2.points[0].time, c.second);
    EXPECT_EQ(d3.points[0].start_time, c.second);
    EXPECT_EQ(d3.points[0].time, c.third);
}

// ── Exponential histogram ────────────────────────────────────────────────────

TEST(MetricTimestampsTest, ExpHistogramCumulative_StartIsFirstMeasurement_TimeIsEachCollection)
{
    mts::ExponentialHistogramStorage<double> storage{20, 160};
    const Collections c;

    Window measured{.before = Clock::now(), .after = {}};
    storage.Record(4.0, SeriesA());
    measured.after = Clock::now();

    const auto d1 = storage.Collect(kCumulative, c.first);
    ASSERT_EQ(d1.points.size(), 1U);
    ExpectWithin(d1.points[0].start_time, measured);
    EXPECT_EQ(d1.points[0].time, c.first);
    const TimePoint start = d1.points[0].start_time;

    storage.Record(5.0, SeriesA());
    const auto d2 = storage.Collect(kCumulative, c.second);
    const auto d3 = storage.Collect(kCumulative, c.third);
    ASSERT_EQ(d2.points.size(), 1U);
    ASSERT_EQ(d3.points.size(), 1U);
    EXPECT_EQ(d2.points[0].start_time, start);
    EXPECT_EQ(d2.points[0].time, c.second);
    EXPECT_EQ(d3.points[0].start_time, start);
    EXPECT_EQ(d3.points[0].time, c.third);
}

TEST(MetricTimestampsTest, ExpHistogramDelta_WindowsChainFromCreation)
{
    Window created{.before = Clock::now(), .after = {}};
    mts::ExponentialHistogramStorage<double> storage{20, 160};
    created.after = Clock::now();
    const Collections c;

    storage.Record(4.0, SeriesA());
    const auto d1 = storage.Collect(kDelta, c.first);
    storage.Record(4.0, SeriesA());
    const auto d2 = storage.Collect(kDelta, c.second);
    storage.Record(4.0, SeriesA());
    const auto d3 = storage.Collect(kDelta, c.third);

    ASSERT_EQ(d1.points.size(), 1U);
    ASSERT_EQ(d2.points.size(), 1U);
    ASSERT_EQ(d3.points.size(), 1U);
    ExpectWithin(d1.points[0].start_time, created);
    EXPECT_EQ(d1.points[0].time, c.first);
    EXPECT_EQ(d2.points[0].start_time, c.first);
    EXPECT_EQ(d2.points[0].time, c.second);
    EXPECT_EQ(d3.points[0].start_time, c.second);
    EXPECT_EQ(d3.points[0].time, c.third);
}

// ── Gauge ────────────────────────────────────────────────────────────────────

TEST(MetricTimestampsTest, Gauge_TimeIsEachCollection)
{
    mts::GaugeStorage<double> storage;
    const Collections c;
    storage.Record(1.0, SeriesA());
    const auto d1 = storage.Collect(c.first);
    const auto d2 = storage.Collect(c.second);
    const auto d3 = storage.Collect(c.third);
    ASSERT_EQ(d1.points.size(), 1U);
    ASSERT_EQ(d2.points.size(), 1U);
    ASSERT_EQ(d3.points.size(), 1U);
    EXPECT_EQ(d1.points[0].time, c.first);
    EXPECT_EQ(d2.points[0].time, c.second);
    EXPECT_EQ(d3.points[0].time, c.third);
}

// ── Observable instruments ───────────────────────────────────────────────────

/// An observable counter's callback reports series A always, and series B from
/// the second collection on.
struct TwoSeriesCallback
{
    int calls = 0;

    void operator()(mts::ObservableResult<std::int64_t>& result)
    {
        ++calls;
        result.Observe(calls, SeriesA());
        if (calls >= 2)
        {
            result.Observe(calls, SeriesB());
        }
    }
};

TEST(MetricTimestampsTest, ObservableSumCumulative_StartsAtCreationOrPriorCollection)
{
    TwoSeriesCallback callback;
    Window created{.before = Clock::now(), .after = {}};
    mts::MetricStreamObservableSum<std::int64_t> stream{
        "observed", "", "", /*monotonic=*/true, std::ref(callback)};
    created.after = Clock::now();
    const Collections c;

    const auto r1 = stream.Collect(kCumulative, c.first);
    const auto& p1 = std::get<mti::SumData>(r1.data).points;
    ASSERT_EQ(p1.size(), 1U);
    ExpectWithin(p1[0].start_time, created);
    EXPECT_EQ(p1[0].time, c.first);
    const TimePoint start_a = p1[0].start_time;

    // B is first observed in the second collection, so it starts at the first.
    for (const auto& [t, r] : {std::pair{c.second, stream.Collect(kCumulative, c.second)},
                               std::pair{c.third, stream.Collect(kCumulative, c.third)}})
    {
        const auto& points = std::get<mti::SumData>(r.data).points;
        ASSERT_EQ(points.size(), 2U);
        EXPECT_EQ(PointFor(points, SeriesA()).start_time, start_a);
        EXPECT_EQ(PointFor(points, SeriesB()).start_time, c.first);
        EXPECT_EQ(PointFor(points, SeriesA()).time, t);
        EXPECT_EQ(PointFor(points, SeriesB()).time, t);
    }
}

// A series the callback skips is forgotten (ICP 0039 amendment), so when it
// returns it is new and starts at the collection before it came back. This is
// what keeps the per-series starts bounded by what one collection observes.
TEST(MetricTimestampsTest, ObservableSumCumulative_ReturningSeriesStartsAfresh)
{
    int calls = 0;
    mts::MetricStreamObservableSum<std::int64_t> stream{
        "observed",
        "",
        "",
        /*monotonic=*/true,
        [&calls](mts::ObservableResult<std::int64_t>& result)
        {
            ++calls;
            result.Observe(calls, SeriesA());
            if (calls != 2)
            {
                result.Observe(calls, SeriesB());
            }
        }};
    const Collections c;

    const auto r1 = stream.Collect(kCumulative, c.first);
    const TimePoint start_a =
        PointFor(std::get<mti::SumData>(r1.data).points, SeriesA()).start_time;
    (void)stream.Collect(kCumulative, c.second);
    const auto r3 = stream.Collect(kCumulative, c.third);

    const auto& points = std::get<mti::SumData>(r3.data).points;
    ASSERT_EQ(points.size(), 2U);
    EXPECT_EQ(PointFor(points, SeriesA()).start_time, start_a);
    EXPECT_EQ(PointFor(points, SeriesB()).start_time, c.second);
}

TEST(MetricTimestampsTest, ObservableSumDelta_WindowsChainFromCreation)
{
    TwoSeriesCallback callback;
    Window created{.before = Clock::now(), .after = {}};
    mts::MetricStreamObservableSum<std::int64_t> stream{
        "observed", "", "", /*monotonic=*/true, std::ref(callback)};
    created.after = Clock::now();
    const Collections c;

    const auto r1 = stream.Collect(kDelta, c.first);
    const auto r2 = stream.Collect(kDelta, c.second);
    const auto r3 = stream.Collect(kDelta, c.third);
    const auto& p1 = std::get<mti::SumData>(r1.data).points;
    ASSERT_EQ(p1.size(), 1U);
    ExpectWithin(p1[0].start_time, created);
    EXPECT_EQ(p1[0].time, c.first);
    for (const auto& p : std::get<mti::SumData>(r2.data).points)
    {
        EXPECT_EQ(p.start_time, c.first);
        EXPECT_EQ(p.time, c.second);
    }
    for (const auto& p : std::get<mti::SumData>(r3.data).points)
    {
        EXPECT_EQ(p.start_time, c.second);
        EXPECT_EQ(p.time, c.third);
    }
}

TEST(MetricTimestampsTest, ObservableGauge_TimeIsEachCollection)
{
    mts::MetricStreamObservableGauge<double> stream{
        "observed.gauge", "", "", [](mts::ObservableResult<double>& result) {
            result.Observe(1.0, SeriesA());
        }};
    const Collections c;
    for (const TimePoint t : {c.first, c.second, c.third})
    {
        const auto r = stream.Collect(kCumulative, t);
        const auto& points = std::get<mti::GaugeData>(r.data).points;
        ASSERT_EQ(points.size(), 1U);
        EXPECT_EQ(points[0].time, t);
    }
}

// ── Producer ─────────────────────────────────────────────────────────────────

/// The time of a sum or histogram record's first point, if it has one.
std::optional<TimePoint> FirstPointTime(const mti::MetricRecord& rec)
{
    if (const auto* sum = std::get_if<mti::SumData>(&rec.data))
    {
        return sum->points.at(0).time;
    }
    if (const auto* hist = std::get_if<mti::HistogramData>(&rec.data))
    {
        return hist->points.at(0).time;
    }
    return std::nullopt;
}

/// The time of each record's first point, in batch order; the epoch for a
/// record that is neither a sum nor a histogram.
std::vector<TimePoint> FirstPointTimes(const std::vector<mti::MetricBatchHandle>& batches)
{
    std::vector<TimePoint> times;
    for (const auto& batch : batches)
    {
        for (const auto& rec : batch.Metrics())
        {
            times.push_back(FirstPointTime(rec).value_or(TimePoint{}));
        }
    }
    return times;
}

TEST(MetricTimestampsTest, ProducerCollect_SharesOneTimeAcrossEveryStream)
{
    mts::MetricProducer producer{std::make_shared<mt::Resource>()};
    auto sum = std::make_unique<mts::MetricStreamSum<std::int64_t>>("sum", "", "", true);
    auto hist =
        std::make_unique<mts::MetricStreamHistogram<double>>("hist", "", "", std::vector{1.0});
    sum->Storage().Add(1, SeriesA());
    hist->Storage().Record(2.0, SeriesA());
    producer.AddStream(mti::InstrumentationScope{.name = "a", .version = {}}, std::move(sum));
    producer.AddStream(mti::InstrumentationScope{.name = "b", .version = {}}, std::move(hist));

    Window collected{.before = Clock::now(), .after = {}};
    const auto batches = producer.Collect();
    collected.after = Clock::now();

    const std::vector<TimePoint> times = FirstPointTimes(batches);
    ASSERT_EQ(times.size(), 2U);
    ExpectWithin(times[0], collected);
    EXPECT_EQ(times[0], times[1]);
}

}  // namespace
