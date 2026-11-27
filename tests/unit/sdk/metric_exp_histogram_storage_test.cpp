// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0
//
// Behavioural tests for ExponentialHistogramStorage<T> — base-2 exponential
// histogram aggregation, M12 (docs/metrics-design.md §3).
//
// Mapping (OTel base-2): for scale s and value v > 0, the bucket index is
// ceil(log2(v) * 2^s) - 1, so bucket i covers (2^(i/2^s), 2^((i+1)/2^s)].
// At scale 0 that is index(1)=-1, index(2)=0, index(3)=1, index(8)=2.
// Downscaling by k floor-divides indices by 2^k (merging buckets), bounded by
// max_buckets. There is a known floating-point boundary subtlety at exact
// powers of two (shared with the OTel reference); tests avoid asserting on it.

#include "sdk/metric_exp_histogram_storage.hpp"

#include "microtel/attribute.hpp"
#include "microtel/internal/icurrent_span_source.hpp"
#include "microtel/internal/metric_batch.hpp"
#include "microtel/trace.hpp"

#include "fakes/fake_diagnostics_sink.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <numeric>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace mt = microtel;
namespace mts = microtel::sdk;
namespace mti = microtel::internal;

namespace
{

mt::KeyValue Kv(std::string key, mt::AttributeValue value)
{
    return mt::KeyValue{.key = std::move(key), .value = std::move(value)};
}

std::uint64_t CardinalityDrops(const mt::testing::FakeDiagnosticsSink& sink)
{
    return sink.drop_counters[static_cast<std::size_t>(mt::DropReason::CardinalityOverflow)];
}

std::uint64_t NonFiniteDrops(const mt::testing::FakeDiagnosticsSink& sink)
{
    return sink.drop_counters[static_cast<std::size_t>(mt::DropReason::NonFiniteValue)];
}

std::uint64_t NegativeDrops(const mt::testing::FakeDiagnosticsSink& sink)
{
    return sink.drop_counters.at(static_cast<std::size_t>(mt::DropReason::NegativeHistogramValue));
}

std::uint64_t Sum(const std::vector<std::uint64_t>& counts)
{
    return std::accumulate(counts.begin(), counts.end(), std::uint64_t{0});
}

void RecordN(mts::ExponentialHistogramStorage<std::int64_t>& storage,
             mt::AttributeSpan attrs,
             std::int64_t value,
             int count)
{
    for (int i = 0; i < count; ++i)
    {
        storage.Record(value, attrs);
    }
}

bool IsOverflowPoint(const mti::ExponentialHistogramPoint& pt)
{
    return std::ranges::any_of(pt.attributes,
                               [](const mt::KeyValue& kv)
                               {
                                   return kv.key == "otel.metric.overflow" &&
                                          std::holds_alternative<bool>(kv.value) &&
                                          std::get<bool>(kv.value);
                               });
}

std::vector<double> SortedRealExpHistoSums(const mti::ExponentialHistogramData& data)
{
    std::vector<double> sums;
    for (const auto& pt : data.points)
    {
        if (!IsOverflowPoint(pt))
        {
            sums.push_back(pt.sum);
        }
    }
    std::ranges::sort(sums);
    return sums;
}

class FakeSpanSource : public mti::ICurrentSpanSource
{
public:
    void SetSpan(mt::SpanContext ctx) noexcept
    {
        m_span = std::move(ctx);
    }
    [[nodiscard]] mt::SpanContext GetCurrentSpan() const override
    {
        return m_span;
    }

private:
    mt::SpanContext m_span;
};

mt::SpanContext MakeSampledContext()
{
    mt::TraceId::Bytes tb{};
    tb[0] = 0x01;
    mt::SpanId::Bytes sb{};
    sb[0] = 0x01;
    return mt::SpanContext{
        .trace_id = mt::TraceId{tb},
        .span_id = mt::SpanId{sb},
        .trace_flags = mt::TraceFlags{mt::TraceFlags::kSampled},
        .trace_state = {},
    };
}

}  // namespace

TEST(ExpHistogramStorageTest, ZeroGoesToZeroCount)
{
    mts::ExponentialHistogramStorage<std::int64_t> storage{/*max_scale=*/0, /*max_buckets=*/160};
    const std::vector<mt::KeyValue> attrs{Kv("k", std::string{"v"})};

    storage.Record(0, mt::AttributeSpan{attrs});

    const mti::ExponentialHistogramData data = storage.Collect();
    ASSERT_EQ(data.points.size(), 1U);
    EXPECT_EQ(data.points[0].zero_count, 1U);
    EXPECT_EQ(data.points[0].count, 1U);
    EXPECT_TRUE(data.points[0].positive.bucket_counts.empty());
}

TEST(ExpHistogramStorageTest, PositiveMappingAtScaleZero)
{
    // scale 0: index(1)=-1, index(2)=0, index(3)=1 → offset -1, counts [1,1,1].
    mts::ExponentialHistogramStorage<std::int64_t> storage{0, 160};
    const std::vector<mt::KeyValue> attrs{Kv("k", std::string{"v"})};

    storage.Record(2, mt::AttributeSpan{attrs});
    storage.Record(3, mt::AttributeSpan{attrs});
    storage.Record(1, mt::AttributeSpan{attrs});

    const mti::ExponentialHistogramData data = storage.Collect();
    ASSERT_EQ(data.points.size(), 1U);
    const mti::ExponentialHistogramPoint& point = data.points[0];
    EXPECT_EQ(point.scale, 0);
    EXPECT_EQ(point.positive.offset, -1);
    const std::vector<std::uint64_t> expected{1, 1, 1};
    EXPECT_EQ(point.positive.bucket_counts, expected);
    EXPECT_EQ(point.zero_count, 0U);
}

// ── Negative values — ICP 0047 ────────────────────────────────────────────────
// A negative measurement is dropped before aggregation, so it reaches neither
// sum / count / min nor the negative buckets (which stay, for #554 / #298).

TEST(ExpHistogramStorageTest, NegativeInt64IsDroppedAndCounted)
{
    mt::testing::FakeDiagnosticsSink sink;
    mts::ExponentialHistogramStorage<std::int64_t> storage{
        0, 160, mts::kDefaultMaxCardinality, nullptr, &sink};
    const std::vector<mt::KeyValue> attrs{Kv("k", std::string{"v"})};

    storage.Record(3, mt::AttributeSpan{attrs});
    storage.Record(-2, mt::AttributeSpan{attrs});

    const mti::ExponentialHistogramData data = storage.Collect();
    ASSERT_EQ(data.points.size(), 1U);
    const mti::ExponentialHistogramPoint& point = data.points.at(0);
    EXPECT_EQ(point.count, 1U);
    EXPECT_DOUBLE_EQ(point.sum, 3.0);
    EXPECT_EQ(point.min, std::optional<double>{3.0});
    EXPECT_EQ(Sum(point.positive.bucket_counts), 1U);
    EXPECT_TRUE(point.negative.bucket_counts.empty());
    EXPECT_EQ(NegativeDrops(sink), 1U);
}

TEST(ExpHistogramStorageTest, NegativeDoubleIsDroppedAndCounted)
{
    mt::testing::FakeDiagnosticsSink sink;
    mts::ExponentialHistogramStorage<double> storage{
        20, 160, mts::kDefaultMaxCardinality, nullptr, &sink};
    const std::vector<mt::KeyValue> attrs{Kv("k", std::string{"v"})};

    storage.Record(2.0, mt::AttributeSpan{attrs});
    storage.Record(-0.25, mt::AttributeSpan{attrs});
    storage.Record(-8.0, mt::AttributeSpan{attrs});

    const mti::ExponentialHistogramData data = storage.Collect();
    ASSERT_EQ(data.points.size(), 1U);
    const mti::ExponentialHistogramPoint& point = data.points.at(0);
    EXPECT_EQ(point.count, 1U);
    EXPECT_DOUBLE_EQ(point.sum, 2.0);
    EXPECT_EQ(point.min, std::optional<double>{2.0});
    EXPECT_EQ(point.max, std::optional<double>{2.0});
    EXPECT_TRUE(point.negative.bucket_counts.empty());
    EXPECT_EQ(NegativeDrops(sink), 2U);
    EXPECT_EQ(NonFiniteDrops(sink), 0U);
}

TEST(ExpHistogramStorageTest, NegativeValueAloneCreatesNoSeries)
{
    mt::testing::FakeDiagnosticsSink sink;
    mts::ExponentialHistogramStorage<double> storage{
        20, 160, mts::kDefaultMaxCardinality, nullptr, &sink};
    const std::vector<mt::KeyValue> attrs{Kv("k", std::string{"v"})};
    storage.Record(-1.0, mt::AttributeSpan{attrs});
    EXPECT_TRUE(storage.Collect().points.empty());
    EXPECT_EQ(NegativeDrops(sink), 1U);
}

TEST(ExpHistogramStorageTest, NegativeZeroIsRecordedAsZero)
{
    mt::testing::FakeDiagnosticsSink sink;
    mts::ExponentialHistogramStorage<double> storage{
        20, 160, mts::kDefaultMaxCardinality, nullptr, &sink};
    const std::vector<mt::KeyValue> attrs{Kv("k", std::string{"v"})};
    storage.Record(-0.0, mt::AttributeSpan{attrs});

    const mti::ExponentialHistogramData data = storage.Collect();
    ASSERT_EQ(data.points.size(), 1U);
    const mti::ExponentialHistogramPoint& point = data.points.at(0);
    EXPECT_EQ(point.count, 1U);
    EXPECT_EQ(point.zero_count, 1U);
    EXPECT_DOUBLE_EQ(point.sum, 0.0);
    EXPECT_EQ(point.min, std::optional<double>{0.0});
    EXPECT_TRUE(point.negative.bucket_counts.empty());
    EXPECT_EQ(NegativeDrops(sink), 0U);
}

TEST(ExpHistogramStorageTest, NegativeValueWithNullSinkIsNotDereferenced)
{
    mts::ExponentialHistogramStorage<double> storage{20, 160};  // no sink
    const std::vector<mt::KeyValue> attrs{Kv("k", std::string{"v"})};
    storage.Record(-1.0, mt::AttributeSpan{attrs});
    EXPECT_TRUE(storage.Collect().points.empty());
}

TEST(ExpHistogramStorageTest, NegativeValueDoesNotReplaceTheExemplar)
{
    FakeSpanSource source;
    source.SetSpan(MakeSampledContext());
    mts::ExponentialHistogramStorage<std::int64_t> storage{
        0, 160, mts::kDefaultMaxCardinality, &source};
    const std::vector<mt::KeyValue> attrs{Kv("k", std::string{"v"})};

    storage.Record(4, mt::AttributeSpan{attrs});
    storage.Record(-4, mt::AttributeSpan{attrs});

    const mti::ExponentialHistogramData data = storage.Collect();
    ASSERT_EQ(data.points.size(), 1U);
    ASSERT_EQ(data.points.at(0).exemplars.size(), 1U);
    EXPECT_DOUBLE_EQ(std::get<double>(data.points.at(0).exemplars.at(0).value), 4.0);
}

TEST(ExpHistogramStorageTest, DownscaleWhenSpanExceedsMaxBuckets)
{
    // scale 0, max_buckets 2. index(1)=-1, index(8)=2 → span 4 > 2 → downscale.
    mts::ExponentialHistogramStorage<std::int64_t> storage{0, 2};
    const std::vector<mt::KeyValue> attrs{Kv("k", std::string{"v"})};

    storage.Record(1, mt::AttributeSpan{attrs});
    storage.Record(8, mt::AttributeSpan{attrs});

    const mti::ExponentialHistogramData data = storage.Collect();
    ASSERT_EQ(data.points.size(), 1U);
    const mti::ExponentialHistogramPoint& point = data.points[0];
    EXPECT_LT(point.scale, 0);  // scale was reduced
    EXPECT_EQ(point.count, 2U);
    EXPECT_EQ(Sum(point.positive.bucket_counts), 2U);    // both values conserved
    EXPECT_LE(point.positive.bucket_counts.size(), 2U);  // within max_buckets
}

TEST(ExpHistogramStorageTest, TracksCountSumMinMax)
{
    mts::ExponentialHistogramStorage<double> storage{20, 160};
    const std::vector<mt::KeyValue> attrs{Kv("k", std::string{"v"})};

    storage.Record(2.0, mt::AttributeSpan{attrs});
    storage.Record(8.0, mt::AttributeSpan{attrs});
    storage.Record(0.0, mt::AttributeSpan{attrs});  // counts toward count, not buckets

    const mti::ExponentialHistogramData data = storage.Collect();
    ASSERT_EQ(data.points.size(), 1U);
    const mti::ExponentialHistogramPoint& point = data.points[0];
    EXPECT_EQ(point.count, 3U);
    EXPECT_DOUBLE_EQ(point.sum, 10.0);
    EXPECT_EQ(point.min, std::optional<double>{0.0});
    EXPECT_EQ(point.max, std::optional<double>{8.0});
    EXPECT_EQ(point.zero_count, 1U);
}

TEST(ExpHistogramStorageTest, DistinctAttributeSetsAreDistinctPoints)
{
    mts::ExponentialHistogramStorage<std::int64_t> storage{0, 160};
    const std::vector<mt::KeyValue> a{Kv("r", std::string{"a"})};
    const std::vector<mt::KeyValue> b{Kv("r", std::string{"b"})};

    storage.Record(2, mt::AttributeSpan{a});
    storage.Record(4, mt::AttributeSpan{b});

    EXPECT_EQ(storage.Collect().points.size(), 2U);
}

TEST(ExpHistogramStorageTest, EmptyStorageCollectsNoPoints)
{
    mts::ExponentialHistogramStorage<double> storage{20, 160};
    EXPECT_TRUE(storage.Collect().points.empty());
}

TEST(ExpHistogramStorageTest, CollectIsCumulativeTemporality)
{
    mts::ExponentialHistogramStorage<std::int64_t> storage{20, 160};
    EXPECT_EQ(storage.Collect().temporality, mti::AggregationTemporality::Cumulative);
}

TEST(ExpHistogramStorageTest, DeltaReportsSinceLastCollectAndClears)
{
    mts::ExponentialHistogramStorage<std::int64_t> storage{0, 160};
    const std::vector<mt::KeyValue> attrs{Kv("k", std::string{"v"})};

    storage.Record(2, mt::AttributeSpan{attrs});
    const mti::ExponentialHistogramData first = storage.Collect(mti::AggregationTemporality::Delta);
    ASSERT_EQ(first.points.size(), 1U);
    EXPECT_EQ(first.temporality, mti::AggregationTemporality::Delta);
    EXPECT_EQ(first.points[0].count, 1U);

    // Nothing recorded since → empty delta.
    EXPECT_TRUE(storage.Collect(mti::AggregationTemporality::Delta).points.empty());

    storage.Record(4, mt::AttributeSpan{attrs});
    const mti::ExponentialHistogramData third = storage.Collect(mti::AggregationTemporality::Delta);
    ASSERT_EQ(third.points.size(), 1U);
    EXPECT_EQ(third.points[0].count, 1U);  // only the value since the last collect
}

TEST(ExpHistogramStorageTest, CardinalityOverflowRoutesToOverflowSeries)
{
    mts::ExponentialHistogramStorage<std::int64_t> storage{0, 160, /*max_cardinality=*/2};
    const std::vector<mt::KeyValue> a{Kv("k", std::string{"a"})};
    const std::vector<mt::KeyValue> b{Kv("k", std::string{"b"})};
    const std::vector<mt::KeyValue> c{Kv("k", std::string{"c"})};

    storage.Record(2, mt::AttributeSpan{a});
    storage.Record(3, mt::AttributeSpan{b});
    storage.Record(4, mt::AttributeSpan{c});  // overflows → overflow series
    storage.Record(5, mt::AttributeSpan{c});  // also overflows

    const mti::ExponentialHistogramData data = storage.Collect();
    ASSERT_EQ(data.points.size(), 3U);

    const auto ov_it = std::ranges::find_if(data.points, IsOverflowPoint);
    ASSERT_NE(ov_it, data.points.end());
    EXPECT_EQ(ov_it->count, 2U);  // 2 overflowed recordings
}

// ── Overflow drop accounting (increment 26) ─────────────────────────────────

TEST(ExpHistogramStorageTest, OverflowIncrementsCardinalityDropCounterPerMeasurement)
{
    mt::testing::FakeDiagnosticsSink sink;
    mts::ExponentialHistogramStorage<std::int64_t> storage{
        0, 160, /*max_cardinality=*/2, nullptr, &sink};
    for (int i = 0; i < 5; ++i)  // 2 real series + 3 folded measurements
    {
        const std::vector<mt::KeyValue> attrs{Kv("k", std::to_string(i))};
        storage.Record(3, mt::AttributeSpan{attrs});
    }

    EXPECT_EQ(CardinalityDrops(sink), 3U);
}

TEST(ExpHistogramStorageTest, ExistingSeriesUpdatesAfterOverflowDoNotCount)
{
    mt::testing::FakeDiagnosticsSink sink;
    mts::ExponentialHistogramStorage<std::int64_t> storage{
        0, 160, /*max_cardinality=*/2, nullptr, &sink};
    const std::vector<mt::KeyValue> a{Kv("k", std::string{"a"})};
    const std::vector<mt::KeyValue> b{Kv("k", std::string{"b"})};
    const std::vector<mt::KeyValue> c{Kv("k", std::string{"c"})};
    storage.Record(3, mt::AttributeSpan{a});
    storage.Record(5, mt::AttributeSpan{b});
    storage.Record(7, mt::AttributeSpan{c});  // folds → 1 drop

    storage.Record(4, mt::AttributeSpan{a});  // pre-existing series — no drop
    storage.Record(6, mt::AttributeSpan{b});  // pre-existing series — no drop

    EXPECT_EQ(CardinalityDrops(sink), 1U);
}

TEST(ExpHistogramStorageTest, NullSinkIsSafe)
{
    mts::ExponentialHistogramStorage<std::int64_t> storage{0,
                                                           160,
                                                           /*max_cardinality=*/1};  // no sink
    const std::vector<mt::KeyValue> a{Kv("k", std::string{"a"})};
    const std::vector<mt::KeyValue> b{Kv("k", std::string{"b"})};
    storage.Record(3, mt::AttributeSpan{a});
    storage.Record(5, mt::AttributeSpan{b});  // folds — must not crash

    const mti::ExponentialHistogramData data = storage.Collect();
    ASSERT_EQ(data.points.size(), 2U);
    EXPECT_EQ(std::ranges::count_if(data.points, IsOverflowPoint), 1);
}

// ── Spec-lock characterisation: cardinality-overflow semantics ─────────────
// These lock the overflow contract in place ahead of drop accounting
// (increment 26); they assert current behaviour and act as a regression net.

TEST(ExpHistogramStorageTest, NoOverflowAtExactlyLimitDistinctSets)
{
    mts::ExponentialHistogramStorage<std::int64_t> storage{0, 160, /*max_cardinality=*/3};
    for (int i = 0; i < 3; ++i)
    {
        const std::vector<mt::KeyValue> attrs{Kv("k", std::to_string(i))};
        storage.Record(3, mt::AttributeSpan{attrs});
    }

    const mti::ExponentialHistogramData data = storage.Collect();
    EXPECT_EQ(data.points.size(), 3U);
    EXPECT_EQ(std::ranges::count_if(data.points, IsOverflowPoint), 0);
}

TEST(ExpHistogramStorageTest, CumulativePreOverflowSeriesKeepExportingAfterOverflow)
{
    mts::ExponentialHistogramStorage<std::int64_t> storage{0, 160, /*max_cardinality=*/2};
    const std::vector<mt::KeyValue> a{Kv("k", std::string{"a"})};
    const std::vector<mt::KeyValue> b{Kv("k", std::string{"b"})};
    const std::vector<mt::KeyValue> c{Kv("k", std::string{"c"})};
    storage.Record(3, mt::AttributeSpan{a});
    storage.Record(5, mt::AttributeSpan{b});
    storage.Record(7, mt::AttributeSpan{c});  // folds into the overflow series

    for (int round = 0; round < 2; ++round)
    {
        const mti::ExponentialHistogramData data = storage.Collect();
        ASSERT_EQ(data.points.size(), 3U);
        EXPECT_EQ(SortedRealExpHistoSums(data), (std::vector<double>{3.0, 5.0}));
    }
}

TEST(ExpHistogramStorageTest, EveryMeasurementReflectedInExactlyOneSeries)
{
    mts::ExponentialHistogramStorage<std::int64_t> storage{0, 160, /*max_cardinality=*/2};
    std::uint64_t recorded_count = 0;
    double recorded_sum = 0.0;
    for (int i = 0; i < 5; ++i)
    {
        const std::vector<mt::KeyValue> attrs{Kv("k", std::to_string(i))};
        storage.Record(static_cast<std::int64_t>(i) + 1, mt::AttributeSpan{attrs});
        ++recorded_count;
        recorded_sum += static_cast<double>(i) + 1;
    }

    const mti::ExponentialHistogramData data = storage.Collect();
    ASSERT_EQ(data.points.size(), 3U);  // two real series + overflow
    std::uint64_t exported_count = 0;
    double exported_sum = 0.0;
    for (const auto& pt : data.points)
    {
        exported_count += pt.count;
        exported_sum += pt.sum;
    }
    EXPECT_EQ(exported_count, recorded_count);
    EXPECT_DOUBLE_EQ(exported_sum, recorded_sum);
}

TEST(ExpHistogramStorageTest, DeltaCollectReclaimsCardinalitySlots)
{
    mts::ExponentialHistogramStorage<std::int64_t> storage{0, 160, /*max_cardinality=*/2};
    const std::vector<mt::KeyValue> a{Kv("k", std::string{"a"})};
    const std::vector<mt::KeyValue> b{Kv("k", std::string{"b"})};
    const std::vector<mt::KeyValue> c{Kv("k", std::string{"c"})};
    storage.Record(3, mt::AttributeSpan{a});
    storage.Record(5, mt::AttributeSpan{b});
    storage.Record(7, mt::AttributeSpan{c});                    // folds into the overflow series
    (void)storage.Collect(mti::AggregationTemporality::Delta);  // clears live state

    const std::vector<mt::KeyValue> d{Kv("k", std::string{"d"})};
    storage.Record(9, mt::AttributeSpan{d});

    const mti::ExponentialHistogramData data = storage.Collect(mti::AggregationTemporality::Delta);
    ASSERT_EQ(data.points.size(), 1U);
    EXPECT_FALSE(IsOverflowPoint(data.points[0]));  // d got a real series again
}

TEST(ExpHistogramStorageTest, OverflowSeriesMergesBucketStateAcrossFoldedMeasurements)
{
    mts::ExponentialHistogramStorage<std::int64_t> storage{0, 160, /*max_cardinality=*/1};
    const std::vector<mt::KeyValue> a{Kv("k", std::string{"a"})};
    const std::vector<mt::KeyValue> b{Kv("k", std::string{"b"})};
    const std::vector<mt::KeyValue> c{Kv("k", std::string{"c"})};
    storage.Record(3, mt::AttributeSpan{a});   // real series
    storage.Record(3, mt::AttributeSpan{b});   // folds → scale-0 index 1
    storage.Record(20, mt::AttributeSpan{c});  // folds → scale-0 index 4

    const mti::ExponentialHistogramData data = storage.Collect();
    ASSERT_EQ(data.points.size(), 2U);
    const auto ov_it = std::ranges::find_if(data.points, IsOverflowPoint);
    ASSERT_NE(ov_it, data.points.end());
    // Merged bucket state — not last-write-wins.
    EXPECT_EQ(ov_it->count, 2U);
    EXPECT_DOUBLE_EQ(ov_it->sum, 23.0);
    EXPECT_EQ(ov_it->min, std::optional<double>{3.0});
    EXPECT_EQ(ov_it->max, std::optional<double>{20.0});
    EXPECT_EQ(Sum(ov_it->positive.bucket_counts), 2U);
    EXPECT_EQ(
        std::ranges::count_if(ov_it->positive.bucket_counts, [](std::uint64_t n) { return n > 0; }),
        2);  // two distinct buckets survived the fold
}

TEST(ExpHistogramStorageTest, NonFiniteValueIsDropped)
{
    mt::testing::FakeDiagnosticsSink sink;
    mts::ExponentialHistogramStorage<double> storage{
        20, 160, mts::kDefaultMaxCardinality, nullptr, &sink};
    const std::vector<mt::KeyValue> attrs{Kv("k", std::string{"v"})};
    storage.Record(2.0, mt::AttributeSpan{attrs});
    storage.Record(std::numeric_limits<double>::quiet_NaN(), mt::AttributeSpan{attrs});
    const mti::ExponentialHistogramData data = storage.Collect();
    ASSERT_EQ(data.points.size(), 1U);
    EXPECT_EQ(data.points[0].count, 1U);
    EXPECT_EQ(NonFiniteDrops(sink), 1U);
}

TEST(ExpHistogramStorageTest, FiniteValueRecordsNoNonFiniteDrop)
{
    mt::testing::FakeDiagnosticsSink sink;
    mts::ExponentialHistogramStorage<double> storage{
        20, 160, mts::kDefaultMaxCardinality, nullptr, &sink};
    const std::vector<mt::KeyValue> attrs{Kv("k", std::string{"v"})};
    storage.Record(2.0, mt::AttributeSpan{attrs});
    EXPECT_EQ(NonFiniteDrops(sink), 0U);
}

TEST(ExpHistogramStorageTest, NonFiniteValueWithNullSinkIsNotDereferenced)
{
    mts::ExponentialHistogramStorage<double> storage{20, 160};  // no sink
    const std::vector<mt::KeyValue> attrs{Kv("k", std::string{"v"})};
    storage.Record(std::numeric_limits<double>::quiet_NaN(), mt::AttributeSpan{attrs});
    EXPECT_TRUE(storage.Collect().points.empty());
}

TEST(ExpHistogramStorageTest, ExemplarCapturedWhenSampledSpanActive)
{
    FakeSpanSource source;
    source.SetSpan(MakeSampledContext());
    mts::ExponentialHistogramStorage<std::int64_t> storage{
        0, 160, mts::kDefaultMaxCardinality, &source};
    const std::vector<mt::KeyValue> attrs{Kv("k", std::string{"v"})};

    storage.Record(4, mt::AttributeSpan{attrs});

    const mti::ExponentialHistogramData data = storage.Collect();
    ASSERT_EQ(data.points.size(), 1U);
    ASSERT_EQ(data.points[0].exemplars.size(), 1U);
    EXPECT_TRUE(data.points[0].exemplars[0].span_context.IsValid());
    EXPECT_DOUBLE_EQ(std::get<double>(data.points[0].exemplars[0].value), 4.0);
}

TEST(ExpHistogramStorageTest, ExemplarIsResetAfterCollect)
{
    FakeSpanSource source;
    source.SetSpan(MakeSampledContext());
    mts::ExponentialHistogramStorage<std::int64_t> storage{
        0, 160, mts::kDefaultMaxCardinality, &source};
    const std::vector<mt::KeyValue> attrs{Kv("k", std::string{"v"})};

    storage.Record(2, mt::AttributeSpan{attrs});
    (void)storage.Collect();  // drains exemplar window

    source.SetSpan({});
    storage.Record(3, mt::AttributeSpan{attrs});
    const mti::ExponentialHistogramData data = storage.Collect();
    ASSERT_EQ(data.points.size(), 1U);
    EXPECT_TRUE(data.points[0].exemplars.empty());
}

TEST(ExpHistogramStorageTest, ConcurrentRecordsConserveCount)
{
    mts::ExponentialHistogramStorage<std::int64_t> storage{20, 160};
    const std::vector<mt::KeyValue> attrs{Kv("k", std::string{"v"})};

    constexpr int kThreads = 8;
    constexpr int kPerThread = 1000;
    std::vector<std::thread> workers;
    workers.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t)
    {
        workers.emplace_back(
            RecordN, std::ref(storage), mt::AttributeSpan{attrs}, std::int64_t{5}, kPerThread);
    }
    for (auto& worker : workers)
    {
        worker.join();
    }

    const mti::ExponentialHistogramData data = storage.Collect();
    ASSERT_EQ(data.points.size(), 1U);
    const std::uint64_t total = static_cast<std::uint64_t>(kThreads) * kPerThread;
    EXPECT_EQ(data.points[0].count, total);
    EXPECT_EQ(Sum(data.points[0].positive.bucket_counts), total);
}
