// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0
//
// Proves TimeoutOptions::retry_budget actually reaches the exporter's retry
// engine through a real SdkBuilder::Build() (issue #179).
//
// BuildExporters used to fill OtlpExporterConfig::export_deadline and leave
// retry_policy default-constructed, so every builder-built provider retried on
// RetryPolicyConfig's 5-minute in-class default no matter what the operator
// configured. A batch to a dead endpoint then sat in the backoff schedule for
// the better part of ten seconds before giving up. The knob was parsed,
// validated and documented, and then discarded.
//
// The existing retry tests in tests/unit/exporter/otlp_exporter_test.cpp
// construct OtlpExporterConfig by hand, so they exercise the budget mechanics
// while being blind to the wiring. Only a test that goes through the builder
// can see this.
//
// Integration tier: this opens a real socket and is wall-clock-bounded, which
// the unit tier's <1ms / no-I/O bar excludes.

#include "microtel/log_record.hpp"
#include "microtel/logger.hpp"
#include "microtel/meter.hpp"
#include "microtel/provider.hpp"
#include "microtel/sdk_builder.hpp"
#include "microtel/status.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>

namespace
{

// Port 1 is reliably closed for a non-root process, so the lazy connect
// (ICP 0017) fails with a retryable Network error rather than hanging on an
// unroutable address — which is exactly what drives the retry loop.
constexpr const char* kClosedPortEndpoint = "http://127.0.0.1:1";

// The value issue #179 used to reproduce. Small enough that the first attempt
// alone outlives it, so a budget that is actually consulted ends the loop
// before any backoff sleep.
constexpr auto kConfiguredRetryBudget = std::chrono::milliseconds(200);

// Generous by design: this separates two behaviours that are ~4x apart, and is
// not a measurement of either. Locally (clang, Debug), with the budget:
//
//   * ignored (the bug): 6.2 - 6.7s, the 5-attempt backoff schedule running to
//     exhaustion under RetryPolicyConfig's 5-minute default. Issue #179
//     independently measured 7.4s.
//   * honoured (fixed): 1.4 - 1.7s, of which none is backoff — the budget is
//     already spent when the loop takes its first budget check. That residual
//     is fixed export-path cost (lazy connect, per-export deadline, flush and
//     teardown); exporter_health_test pays the same ~1.5s.
//
// Confirmed by sweeping the budget in this harness: 1ms and 200ms both land at
// ~1.5s (zero sleeps), while 2000ms rises to ~3.1 - 3.9s as one backoff sleep
// fits inside it. So the assertion below is sensitive to the plumbing, not to
// machine speed.
//
// 5s leaves ~3x headroom above the slowest green run and ~1.2s below the
// *fastest* red one, which is the margin that matters for a false pass.
// Sanitizers barely move either — both paths are dominated by sleeps and
// socket timeouts rather than CPU (tsan 1.3 - 1.5s, asan 1.6s).
constexpr auto kGiveUpCeiling = std::chrono::seconds(5);

[[nodiscard]] std::uint64_t DropCount(const microtel::HealthSnapshot& health,
                                      microtel::DropReason reason)
{
    return health.drop_counters.at(static_cast<std::size_t>(reason));
}

}  // namespace

TEST(RetryBudgetIntegrationTest, ConfiguredRetryBudgetBoundsTheExporterRetryLoop)
{
    auto result = microtel::SdkBuilder()
                      .WithEndpoint(kClosedPortEndpoint)
                      .WithTimeouts(microtel::TimeoutOptions{
                          .connect = std::chrono::milliseconds(200),
                          .tls_handshake = std::chrono::milliseconds(200),
                          .per_export = std::chrono::milliseconds(200),
                          .retry_budget = kConfiguredRetryBudget,
                          .flush = std::chrono::seconds(30),
                          .shutdown = std::chrono::seconds(10),
                      })
                      .Build();
    ASSERT_TRUE(result.has_value()) << result.error().message;
    const std::shared_ptr<microtel::Provider> provider = std::move(*result);

    const auto started = std::chrono::steady_clock::now();

    // Deliberately no Connect() — the export path connects lazily (ICP 0017),
    // so the failure lands inside the wire codec and is classified retryable.
    {
        const auto tracer = provider->GetTracer("retry-budget-wiring", "1.0");
        const auto span = tracer->StartSpan("doomed-export");
        span->End();
    }
    // The flush timeout is deliberately far longer than the ceiling: a timeout
    // here would hide the bug behind a Status::TimedOut instead of exposing it
    // as elapsed time.
    ASSERT_EQ(provider->ForceFlush(std::chrono::seconds(30)), microtel::Status::Failed);

    const auto elapsed = std::chrono::steady_clock::now() - started;
    const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed);

    const microtel::HealthSnapshot health = provider->GetExporterHealth();

    // Outcome first: the batch was retried and still lost, which is what puts
    // the exporter in the retry loop this test is timing. Asserting the
    // counter as well as the clock keeps a fast-but-wrong path (one that never
    // retried at all) from passing.
    EXPECT_GE(DropCount(health, microtel::DropReason::RetryBudgetExhausted), 1U)
        << "the batch must end in the retried-and-lost funnel";
    EXPECT_GE(health.batches_failed, 1U);
    EXPECT_EQ(health.batches_sent, 0U);

    EXPECT_LT(elapsed_ms, kGiveUpCeiling)
        << "a " << kConfiguredRetryBudget.count() << "ms configured retry budget must bound the "
        << "retry loop; giving up took " << elapsed_ms.count()
        << "ms, which means SdkBuilder left OtlpExporterConfig::retry_policy "
           "default-constructed (issue #179)";
}

// ---------------------------------------------------------------------------
// Issue #222: the metric and log exporters now retry through the same engine,
// so the same knob must reach them. Same harness and ceiling as above; only
// the signal differs. Without the wiring the log / metric exporter runs
// RetryPolicyConfig's 5-minute default and the schedule outlives the ceiling.
// ---------------------------------------------------------------------------

namespace
{

void ExpectRetriedAndLost(const microtel::HealthSnapshot& health)
{
    EXPECT_GE(DropCount(health, microtel::DropReason::RetryBudgetExhausted), 1U)
        << "the batch must end in the retried-and-lost funnel";
    EXPECT_GE(health.batches_failed, 1U);
    EXPECT_EQ(health.batches_sent, 0U);
}

// Build against the closed port, produce one record of the signal under test,
// flush, and check the retry loop was entered and bounded by the budget.
void ExpectSignalRetryBoundedByBudget(
    const std::function<void(microtel::Provider&)>& produce_one_record)
{
    auto result = microtel::SdkBuilder()
                      .WithEndpoint(kClosedPortEndpoint)
                      .WithTimeouts(microtel::TimeoutOptions{
                          .connect = std::chrono::milliseconds(200),
                          .tls_handshake = std::chrono::milliseconds(200),
                          .per_export = std::chrono::milliseconds(200),
                          .retry_budget = kConfiguredRetryBudget,
                          .flush = std::chrono::seconds(30),
                          .shutdown = std::chrono::seconds(10),
                      })
                      .Build();
    ASSERT_TRUE(result.has_value()) << result.error().message;
    const std::shared_ptr<microtel::Provider> provider = std::move(*result);

    const auto started = std::chrono::steady_clock::now();
    produce_one_record(*provider);
    ASSERT_EQ(provider->ForceFlush(std::chrono::seconds(30)), microtel::Status::Failed);
    const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started);

    ExpectRetriedAndLost(provider->GetExporterHealth());
    EXPECT_LT(elapsed_ms, kGiveUpCeiling)
        << "giving up took " << elapsed_ms.count()
        << "ms: SdkBuilder did not pass retry_budget to this signal's exporter";
}

}  // namespace

TEST(RetryBudgetIntegrationTest, ConfiguredRetryBudgetBoundsTheLogExporterRetryLoop)
{
    ExpectSignalRetryBoundedByBudget(
        [](microtel::Provider& provider)
        {
            microtel::LogRecord record;
            record.body = std::string{"doomed-export"};
            provider.GetLogger("retry-budget-wiring")->Emit(std::move(record));
        });
}

TEST(RetryBudgetIntegrationTest, ConfiguredRetryBudgetBoundsTheMetricExporterRetryLoop)
{
    ExpectSignalRetryBoundedByBudget(
        [](microtel::Provider& provider)
        {
            provider.GetMeter("retry-budget-wiring")
                ->CreateCounter<std::int64_t>("doomed.export", "", "1")
                ->Add(1, {});
        });
}
