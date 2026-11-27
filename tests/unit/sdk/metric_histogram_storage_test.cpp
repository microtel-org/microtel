// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0
//
// Behavioural tests for HistogramStorage<T> — explicit-bucket histogram
// aggregation state, M12 (docs/metrics-design.md §3).
//
// Bucket semantics (OTLP explicit bucket): for boundaries b[0..n-1] there are
// n+1 buckets; a value v falls in bucket i = the smallest i with v <= b[i]
// (upper-inclusive), or bucket n if v > b[n-1]. So bucket i counts
// (b[i-1], b[i]].

#include "sdk/metric_histogram_storage.hpp"

#include "microtel/attribute.hpp"
#include "microtel/internal/icurrent_span_source.hpp"
#include "microtel/internal/metric_batch.hpp"
#include "microtel/log_sink.hpp"
#include "microtel/trace.hpp"

#include "fakes/fake_diagnostics_sink.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iterator>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
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

/// RAII capture of microtel's internal `Warn` lines.
class WarnCapture
{
public:
    WarnCapture()
    {
        mt::SetLogSink(
            [this](mt::LogLevel level, std::string_view message)
            {
                if (level == mt::LogLevel::Warn)
                {
                    m_warns.emplace_back(message);
                }
            });
    }
    ~WarnCapture()
    {
        mt::ResetLogSink();
    }
    WarnCapture(const WarnCapture&) = delete;
    WarnCapture& operator=(const WarnCapture&) = delete;
    WarnCapture(WarnCapture&&) = delete;
    WarnCapture& operator=(WarnCapture&&) = delete;

    /// The captured `Warn` lines that mention @p needle.
    [[nodiscard]] std::vector<std::string> Mentioning(std::string_view needle) const
    {
        std::vector<std::string> out;
        std::ranges::copy_if(m_warns,
                             std::back_inserter(out),
                             [needle](const std::string& line)
                             { return line.find(needle) != std::string::npos; });
        return out;
    }

private:
    std::vector<std::string> m_warns;
};

void RecordN(mts::HistogramStorage<std::int64_t>& storage,
             mt::AttributeSpan attrs,
             std::int64_t value,
             int count)
{
    for (int i = 0; i < count; ++i)
    {
        storage.Record(value, attrs);
    }
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

bool IsOverflowPoint(const mti::HistogramPoint& pt)
{
    return std::ranges::any_of(pt.attributes,
                               [](const mt::KeyValue& kv)
                               {
                                   return kv.key == "otel.metric.overflow" &&
                                          std::holds_alternative<bool>(kv.value) &&
                                          std::get<bool>(kv.value);
                               });
}

std::vector<double> SortedRealHistoSums(const mti::HistogramData& data)
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

}  // namespace

TEST(HistogramStorageTest, TracksCountSumMinMax)
{
    mts::HistogramStorage<std::int64_t> storage{std::vector<double>{10, 20}};
    const std::vector<mt::KeyValue> attrs{Kv("k", std::string{"v"})};

    storage.Record(5, mt::AttributeSpan{attrs});
    storage.Record(15, mt::AttributeSpan{attrs});
    storage.Record(25, mt::AttributeSpan{attrs});

    const mti::HistogramData data = storage.Collect();
    ASSERT_EQ(data.points.size(), 1U);
    const mti::HistogramPoint& point = data.points[0];
    EXPECT_EQ(point.count, 3U);
    EXPECT_DOUBLE_EQ(point.sum, 45.0);
    // Compare the optionals directly (5.0/25.0 are exact) — avoids unchecked
    // optional access, which the analyzer flags even after ASSERT_TRUE.
    EXPECT_EQ(point.min, std::optional<double>{5.0});
    EXPECT_EQ(point.max, std::optional<double>{25.0});
}

TEST(HistogramStorageTest, BucketsAreUpperInclusive)
{
    mts::HistogramStorage<std::int64_t> storage{std::vector<double>{10, 20}};
    const std::vector<mt::KeyValue> attrs{Kv("k", std::string{"v"})};

    for (const std::int64_t v : {5, 10, 15, 20, 25})  // b0:{5,10} b1:{15,20} b2:{25}
    {
        storage.Record(v, mt::AttributeSpan{attrs});
    }

    const mti::HistogramData data = storage.Collect();
    ASSERT_EQ(data.points.size(), 1U);
    const std::vector<std::uint64_t> expected{2, 2, 1};
    EXPECT_EQ(data.points[0].bucket_counts, expected);
}

TEST(HistogramStorageTest, BucketCountsSizeIsBoundariesPlusOne)
{
    mts::HistogramStorage<std::int64_t> storage{std::vector<double>{1, 2, 3}};
    const std::vector<mt::KeyValue> attrs{Kv("k", std::string{"v"})};
    storage.Record(2, mt::AttributeSpan{attrs});

    EXPECT_EQ(storage.Collect().points[0].bucket_counts.size(), 4U);
}

TEST(HistogramStorageTest, ExplicitBoundsEchoedInOutput)
{
    const std::vector<double> bounds{1, 5, 10};
    mts::HistogramStorage<double> storage{bounds};
    const std::vector<mt::KeyValue> attrs{Kv("k", std::string{"v"})};
    storage.Record(3.0, mt::AttributeSpan{attrs});

    EXPECT_EQ(storage.Collect().points[0].explicit_bounds, bounds);
}

TEST(HistogramStorageTest, DoubleValues)
{
    mts::HistogramStorage<double> storage{std::vector<double>{1.0}};
    const std::vector<mt::KeyValue> attrs{Kv("k", std::string{"v"})};

    storage.Record(0.5, mt::AttributeSpan{attrs});  // b0
    storage.Record(1.0, mt::AttributeSpan{attrs});  // b0 (<=1.0)
    storage.Record(1.5, mt::AttributeSpan{attrs});  // b1

    const mti::HistogramData data = storage.Collect();
    ASSERT_EQ(data.points.size(), 1U);
    const std::vector<std::uint64_t> expected{2, 1};
    EXPECT_EQ(data.points[0].bucket_counts, expected);
    EXPECT_DOUBLE_EQ(data.points[0].sum, 3.0);
}

TEST(HistogramStorageTest, DistinctAttributeSetsAreDistinctPoints)
{
    mts::HistogramStorage<std::int64_t> storage{std::vector<double>{10}};
    const std::vector<mt::KeyValue> a{Kv("r", std::string{"a"})};
    const std::vector<mt::KeyValue> b{Kv("r", std::string{"b"})};

    storage.Record(1, mt::AttributeSpan{a});
    storage.Record(2, mt::AttributeSpan{b});

    EXPECT_EQ(storage.Collect().points.size(), 2U);
}

TEST(HistogramStorageTest, EmptyStorageCollectsNoPoints)
{
    mts::HistogramStorage<double> storage{std::vector<double>{1, 2}};
    EXPECT_TRUE(storage.Collect().points.empty());
}

TEST(HistogramStorageTest, CollectIsCumulativeTemporality)
{
    mts::HistogramStorage<std::int64_t> storage{std::vector<double>{1}};
    EXPECT_EQ(storage.Collect().temporality, mti::AggregationTemporality::Cumulative);
}

TEST(HistogramStorageTest, DeltaReportsSinceLastCollectAndClears)
{
    mts::HistogramStorage<std::int64_t> storage{std::vector<double>{10}};
    const std::vector<mt::KeyValue> attrs{Kv("k", std::string{"v"})};

    storage.Record(5, mt::AttributeSpan{attrs});
    const mti::HistogramData first = storage.Collect(mti::AggregationTemporality::Delta);
    ASSERT_EQ(first.points.size(), 1U);
    EXPECT_EQ(first.temporality, mti::AggregationTemporality::Delta);
    EXPECT_EQ(first.points[0].count, 1U);

    // Nothing recorded since → empty delta.
    EXPECT_TRUE(storage.Collect(mti::AggregationTemporality::Delta).points.empty());

    storage.Record(15, mt::AttributeSpan{attrs});
    const mti::HistogramData third = storage.Collect(mti::AggregationTemporality::Delta);
    ASSERT_EQ(third.points.size(), 1U);
    EXPECT_EQ(third.points[0].count, 1U);  // only the value since the last collect
}

TEST(HistogramStorageTest, ExemplarCapturedWhenSampledSpanActive)
{
    FakeSpanSource source;
    source.SetSpan(MakeSampledContext());
    mts::HistogramStorage<std::int64_t> storage{
        std::vector<double>{10}, mts::kDefaultMaxCardinality, &source};
    const std::vector<mt::KeyValue> attrs{Kv("k", std::string{"v"})};

    storage.Record(5, mt::AttributeSpan{attrs});

    const mti::HistogramData data = storage.Collect();
    ASSERT_EQ(data.points.size(), 1U);
    ASSERT_EQ(data.points[0].exemplars.size(), 1U);
    EXPECT_TRUE(data.points[0].exemplars[0].span_context.IsValid());
    EXPECT_DOUBLE_EQ(std::get<double>(data.points[0].exemplars[0].value), 5.0);
}

TEST(HistogramStorageTest, ExemplarIsResetAfterCollect)
{
    FakeSpanSource source;
    source.SetSpan(MakeSampledContext());
    mts::HistogramStorage<std::int64_t> storage{
        std::vector<double>{10}, mts::kDefaultMaxCardinality, &source};
    const std::vector<mt::KeyValue> attrs{Kv("k", std::string{"v"})};

    storage.Record(1, mt::AttributeSpan{attrs});
    (void)storage.Collect();  // drains exemplar window

    source.SetSpan({});
    storage.Record(2, mt::AttributeSpan{attrs});
    const mti::HistogramData data = storage.Collect();
    ASSERT_EQ(data.points.size(), 1U);
    EXPECT_TRUE(data.points[0].exemplars.empty());
}

TEST(HistogramStorageTest, CardinalityOverflowRoutesToOverflowSeries)
{
    mts::HistogramStorage<std::int64_t> storage{std::vector<double>{10}, /*max_cardinality=*/2};
    const std::vector<mt::KeyValue> a{Kv("k", std::string{"a"})};
    const std::vector<mt::KeyValue> b{Kv("k", std::string{"b"})};
    const std::vector<mt::KeyValue> c{Kv("k", std::string{"c"})};

    storage.Record(1, mt::AttributeSpan{a});
    storage.Record(2, mt::AttributeSpan{b});
    storage.Record(3, mt::AttributeSpan{c});  // overflows → overflow series
    storage.Record(4, mt::AttributeSpan{c});  // also overflows

    const mti::HistogramData data = storage.Collect();
    ASSERT_EQ(data.points.size(), 3U);

    const auto ov_it = std::ranges::find_if(data.points, IsOverflowPoint);
    ASSERT_NE(ov_it, data.points.end());
    EXPECT_EQ(ov_it->count, 2U);  // 2 overflowed recordings
    EXPECT_DOUBLE_EQ(ov_it->sum, 7.0);
}

// ── Overflow drop accounting (increment 26) ─────────────────────────────────

TEST(HistogramStorageTest, OverflowIncrementsCardinalityDropCounterPerMeasurement)
{
    mt::testing::FakeDiagnosticsSink sink;
    mts::HistogramStorage<std::int64_t> storage{
        std::vector<double>{10}, /*max_cardinality=*/2, nullptr, &sink};
    for (int i = 0; i < 5; ++i)  // 2 real series + 3 folded measurements
    {
        const std::vector<mt::KeyValue> attrs{Kv("k", std::to_string(i))};
        storage.Record(1, mt::AttributeSpan{attrs});
    }

    EXPECT_EQ(CardinalityDrops(sink), 3U);
}

TEST(HistogramStorageTest, ExistingSeriesUpdatesAfterOverflowDoNotCount)
{
    mt::testing::FakeDiagnosticsSink sink;
    mts::HistogramStorage<std::int64_t> storage{
        std::vector<double>{10}, /*max_cardinality=*/2, nullptr, &sink};
    const std::vector<mt::KeyValue> a{Kv("k", std::string{"a"})};
    const std::vector<mt::KeyValue> b{Kv("k", std::string{"b"})};
    const std::vector<mt::KeyValue> c{Kv("k", std::string{"c"})};
    storage.Record(1, mt::AttributeSpan{a});
    storage.Record(2, mt::AttributeSpan{b});
    storage.Record(3, mt::AttributeSpan{c});  // folds → 1 drop

    storage.Record(4, mt::AttributeSpan{a});  // pre-existing series — no drop
    storage.Record(5, mt::AttributeSpan{b});  // pre-existing series — no drop

    EXPECT_EQ(CardinalityDrops(sink), 1U);
}

TEST(HistogramStorageTest, NullSinkIsSafe)
{
    mts::HistogramStorage<std::int64_t> storage{std::vector<double>{10},
                                                /*max_cardinality=*/1};  // no sink
    const std::vector<mt::KeyValue> a{Kv("k", std::string{"a"})};
    const std::vector<mt::KeyValue> b{Kv("k", std::string{"b"})};
    storage.Record(1, mt::AttributeSpan{a});
    storage.Record(2, mt::AttributeSpan{b});  // folds — must not crash

    const mti::HistogramData data = storage.Collect();
    ASSERT_EQ(data.points.size(), 2U);
    EXPECT_EQ(std::ranges::count_if(data.points, IsOverflowPoint), 1);
}

// ── Spec-lock characterisation: cardinality-overflow semantics ─────────────
// These lock the overflow contract in place ahead of drop accounting
// (increment 26); they assert current behaviour and act as a regression net.

TEST(HistogramStorageTest, NoOverflowAtExactlyLimitDistinctSets)
{
    mts::HistogramStorage<std::int64_t> storage{std::vector<double>{10}, /*max_cardinality=*/3};
    for (int i = 0; i < 3; ++i)
    {
        const std::vector<mt::KeyValue> attrs{Kv("k", std::to_string(i))};
        storage.Record(1, mt::AttributeSpan{attrs});
    }

    const mti::HistogramData data = storage.Collect();
    EXPECT_EQ(data.points.size(), 3U);
    EXPECT_EQ(std::ranges::count_if(data.points, IsOverflowPoint), 0);
}

TEST(HistogramStorageTest, CumulativePreOverflowSeriesKeepExportingAfterOverflow)
{
    mts::HistogramStorage<std::int64_t> storage{std::vector<double>{10}, /*max_cardinality=*/2};
    const std::vector<mt::KeyValue> a{Kv("k", std::string{"a"})};
    const std::vector<mt::KeyValue> b{Kv("k", std::string{"b"})};
    const std::vector<mt::KeyValue> c{Kv("k", std::string{"c"})};
    storage.Record(1, mt::AttributeSpan{a});
    storage.Record(2, mt::AttributeSpan{b});
    storage.Record(5, mt::AttributeSpan{c});  // folds into the overflow series

    for (int round = 0; round < 2; ++round)
    {
        const mti::HistogramData data = storage.Collect();
        ASSERT_EQ(data.points.size(), 3U);
        EXPECT_EQ(SortedRealHistoSums(data), (std::vector<double>{1.0, 2.0}));
    }
}

TEST(HistogramStorageTest, EveryMeasurementReflectedInExactlyOneSeries)
{
    mts::HistogramStorage<std::int64_t> storage{std::vector<double>{10}, /*max_cardinality=*/2};
    std::uint64_t recorded_count = 0;
    double recorded_sum = 0.0;
    for (int i = 0; i < 5; ++i)
    {
        const std::vector<mt::KeyValue> attrs{Kv("k", std::to_string(i))};
        storage.Record(static_cast<std::int64_t>(i) + 1, mt::AttributeSpan{attrs});
        ++recorded_count;
        recorded_sum += static_cast<double>(i) + 1;
    }

    const mti::HistogramData data = storage.Collect();
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

TEST(HistogramStorageTest, DeltaCollectReclaimsCardinalitySlots)
{
    mts::HistogramStorage<std::int64_t> storage{std::vector<double>{10}, /*max_cardinality=*/2};
    const std::vector<mt::KeyValue> a{Kv("k", std::string{"a"})};
    const std::vector<mt::KeyValue> b{Kv("k", std::string{"b"})};
    const std::vector<mt::KeyValue> c{Kv("k", std::string{"c"})};
    storage.Record(1, mt::AttributeSpan{a});
    storage.Record(2, mt::AttributeSpan{b});
    storage.Record(3, mt::AttributeSpan{c});                    // folds into the overflow series
    (void)storage.Collect(mti::AggregationTemporality::Delta);  // clears live state

    const std::vector<mt::KeyValue> d{Kv("k", std::string{"d"})};
    storage.Record(4, mt::AttributeSpan{d});

    const mti::HistogramData data = storage.Collect(mti::AggregationTemporality::Delta);
    ASSERT_EQ(data.points.size(), 1U);
    EXPECT_FALSE(IsOverflowPoint(data.points[0]));  // d got a real series again
}

TEST(HistogramStorageTest, OverflowSeriesMergesBucketStateAcrossFoldedMeasurements)
{
    mts::HistogramStorage<std::int64_t> storage{std::vector<double>{10}, /*max_cardinality=*/1};
    const std::vector<mt::KeyValue> a{Kv("k", std::string{"a"})};
    const std::vector<mt::KeyValue> b{Kv("k", std::string{"b"})};
    const std::vector<mt::KeyValue> c{Kv("k", std::string{"c"})};
    storage.Record(1, mt::AttributeSpan{a});   // real series
    storage.Record(5, mt::AttributeSpan{b});   // folds → bucket (-inf, 10]
    storage.Record(15, mt::AttributeSpan{c});  // folds → bucket (10, +inf)

    const mti::HistogramData data = storage.Collect();
    ASSERT_EQ(data.points.size(), 2U);
    const auto ov_it = std::ranges::find_if(data.points, IsOverflowPoint);
    ASSERT_NE(ov_it, data.points.end());
    // Merged bucket state — not last-write-wins.
    EXPECT_EQ(ov_it->count, 2U);
    EXPECT_DOUBLE_EQ(ov_it->sum, 20.0);
    EXPECT_EQ(ov_it->min, std::optional<double>{5.0});
    EXPECT_EQ(ov_it->max, std::optional<double>{15.0});
    EXPECT_EQ(ov_it->bucket_counts, (std::vector<std::uint64_t>{1, 1}));
}

TEST(HistogramStorageTest, NonFiniteValueIsDropped)
{
    mt::testing::FakeDiagnosticsSink sink;
    mts::HistogramStorage<double> storage{
        std::vector<double>{10.0}, mts::kDefaultMaxCardinality, nullptr, &sink};
    const std::vector<mt::KeyValue> attrs{Kv("k", std::string{"v"})};
    storage.Record(5.0, mt::AttributeSpan{attrs});
    storage.Record(std::numeric_limits<double>::infinity(), mt::AttributeSpan{attrs});
    storage.Record(std::numeric_limits<double>::quiet_NaN(), mt::AttributeSpan{attrs});
    const mti::HistogramData data = storage.Collect();
    ASSERT_EQ(data.points.size(), 1U);
    EXPECT_EQ(data.points[0].count, 1U);
    EXPECT_DOUBLE_EQ(data.points[0].sum, 5.0);
    EXPECT_EQ(NonFiniteDrops(sink), 2U);
}

TEST(HistogramStorageTest, FiniteValueRecordsNoNonFiniteDrop)
{
    mt::testing::FakeDiagnosticsSink sink;
    mts::HistogramStorage<double> storage{
        std::vector<double>{10.0}, mts::kDefaultMaxCardinality, nullptr, &sink};
    const std::vector<mt::KeyValue> attrs{Kv("k", std::string{"v"})};
    storage.Record(5.0, mt::AttributeSpan{attrs});
    EXPECT_EQ(NonFiniteDrops(sink), 0U);
}

TEST(HistogramStorageTest, NonFiniteValueWithNullSinkIsNotDereferenced)
{
    mts::HistogramStorage<double> storage{std::vector<double>{10.0}};  // no sink
    const std::vector<mt::KeyValue> attrs{Kv("k", std::string{"v"})};
    storage.Record(std::numeric_limits<double>::quiet_NaN(), mt::AttributeSpan{attrs});
    EXPECT_TRUE(storage.Collect().points.empty());
}

// ── Negative values — ICP 0047 ────────────────────────────────────────────────

TEST(HistogramStorageTest, NegativeDoubleIsDroppedAndCounted)
{
    mt::testing::FakeDiagnosticsSink sink;
    mts::HistogramStorage<double> storage{
        std::vector<double>{10.0}, mts::kDefaultMaxCardinality, nullptr, &sink};
    const std::vector<mt::KeyValue> attrs{Kv("k", std::string{"v"})};
    storage.Record(5.0, mt::AttributeSpan{attrs});
    storage.Record(-1.0, mt::AttributeSpan{attrs});
    storage.Record(-0.5, mt::AttributeSpan{attrs});

    const mti::HistogramData data = storage.Collect();
    ASSERT_EQ(data.points.size(), 1U);
    EXPECT_EQ(data.points.at(0).count, 1U);
    EXPECT_DOUBLE_EQ(data.points.at(0).sum, 5.0);
    EXPECT_EQ(data.points.at(0).min, std::optional<double>{5.0});
    EXPECT_EQ(data.points.at(0).max, std::optional<double>{5.0});
    EXPECT_EQ(data.points.at(0).bucket_counts, (std::vector<std::uint64_t>{1, 0}));
    EXPECT_EQ(NegativeDrops(sink), 2U);
    EXPECT_EQ(NonFiniteDrops(sink), 0U);
}

TEST(HistogramStorageTest, NegativeInt64IsDroppedAndCounted)
{
    mt::testing::FakeDiagnosticsSink sink;
    mts::HistogramStorage<std::int64_t> storage{
        std::vector<double>{10.0}, mts::kDefaultMaxCardinality, nullptr, &sink};
    const std::vector<mt::KeyValue> attrs{Kv("k", std::string{"v"})};
    storage.Record(3, mt::AttributeSpan{attrs});
    storage.Record(-7, mt::AttributeSpan{attrs});

    const mti::HistogramData data = storage.Collect();
    ASSERT_EQ(data.points.size(), 1U);
    EXPECT_EQ(data.points.at(0).count, 1U);
    EXPECT_DOUBLE_EQ(data.points.at(0).sum, 3.0);
    EXPECT_EQ(data.points.at(0).min, std::optional<double>{3.0});
    EXPECT_EQ(data.points.at(0).bucket_counts, (std::vector<std::uint64_t>{1, 0}));
    EXPECT_EQ(NegativeDrops(sink), 1U);
}

TEST(HistogramStorageTest, NegativeValueAloneCreatesNoSeries)
{
    mt::testing::FakeDiagnosticsSink sink;
    mts::HistogramStorage<double> storage{
        std::vector<double>{10.0}, mts::kDefaultMaxCardinality, nullptr, &sink};
    const std::vector<mt::KeyValue> attrs{Kv("k", std::string{"v"})};
    storage.Record(-1.0, mt::AttributeSpan{attrs});
    EXPECT_TRUE(storage.Collect().points.empty());
    EXPECT_EQ(NegativeDrops(sink), 1U);
}

TEST(HistogramStorageTest, NegativeZeroIsRecordedAsZero)
{
    mt::testing::FakeDiagnosticsSink sink;
    mts::HistogramStorage<double> storage{
        std::vector<double>{0.0, 10.0}, mts::kDefaultMaxCardinality, nullptr, &sink};
    const std::vector<mt::KeyValue> attrs{Kv("k", std::string{"v"})};
    storage.Record(-0.0, mt::AttributeSpan{attrs});

    const mti::HistogramData data = storage.Collect();
    ASSERT_EQ(data.points.size(), 1U);
    EXPECT_EQ(data.points.at(0).count, 1U);
    EXPECT_DOUBLE_EQ(data.points.at(0).sum, 0.0);
    EXPECT_EQ(data.points.at(0).min, std::optional<double>{0.0});
    EXPECT_EQ(data.points.at(0).bucket_counts, (std::vector<std::uint64_t>{1, 0, 0}));
    EXPECT_EQ(NegativeDrops(sink), 0U);
}

TEST(HistogramStorageTest, NegativeValueDoesNotReplaceTheExemplar)
{
    FakeSpanSource source;
    source.SetSpan(MakeSampledContext());
    mts::HistogramStorage<std::int64_t> storage{
        std::vector<double>{10}, mts::kDefaultMaxCardinality, &source};
    const std::vector<mt::KeyValue> attrs{Kv("k", std::string{"v"})};

    storage.Record(5, mt::AttributeSpan{attrs});
    storage.Record(-3, mt::AttributeSpan{attrs});

    const mti::HistogramData data = storage.Collect();
    ASSERT_EQ(data.points.size(), 1U);
    ASSERT_EQ(data.points.at(0).exemplars.size(), 1U);
    EXPECT_DOUBLE_EQ(std::get<double>(data.points.at(0).exemplars.at(0).value), 5.0);
}

TEST(HistogramStorageTest, NegativeValueWithNullSinkIsNotDereferenced)
{
    mts::HistogramStorage<double> storage{std::vector<double>{10.0}};  // no sink
    const std::vector<mt::KeyValue> attrs{Kv("k", std::string{"v"})};
    storage.Record(-1.0, mt::AttributeSpan{attrs});
    EXPECT_TRUE(storage.Collect().points.empty());
}

TEST(HistogramStorageTest, NegativeValueWarnNamesTheInstrumentRateLimited)
{
    const WarnCapture capture;
    mts::HistogramStorage<double> storage{
        std::vector<double>{10.0}, mts::kDefaultMaxCardinality, nullptr, nullptr, "rpc.latency"};
    const std::vector<mt::KeyValue> attrs{Kv("k", std::string{"v"})};
    constexpr int kDrops = 5;
    for (int i = 0; i < kDrops; ++i)
    {
        storage.Record(-1.0, mt::AttributeSpan{attrs});
    }
    // The 1st, 2nd and 4th drop are logged; the 3rd and 5th are not.
    const auto warns = capture.Mentioning("'rpc.latency'");
    ASSERT_EQ(warns.size(), 3U);
    EXPECT_NE(warns.at(0).find("negative_histogram_value"), std::string::npos);
    EXPECT_NE(warns.at(2).find("4 dropped"), std::string::npos);
}

TEST(HistogramStorageTest, NegativeValueWarnEscapesTheInstrumentName)
{
    const WarnCapture capture;
    mts::HistogramStorage<std::int64_t> storage{
        std::vector<double>{10.0}, mts::kDefaultMaxCardinality, nullptr, nullptr, "bad\nname"};
    const std::vector<mt::KeyValue> attrs{Kv("k", std::string{"v"})};
    storage.Record(-1, mt::AttributeSpan{attrs});
    EXPECT_EQ(capture.Mentioning("'bad\\nname'").size(), 1U);
}

TEST(HistogramStorageTest, PointCarriesItsAttributes)
{
    mts::HistogramStorage<std::int64_t> storage{std::vector<double>{10}};
    const std::vector<mt::KeyValue> attrs{Kv("route", std::string{"/x"})};
    storage.Record(1, mt::AttributeSpan{attrs});

    const mti::HistogramData data = storage.Collect();
    ASSERT_EQ(data.points[0].attributes.size(), 1U);
    EXPECT_EQ(data.points[0].attributes[0].key, "route");
}

TEST(HistogramStorageTest, ConcurrentRecordsConserveCount)
{
    mts::HistogramStorage<std::int64_t> storage{std::vector<double>{10, 20}};
    const std::vector<mt::KeyValue> attrs{Kv("k", std::string{"v"})};

    constexpr int kThreads = 8;
    constexpr int kPerThread = 1000;
    std::vector<std::thread> workers;
    workers.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t)
    {
        workers.emplace_back(
            RecordN, std::ref(storage), mt::AttributeSpan{attrs}, std::int64_t{15}, kPerThread);
    }
    for (auto& worker : workers)
    {
        worker.join();
    }

    const mti::HistogramData data = storage.Collect();
    ASSERT_EQ(data.points.size(), 1U);
    const std::uint64_t total = static_cast<std::uint64_t>(kThreads) * kPerThread;
    EXPECT_EQ(data.points[0].count, total);
    EXPECT_EQ(data.points[0].bucket_counts.at(1), total);  // 15 → bucket 1 (10,20]
}
