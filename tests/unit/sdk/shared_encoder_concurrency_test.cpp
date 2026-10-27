// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0
//
// One Provider builds one wire::OtlpEncoder and hands it to the trace, metric
// and log exporters, each of which encodes on its own worker thread. This test
// keeps all three workers encoding at once, so that under TSan
// (-DMICROTEL_SANITIZER=tsan) any state the encode paths share is reported.

#include "microtel/export_transport.hpp"
#include "microtel/log_record.hpp"
#include "microtel/logger.hpp"
#include "microtel/meter.hpp"
#include "microtel/provider.hpp"
#include "microtel/sdk_builder.hpp"
#include "microtel/status.hpp"
#include "microtel/tracer.hpp"

#include "fakes/fake_export_transport.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace
{

namespace mt = microtel;
namespace mtt = microtel::testing;

using namespace std::chrono_literals;

constexpr int kRounds = 200;
constexpr int kRecordsPerRound = 64;
constexpr int kAttributes = 8;
constexpr std::size_t kMinRequestsPerSignal = 10;

std::vector<mt::KeyValue> Attributes(int round)
{
    std::vector<mt::KeyValue> attrs;
    attrs.reserve(kAttributes);
    for (int i = 0; i < kAttributes; ++i)
    {
        attrs.push_back(mt::KeyValue{.key = "k" + std::to_string(i),
                                     .value = std::string(32, static_cast<char>('a' + i)) +
                                              std::to_string(round % kRecordsPerRound)});
    }
    return attrs;
}

std::size_t CountOf(const std::vector<mtt::FakeExportTransport::Recorded>& sent,
                    mt::ExportSignal signal)
{
    return static_cast<std::size_t>(
        std::ranges::count_if(sent, [signal](const auto& r) { return r.signal == signal; }));
}
/// Spins until the test fires the starting gun, so the producers start together.
void AwaitStart(const std::atomic<bool>& go)
{
    while (!go.load())
    {
        std::this_thread::yield();
    }
}

void EmitSpan(mt::Tracer& tracer, const std::vector<mt::KeyValue>& attrs)
{
    auto span = tracer.StartSpan("op");
    for (const auto& kv : attrs)
    {
        span->SetAttribute(kv.key, kv.value);
    }
    span->End();
}

void ProduceSpans(mt::Provider& provider, const std::atomic<bool>& go)
{
    const auto tracer = provider.GetTracer("traces");
    AwaitStart(go);
    for (int r = 0; r < kRounds; ++r)
    {
        const auto attrs = Attributes(r);
        for (int i = 0; i < kRecordsPerRound; ++i)
        {
            EmitSpan(*tracer, attrs);
        }
    }
}

void ProduceMetrics(mt::Provider& provider, const std::atomic<bool>& go)
{
    const auto counter = provider.GetMeter("metrics")->CreateCounter<std::int64_t>("c");
    AwaitStart(go);
    for (int r = 0; r < kRounds; ++r)
    {
        for (int i = 0; i < kRecordsPerRound; ++i)
        {
            const auto attrs = Attributes(i);
            counter->Add(1, attrs);
        }
        std::this_thread::sleep_for(1ms);
    }
}

void ProduceLogs(mt::Provider& provider, const std::atomic<bool>& go)
{
    const auto logger = provider.GetLogger("logs");
    AwaitStart(go);
    for (int r = 0; r < kRounds; ++r)
    {
        const auto attrs = Attributes(r);
        for (int i = 0; i < kRecordsPerRound; ++i)
        {
            logger->Emit(mt::LogRecord{.body = std::string{"message"}, .attributes = attrs});
        }
    }
}

}  // namespace

TEST(SharedEncoderConcurrencyTest, TraceMetricAndLogWorkersEncodeConcurrently)
{
    auto owner = std::make_unique<mtt::FakeExportTransport>();
    const mtt::FakeExportTransport* const fake = owner.get();
    auto built = mt::SdkBuilder()
                     .WithServiceName("shared-encoder")
                     .WithExportTransport(std::move(owner),
                                          mt::ExportTransportOptions{.metrics = true, .logs = true})
                     .WithBatch(mt::BatchOptions{.max_queue_size = 8192,
                                                 .max_export_batch_size = kRecordsPerRound,
                                                 .schedule_delay = 1ms})
                     .WithMetricInterval(20ms)
                     .Build();
    ASSERT_TRUE(built.has_value()) << built.error().message;
    const std::shared_ptr<mt::Provider> provider = *built;

    // Each producer feeds one signal's pipeline; its worker encodes while the
    // other two workers do the same.
    std::atomic<bool> go{false};
    std::vector<std::thread> producers;
    producers.emplace_back(ProduceSpans, std::ref(*provider), std::cref(go));
    producers.emplace_back(ProduceMetrics, std::ref(*provider), std::cref(go));
    producers.emplace_back(ProduceLogs, std::ref(*provider), std::cref(go));
    go.store(true);
    for (auto& t : producers)
    {
        t.join();
    }
    ASSERT_EQ(provider->ForceFlush(30s), mt::Status::Completed);

    const auto sent = fake->Sent();
    EXPECT_GE(CountOf(sent, mt::ExportSignal::Traces), kMinRequestsPerSignal);
    EXPECT_GE(CountOf(sent, mt::ExportSignal::Metrics), kMinRequestsPerSignal);
    EXPECT_GE(CountOf(sent, mt::ExportSignal::Logs), kMinRequestsPerSignal);
    EXPECT_EQ(provider->GetExporterHealth().batches_failed, 0U);
    EXPECT_EQ(provider->Shutdown(10s), mt::Status::Completed);
}
