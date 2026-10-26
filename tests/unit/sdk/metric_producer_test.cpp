// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0
//
// Behavioural tests for MetricProducer — the IMetricProducer concrete impl
// that assembles MetricBatchHandles from registered IMetricStream instances.
//
// Contract under test:
//  - An empty producer yields an empty vector from Collect().
//  - Streams grouped by InstrumentationScope (same name+version → one handle).
//  - Each handle carries all records for its scope, the shared Resource, and
//    the scope identity.
//  - Collect() delegates the AggregationTemporality to each stream.

#include "sdk/metric_producer.hpp"

#include "microtel/internal/metric_batch.hpp"
#include "microtel/resource.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>

namespace mts = microtel::sdk;
namespace mti = microtel::internal;
namespace mt = microtel;

namespace
{

class FakeMetricStream : public mts::IMetricStream
{
public:
    explicit FakeMetricStream(std::string name, mti::MetricData data = mti::SumData{}) noexcept
        : m_name(std::move(name)), m_data(std::move(data))
    {
    }

    using mts::IMetricStream::Collect;

    [[nodiscard]] mti::MetricRecord Collect(mti::AggregationTemporality temporality,
                                            std::chrono::system_clock::time_point /*time*/) override
    {
        m_last_temporality = temporality;
        ++m_call_count;
        return mti::MetricRecord{
            .name = m_name,
            .description = {},
            .unit = {},
            .data = m_data,
        };
    }

    [[nodiscard]] mti::AggregationTemporality LastTemporality() const noexcept
    {
        return m_last_temporality;
    }

    [[nodiscard]] int CallCount() const noexcept
    {
        return m_call_count;
    }

private:
    std::string m_name;
    mti::MetricData m_data;
    mti::AggregationTemporality m_last_temporality = mti::AggregationTemporality::Cumulative;
    int m_call_count = 0;
};

mti::InstrumentationScope MakeScope(std::string name, std::string version = "1.0")
{
    return mti::InstrumentationScope{.name = std::move(name), .version = std::move(version)};
}

std::shared_ptr<const mt::Resource> MakeResource()
{
    return std::make_shared<const mt::Resource>();
}

}  // namespace

TEST(MetricProducerTest, EmptyProducerCollectsEmptyVector)
{
    mts::MetricProducer producer{MakeResource()};
    EXPECT_TRUE(producer.Collect().empty());
}

TEST(MetricProducerTest, SingleScopeOneStreamYieldsOneHandleOneRecord)
{
    mts::MetricProducer producer{MakeResource()};
    producer.AddStream(MakeScope("lib"), std::make_unique<FakeMetricStream>("req.count"));

    const auto handles = producer.Collect();
    ASSERT_EQ(handles.size(), 1U);
    EXPECT_EQ(handles[0].Metrics().size(), 1U);
    EXPECT_EQ(handles[0].Metrics()[0].name, "req.count");
}

TEST(MetricProducerTest, SingleScopeTwoStreamsYieldsOneHandleTwoRecords)
{
    mts::MetricProducer producer{MakeResource()};
    producer.AddStream(MakeScope("lib"), std::make_unique<FakeMetricStream>("a"));
    producer.AddStream(MakeScope("lib"), std::make_unique<FakeMetricStream>("b"));

    const auto handles = producer.Collect();
    ASSERT_EQ(handles.size(), 1U);
    EXPECT_EQ(handles[0].Metrics().size(), 2U);
}

TEST(MetricProducerTest, TwoScopesYieldTwoHandles)
{
    mts::MetricProducer producer{MakeResource()};
    producer.AddStream(MakeScope("lib-a"), std::make_unique<FakeMetricStream>("x"));
    producer.AddStream(MakeScope("lib-b"), std::make_unique<FakeMetricStream>("y"));

    EXPECT_EQ(producer.Collect().size(), 2U);
}

TEST(MetricProducerTest, SameNameDifferentVersionYieldsSeparateHandles)
{
    mts::MetricProducer producer{MakeResource()};
    producer.AddStream(MakeScope("lib", "1.0"), std::make_unique<FakeMetricStream>("x"));
    producer.AddStream(MakeScope("lib", "2.0"), std::make_unique<FakeMetricStream>("y"));

    EXPECT_EQ(producer.Collect().size(), 2U);
}

TEST(MetricProducerTest, HandleCarriesScopeIdentity)
{
    mts::MetricProducer producer{MakeResource()};
    producer.AddStream(MakeScope("my.lib", "3.0"), std::make_unique<FakeMetricStream>("m"));

    const auto handles = producer.Collect();
    ASSERT_EQ(handles.size(), 1U);
    EXPECT_EQ(handles[0].Scope().name, "my.lib");
    EXPECT_EQ(handles[0].Scope().version, "3.0");
}

TEST(MetricProducerTest, CollectDelegatesTemporalityToStream)
{
    mts::MetricProducer producer{MakeResource()};
    auto stream = std::make_unique<FakeMetricStream>("m");
    auto* stream_ptr = stream.get();
    producer.AddStream(MakeScope("lib"), std::move(stream));

    (void)producer.Collect(mti::AggregationTemporality::Delta);
    EXPECT_EQ(stream_ptr->LastTemporality(), mti::AggregationTemporality::Delta);
}

TEST(MetricProducerTest, CollectDefaultIsCumulative)
{
    mts::MetricProducer producer{MakeResource()};
    auto stream = std::make_unique<FakeMetricStream>("m");
    auto* stream_ptr = stream.get();
    producer.AddStream(MakeScope("lib"), std::move(stream));

    (void)producer.Collect();
    EXPECT_EQ(stream_ptr->LastTemporality(), mti::AggregationTemporality::Cumulative);
}

TEST(MetricProducerTest, CollectCallsEachStreamExactlyOnce)
{
    mts::MetricProducer producer{MakeResource()};
    auto a = std::make_unique<FakeMetricStream>("a");
    auto b = std::make_unique<FakeMetricStream>("b");
    auto* a_ptr = a.get();
    auto* b_ptr = b.get();
    producer.AddStream(MakeScope("lib"), std::move(a));
    producer.AddStream(MakeScope("lib"), std::move(b));

    (void)producer.Collect();
    EXPECT_EQ(a_ptr->CallCount(), 1);
    EXPECT_EQ(b_ptr->CallCount(), 1);
}
