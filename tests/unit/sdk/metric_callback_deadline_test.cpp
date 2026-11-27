// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0
//
// The per-collection deadline for async callbacks (ICP 0046, issue #237).
//
// Contract under test:
//  - Synchronous streams are always collected.
//  - The first async stream of a collection always runs. Each later async
//    stream checks the deadline before its callback; once it has passed, the
//    stream is skipped, counted once as `MetricCallbackTimeout` and logged,
//    rate-limited.
//  - A callback that returns after the deadline keeps its measurements and is
//    not counted.
//  - A skipped stream's state is untouched: a delta series reports the change
//    since its last exported observation, a cumulative one resumes with its
//    running value and original start time.
//  - The periodic reader's deadline is its interval, capped by the time a
//    `ForceFlush(timeout)` or `Shutdown(timeout)` caller has left.

#include "microtel/attribute.hpp"
#include "microtel/internal/metric_batch.hpp"
#include "microtel/internal/metric_producer.hpp"
#include "microtel/log_sink.hpp"
#include "microtel/provider.hpp"
#include "microtel/resource.hpp"
#include "microtel/status.hpp"
#include "microtel/view.hpp"

#include "fakes/fake_diagnostics_sink.hpp"
#include "mocks/mock_metric_exporter.hpp"
#include "sdk/metric_observable_instruments.hpp"
#include "sdk/metric_producer.hpp"
#include "sdk/metric_stream_impls.hpp"
#include "sdk/periodic_exporting_metric_reader.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iterator>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

namespace mt = microtel;
namespace mti = microtel::internal;
namespace mts = microtel::sdk;

using namespace std::chrono_literals;

namespace
{

using SteadyClock = std::chrono::steady_clock;

constexpr mti::TemporalitySelector kDelta{mti::AggregationTemporality::Delta};
constexpr auto kLongInterval = 60'000ms;
constexpr auto kLongTimeout = 5'000ms;
constexpr auto kShutdownTimeout = 5ms;
constexpr auto kInterval = 20ms;
// Longer than both, so a callback this slow always outlasts them.
constexpr auto kSlowCallback = 2 * kInterval;
constexpr auto kPoll = 5ms;

std::uint64_t Timeouts(const mt::testing::FakeDiagnosticsSink& sink)
{
    return sink.drop_counters.at(static_cast<std::size_t>(mt::DropReason::MetricCallbackTimeout));
}

std::shared_ptr<const mt::Resource> MakeResource()
{
    return std::make_shared<const mt::Resource>();
}

mti::InstrumentationScope Scope(std::string name)
{
    return mti::InstrumentationScope{.name = std::move(name), .version = "1.0"};
}

std::vector<mt::KeyValue> Series()
{
    return {mt::KeyValue{.key = "pid", .value = std::int64_t{1001}}};
}

/// Register an observable gauge named @p name that counts its calls in
/// @p calls, runs @p before first, then reports 1.
void AddGauge(mts::MetricProducer& producer,
              const std::string& scope,
              std::string name,
              std::atomic<int>& calls,
              std::function<void()> before = {})
{
    producer.AddStream(
        Scope(scope),
        std::make_unique<mts::MetricStreamObservableGauge<std::int64_t>>(
            std::move(name),
            "",
            "",
            [&calls, before = std::move(before)](mts::ObservableResult<std::int64_t>& result)
            {
                calls.fetch_add(1, std::memory_order_relaxed);
                if (before)
                {
                    before();
                }
                result.Observe(1, Series());
            }));
}

/// Register an observable counter named "c" that reports `*value`.
void AddCounter(mts::MetricProducer& producer, const std::int64_t& value)
{
    producer.AddStream(Scope("lib"),
                       std::make_unique<mts::MetricStreamObservableSum<std::int64_t>>(
                           "c",
                           "",
                           "",
                           mt::InstrumentKind::ObservableCounter,
                           [&value](mts::ObservableResult<std::int64_t>& result)
                           { result.Observe(value, Series()); }));
}

/// Every record name of @p handles, in order.
std::vector<std::string> Names(const std::vector<mti::MetricBatchHandle>& handles)
{
    std::vector<std::string> names;
    for (const auto& handle : handles)
    {
        for (const mti::MetricRecord& record : handle.Metrics())
        {
            names.push_back(record.name);
        }
    }
    return names;
}

/// Whether any record of @p handles is named @p name.
bool HasRecord(const std::vector<mti::MetricBatchHandle>& handles, const std::string& name)
{
    const auto names = Names(handles);
    return std::ranges::find(names, name) != names.end();
}

/// The point of the Sum record named @p name; a test failure, and a default
/// point, if no record has it.
mti::NumberPoint SumPoint(const std::vector<mti::MetricBatchHandle>& handles, std::string_view name)
{
    for (const auto& handle : handles)
    {
        const auto metrics = handle.Metrics();
        const auto it = std::ranges::find(metrics, name, &mti::MetricRecord::name);
        if (it != metrics.end())
        {
            return std::get<mti::SumData>(it->data).points.at(0);
        }
    }
    ADD_FAILURE() << "no record named " << name;
    return {};
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
                    const std::scoped_lock lk{m_mu};
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
        const std::scoped_lock lk{m_mu};
        std::vector<std::string> out;
        std::ranges::copy_if(m_warns,
                             std::back_inserter(out),
                             [needle](const std::string& line)
                             { return line.find(needle) != std::string::npos; });
        return out;
    }

private:
    mutable std::mutex m_mu;
    std::vector<std::string> m_warns;
};

}  // namespace

// ── The request ───────────────────────────────────────────────────────────────

TEST(CallbackDeadlineTest, DefaultRequestHasNoDeadline)
{
    EXPECT_EQ(mti::CollectRequest{}.callback_deadline, SteadyClock::time_point::max());
}

TEST(CallbackDeadlineTest, ObservableStreamsRunCallbacksAndSyncStreamsDoNot)
{
    const mts::MetricStreamObservableGauge<double> gauge{"g", "", "", [](auto&) {}};
    const mts::MetricStreamObservableSum<double> sum{
        "s", "", "", mt::InstrumentKind::ObservableUpDownCounter, [](auto&) {}};
    const mts::MetricStreamSum<double> counter{"c", "", "", mt::InstrumentKind::Counter};
    EXPECT_TRUE(gauge.RunsCallback());
    EXPECT_TRUE(sum.RunsCallback());
    EXPECT_FALSE(counter.RunsCallback());
}

// ── The producer ──────────────────────────────────────────────────────────────

TEST(CallbackDeadlineTest, EveryCallbackRunsBeforeTheDeadline)
{
    auto sink = std::make_shared<mt::testing::FakeDiagnosticsSink>();
    mts::MetricProducer producer{MakeResource(), sink};
    std::atomic<int> a{0};
    std::atomic<int> b{0};
    AddGauge(producer, "lib", "a", a);
    AddGauge(producer, "lib", "b", b);

    const auto handles = producer.Collect({.callback_deadline = SteadyClock::now() + 1h});

    EXPECT_EQ(Names(handles), (std::vector<std::string>{"a", "b"}));
    EXPECT_EQ(Timeouts(*sink), 0U);
}

TEST(CallbackDeadlineTest, SlowFirstCallbackIsExportedAndTheLaterOnesAreSkippedAndCountedOnceEach)
{
    auto sink = std::make_shared<mt::testing::FakeDiagnosticsSink>();
    mts::MetricProducer producer{MakeResource(), sink};
    const auto deadline = SteadyClock::now() + 10ms;
    std::atomic<int> first{0};
    std::atomic<int> second{0};
    std::atomic<int> third{0};
    // Returns only once the deadline has passed.
    AddGauge(
        producer, "lib", "first", first, [deadline] { std::this_thread::sleep_until(deadline); });
    AddGauge(producer, "lib", "second", second);
    // In another scope: the deadline spans the whole collection.
    AddGauge(producer, "other", "third", third);

    const auto handles = producer.Collect({.callback_deadline = deadline});

    // The late callback keeps its measurement and is not counted.
    EXPECT_EQ(Names(handles), (std::vector<std::string>{"first"}));
    EXPECT_EQ(first.load(), 1);
    EXPECT_EQ(second.load(), 0);
    EXPECT_EQ(third.load(), 0);
    EXPECT_EQ(Timeouts(*sink), 2U);
    // Every stream of "other" was skipped, so it contributes no batch.
    ASSERT_EQ(handles.size(), 1U);
    EXPECT_EQ(handles.at(0).Scope().name, "lib");
}

TEST(CallbackDeadlineTest, SyncStreamsAreCollectedAfterTheDeadline)
{
    auto sink = std::make_shared<mt::testing::FakeDiagnosticsSink>();
    mts::MetricProducer producer{MakeResource(), sink};
    auto before = std::make_unique<mts::MetricStreamSum<std::int64_t>>(
        "sync.before", "", "", mt::InstrumentKind::Counter);
    auto after = std::make_unique<mts::MetricStreamSum<std::int64_t>>(
        "sync.after", "", "", mt::InstrumentKind::Counter);
    const auto attrs = Series();
    before->Storage().Add(3, mt::AttributeSpan{attrs});
    after->Storage().Add(4, mt::AttributeSpan{attrs});
    std::atomic<int> a1{0};
    std::atomic<int> a2{0};
    // A sync stream registered first is not "the first async stream".
    producer.AddStream(Scope("lib"), std::move(before));
    AddGauge(producer, "lib", "async.1", a1);
    AddGauge(producer, "lib", "async.2", a2);
    producer.AddStream(Scope("lib"), std::move(after));

    // Already passed when the collection starts.
    const auto handles = producer.Collect({.callback_deadline = SteadyClock::now()});

    EXPECT_EQ(Names(handles), (std::vector<std::string>{"sync.before", "async.1", "sync.after"}));
    EXPECT_EQ(SumPoint(handles, "sync.after").value, mti::MetricValue{std::int64_t{4}});
    EXPECT_EQ(a2.load(), 0);
    EXPECT_EQ(Timeouts(*sink), 1U);
}

TEST(CallbackDeadlineTest, SkippedDeltaCounterReportsTheChangeSinceItsLastExportedObservation)
{
    auto sink = std::make_shared<mt::testing::FakeDiagnosticsSink>();
    mts::MetricProducer producer{MakeResource(), sink};
    std::atomic<int> gate{0};
    AddGauge(producer, "lib", "gate", gate);  // the first async stream always runs
    std::int64_t value = 10;
    AddCounter(producer, value);

    const auto first = SumPoint(producer.Collect({.selector = kDelta}), "c");
    EXPECT_EQ(first.value, mti::MetricValue{std::int64_t{10}});

    value = 15;
    const auto skipped =
        producer.Collect({.selector = kDelta, .callback_deadline = SteadyClock::now()});
    EXPECT_FALSE(HasRecord(skipped, "c"));
    EXPECT_EQ(Timeouts(*sink), 1U);

    // One window, widened over the skipped cycle: 22 - 10, from the last
    // exported observation's collection.
    value = 22;
    const auto next = SumPoint(producer.Collect({.selector = kDelta}), "c");
    EXPECT_EQ(next.value, mti::MetricValue{std::int64_t{12}});
    EXPECT_EQ(next.start_time, first.time);
}

TEST(CallbackDeadlineTest, SkippedCumulativeCounterResumesWithItsRunningValueAndStartTime)
{
    auto sink = std::make_shared<mt::testing::FakeDiagnosticsSink>();
    mts::MetricProducer producer{MakeResource(), sink};
    std::atomic<int> gate{0};
    AddGauge(producer, "lib", "gate", gate);
    std::int64_t value = 10;
    AddCounter(producer, value);

    const auto first = SumPoint(producer.Collect(), "c");

    value = 15;
    const auto skipped = producer.Collect({.callback_deadline = SteadyClock::now()});
    EXPECT_FALSE(HasRecord(skipped, "c"));

    // Not forgotten (ICP 0039): the series keeps its original start.
    value = 22;
    const auto next = SumPoint(producer.Collect(), "c");
    EXPECT_EQ(next.value, mti::MetricValue{std::int64_t{22}});
    EXPECT_EQ(next.start_time, first.start_time);
}

TEST(CallbackDeadlineTest, EachSkipIsLoggedRateLimited)
{
    const WarnCapture capture;
    auto sink = std::make_shared<mt::testing::FakeDiagnosticsSink>();
    mts::MetricProducer producer{MakeResource(), sink};
    constexpr int kSkipped = 5;
    std::atomic<int> calls{0};
    AddGauge(producer, "lib", "gate", calls);
    for (int i = 0; i < kSkipped; ++i)
    {
        AddGauge(producer, "lib", "late." + std::to_string(i), calls);
    }

    (void)producer.Collect({.callback_deadline = SteadyClock::now()});

    EXPECT_EQ(Timeouts(*sink), static_cast<std::uint64_t>(kSkipped));
    // The 1st, 2nd and 4th skip are logged; the 3rd and 5th are not.
    const auto warns = capture.Mentioning("metric_callback_timeout");
    ASSERT_EQ(warns.size(), 3U);
    EXPECT_NE(warns.at(2).find("4 skipped"), std::string::npos);
}

TEST(CallbackDeadlineTest, ProducerWithoutASinkStillSkips)
{
    mts::MetricProducer producer{MakeResource()};
    std::atomic<int> a{0};
    std::atomic<int> b{0};
    AddGauge(producer, "lib", "a", a);
    AddGauge(producer, "lib", "b", b);

    const auto handles = producer.Collect({.callback_deadline = SteadyClock::now()});

    EXPECT_EQ(Names(handles), (std::vector<std::string>{"a"}));
    EXPECT_EQ(b.load(), 0);
}

// ── Through the periodic reader ───────────────────────────────────────────────

TEST(CallbackDeadlineTest, ForceFlushWithNoTimeLeftRunsTheFirstCallbackAndSkipsTheRest)
{
    auto sink = std::make_shared<mt::testing::FakeDiagnosticsSink>();
    mts::MetricProducer producer{MakeResource(), sink};
    std::atomic<int> first{0};
    std::atomic<int> second{0};
    std::atomic<int> third{0};
    AddGauge(producer, "lib", "first", first);
    AddGauge(producer, "lib", "second", second);
    AddGauge(producer, "lib", "third", third);
    mt::testing::MockMetricExporter exporter;
    mts::PeriodicExportingMetricReader reader{producer, exporter, kLongInterval};

    EXPECT_EQ(reader.ForceFlush(0ms), mt::Status::Completed);

    {
        const std::scoped_lock lk{exporter.scopes_mu};
        EXPECT_EQ(Names(exporter.exported_batches), (std::vector<std::string>{"first"}));
    }
    EXPECT_EQ(first.load(), 1);
    EXPECT_EQ(second.load(), 0);
    EXPECT_EQ(third.load(), 0);
    EXPECT_EQ(Timeouts(*sink), 2U);
}

TEST(CallbackDeadlineTest, ShutdownWithAShortTimeoutCapsItsFinalCollection)
{
    auto sink = std::make_shared<mt::testing::FakeDiagnosticsSink>();
    mts::MetricProducer producer{MakeResource(), sink};
    std::atomic<int> first{0};
    std::atomic<int> second{0};
    AddGauge(producer, "lib", "first", first, [] { std::this_thread::sleep_for(kSlowCallback); });
    AddGauge(producer, "lib", "second", second);
    mt::testing::MockMetricExporter exporter;
    mts::PeriodicExportingMetricReader reader{producer, exporter, kLongInterval};

    EXPECT_EQ(reader.Shutdown(kShutdownTimeout), mt::Status::Completed);

    {
        const std::scoped_lock lk{exporter.scopes_mu};
        EXPECT_EQ(Names(exporter.exported_batches), (std::vector<std::string>{"first"}));
    }
    EXPECT_EQ(second.load(), 0);
    EXPECT_EQ(Timeouts(*sink), 1U);
}

TEST(CallbackDeadlineTest, ABackgroundCycleIsBoundedByTheInterval)
{
    auto sink = std::make_shared<mt::testing::FakeDiagnosticsSink>();
    mts::MetricProducer producer{MakeResource(), sink};
    std::atomic<int> first{0};
    std::atomic<int> second{0};
    AddGauge(producer, "lib", "first", first, [] { std::this_thread::sleep_for(kSlowCallback); });
    AddGauge(producer, "lib", "second", second);
    mt::testing::MockMetricExporter exporter;
    mts::PeriodicExportingMetricReader reader{producer, exporter, kInterval};

    // Two background cycles have started, so one has finished.
    const auto give_up = SteadyClock::now() + kLongTimeout;
    while (first.load() < 2 && SteadyClock::now() < give_up)
    {
        std::this_thread::sleep_for(kPoll);
    }
    ASSERT_GE(first.load(), 2);
    EXPECT_EQ(reader.Shutdown(kLongTimeout), mt::Status::Completed);

    // Every cycle, the background ones and the final one, ran past its
    // interval in the first callback, so none ran the second.
    EXPECT_EQ(second.load(), 0);
    EXPECT_GE(Timeouts(*sink), 2U);
}
