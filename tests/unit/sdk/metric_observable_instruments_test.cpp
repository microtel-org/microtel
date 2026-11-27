// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0
//
// Behavioural tests for the observable instrument stream classes and for
// Meter::CreateObservable*<T>() factory methods.
//
// Contract under test:
//  - MetricStreamObservableSum::Collect() invokes the callback, converts
//    observations to SumData, and resets between calls.
//  - MetricStreamObservableGauge::Collect() does the same, yielding GaugeData.
//  - Monotonic / non-monotonic flag propagates via is_monotonic.
//  - Temporality is forwarded into SumData.temporality.
//  - Multiple Observe() calls within one cycle produce multiple NumberPoints.
//  - A second Collect() sees only the second callback's observations.
//  - Meter::CreateObservableCounter/UpDownCounter/Gauge register streams and
//    have them appear in MetricProducer::Collect().

#include "sdk/metric_observable_instruments.hpp"

#include "microtel/attribute.hpp"
#include "microtel/internal/metric_batch.hpp"
#include "microtel/resource.hpp"
#include "microtel/view.hpp"

#include "fakes/fake_diagnostics_sink.hpp"
#include "sdk/meter.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <variant>
#include <vector>

namespace mts = microtel::sdk;
namespace mti = microtel::internal;
namespace mt = microtel;

namespace
{

mt::KeyValue Kv(std::string key, mt::AttributeValue value)
{
    return mt::KeyValue{.key = std::move(key), .value = std::move(value)};
}

std::vector<mt::KeyValue> Attrs(std::string key, mt::AttributeValue value)
{
    return {Kv(std::move(key), std::move(value))};
}

std::shared_ptr<mts::MetricProducer> MakeProducer()
{
    return std::make_shared<mts::MetricProducer>(std::make_shared<const mt::Resource>());
}

mti::InstrumentationScope MakeScope(std::string name = "lib")
{
    return mti::InstrumentationScope{.name = std::move(name), .version = "1.0"};
}

}  // namespace

// ── MetricStreamObservableSum ─────────────────────────────────────────────────

TEST(MetricStreamObservableSumTest, CallbackInvokedOnCollect)
{
    bool called = false;
    mts::MetricStreamObservableSum<std::int64_t> stream{
        "req.total",
        "",
        "1",
        mt::InstrumentKind::ObservableCounter,
        [&called](mts::ObservableResult<std::int64_t>& result)
        {
            called = true;
            const auto attrs = Attrs("k", std::string{"v"});
            result.Observe(42, mt::AttributeSpan{attrs});
        }};

    (void)stream.Collect(mti::TemporalitySelector::Cumulative());
    EXPECT_TRUE(called);
}

TEST(MetricStreamObservableSumTest, SumDataIsMonotonicForCounter)
{
    mts::MetricStreamObservableSum<std::int64_t> stream{
        "c",
        "",
        "1",
        mt::InstrumentKind::ObservableCounter,
        [](mts::ObservableResult<std::int64_t>&) {}};

    const auto rec = stream.Collect(mti::TemporalitySelector::Cumulative());
    EXPECT_TRUE(std::get<mti::SumData>(rec.data).is_monotonic);
}

TEST(MetricStreamObservableSumTest, SumDataIsNonMonotonicForUpDownCounter)
{
    mts::MetricStreamObservableSum<std::int64_t> stream{
        "c",
        "",
        "1",
        mt::InstrumentKind::ObservableUpDownCounter,
        [](mts::ObservableResult<std::int64_t>&) {}};

    const auto rec = stream.Collect(mti::TemporalitySelector::Cumulative());
    EXPECT_FALSE(std::get<mti::SumData>(rec.data).is_monotonic);
}

TEST(MetricStreamObservableSumTest, ObservationAppearsAsNumberPoint)
{
    const auto attrs = Attrs("host", std::string{"web-01"});
    mts::MetricStreamObservableSum<std::int64_t> stream{
        "req.total",
        "Total requests",
        "1",
        mt::InstrumentKind::ObservableCounter,
        [&attrs](mts::ObservableResult<std::int64_t>& result)
        { result.Observe(7, mt::AttributeSpan{attrs}); }};

    const auto rec = stream.Collect(mti::TemporalitySelector::Cumulative());
    const auto& sum = std::get<mti::SumData>(rec.data);
    ASSERT_EQ(sum.points.size(), 1U);
    EXPECT_EQ(std::get<std::int64_t>(sum.points[0].value), 7);
}

TEST(MetricStreamObservableSumTest, MultipleObservationsYieldMultiplePoints)
{
    const auto a1 = Attrs("host", std::string{"web-01"});
    const auto a2 = Attrs("host", std::string{"web-02"});
    mts::MetricStreamObservableSum<std::int64_t> stream{
        "c",
        "",
        "1",
        mt::InstrumentKind::ObservableCounter,
        [&a1, &a2](mts::ObservableResult<std::int64_t>& result)
        {
            result.Observe(1, mt::AttributeSpan{a1});
            result.Observe(2, mt::AttributeSpan{a2});
        }};

    const auto rec = stream.Collect(mti::TemporalitySelector::Cumulative());
    EXPECT_EQ(std::get<mti::SumData>(rec.data).points.size(), 2U);
}

TEST(MetricStreamObservableSumTest, CallbackResultResetBetweenCollects)
{
    int calls = 0;
    const auto attrs = Attrs("k", std::string{"v"});
    mts::MetricStreamObservableSum<std::int64_t> stream{
        "c",
        "",
        "1",
        mt::InstrumentKind::ObservableCounter,
        [&calls, &attrs](mts::ObservableResult<std::int64_t>& result)
        {
            ++calls;
            // First call: observe 10; second call: observe 20.
            result.Observe(calls == 1 ? 10 : 20, mt::AttributeSpan{attrs});
        }};

    (void)stream.Collect(mti::TemporalitySelector::Cumulative());
    const auto rec = stream.Collect(mti::TemporalitySelector::Cumulative());
    const auto& sum = std::get<mti::SumData>(rec.data);
    ASSERT_EQ(sum.points.size(), 1U);
    EXPECT_EQ(std::get<std::int64_t>(sum.points[0].value), 20);
}

TEST(MetricStreamObservableSumTest, TemporalityForwardedIntoSumData)
{
    mts::MetricStreamObservableSum<std::int64_t> stream{
        "c",
        "",
        "1",
        mt::InstrumentKind::ObservableCounter,
        [](mts::ObservableResult<std::int64_t>&) {}};

    const auto rec = stream.Collect(mti::TemporalitySelector::Delta());
    EXPECT_EQ(std::get<mti::SumData>(rec.data).temporality, mti::AggregationTemporality::Delta);
}

// ── Temporality resolved through the stream's kind (ICP 0045) ────────────────

namespace
{

/// The temporality an observable Sum of @p kind collects with under @p selector.
mti::AggregationTemporality ObservableSumTemporality(mt::InstrumentKind kind,
                                                     const mti::TemporalitySelector& selector)
{
    mts::MetricStreamObservableSum<std::int64_t> stream{
        "c", "", "1", kind, [](mts::ObservableResult<std::int64_t>& result) {
            result.Observe(1, {});
        }};
    return std::get<mti::SumData>(stream.Collect(selector).data).temporality;
}

}  // namespace

TEST(MetricStreamObservableSumTest, ObservableUpDownCounterIsCumulativeUnderDeltaPreset)
{
    EXPECT_EQ(ObservableSumTemporality(mt::InstrumentKind::ObservableUpDownCounter,
                                       mti::TemporalitySelector::Delta()),
              mti::AggregationTemporality::Cumulative);
}

TEST(MetricStreamObservableSumTest, ObservableCounterIsCumulativeUnderLowMemoryPreset)
{
    EXPECT_EQ(ObservableSumTemporality(mt::InstrumentKind::ObservableCounter,
                                       mti::TemporalitySelector::LowMemory()),
              mti::AggregationTemporality::Cumulative);
}

TEST(MetricStreamObservableSumTest, ObservableUpDownCounterFollowsAnAllDeltaSelector)
{
    // The stream asks the selector for its own kind; nothing hard-codes the
    // up-down counter as cumulative.
    EXPECT_EQ(
        ObservableSumTemporality(mt::InstrumentKind::ObservableUpDownCounter,
                                 mti::TemporalitySelector{mti::AggregationTemporality::Delta}),
        mti::AggregationTemporality::Delta);
}

TEST(MetricStreamObservableSumTest, CumulativeResolvedSeriesIsNotDiffed)
{
    // Under Delta an ObservableUpDownCounter is cumulative, so each export
    // carries the observed value, not the change since the last one.
    std::int64_t observed = 5;
    mts::MetricStreamObservableSum<std::int64_t> stream{
        "c",
        "",
        "1",
        mt::InstrumentKind::ObservableUpDownCounter,
        [&observed](mts::ObservableResult<std::int64_t>& result) { result.Observe(observed, {}); }};

    (void)stream.Collect(mti::TemporalitySelector::Delta());
    observed = 8;
    const auto sum = std::get<mti::SumData>(stream.Collect(mti::TemporalitySelector::Delta()).data);
    ASSERT_EQ(sum.points.size(), 1U);
    EXPECT_EQ(std::get<std::int64_t>(sum.points.at(0).value), 8);
}

// ── MetricStreamObservableGauge ───────────────────────────────────────────────

TEST(MetricStreamObservableGaugeTest, ObservationAppearsAsGaugePoint)
{
    const auto attrs = Attrs("sensor", std::string{"therm-1"});
    mts::MetricStreamObservableGauge<double> stream{
        "room.temp", "Room temperature", "Cel", [&attrs](auto& result) {
            result.Observe(21.5, mt::AttributeSpan{attrs});
        }};

    const auto rec = stream.Collect(mti::TemporalitySelector::Cumulative());
    const auto& gd = std::get<mti::GaugeData>(rec.data);
    ASSERT_EQ(gd.points.size(), 1U);
    EXPECT_DOUBLE_EQ(std::get<double>(gd.points[0].value), 21.5);
}

TEST(MetricStreamObservableGaugeTest, MultipleObservationsYieldMultiplePoints)
{
    const auto a1 = Attrs("sensor", std::string{"s1"});
    const auto a2 = Attrs("sensor", std::string{"s2"});
    mts::MetricStreamObservableGauge<double> stream{
        "temp",
        "",
        "Cel",
        [&a1, &a2](mts::ObservableResult<double>& result)
        {
            result.Observe(20.0, mt::AttributeSpan{a1});
            result.Observe(25.0, mt::AttributeSpan{a2});
        }};

    const auto rec = stream.Collect(mti::TemporalitySelector::Cumulative());
    EXPECT_EQ(std::get<mti::GaugeData>(rec.data).points.size(), 2U);
}

TEST(MetricStreamObservableGaugeTest, CallbackResultResetBetweenCollects)
{
    int calls = 0;
    const auto attrs = Attrs("k", std::string{"v"});
    mts::MetricStreamObservableGauge<double> stream{
        "g",
        "",
        "1",
        [&calls, &attrs](mts::ObservableResult<double>& result)
        {
            ++calls;
            result.Observe(calls == 1 ? 1.0 : 2.0, mt::AttributeSpan{attrs});
        }};

    (void)stream.Collect(mti::TemporalitySelector::Cumulative());
    const auto rec = stream.Collect(mti::TemporalitySelector::Cumulative());
    const auto& gd = std::get<mti::GaugeData>(rec.data);
    ASSERT_EQ(gd.points.size(), 1U);
    EXPECT_DOUBLE_EQ(std::get<double>(gd.points[0].value), 2.0);
}

// ── Meter integration ─────────────────────────────────────────────────────────

TEST(MeterObservableTest, CreateObservableCounterRegistersWithProducer)
{
    auto producer = MakeProducer();
    mts::Meter meter{MakeScope(), producer};
    const auto attrs = Attrs("k", std::string{"v"});
    (void)meter.CreateObservableCounter<std::int64_t>(
        "req.total",
        "",
        "1",
        [&attrs](mts::ObservableResult<std::int64_t>& result)
        { result.Observe(5, mt::AttributeSpan{attrs}); });

    const auto handles = producer->Collect();
    ASSERT_EQ(handles.size(), 1U);
    ASSERT_EQ(handles[0].Metrics().size(), 1U);
    const auto& sum = std::get<mti::SumData>(handles[0].Metrics()[0].data);
    EXPECT_TRUE(sum.is_monotonic);
    ASSERT_EQ(sum.points.size(), 1U);
    EXPECT_EQ(std::get<std::int64_t>(sum.points[0].value), 5);
}

TEST(MeterObservableTest, CreateObservableUpDownCounterNonMonotonic)
{
    auto producer = MakeProducer();
    mts::Meter meter{MakeScope(), producer};
    (void)meter.CreateObservableUpDownCounter<std::int64_t>(
        "q.depth", "", "1", [](mts::ObservableResult<std::int64_t>&) {});

    const auto handles = producer->Collect();
    const auto& sum = std::get<mti::SumData>(handles[0].Metrics()[0].data);
    EXPECT_FALSE(sum.is_monotonic);
}

TEST(MeterObservableTest, CreateObservableGaugeYieldsGaugeData)
{
    auto producer = MakeProducer();
    mts::Meter meter{MakeScope(), producer};
    const auto attrs = Attrs("sensor", std::string{"s1"});
    (void)meter.CreateObservableGauge<double>("room.temp",
                                              "",
                                              "Cel",
                                              [&attrs](mts::ObservableResult<double>& result)
                                              { result.Observe(22.0, mt::AttributeSpan{attrs}); });

    const auto handles = producer->Collect();
    const auto& gd = std::get<mti::GaugeData>(handles[0].Metrics()[0].data);
    ASSERT_EQ(gd.points.size(), 1U);
    EXPECT_DOUBLE_EQ(std::get<double>(gd.points[0].value), 22.0);
}

// ── ObservableResult — cardinality cap ───────────────────────────────────────

namespace
{

std::uint64_t CardinalityDrops(const mt::testing::FakeDiagnosticsSink& sink)
{
    return sink.drop_counters[static_cast<std::size_t>(mt::DropReason::CardinalityOverflow)];
}

bool IsOverflowPoint(const mti::NumberPoint& pt)
{
    return std::ranges::any_of(pt.attributes,
                               [](const mt::KeyValue& kv)
                               {
                                   return kv.key == "otel.metric.overflow" &&
                                          std::holds_alternative<bool>(kv.value) &&
                                          std::get<bool>(kv.value);
                               });
}

}  // namespace

TEST(ObservableResultTest, ObserveEnforcesCardinalityCapForNewKeys)
{
    mt::testing::FakeDiagnosticsSink sink;
    mts::ObservableResult<std::int64_t> result{2, &sink};

    const auto a1 = Attrs("host", std::string{"web-01"});
    const auto a2 = Attrs("host", std::string{"web-02"});
    const auto a3 = Attrs("host", std::string{"web-03"});

    result.Observe(1, mt::AttributeSpan{a1});
    result.Observe(2, mt::AttributeSpan{a2});  // fills cap
    result.Observe(3, mt::AttributeSpan{a3});  // must fold to overflow

    // Map: 2 real entries + 1 overflow entry; 1 drop recorded.
    EXPECT_EQ(result.Map().size(), 3U);
    EXPECT_EQ(CardinalityDrops(sink), 1U);
}

TEST(ObservableResultTest, ExistingKeyUpdateAfterCapDoesNotDrop)
{
    mt::testing::FakeDiagnosticsSink sink;
    mts::ObservableResult<std::int64_t> result{2, &sink};

    const auto a1 = Attrs("host", std::string{"web-01"});
    const auto a2 = Attrs("host", std::string{"web-02"});

    result.Observe(1, mt::AttributeSpan{a1});
    result.Observe(2, mt::AttributeSpan{a2});   // fills cap
    result.Observe(99, mt::AttributeSpan{a1});  // update existing — no overflow

    EXPECT_EQ(CardinalityDrops(sink), 0U);
    EXPECT_EQ(result.Map().size(), 2U);  // no overflow entry
}

TEST(ObservableResultTest, NullSinkIsSafe)
{
    // max_cardinality=1, no sink — exceeding cap must not crash.
    mts::ObservableResult<std::int64_t> result{1, nullptr};

    const auto a1 = Attrs("k", std::string{"a"});
    const auto a2 = Attrs("k", std::string{"b"});

    result.Observe(1, mt::AttributeSpan{a1});  // fills cap
    result.Observe(2, mt::AttributeSpan{a2});  // folds — null sink must be tolerated

    // Map: {a1: 1, overflow: 2}.
    EXPECT_EQ(result.Map().size(), 2U);
}

TEST(ObservableResultTest, ClearResetsMapSoSlotsAreReused)
{
    mt::testing::FakeDiagnosticsSink sink;
    mts::ObservableResult<std::int64_t> result{2, &sink};

    const auto a1 = Attrs("k", std::string{"a"});
    const auto a2 = Attrs("k", std::string{"b"});
    result.Observe(1, mt::AttributeSpan{a1});
    result.Observe(2, mt::AttributeSpan{a2});  // fills cap

    result.Clear();  // reset — map is empty, slots freed

    const auto a3 = Attrs("k", std::string{"c"});
    const auto a4 = Attrs("k", std::string{"d"});
    result.Observe(3, mt::AttributeSpan{a3});
    result.Observe(4, mt::AttributeSpan{a4});

    EXPECT_EQ(CardinalityDrops(sink), 0U);
    EXPECT_EQ(result.Map().size(), 2U);
}

// ── Stream-level overflow tests ───────────────────────────────────────────────

TEST(MetricStreamObservableSumTest, OverflowFoldsIntoOverflowPoint)
{
    mt::testing::FakeDiagnosticsSink sink;
    mts::MetricStreamObservableSum<std::int64_t> stream{
        "c",
        "",
        "1",
        mt::InstrumentKind::ObservableCounter,
        [](mts::ObservableResult<std::int64_t>& result)
        {
            const auto a1 = Attrs("host", std::string{"web-01"});
            const auto a2 = Attrs("host", std::string{"web-02"});
            const auto a3 = Attrs("host", std::string{"web-03"});
            result.Observe(1, mt::AttributeSpan{a1});
            result.Observe(2, mt::AttributeSpan{a2});
            result.Observe(3, mt::AttributeSpan{a3});
        },
        /*max_cardinality=*/2,
        &sink};

    const auto rec = stream.Collect(mti::TemporalitySelector::Cumulative());
    const auto& sum = std::get<mti::SumData>(rec.data);

    EXPECT_EQ(sum.points.size(), 3U);  // 2 real + 1 overflow
    EXPECT_EQ(std::ranges::count_if(sum.points, IsOverflowPoint), 1);
    EXPECT_EQ(CardinalityDrops(sink), 1U);
}

TEST(MetricStreamObservableGaugeTest, OverflowFoldsIntoOverflowPoint)
{
    mt::testing::FakeDiagnosticsSink sink;
    mts::MetricStreamObservableGauge<double> stream{
        "temp",
        "",
        "Cel",
        [](mts::ObservableResult<double>& result)
        {
            const auto a1 = Attrs("sensor", std::string{"s1"});
            const auto a2 = Attrs("sensor", std::string{"s2"});
            const auto a3 = Attrs("sensor", std::string{"s3"});
            result.Observe(20.0, mt::AttributeSpan{a1});
            result.Observe(25.0, mt::AttributeSpan{a2});
            result.Observe(30.0, mt::AttributeSpan{a3});
        },
        /*max_cardinality=*/2,
        &sink};

    const auto rec = stream.Collect(mti::TemporalitySelector::Cumulative());
    const auto& gd = std::get<mti::GaugeData>(rec.data);

    EXPECT_EQ(gd.points.size(), 3U);  // 2 real + 1 overflow
    EXPECT_EQ(std::ranges::count_if(gd.points, IsOverflowPoint), 1);
    EXPECT_EQ(CardinalityDrops(sink), 1U);
}
