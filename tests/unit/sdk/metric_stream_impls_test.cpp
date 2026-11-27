// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0
//
// Behavioural tests for the concrete IMetricStream implementations
// (MetricStreamSum, MetricStreamGauge, MetricStreamHistogram,
// MetricStreamExpHistogram). Each wraps a storage type and exposes it via
// the IMetricStream interface for MetricProducer::Collect().
//
// Contract under test:
//  - Collect() wraps storage output in MetricRecord with correct name/unit.
//  - The MetricData variant holds the right alternative for each stream type.
//  - Storage() gives a mutable reference for hot-path Add/Record.
//  - Temporality is resolved from the selector by the stream's own kind
//    (ICP 0045) and delegated to the underlying storage Collect().

#include "sdk/metric_stream_impls.hpp"

#include "microtel/attribute.hpp"
#include "microtel/internal/metric_batch.hpp"
#include "microtel/view.hpp"

#include "fakes/fake_diagnostics_sink.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
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

std::uint64_t CardinalityDrops(const mt::testing::FakeDiagnosticsSink& sink)
{
    return sink.drop_counters[static_cast<std::size_t>(mt::DropReason::CardinalityOverflow)];
}

}  // namespace

// ── MetricStreamSum ────────────────────────────────────────────────────────

TEST(MetricStreamSumTest, CollectReturnsRecordWithCorrectMetadata)
{
    mts::MetricStreamSum<std::int64_t> stream{
        "req.count", "Total requests", "1", mt::InstrumentKind::Counter};

    const mti::MetricRecord rec = stream.Collect(mti::TemporalitySelector::Cumulative());
    EXPECT_EQ(rec.name, "req.count");
    EXPECT_EQ(rec.description, "Total requests");
    EXPECT_EQ(rec.unit, "1");
}

TEST(MetricStreamSumTest, DataVariantIsSumData)
{
    mts::MetricStreamSum<double> stream{"m", "", "", mt::InstrumentKind::UpDownCounter};
    const mti::MetricRecord rec = stream.Collect(mti::TemporalitySelector::Cumulative());
    EXPECT_TRUE(std::holds_alternative<mti::SumData>(rec.data));
}

TEST(MetricStreamSumTest, MonotonicFlagPropagates)
{
    mts::MetricStreamSum<std::int64_t> mono{"m", "", "", mt::InstrumentKind::Counter};
    mts::MetricStreamSum<std::int64_t> bidir{"m", "", "", mt::InstrumentKind::UpDownCounter};
    EXPECT_TRUE(std::get<mti::SumData>(mono.Collect(mti::TemporalitySelector::Cumulative()).data)
                    .is_monotonic);
    EXPECT_FALSE(std::get<mti::SumData>(bidir.Collect(mti::TemporalitySelector::Cumulative()).data)
                     .is_monotonic);
}

TEST(MetricStreamSumTest, StorageAccessorEnablesHotPathAdd)
{
    mts::MetricStreamSum<std::int64_t> stream{"m", "", "", mt::InstrumentKind::Counter};
    const std::vector<mt::KeyValue> attrs{Kv("k", std::string{"v"})};

    stream.Storage().Add(3, mt::AttributeSpan{attrs});
    stream.Storage().Add(5, mt::AttributeSpan{attrs});

    const auto rec = stream.Collect(mti::TemporalitySelector::Cumulative());
    const auto& sum_data = std::get<mti::SumData>(rec.data);
    ASSERT_EQ(sum_data.points.size(), 1U);
    EXPECT_EQ(std::get<std::int64_t>(sum_data.points[0].value), 8);
}

TEST(MetricStreamSumTest, DeltaTemporalityDelegatesToStorage)
{
    mts::MetricStreamSum<std::int64_t> stream{"m", "", "", mt::InstrumentKind::Counter};
    const std::vector<mt::KeyValue> attrs{Kv("k", std::string{"v"})};

    stream.Storage().Add(7, mt::AttributeSpan{attrs});
    const auto first =
        std::get<mti::SumData>(stream.Collect(mti::TemporalitySelector::Delta()).data);
    EXPECT_EQ(first.temporality, mti::AggregationTemporality::Delta);
    ASSERT_EQ(first.points.size(), 1U);

    // Delta clears state — second collect is empty.
    const auto second =
        std::get<mti::SumData>(stream.Collect(mti::TemporalitySelector::Delta()).data);
    EXPECT_TRUE(second.points.empty());
}

// ── Temporality resolved through the stream's kind (ICP 0045) ──────────────

TEST(MetricStreamSumTest, UpDownCounterIsCumulativeUnderDeltaPreset)
{
    mts::MetricStreamSum<std::int64_t> stream{"m", "", "", mt::InstrumentKind::UpDownCounter};
    const std::vector<mt::KeyValue> attrs{Kv("k", std::string{"v"})};

    stream.Storage().Add(7, mt::AttributeSpan{attrs});
    const auto first =
        std::get<mti::SumData>(stream.Collect(mti::TemporalitySelector::Delta()).data);
    EXPECT_EQ(first.temporality, mti::AggregationTemporality::Cumulative);

    // Cumulative keeps state: the second collection still reports the total.
    stream.Storage().Add(-2, mt::AttributeSpan{attrs});
    const auto second =
        std::get<mti::SumData>(stream.Collect(mti::TemporalitySelector::Delta()).data);
    EXPECT_EQ(second.temporality, mti::AggregationTemporality::Cumulative);
    ASSERT_EQ(second.points.size(), 1U);
    EXPECT_EQ(std::get<std::int64_t>(second.points.at(0).value), 5);
}

TEST(MetricStreamSumTest, UpDownCounterIsCumulativeUnderLowMemoryPreset)
{
    mts::MetricStreamSum<double> stream{"m", "", "", mt::InstrumentKind::UpDownCounter};
    const auto data =
        std::get<mti::SumData>(stream.Collect(mti::TemporalitySelector::LowMemory()).data);
    EXPECT_EQ(data.temporality, mti::AggregationTemporality::Cumulative);
}

TEST(MetricStreamSumTest, CounterIsDeltaUnderLowMemoryPreset)
{
    mts::MetricStreamSum<double> stream{"m", "", "", mt::InstrumentKind::Counter};
    const auto data =
        std::get<mti::SumData>(stream.Collect(mti::TemporalitySelector::LowMemory()).data);
    EXPECT_EQ(data.temporality, mti::AggregationTemporality::Delta);
}

TEST(MetricStreamSumTest, UpDownCounterFollowsAnAllDeltaSelector)
{
    // The stream asks the selector for its own kind; nothing hard-codes the
    // up-down counter as cumulative.
    mts::MetricStreamSum<std::int64_t> stream{"m", "", "", mt::InstrumentKind::UpDownCounter};
    const mti::TemporalitySelector all_delta{mti::AggregationTemporality::Delta};
    const auto data = std::get<mti::SumData>(stream.Collect(all_delta).data);
    EXPECT_EQ(data.temporality, mti::AggregationTemporality::Delta);
}

TEST(MetricStreamHistogramTest, HistogramFollowsPresets)
{
    mts::MetricStreamHistogram<double> stream{"m", "", "", std::vector<double>{10}};
    EXPECT_EQ(std::get<mti::HistogramData>(stream.Collect(mti::TemporalitySelector::Delta()).data)
                  .temporality,
              mti::AggregationTemporality::Delta);
    EXPECT_EQ(
        std::get<mti::HistogramData>(stream.Collect(mti::TemporalitySelector::LowMemory()).data)
            .temporality,
        mti::AggregationTemporality::Delta);
    EXPECT_EQ(
        std::get<mti::HistogramData>(stream.Collect(mti::TemporalitySelector::Cumulative()).data)
            .temporality,
        mti::AggregationTemporality::Cumulative);
}

TEST(MetricStreamExpHistogramTest, ExpHistogramFollowsPresets)
{
    mts::MetricStreamExpHistogram<double> stream{"m", "", "", 20, 160};
    EXPECT_EQ(std::get<mti::ExponentialHistogramData>(
                  stream.Collect(mti::TemporalitySelector::Delta()).data)
                  .temporality,
              mti::AggregationTemporality::Delta);
    EXPECT_EQ(std::get<mti::ExponentialHistogramData>(
                  stream.Collect(mti::TemporalitySelector::LowMemory()).data)
                  .temporality,
              mti::AggregationTemporality::Delta);
    EXPECT_EQ(std::get<mti::ExponentialHistogramData>(
                  stream.Collect(mti::TemporalitySelector::Cumulative()).data)
                  .temporality,
              mti::AggregationTemporality::Cumulative);
}

// ── MetricStreamGauge ─────────────────────────────────────────────────────

TEST(MetricStreamGaugeTest, CollectReturnsRecordWithCorrectMetadata)
{
    mts::MetricStreamGauge<double> stream{"cpu.util", "CPU utilisation", "1"};

    const mti::MetricRecord rec = stream.Collect(mti::TemporalitySelector::Cumulative());
    EXPECT_EQ(rec.name, "cpu.util");
    EXPECT_EQ(rec.description, "CPU utilisation");
    EXPECT_EQ(rec.unit, "1");
}

TEST(MetricStreamGaugeTest, DataVariantIsGaugeData)
{
    mts::MetricStreamGauge<double> stream{"m", "", ""};
    EXPECT_TRUE(std::holds_alternative<mti::GaugeData>(
        stream.Collect(mti::TemporalitySelector::Cumulative()).data));
}

TEST(MetricStreamGaugeTest, StorageAccessorEnablesHotPathRecord)
{
    mts::MetricStreamGauge<double> stream{"m", "", ""};
    const std::vector<mt::KeyValue> attrs{Kv("k", std::string{"v"})};

    stream.Storage().Record(4.5, mt::AttributeSpan{attrs});
    stream.Storage().Record(9.0, mt::AttributeSpan{attrs});  // last-write-wins

    const auto rec = stream.Collect(mti::TemporalitySelector::Cumulative());
    const auto& gauge_data = std::get<mti::GaugeData>(rec.data);
    ASSERT_EQ(gauge_data.points.size(), 1U);
    EXPECT_DOUBLE_EQ(std::get<double>(gauge_data.points[0].value), 9.0);
}

// ── MetricStreamHistogram ─────────────────────────────────────────────────

TEST(MetricStreamHistogramTest, CollectReturnsRecordWithCorrectMetadata)
{
    mts::MetricStreamHistogram<double> stream{
        "http.latency", "Request latency", "ms", std::vector<double>{10, 100, 1000}};

    const mti::MetricRecord rec = stream.Collect(mti::TemporalitySelector::Cumulative());
    EXPECT_EQ(rec.name, "http.latency");
    EXPECT_EQ(rec.description, "Request latency");
    EXPECT_EQ(rec.unit, "ms");
}

TEST(MetricStreamHistogramTest, DataVariantIsHistogramData)
{
    mts::MetricStreamHistogram<std::int64_t> stream{"m", "", "", std::vector<double>{10}};
    EXPECT_TRUE(std::holds_alternative<mti::HistogramData>(
        stream.Collect(mti::TemporalitySelector::Cumulative()).data));
}

TEST(MetricStreamHistogramTest, StorageAccessorEnablesHotPathRecord)
{
    mts::MetricStreamHistogram<std::int64_t> stream{"m", "", "", std::vector<double>{10}};
    const std::vector<mt::KeyValue> attrs{Kv("k", std::string{"v"})};

    stream.Storage().Record(5, mt::AttributeSpan{attrs});

    const auto rec = stream.Collect(mti::TemporalitySelector::Cumulative());
    const auto& histo_data = std::get<mti::HistogramData>(rec.data);
    ASSERT_EQ(histo_data.points.size(), 1U);
    EXPECT_EQ(histo_data.points[0].count, 1U);
    EXPECT_DOUBLE_EQ(histo_data.points[0].sum, 5.0);
}

// ── MetricStreamExpHistogram ──────────────────────────────────────────────

TEST(MetricStreamExpHistogramTest, CollectReturnsRecordWithCorrectMetadata)
{
    mts::MetricStreamExpHistogram<double> stream{
        "latency", "Latency", "ms", /*max_scale=*/20, /*max_buckets=*/160};

    const mti::MetricRecord rec = stream.Collect(mti::TemporalitySelector::Cumulative());
    EXPECT_EQ(rec.name, "latency");
    EXPECT_EQ(rec.description, "Latency");
    EXPECT_EQ(rec.unit, "ms");
}

TEST(MetricStreamExpHistogramTest, DataVariantIsExpHistogramData)
{
    mts::MetricStreamExpHistogram<std::int64_t> stream{"m", "", "", 20, 160};
    EXPECT_TRUE(std::holds_alternative<mti::ExponentialHistogramData>(
        stream.Collect(mti::TemporalitySelector::Cumulative()).data));
}

TEST(MetricStreamExpHistogramTest, StorageAccessorEnablesHotPathRecord)
{
    mts::MetricStreamExpHistogram<std::int64_t> stream{"m", "", "", 20, 160};
    const std::vector<mt::KeyValue> attrs{Kv("k", std::string{"v"})};

    stream.Storage().Record(4, mt::AttributeSpan{attrs});
    stream.Storage().Record(8, mt::AttributeSpan{attrs});

    const auto rec = stream.Collect(mti::TemporalitySelector::Cumulative());
    const auto& data = std::get<mti::ExponentialHistogramData>(rec.data);
    ASSERT_EQ(data.points.size(), 1U);
    EXPECT_EQ(data.points[0].count, 2U);
    EXPECT_DOUBLE_EQ(data.points[0].sum, 12.0);
}

// ── StorageOptions wiring (increment 26) ──────────────────────────────────
// Each stream ctor forwards StorageOptions to its storage; a cardinality
// fold recorded through the storage must reach the configured sink.

TEST(MetricStreamSumTest, StorageOptionsForwardDiagnosticsSinkToStorage)
{
    mt::testing::FakeDiagnosticsSink sink;
    mts::MetricStreamSum<std::int64_t> stream{
        "m",
        "",
        "",
        mt::InstrumentKind::Counter,
        mts::StorageOptions{.max_cardinality = 1, .diag = &sink}};
    const std::vector<mt::KeyValue> a{Kv("k", std::string{"a"})};
    const std::vector<mt::KeyValue> b{Kv("k", std::string{"b"})};

    stream.Storage().Add(1, mt::AttributeSpan{a});
    stream.Storage().Add(1, mt::AttributeSpan{b});  // folds → 1 drop

    EXPECT_EQ(CardinalityDrops(sink), 1U);
}

TEST(MetricStreamGaugeTest, StorageOptionsForwardDiagnosticsSinkToStorage)
{
    mt::testing::FakeDiagnosticsSink sink;
    mts::MetricStreamGauge<std::int64_t> stream{
        "m", "", "", mts::StorageOptions{.max_cardinality = 1, .diag = &sink}};
    const std::vector<mt::KeyValue> a{Kv("k", std::string{"a"})};
    const std::vector<mt::KeyValue> b{Kv("k", std::string{"b"})};

    stream.Storage().Record(1, mt::AttributeSpan{a});
    stream.Storage().Record(2, mt::AttributeSpan{b});  // folds → 1 drop

    EXPECT_EQ(CardinalityDrops(sink), 1U);
}

TEST(MetricStreamHistogramTest, StorageOptionsForwardDiagnosticsSinkToStorage)
{
    mt::testing::FakeDiagnosticsSink sink;
    mts::MetricStreamHistogram<std::int64_t> stream{
        "m",
        "",
        "",
        std::vector<double>{10},
        mts::StorageOptions{.max_cardinality = 1, .diag = &sink}};
    const std::vector<mt::KeyValue> a{Kv("k", std::string{"a"})};
    const std::vector<mt::KeyValue> b{Kv("k", std::string{"b"})};

    stream.Storage().Record(1, mt::AttributeSpan{a});
    stream.Storage().Record(2, mt::AttributeSpan{b});  // folds → 1 drop

    EXPECT_EQ(CardinalityDrops(sink), 1U);
}

TEST(MetricStreamExpHistogramTest, StorageOptionsForwardDiagnosticsSinkToStorage)
{
    mt::testing::FakeDiagnosticsSink sink;
    mts::MetricStreamExpHistogram<std::int64_t> stream{
        "m",
        "",
        "",
        /*max_scale=*/0,
        /*max_buckets=*/160,
        mts::StorageOptions{.max_cardinality = 1, .diag = &sink}};
    const std::vector<mt::KeyValue> a{Kv("k", std::string{"a"})};
    const std::vector<mt::KeyValue> b{Kv("k", std::string{"b"})};

    stream.Storage().Record(3, mt::AttributeSpan{a});
    stream.Storage().Record(5, mt::AttributeSpan{b});  // folds → 1 drop

    EXPECT_EQ(CardinalityDrops(sink), 1U);
}
