// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0
//
// Unit tests for OtlpMetricExporter (M12).

#include "exporter/otlp_metric_exporter.hpp"

#include "microtel/internal/exporter.hpp"
#include "microtel/internal/metric_batch.hpp"
#include "microtel/internal/wire_result.hpp"
#include "microtel/provider.hpp"
#include "microtel/resource.hpp"
#include "microtel/status.hpp"

#include "fakes/fake_diagnostics_sink.hpp"
#include "fakes/fake_steady_clock.hpp"
#include "fakes/fake_wire_codec.hpp"
#include "mocks/mock_metric_encoder.hpp"
#include "mocks/mock_wire_codec.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

namespace mt = microtel;
namespace mti = microtel::internal;
namespace mtmk = microtel::testing;
namespace mte = microtel::exporter;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static mti::MetricBatchHandle MakeBatchOf(std::size_t metric_count)
{
    return mti::MetricBatchHandle{
        std::vector<mti::MetricRecord>(metric_count),
        std::make_shared<mt::Resource>(),
        mti::InstrumentationScope{.name = "test", .version = "0.1"},
    };
}

static mti::MetricBatchHandle MakeBatch()
{
    return MakeBatchOf(0);
}

static std::uint64_t DropCount(const mtmk::FakeDiagnosticsSink& sink, mt::DropReason reason)
{
    return sink.drop_counters.at(static_cast<std::size_t>(reason));
}

static constexpr auto kFlushTimeout = std::chrono::milliseconds(500);

// ---------------------------------------------------------------------------
// Basic encode + send
// ---------------------------------------------------------------------------

TEST(OtlpMetricExporterTest, Export_CallsEncoderAndCodec)
{
    mtmk::MockMetricEncoder encoder;
    mtmk::MockWireCodec codec;
    codec.result_to_return.success = true;
    mte::OtlpMetricExporter exporter{&encoder, &codec};

    EXPECT_EQ(exporter.Export(MakeBatch()), mti::ExportResult::Success);
    ASSERT_EQ(exporter.ForceFlush(kFlushTimeout), mt::Status::Completed);
    EXPECT_EQ(encoder.encode_call_count.load(), 1);
    EXPECT_EQ(codec.send_call_count.load(), 1);
}

TEST(OtlpMetricExporterTest, Export_MultipleExports_AllProcessed)
{
    mtmk::MockMetricEncoder encoder;
    mtmk::MockWireCodec codec;
    codec.result_to_return.success = true;
    mte::OtlpMetricExporter exporter{&encoder, &codec};

    (void)exporter.Export(MakeBatch());
    (void)exporter.Export(MakeBatch());
    (void)exporter.Export(MakeBatch());

    ASSERT_EQ(exporter.ForceFlush(kFlushTimeout), mt::Status::Completed);
    EXPECT_EQ(encoder.encode_call_count.load(), 3);
    EXPECT_EQ(codec.send_call_count.load(), 3);
}

// ---------------------------------------------------------------------------
// Lifecycle: shutdown
// ---------------------------------------------------------------------------

TEST(OtlpMetricExporterTest, Export_AfterShutdown_ReturnsAlreadyShutDown)
{
    mtmk::MockMetricEncoder encoder;
    mtmk::MockWireCodec codec;
    mte::OtlpMetricExporter exporter{&encoder, &codec};

    ASSERT_EQ(exporter.Shutdown(kFlushTimeout), mt::Status::Completed);
    EXPECT_EQ(exporter.Export(MakeBatch()), mti::ExportResult::AlreadyShutDown);
}

TEST(OtlpMetricExporterTest, Shutdown_Idempotent)
{
    mtmk::MockMetricEncoder encoder;
    mtmk::MockWireCodec codec;
    mte::OtlpMetricExporter exporter{&encoder, &codec};

    EXPECT_EQ(exporter.Shutdown(kFlushTimeout), mt::Status::Completed);
    EXPECT_EQ(exporter.Shutdown(kFlushTimeout), mt::Status::AlreadyShutDown);
}

TEST(OtlpMetricExporterTest, Shutdown_WaitsForPendingBatch)
{
    mtmk::MockMetricEncoder encoder;
    mtmk::MockWireCodec codec;
    codec.result_to_return.success = true;
    mte::OtlpMetricExporter exporter{&encoder, &codec};

    (void)exporter.Export(MakeBatch());
    ASSERT_EQ(exporter.Shutdown(kFlushTimeout), mt::Status::Completed);
    EXPECT_EQ(encoder.encode_call_count.load(), 1);
}

// ---------------------------------------------------------------------------
// Queue capacity
// ---------------------------------------------------------------------------

TEST(OtlpMetricExporterTest, Export_QueueFull_ReturnsDropped)
{
    mtmk::MockMetricEncoder encoder;
    mtmk::MockWireCodec codec;
    mte::OtlpMetricExporterConfig cfg;
    cfg.max_queue_size = 0;
    mte::OtlpMetricExporter exporter{&encoder, &codec, cfg};

    EXPECT_EQ(exporter.Export(MakeBatch()), mti::ExportResult::Dropped);
}

// ---------------------------------------------------------------------------
// ForceFlush
// ---------------------------------------------------------------------------

TEST(OtlpMetricExporterTest, ForceFlush_EmptyQueue_ReturnsCompleted)
{
    mtmk::MockMetricEncoder encoder;
    mtmk::MockWireCodec codec;
    mte::OtlpMetricExporter exporter{&encoder, &codec};

    EXPECT_EQ(exporter.ForceFlush(kFlushTimeout), mt::Status::Completed);
}

// ---------------------------------------------------------------------------
// Diagnostics — issue #169. This exporter had no sink at all: a metric
// pipeline could drop every batch and GetExporterHealth() showed nothing.
// ---------------------------------------------------------------------------

TEST(OtlpMetricExporterTest, Diagnostics_SuccessfulExport_RecordsBatchSent)
{
    mtmk::MockMetricEncoder encoder;
    mtmk::MockWireCodec codec;
    mtmk::FakeDiagnosticsSink sink;
    codec.result_to_return.success = true;
    mte::OtlpMetricExporter exporter{&encoder, &codec, {}, &sink};

    (void)exporter.Export(MakeBatch());
    ASSERT_EQ(exporter.ForceFlush(kFlushTimeout), mt::Status::Completed);

    EXPECT_EQ(sink.batches_sent, 1U);
    EXPECT_EQ(sink.batches_failed, 0U);
}

TEST(OtlpMetricExporterTest, Diagnostics_FailedExport_RecordsBatchFailedAndMessage)
{
    mtmk::MockMetricEncoder encoder;
    mtmk::MockWireCodec codec;
    mtmk::FakeDiagnosticsSink sink;
    codec.result_to_return = mti::WireResult{
        .success = false,
        .retryable = false,
        .error = mt::Error{.kind = mt::Error::Kind::Network, .message = "HTTP 401"},
    };
    mte::OtlpMetricExporter exporter{&encoder, &codec, {}, &sink};

    (void)exporter.Export(MakeBatch());
    ASSERT_EQ(exporter.ForceFlush(kFlushTimeout), mt::Status::Failed);

    EXPECT_EQ(sink.batches_failed, 1U);
    EXPECT_EQ(sink.last_error_message, "HTTP 401");
}

TEST(OtlpMetricExporterTest, Diagnostics_QueueFull_CountsEveryMetricInTheRejectedBatch)
{
    mtmk::MockMetricEncoder encoder;
    mtmk::MockWireCodec codec;
    mtmk::FakeDiagnosticsSink sink;
    mte::OtlpMetricExporterConfig cfg;
    cfg.max_queue_size = 0;
    mte::OtlpMetricExporter exporter{&encoder, &codec, cfg, &sink};

    EXPECT_EQ(exporter.Export(MakeBatchOf(3)), mti::ExportResult::Dropped);

    EXPECT_EQ(DropCount(sink, mt::DropReason::QueueFull), 3U);
}

TEST(OtlpMetricExporterTest, Diagnostics_ExportAfterShutdown_CountsPostShutdown)
{
    mtmk::MockMetricEncoder encoder;
    mtmk::MockWireCodec codec;
    mtmk::FakeDiagnosticsSink sink;
    mte::OtlpMetricExporter exporter{&encoder, &codec, {}, &sink};

    ASSERT_EQ(exporter.Shutdown(kFlushTimeout), mt::Status::Completed);
    EXPECT_EQ(exporter.Export(MakeBatchOf(2)), mti::ExportResult::AlreadyShutDown);

    EXPECT_EQ(DropCount(sink, mt::DropReason::PostShutdown), 2U);
}

TEST(OtlpMetricExporterTest, Diagnostics_NullSink_IsNotDereferenced)
{
    mtmk::MockMetricEncoder encoder;
    mtmk::MockWireCodec codec;
    codec.result_to_return.success = true;
    mte::OtlpMetricExporter exporter{&encoder, &codec};  // no sink

    (void)exporter.Export(MakeBatch());
    EXPECT_EQ(exporter.ForceFlush(kFlushTimeout), mt::Status::Completed);
}

// ---------------------------------------------------------------------------
// Drain-path exception accounting — issue #224.
//
// `ProcessBatches` runs with the queue lock released, inside a `noexcept`
// worker, so `DrainQueue` catches everything it can throw. The catch was empty
// behind a `NOLINTNEXTLINE(bugprone-empty-catch)` and a "diag hook deferred"
// comment: the batches vanished and `GetExporterHealth()` reported a clean
// pipeline. They are still lost — there is nowhere to put them — but the loss
// is now countable.
//
// `RecordBatchFailed` is the surface, not a `DropReason`: no existing reason
// names "the encoder threw", and adding one is an ICP (interfaces.md §3.5).
// ---------------------------------------------------------------------------

namespace
{

/// An encoder that always throws — the only way to reach `DrainQueue`'s catch
/// from a test, every other collaborator on that path being `noexcept`.
class ThrowingMetricEncoder final : public mti::IMetricEncoder
{
public:
    [[nodiscard]] mti::EncodedPayload Encode(const mti::MetricBatchHandle& /*batch*/) override
    {
        throw std::runtime_error("metric encode blew up");
    }
};

}  // namespace

TEST(OtlpMetricExporterTest, Diagnostics_DrainThrows_RecordsBatchFailed)
{
    ThrowingMetricEncoder encoder;
    mtmk::MockWireCodec codec;
    mtmk::FakeDiagnosticsSink sink;
    mte::OtlpMetricExporter exporter{&encoder, &codec, {}, &sink};

    EXPECT_EQ(exporter.Export(MakeBatchOf(2)), mti::ExportResult::Success);
    ASSERT_EQ(exporter.ForceFlush(kFlushTimeout), mt::Status::Failed);

    EXPECT_EQ(sink.batches_failed, 1U) << "a swallowed drain failure must still be countable";
    EXPECT_EQ(sink.batches_sent, 0U);
    EXPECT_FALSE(sink.last_error_message.empty())
        << "GetExporterHealth() must be able to say why the batch was lost";
    EXPECT_TRUE(sink.last_error_time.has_value());
}

TEST(OtlpMetricExporterTest, Diagnostics_DrainThrows_WithoutSink_StillDrains)
{
    ThrowingMetricEncoder encoder;
    mtmk::MockWireCodec codec;
    mte::OtlpMetricExporter exporter{&encoder, &codec};  // no sink

    (void)exporter.Export(MakeBatch());
    EXPECT_EQ(exporter.ForceFlush(kFlushTimeout), mt::Status::Failed);
}

// ---------------------------------------------------------------------------
// Retry — issue #222. This exporter made one attempt per batch; it now runs
// the trace exporter's retry engine, so the error-model.md §7 classification,
// the backoff, the retry budget and the final-outcome drop counters (§3)
// behave the same for every signal.
// ---------------------------------------------------------------------------

namespace
{

// Zero-delay retry config: retries happen without sleeping, for fast tests.
mte::RetryPolicyConfig ZeroDelayRetry(std::uint32_t max_attempts)
{
    return mte::RetryPolicyConfig{
        .max_attempts = max_attempts,
        .initial_backoff = std::chrono::milliseconds{0},
        .max_backoff = std::chrono::milliseconds{0},
        .backoff_multiplier = 1.0,
        .jitter_fraction = 0.0,
        .retry_budget = std::chrono::minutes(5),
    };
}

mti::WireResult RetryableFailure()
{
    return mti::WireResult{.success = false, .retryable = true};
}

// Far longer than any bound asserted below, so a sleep that Shutdown cannot
// interrupt is unmistakable in the elapsed time.
constexpr auto kLongBackoff = std::chrono::seconds(10);
constexpr auto kShutdownTimeout = std::chrono::milliseconds(500);
// Generous for sanitizer builds, and still a fraction of kLongBackoff.
constexpr auto kShutdownReturnBound = std::chrono::seconds(3);
constexpr auto kWaitForSendBound = std::chrono::seconds(5);
constexpr auto kPollInterval = std::chrono::milliseconds(1);

// Block until the codec has seen `n` sends, or the bound passes.
bool WaitForSends(const mtmk::FakeWireCodec& codec, int n)
{
    const auto deadline = std::chrono::steady_clock::now() + kWaitForSendBound;
    while (codec.send_call_count.load() < n)
    {
        if (std::chrono::steady_clock::now() >= deadline)
        {
            return false;
        }
        std::this_thread::sleep_for(kPollInterval);
    }
    return true;
}

}  // namespace

TEST(OtlpMetricExporterTest, Retry_RetryableThenSuccess_RecordsRecovered)
{
    mtmk::MockMetricEncoder encoder;
    mtmk::FakeWireCodec codec;
    mtmk::FakeDiagnosticsSink sink;
    codec.scripted_results.push_back(RetryableFailure());
    codec.default_result = mti::WireResult{.success = true};
    mte::OtlpMetricExporterConfig cfg;
    cfg.retry_policy = ZeroDelayRetry(5);
    mte::OtlpMetricExporter exporter{&encoder, &codec, cfg, &sink};

    (void)exporter.Export(MakeBatch());
    ASSERT_EQ(exporter.ForceFlush(kFlushTimeout), mt::Status::Completed);

    EXPECT_EQ(codec.send_call_count.load(), 2);
    EXPECT_EQ(DropCount(sink, mt::DropReason::RetryableFailureRecovered), 1U);
    EXPECT_EQ(sink.batches_sent, 1U);
    EXPECT_EQ(sink.batches_failed, 0U);
}

TEST(OtlpMetricExporterTest, Retry_RetryableUntilMaxAttempts_RecordsBudgetExhaustedOnce)
{
    mtmk::MockMetricEncoder encoder;
    mtmk::FakeWireCodec codec;
    mtmk::FakeDiagnosticsSink sink;
    codec.default_result = RetryableFailure();
    mte::OtlpMetricExporterConfig cfg;
    cfg.retry_policy = ZeroDelayRetry(3);
    mte::OtlpMetricExporter exporter{&encoder, &codec, cfg, &sink};

    (void)exporter.Export(MakeBatch());
    ASSERT_EQ(exporter.ForceFlush(kFlushTimeout), mt::Status::Failed);

    EXPECT_EQ(codec.send_call_count.load(), 3);
    EXPECT_EQ(encoder.encode_call_count.load(), 3) << "each attempt re-encodes (memory-model §3.1)";
    EXPECT_EQ(DropCount(sink, mt::DropReason::RetryBudgetExhausted), 1U);
    EXPECT_EQ(sink.batches_failed, 1U);
    EXPECT_EQ(sink.batches_sent, 0U);
}

TEST(OtlpMetricExporterTest, Retry_BudgetSpentOnEntry_NoRetryAndRecordsBudgetExhausted)
{
    mtmk::MockMetricEncoder encoder;
    mtmk::FakeWireCodec codec;
    mtmk::FakeDiagnosticsSink sink;
    mtmk::FakeSteadyClock clock;  // never advances: a 0 ms budget is spent on entry
    codec.default_result = RetryableFailure();
    mte::OtlpMetricExporterConfig cfg;
    cfg.retry_policy = ZeroDelayRetry(5);
    cfg.retry_policy.retry_budget = std::chrono::milliseconds{0};
    mte::OtlpMetricExporter exporter{&encoder, &codec, cfg, &sink, &clock};

    (void)exporter.Export(MakeBatch());
    ASSERT_EQ(exporter.ForceFlush(kFlushTimeout), mt::Status::Failed);

    EXPECT_EQ(codec.send_call_count.load(), 1);
    EXPECT_EQ(DropCount(sink, mt::DropReason::RetryBudgetExhausted), 1U);
    EXPECT_EQ(sink.batches_failed, 1U);
}

TEST(OtlpMetricExporterTest, Retry_NonRetryable_SingleAttemptAndRecordsNonRetryable)
{
    mtmk::MockMetricEncoder encoder;
    mtmk::FakeWireCodec codec;
    mtmk::FakeDiagnosticsSink sink;
    codec.default_result = mti::WireResult{.success = false, .retryable = false};
    mte::OtlpMetricExporterConfig cfg;
    cfg.retry_policy = ZeroDelayRetry(5);
    mte::OtlpMetricExporter exporter{&encoder, &codec, cfg, &sink};

    (void)exporter.Export(MakeBatch());
    ASSERT_EQ(exporter.ForceFlush(kFlushTimeout), mt::Status::Failed);

    EXPECT_EQ(codec.send_call_count.load(), 1);
    EXPECT_EQ(DropCount(sink, mt::DropReason::NonRetryableFailure), 1U);
    EXPECT_EQ(DropCount(sink, mt::DropReason::RetryBudgetExhausted), 0U);
    EXPECT_EQ(sink.batches_failed, 1U);
}

// The fan-out carries no `retry_after`, so the zero backoff lets the first
// retry through; its `retry_after` then governs the next sleep. A server
// asking for longer than the remaining budget ends the loop at once instead
// of being retried early.
TEST(OtlpMetricExporterTest, Retry_RetryAfterBeyondBudget_StopsRetrying)
{
    mtmk::MockMetricEncoder encoder;
    mtmk::FakeWireCodec codec;
    mtmk::FakeDiagnosticsSink sink;
    mtmk::FakeSteadyClock clock;  // never advances
    codec.scripted_results.push_back(RetryableFailure());
    codec.scripted_results.push_back(mti::WireResult{
        .success = false, .retryable = true, .retry_after = std::chrono::seconds{10}});
    codec.default_result = mti::WireResult{.success = true};
    mte::OtlpMetricExporterConfig cfg;
    cfg.retry_policy = ZeroDelayRetry(5);
    cfg.retry_policy.retry_budget = std::chrono::seconds{1};
    mte::OtlpMetricExporter exporter{&encoder, &codec, cfg, &sink, &clock};

    (void)exporter.Export(MakeBatch());
    ASSERT_EQ(exporter.ForceFlush(kFlushTimeout), mt::Status::Failed);

    // Zero-delay backoff alone would have made the third (successful) attempt.
    EXPECT_EQ(codec.send_call_count.load(), 2);
    EXPECT_EQ(DropCount(sink, mt::DropReason::RetryBudgetExhausted), 1U);
}

TEST(OtlpMetricExporterTest, Retry_RetryAfter_SleepsAtLeastThatLong)
{
    constexpr auto kRetryAfter = std::chrono::milliseconds{100};
    mtmk::MockMetricEncoder encoder;
    mtmk::FakeWireCodec codec;
    mtmk::FakeDiagnosticsSink sink;
    codec.scripted_results.push_back(RetryableFailure());
    codec.scripted_results.push_back(
        mti::WireResult{.success = false, .retryable = true, .retry_after = kRetryAfter});
    codec.default_result = mti::WireResult{.success = true};
    mte::OtlpMetricExporterConfig cfg;
    cfg.retry_policy = ZeroDelayRetry(5);
    mte::OtlpMetricExporter exporter{&encoder, &codec, cfg, &sink};

    const auto started = std::chrono::steady_clock::now();
    (void)exporter.Export(MakeBatch());
    ASSERT_EQ(exporter.ForceFlush(std::chrono::seconds{5}), mt::Status::Completed);
    const auto elapsed = std::chrono::steady_clock::now() - started;

    EXPECT_EQ(codec.send_call_count.load(), 3);
    EXPECT_GE(elapsed, kRetryAfter) << "retry_after overrides a zero backoff";
    EXPECT_EQ(DropCount(sink, mt::DropReason::RetryableFailureRecovered), 1U);
}

TEST(OtlpMetricExporterTest, Retry_PartialSuccess_NeverRetried)
{
    mtmk::MockMetricEncoder encoder;
    mtmk::FakeWireCodec codec;
    mtmk::FakeDiagnosticsSink sink;
    codec.default_result = mti::WireResult{.success = true, .partial_success_rejected = 4};
    mte::OtlpMetricExporterConfig cfg;
    cfg.retry_policy = ZeroDelayRetry(3);
    mte::OtlpMetricExporter exporter{&encoder, &codec, cfg, &sink};

    (void)exporter.Export(MakeBatch());
    ASSERT_EQ(exporter.ForceFlush(kFlushTimeout), mt::Status::Completed);

    EXPECT_EQ(codec.send_call_count.load(), 1);
    EXPECT_EQ(DropCount(sink, mt::DropReason::PartialSuccessRejection), 4U);
    EXPECT_EQ(sink.batches_sent, 1U);
    EXPECT_EQ(sink.batches_failed, 0U);
}

// docs/sequences/shutdown-drain.md: "Shutdown during active retry — backoff
// sleep wakes; worker exits the retry loop". The batch is lost, and counted.
TEST(OtlpMetricExporterTest, Retry_ShutdownDuringBackoff_ReturnsWithinTimeout)
{
    mtmk::MockMetricEncoder encoder;
    mtmk::FakeWireCodec codec;
    mtmk::FakeDiagnosticsSink sink;
    codec.default_result = RetryableFailure();
    mte::OtlpMetricExporterConfig cfg;
    cfg.retry_policy = mte::RetryPolicyConfig{
        .max_attempts = 3,
        .initial_backoff = kLongBackoff,
        .max_backoff = kLongBackoff,
        .backoff_multiplier = 1.0,
        .jitter_fraction = 0.0,
        .retry_budget = std::chrono::minutes(5),
    };
    mte::OtlpMetricExporter exporter{&encoder, &codec, cfg, &sink};

    (void)exporter.Export(MakeBatch());
    // The fan-out only; the worker then sleeps before the first retry
    // (issue #311).
    ASSERT_TRUE(WaitForSends(codec, 1));

    const auto started = std::chrono::steady_clock::now();
    const auto status = exporter.Shutdown(kShutdownTimeout);
    const auto elapsed = std::chrono::steady_clock::now() - started;

    EXPECT_LT(elapsed, kShutdownReturnBound);
    EXPECT_EQ(status, mt::Status::Failed);
    EXPECT_EQ(codec.send_call_count.load(), 1) << "no retry after Shutdown";
    EXPECT_EQ(DropCount(sink, mt::DropReason::RetryBudgetExhausted), 1U);
    EXPECT_EQ(sink.batches_failed, 1U);
}

// Issue #311: the first retry backs off too. The fan-out's own `retry_after`
// (HTTP `Retry-After`, gRPC `RetryInfo`) is honoured before attempt 1 instead
// of the server being hit again at once.
TEST(OtlpMetricExporterTest, Retry_FirstRetryWaitsForFanOutRetryAfter)
{
    constexpr auto kFanOutRetryAfter = std::chrono::milliseconds{60};
    mtmk::MockMetricEncoder encoder;
    mtmk::FakeWireCodec codec;
    mtmk::FakeDiagnosticsSink sink;
    codec.scripted_results.push_back(
        mti::WireResult{.success = false, .retryable = true, .retry_after = kFanOutRetryAfter});
    codec.default_result = mti::WireResult{.success = true};
    mte::OtlpMetricExporterConfig cfg;
    cfg.retry_policy = ZeroDelayRetry(5);
    mte::OtlpMetricExporter exporter{&encoder, &codec, cfg, &sink};

    const auto started = std::chrono::steady_clock::now();
    (void)exporter.Export(MakeBatch());
    ASSERT_EQ(exporter.ForceFlush(std::chrono::seconds{5}), mt::Status::Completed);
    const auto elapsed = std::chrono::steady_clock::now() - started;

    EXPECT_EQ(codec.send_call_count.load(), 2);
    EXPECT_GE(elapsed, kFanOutRetryAfter) << "the fan-out's retry_after is slept before attempt 1";
    EXPECT_EQ(DropCount(sink, mt::DropReason::RetryableFailureRecovered), 1U);
}

// ---------------------------------------------------------------------------
// on_shutdown_timeout (ICP 0036 Decision 2)
// ---------------------------------------------------------------------------

namespace
{

/// A codec whose Send blocks until Release, as a transport ignoring its
/// deadline would.
class BlockingCodec final : public mti::IWireCodec
{
public:
    // NOLINTNEXTLINE(cppcoreguidelines-rvalue-reference-param-not-moved)
    [[nodiscard]] mti::WireResult Send(mti::EncodedPayload&& /*payload*/,
                                       std::chrono::milliseconds /*deadline*/) override
    {
        std::unique_lock lock{m_mu};
        m_entered = true;
        m_cv.notify_all();
        m_cv.wait(lock, [this] { return m_released; });
        return mti::WireResult{.success = true};
    }

    void WaitUntilEntered()
    {
        std::unique_lock lock{m_mu};
        m_cv.wait(lock, [this] { return m_entered; });
    }

    void Release()
    {
        const std::scoped_lock lock{m_mu};
        m_released = true;
        m_cv.notify_all();
    }

private:
    std::mutex m_mu;
    std::condition_variable m_cv;
    bool m_entered = false;
    bool m_released = false;
};

}  // namespace

TEST(OtlpMetricExporterTest, ShutdownTimeout_CallsTheHookBeforeJoiningTheWorker)
{
    mtmk::MockMetricEncoder encoder;
    BlockingCodec codec;
    int hook_calls = 0;
    mte::OtlpMetricExporterConfig cfg;
    cfg.on_shutdown_timeout = [&]
    {
        ++hook_calls;
        codec.Release();
    };
    mte::OtlpMetricExporter exporter{&encoder, &codec, cfg};

    ASSERT_EQ(exporter.Export(MakeBatch()), mti::ExportResult::Success);
    codec.WaitUntilEntered();

    EXPECT_EQ(exporter.Shutdown(std::chrono::milliseconds(50)), mt::Status::TimedOut);
    EXPECT_EQ(hook_calls, 1);
}

TEST(OtlpMetricExporterTest, ShutdownCompleted_DoesNotCallTheHook)
{
    mtmk::MockMetricEncoder encoder;
    mtmk::MockWireCodec codec;
    codec.result_to_return.success = true;
    int hook_calls = 0;
    mte::OtlpMetricExporterConfig cfg;
    cfg.on_shutdown_timeout = [&] { ++hook_calls; };
    mte::OtlpMetricExporter exporter{&encoder, &codec, cfg};

    (void)exporter.Export(MakeBatch());
    EXPECT_EQ(exporter.Shutdown(std::chrono::seconds(5)), mt::Status::Completed);
    EXPECT_EQ(hook_calls, 0);
}

// ---------------------------------------------------------------------------
// ForceFlush waits for the batches queued before the call, not for an empty
// queue (ICP 0041, #484)
// ---------------------------------------------------------------------------

namespace
{

constexpr auto kSustainedSendDelay = std::chrono::milliseconds{2};
constexpr auto kSustainedArrivalGap = std::chrono::microseconds{500};
constexpr auto kSustainedWarmup = std::chrono::milliseconds{50};
constexpr auto kSustainedFlushTimeout = std::chrono::seconds{5};
constexpr auto kRetryBackoff = std::chrono::milliseconds{100};

mti::WireResult NonRetryableFailure()
{
    return mti::WireResult{.success = false, .retryable = false};
}

}  // namespace

// Batches arrive four times faster than they are sent, so the queue never
// empties. A flush that waited for an empty queue timed out every time.
TEST(OtlpMetricExporterTest, ForceFlush_UnderSustainedLoad_CompletesForWhatWasQueuedBeforeIt)
{
    mtmk::MockMetricEncoder encoder;
    mtmk::FakeWireCodec codec;
    codec.default_result = mti::WireResult{.success = true};
    codec.send_delay = kSustainedSendDelay;
    mte::OtlpMetricExporter exporter{&encoder, &codec};

    std::atomic<bool> stop{false};
    std::thread producer{[&]
                         {
                             while (!stop.load())
                             {
                                 (void)exporter.Export(MakeBatch());
                                 std::this_thread::sleep_for(kSustainedArrivalGap);
                             }
                         }};
    std::this_thread::sleep_for(kSustainedWarmup);
    const mt::Status status = exporter.ForceFlush(kSustainedFlushTimeout);
    stop.store(true);
    producer.join();

    EXPECT_EQ(status, mt::Status::Completed);
}

TEST(OtlpMetricExporterTest, ForceFlush_ABatchThatFailedAfterItsFinalAttempt_ReturnsFailed)
{
    mtmk::MockMetricEncoder encoder;
    mtmk::FakeWireCodec codec;
    codec.default_result = NonRetryableFailure();
    mte::OtlpMetricExporter exporter{&encoder, &codec};

    (void)exporter.Export(MakeBatch());
    EXPECT_EQ(exporter.ForceFlush(kFlushTimeout), mt::Status::Failed);
}

TEST(OtlpMetricExporterTest, ForceFlush_AFailureBeforeTheCall_DoesNotFailALaterFlush)
{
    mtmk::MockMetricEncoder encoder;
    mtmk::FakeWireCodec codec;
    codec.scripted_results.push_back(NonRetryableFailure());
    codec.default_result = mti::WireResult{.success = true};
    mte::OtlpMetricExporter exporter{&encoder, &codec};

    (void)exporter.Export(MakeBatch());
    ASSERT_EQ(exporter.ForceFlush(kFlushTimeout), mt::Status::Failed);
    (void)exporter.Export(MakeBatch());
    EXPECT_EQ(exporter.ForceFlush(kFlushTimeout), mt::Status::Completed);
}

// A batch in retry backoff is still pending: the flush waits for its retry.
TEST(OtlpMetricExporterTest, ForceFlush_ABatchInRetryBackoff_IsStillPending)
{
    mtmk::MockMetricEncoder encoder;
    mtmk::FakeWireCodec codec;
    codec.scripted_results.push_back(mti::WireResult{.success = false, .retryable = true});
    codec.default_result = mti::WireResult{.success = true};
    mte::OtlpMetricExporterConfig cfg;
    cfg.retry_policy.initial_backoff = kRetryBackoff;
    cfg.retry_policy.jitter_fraction = 0.0;
    mte::OtlpMetricExporter exporter{&encoder, &codec, cfg};

    (void)exporter.Export(MakeBatch());
    EXPECT_EQ(exporter.ForceFlush(kSustainedFlushTimeout), mt::Status::Completed);
    EXPECT_EQ(codec.send_call_count.load(), 2);
}

TEST(OtlpMetricExporterTest, Shutdown_ABatchThatFailed_ReturnsFailed)
{
    mtmk::MockMetricEncoder encoder;
    mtmk::FakeWireCodec codec;
    codec.default_result = NonRetryableFailure();
    mte::OtlpMetricExporter exporter{&encoder, &codec};

    (void)exporter.Export(MakeBatch());
    EXPECT_EQ(exporter.Shutdown(kFlushTimeout), mt::Status::Failed);
}
