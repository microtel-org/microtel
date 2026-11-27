// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0
//
// Unit tests for internal::TemporalitySelector (ICP 0045): the per-instrument-
// kind temporality table a collection is asked for.
//
// Contract under test:
//  - Cumulative(), Delta() and LowMemory() encode the OTLP exporter spec's
//    table, every InstrumentKind x preset cell.
//  - Gauges map to Cumulative in every preset.
//  - A default-constructed selector is Cumulative(); one built from a single
//    temporality maps every kind to it.
//  - The built-in selectors are usable in constant expressions.
//  - CollectRequest's default selector is Cumulative().

#include "microtel/internal/metric_batch.hpp"
#include "microtel/internal/metric_producer.hpp"
#include "microtel/view.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>

namespace mt = microtel;
namespace mti = microtel::internal;

namespace
{

constexpr auto kCumulative = mti::AggregationTemporality::Cumulative;
constexpr auto kDelta = mti::AggregationTemporality::Delta;

/// One row of the OTLP exporter spec's table: a kind and its temporality
/// under each preset.
struct TableRow
{
    mt::InstrumentKind kind;
    mti::AggregationTemporality cumulative;
    mti::AggregationTemporality delta;
    mti::AggregationTemporality low_memory;
};

constexpr std::array<TableRow, mti::kInstrumentKindCount> kSpecTable{{
    {.kind = mt::InstrumentKind::Counter,
     .cumulative = kCumulative,
     .delta = kDelta,
     .low_memory = kDelta},
    {.kind = mt::InstrumentKind::UpDownCounter,
     .cumulative = kCumulative,
     .delta = kCumulative,
     .low_memory = kCumulative},
    {.kind = mt::InstrumentKind::Gauge,
     .cumulative = kCumulative,
     .delta = kCumulative,
     .low_memory = kCumulative},
    {.kind = mt::InstrumentKind::Histogram,
     .cumulative = kCumulative,
     .delta = kDelta,
     .low_memory = kDelta},
    {.kind = mt::InstrumentKind::ExponentialHistogram,
     .cumulative = kCumulative,
     .delta = kDelta,
     .low_memory = kDelta},
    {.kind = mt::InstrumentKind::ObservableCounter,
     .cumulative = kCumulative,
     .delta = kDelta,
     .low_memory = kCumulative},
    {.kind = mt::InstrumentKind::ObservableUpDownCounter,
     .cumulative = kCumulative,
     .delta = kCumulative,
     .low_memory = kCumulative},
    {.kind = mt::InstrumentKind::ObservableGauge,
     .cumulative = kCumulative,
     .delta = kCumulative,
     .low_memory = kCumulative},
}};

}  // namespace

TEST(TemporalitySelectorTest, KindCountCoversEveryEnumerator)
{
    EXPECT_EQ(mti::kInstrumentKindCount,
              static_cast<std::size_t>(mt::InstrumentKind::ObservableGauge) + 1);
}

TEST(TemporalitySelectorTest, CumulativePresetMatchesSpecTable)
{
    constexpr auto kSelector = mti::TemporalitySelector::Cumulative();
    for (const TableRow& row : kSpecTable)
    {
        EXPECT_EQ(kSelector.For(row.kind), row.cumulative) << "kind " << static_cast<int>(row.kind);
    }
}

TEST(TemporalitySelectorTest, DeltaPresetMatchesSpecTable)
{
    constexpr auto kSelector = mti::TemporalitySelector::Delta();
    for (const TableRow& row : kSpecTable)
    {
        EXPECT_EQ(kSelector.For(row.kind), row.delta) << "kind " << static_cast<int>(row.kind);
    }
}

TEST(TemporalitySelectorTest, LowMemoryPresetMatchesSpecTable)
{
    constexpr auto kSelector = mti::TemporalitySelector::LowMemory();
    for (const TableRow& row : kSpecTable)
    {
        EXPECT_EQ(kSelector.For(row.kind), row.low_memory) << "kind " << static_cast<int>(row.kind);
    }
}

TEST(TemporalitySelectorTest, PresetsAreConstantExpressions)
{
    static_assert(mti::TemporalitySelector::Delta().For(mt::InstrumentKind::Counter) == kDelta);
    static_assert(mti::TemporalitySelector::Delta().For(mt::InstrumentKind::UpDownCounter) ==
                  kCumulative);
    static_assert(mti::TemporalitySelector::LowMemory().For(
                      mt::InstrumentKind::ObservableCounter) == kCumulative);
    SUCCEED();
}

TEST(TemporalitySelectorTest, DefaultConstructedIsCumulativePreset)
{
    constexpr mti::TemporalitySelector kSelector;
    EXPECT_EQ(kSelector, mti::TemporalitySelector::Cumulative());
}

TEST(TemporalitySelectorTest, SingleTemporalityMapsEveryKind)
{
    constexpr mti::TemporalitySelector kAllDelta{kDelta};
    for (const TableRow& row : kSpecTable)
    {
        EXPECT_EQ(kAllDelta.For(row.kind), kDelta) << "kind " << static_cast<int>(row.kind);
    }
}

TEST(TemporalitySelectorTest, PresetsDiffer)
{
    EXPECT_NE(mti::TemporalitySelector::Delta(), mti::TemporalitySelector::LowMemory());
    EXPECT_NE(mti::TemporalitySelector::Delta(), mti::TemporalitySelector::Cumulative());
    EXPECT_NE(mti::TemporalitySelector::Delta(), mti::TemporalitySelector{kDelta});
}

TEST(TemporalitySelectorTest, OutOfRangeKindIsCumulative)
{
    constexpr auto kNotAKind = static_cast<mt::InstrumentKind>(std::uint8_t{200});
    EXPECT_EQ(mti::TemporalitySelector{kDelta}.For(kNotAKind), kCumulative);
}

TEST(CollectRequestTest, DefaultSelectorIsCumulative)
{
    const mti::CollectRequest request;
    EXPECT_EQ(request.selector, mti::TemporalitySelector::Cumulative());
}

TEST(CollectRequestTest, DesignatedSelectorIsKept)
{
    const mti::CollectRequest request{.selector = mti::TemporalitySelector::LowMemory()};
    EXPECT_EQ(request.selector, mti::TemporalitySelector::LowMemory());
}
