// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0
//
// MetricProducer had no mutex at all. AddStream appends to m_scopes from the
// application thread on every instrument creation (25 SdkMeter::Create* call
// sites) while the reader thread walks the same vector in Collect. A push_back
// that reallocates invalidates the iterators Collect is holding, so this was a
// use-after-free, not merely a torn read.
//
// The header claimed "Streams are registered at SDK-build time" and
// "@threadsafety Single-caller (the reader thread)"; sdk_meter.hpp claimed
// Create*() was "guarded by the provider's meter mutex". None of the three was
// true — m_meter_mu is held only inside GetMeter, never during CreateCounter.
//
// Meaningful under TSAN (-DMICROTEL_SANITIZER=tsan).

#include "microtel/internal/metric_batch.hpp"
#include "microtel/resource.hpp"

#include "sdk/metric_producer.hpp"
#include "sdk/metric_stream.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace mt = microtel;
namespace mti = microtel::internal;
namespace mts = microtel::sdk;

namespace
{

// Minimal stream: Collect must be callable from the reader thread while the
// application thread registers more streams.
class StubStream final : public mts::IMetricStream
{
public:
    using mts::IMetricStream::Collect;

    [[nodiscard]] mti::MetricRecord Collect(mti::AggregationTemporality /*temporality*/,
                                            std::chrono::system_clock::time_point /*time*/) override
    {
        return mti::MetricRecord{.name = "stub"};
    }
};

// Enough registrations to force several reallocations of m_scopes, which is
// what turns the race into a use-after-free rather than a benign stale read.
constexpr int kStreamsPerRound = 64;
constexpr int kRounds = 40;

void RegisterStreams(mts::MetricProducer& producer, const std::atomic<bool>& go)
{
    while (!go.load(std::memory_order_acquire))
    {
    }
    for (int i = 0; i < kStreamsPerRound; ++i)
    {
        // Distinct scope names so each call appends a new ScopeEntry rather
        // than pushing into an existing entry's stream vector.
        producer.AddStream(
            mti::InstrumentationScope{.name = "scope" + std::to_string(i), .version = "1.0"},
            std::make_unique<StubStream>());
    }
}

void CollectRepeatedly(mts::MetricProducer& producer, const std::atomic<bool>& go)
{
    while (!go.load(std::memory_order_acquire))
    {
    }
    for (int i = 0; i < kStreamsPerRound; ++i)
    {
        (void)producer.Collect(mti::AggregationTemporality::Cumulative);
    }
}

TEST(MetricProducerRaceTest, AddStreamConcurrentWithCollectIsRaceFree)
{
    for (int round = 0; round < kRounds; ++round)
    {
        mts::MetricProducer producer{std::make_shared<mt::Resource>()};
        std::atomic<bool> go{false};

        std::thread registrar([&] { RegisterStreams(producer, go); });
        std::thread collector([&] { CollectRepeatedly(producer, go); });

        go.store(true, std::memory_order_release);
        registrar.join();
        collector.join();
    }
    SUCCEED();
}

}  // namespace
