// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0
//
// Unit tests for OtlpLogExporter — M14 L4.3 (docs/logs-design.md §3).
// Mirrors the OtlpMetricExporter tests: encode-and-send, lifecycle, queue cap.

#include "exporter/otlp_log_exporter.hpp"

#include "microtel/internal/exporter.hpp"
#include "microtel/internal/log_batch.hpp"
#include "microtel/internal/wire_result.hpp"
#include "microtel/provider.hpp"
#include "microtel/resource.hpp"
#include "microtel/status.hpp"

#include "fakes/fake_diagnostics_sink.hpp"
#include "fakes/fake_steady_clock.hpp"
#include "fakes/fake_wire_codec.hpp"
#include "mocks/mock_log_encoder.hpp"
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

namespace
{

mti::LogBatchHandle MakeBatchOf(std::size_t record_count)
{
    return mti::LogBatchHandle{
        std::vector<mt::LogRecord>(record_count),
        std::make_shared<mt::Resource>(),
        mti::InstrumentationScope{.name = "test", .version = "0.1"},
    };
}

mti::LogBatchHandle MakeBatch()
{
    return MakeBatchOf(0);
}

std::uint64_t DropCount(const mtmk::FakeDiagnosticsSink& sink, mt::DropReason reason)
{
    return sink.drop_counters.at(static_cast<std::size_t>(reason));
}

constexpr auto kFlushTimeout = std::chrono::milliseconds(500);

TEST(OtlpLogExporterTest, ExportCallsEncoderAndCodec)
{
    mtmk::MockLogEncoder encoder;
    mtmk::MockWireCodec codec;
    codec.result_to_return.success = true;
    mte::OtlpLogExporter exporter{&encoder, &codec};

    EXPECT_EQ(exporter.Export(MakeBatch()), mti::ExportResult::Success);
    ASSERT_EQ(exporter.ForceFlush(kFlushTimeout), mt::Status::Completed);
    EXPECT_EQ(encoder.encode_call_count.load(), 1);
    EXPECT_EQ(codec.send_call_count.load(), 1);
}

TEST(OtlpLogExporterTest, MultipleExportsAllProcessed)
{
    mtmk::MockLogEncoder encoder;
    mtmk::MockWireCodec codec;
    codec.result_to_return.success = true;
    mte::OtlpLogExporter exporter{&encoder, &codec};

    (void)exporter.Export(MakeBatch());
    (void)exporter.Export(MakeBatch());
    (void)exporter.Export(MakeBatch());

    ASSERT_EQ(exporter.ForceFlush(kFlushTimeout), mt::Status::Completed);
    EXPECT_EQ(encoder.encode_call_count.load(), 3);
}

TEST(OtlpLogExporterTest, ExportAfterShutdownReturnsAlreadyShutDown)
{
    mtmk::MockLogEncoder encoder;
    mtmk::MockWireCodec codec;
    mte::OtlpLogExporter exporter{&encoder, &codec};

    ASSERT_EQ(exporter.Shutdown(kFlushTimeout), mt::Status::Completed);
    EXPECT_EQ(exporter.Export(MakeBatch()), mti::ExportResult::AlreadyShutDown);
}

TEST(OtlpLogExporterTest, ShutdownIsIdempotent)
{
    mtmk::MockLogEncoder encoder;
    mtmk::MockWireCodec codec;
    mte::OtlpLogExporter exporter{&encoder, &codec};

    EXPECT_EQ(exporter.Shutdown(kFlushTimeout), mt::Status::Completed);
    EXPECT_EQ(exporter.Shutdown(kFlushTimeout), mt::Status::AlreadyShutDown);
}

TEST(OtlpLogExporterTest, ShutdownWaitsForPendingBatch)
{
    mtmk::MockLogEncoder encoder;
    mtmk::MockWireCodec codec;
    codec.result_to_return.success = true;
    mte::OtlpLogExporter exporter{&encoder, &codec};

    (void)exporter.Export(MakeBatch());
    ASSERT_EQ(exporter.Shutdown(kFlushTimeout), mt::Status::Completed);
    EXPECT_EQ(encoder.encode_call_count.load(), 1);
}

TEST(OtlpLogExporterTest, ExportQueueFullReturnsDropped)
{
    mtmk::MockLogEncoder encoder;
    mtmk::MockWireCodec codec;
    mte::OtlpLogExporterConfig cfg;
    cfg.max_queue_size = 0;
    mte::OtlpLogExporter exporter{&encoder, &codec, cfg};

    EXPECT_EQ(exporter.Export(MakeBatch()), mti::ExportResult::Dropped);
}

TEST(OtlpLogExporterTest, ForceFlushEmptyQueueReturnsCompleted)
{
    mtmk::MockLogEncoder encoder;
    mtmk::MockWireCodec codec;
    mte::OtlpLogExporter exporter{&encoder, &codec};

    EXPECT_EQ(exporter.ForceFlush(kFlushTimeout), mt::Status::Completed);
}

// ---------------------------------------------------------------------------
// Diagnostics — issue #169. This exporter had no sink at all: a log pipeline
// could drop every batch it was given and GetExporterHealth() showed nothing.
// ---------------------------------------------------------------------------

TEST(OtlpLogExporterTest, DiagnosticsSuccessfulExportRecordsBatchSent)
{
    mtmk::MockLogEncoder encoder;
    mtmk::MockWireCodec codec;
    mtmk::FakeDiagnosticsSink sink;
    codec.result_to_return.success = true;
    mte::OtlpLogExporter exporter{&encoder, &codec, {}, &sink};

    (void)exporter.Export(MakeBatch());
    ASSERT_EQ(exporter.ForceFlush(kFlushTimeout), mt::Status::Completed);

    EXPECT_EQ(sink.batches_sent, 1U);
    EXPECT_EQ(sink.batches_failed, 0U);
}

TEST(OtlpLogExporterTest, DiagnosticsFailedExportRecordsBatchFailedAndMessage)
{
    mtmk::MockLogEncoder encoder;
    mtmk::MockWireCodec codec;
    mtmk::FakeDiagnosticsSink sink;
    codec.result_to_return = mti::WireResult{
        .success = false,
        .retryable = false,
        .error = mt::Error{.kind = mt::Error::Kind::Network, .message = "HTTP 401"},
    };
    mte::OtlpLogExporter exporter{&encoder, &codec, {}, &sink};

    (void)exporter.Export(MakeBatch());
    ASSERT_EQ(exporter.ForceFlush(kFlushTimeout), mt::Status::Failed);

    EXPECT_EQ(sink.batches_failed, 1U);
    EXPECT_EQ(sink.last_error_message, "HTTP 401");
}

TEST(OtlpLogExporterTest, DiagnosticsQueueFullCountsEveryRecordInTheRejectedBatch)
{
    mtmk::MockLogEncoder encoder;
    mtmk::MockWireCodec codec;
    mtmk::FakeDiagnosticsSink sink;
    mte::OtlpLogExporterConfig cfg;
    cfg.max_queue_size = 0;
    mte::OtlpLogExporter exporter{&encoder, &codec, cfg, &sink};

    EXPECT_EQ(exporter.Export(MakeBatchOf(3)), mti::ExportResult::Dropped);

    EXPECT_EQ(DropCount(sink, mt::DropReason::QueueFull), 3U);
}

TEST(OtlpLogExporterTest, DiagnosticsExportAfterShutdownCountsPostShutdown)
{
    mtmk::MockLogEncoder encoder;
    mtmk::MockWireCodec codec;
    mtmk::FakeDiagnosticsSink sink;
    mte::OtlpLogExporter exporter{&encoder, &codec, {}, &sink};

    ASSERT_EQ(exporter.Shutdown(kFlushTimeout), mt::Status::Completed);
    EXPECT_EQ(exporter.Export(MakeBatchOf(2)), mti::ExportResult::AlreadyShutDown);

    EXPECT_EQ(DropCount(sink, mt::DropReason::PostShutdown), 2U);
}

TEST(OtlpLogExporterTest, DiagnosticsNullSinkIsNotDereferenced)
{
    mtmk::MockLogEncoder encoder;
    mtmk::MockWireCodec codec;
    codec.result_to_return.success = true;
    mte::OtlpLogExporter exporter{&encoder, &codec};  // no sink

    (void)exporter.Export(MakeBatch());
    EXPECT_EQ(exporter.ForceFlush(kFlushTimeout), mt::Status::Completed);
}

// ---------------------------------------------------------------------------
// Drain-path exception accounting — issue #224, mirroring the metrics
// exporter. `DrainQueue`'s catch was empty behind a "diag hook deferred"
// comment, so a batch lost to a throwing encoder left `GetExporterHealth()`
// reporting a clean pipeline. The batch is still lost; the loss is countable.
// ---------------------------------------------------------------------------

/// An encoder that always throws — the only way to reach `DrainQueue`'s catch
/// from a test, every other collaborator on that path being `noexcept`.
class ThrowingLogEncoder final : public mti::ILogEncoder
{
public:
    [[nodiscard]] mti::EncodedPayload Encode(const mti::LogBatchHandle& /*batch*/) override
    {
        throw std::runtime_error("log encode blew up");
    }
};

TEST(OtlpLogExporterTest, DiagnosticsDrainThrowsRecordsBatchFailed)
{
    ThrowingLogEncoder encoder;
    mtmk::MockWireCodec codec;
    mtmk::FakeDiagnosticsSink sink;
    mte::OtlpLogExporter exporter{&encoder, &codec, {}, &sink};

    EXPECT_EQ(exporter.Export(MakeBatchOf(2)), mti::ExportResult::Success);
    ASSERT_EQ(exporter.ForceFlush(kFlushTimeout), mt::Status::Failed);

    EXPECT_EQ(sink.batches_failed, 1U) << "a swallowed drain failure must still be countable";
    EXPECT_EQ(sink.batches_sent, 0U);
    EXPECT_FALSE(sink.last_error_message.empty())
        << "GetExporterHealth() must be able to say why the batch was lost";
    EXPECT_TRUE(sink.last_error_time.has_value());
}

TEST(OtlpLogExporterTest, DiagnosticsDrainThrowsWithoutSinkStillDrains)
{
    ThrowingLogEncoder encoder;
    mtmk::MockWireCodec codec;
    mte::OtlpLogExporter exporter{&encoder, &codec};  // no sink

    (void)exporter.Export(MakeBatch());
    EXPECT_EQ(exporter.ForceFlush(kFlushTimeout), mt::Status::Failed);
}

// ---------------------------------------------------------------------------
// Retry — issue #222. This exporter made one attempt per batch; it now runs
// the trace exporter's retry engine, so the error-model.md §7 classification,
// the backoff, the retry budget and the final-outcome drop counters (§3)
// behave the same for every signal.
// ---------------------------------------------------------------------------

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

TEST(OtlpLogExporterTest, Retry_RetryableThenSuccess_RecordsRecovered)
{
    mtmk::MockLogEncoder encoder;
    mtmk::FakeWireCodec codec;
    mtmk::FakeDiagnosticsSink sink;
    codec.scripted_results.push_back(RetryableFailure());
    codec.default_result = mti::WireResult{.success = true};
    mte::OtlpLogExporterConfig cfg;
    cfg.retry_policy = ZeroDelayRetry(5);
    mte::OtlpLogExporter exporter{&encoder, &codec, cfg, &sink};

    (void)exporter.Export(MakeBatch());
    ASSERT_EQ(exporter.ForceFlush(kFlushTimeout), mt::Status::Completed);

    EXPECT_EQ(codec.send_call_count.load(), 2);
    EXPECT_EQ(DropCount(sink, mt::DropReason::RetryableFailureRecovered), 1U);
    EXPECT_EQ(sink.batches_sent, 1U);
    EXPECT_EQ(sink.batches_failed, 0U);
}

TEST(OtlpLogExporterTest, Retry_RetryableUntilMaxAttempts_RecordsBudgetExhaustedOnce)
{
    mtmk::MockLogEncoder encoder;
    mtmk::FakeWireCodec codec;
    mtmk::FakeDiagnosticsSink sink;
    codec.default_result = RetryableFailure();
    mte::OtlpLogExporterConfig cfg;
    cfg.retry_policy = ZeroDelayRetry(3);
    mte::OtlpLogExporter exporter{&encoder, &codec, cfg, &sink};

    (void)exporter.Export(MakeBatch());
    ASSERT_EQ(exporter.ForceFlush(kFlushTimeout), mt::Status::Failed);

    EXPECT_EQ(codec.send_call_count.load(), 3);
    EXPECT_EQ(encoder.encode_call_count.load(), 3) << "each attempt re-encodes (memory-model §3.1)";
    EXPECT_EQ(DropCount(sink, mt::DropReason::RetryBudgetExhausted), 1U);
    EXPECT_EQ(sink.batches_failed, 1U);
    EXPECT_EQ(sink.batches_sent, 0U);
}

TEST(OtlpLogExporterTest, Retry_BudgetSpentOnEntry_NoRetryAndRecordsBudgetExhausted)
{
    mtmk::MockLogEncoder encoder;
    mtmk::FakeWireCodec codec;
    mtmk::FakeDiagnosticsSink sink;
    mtmk::FakeSteadyClock clock;  // never advances: a 0 ms budget is spent on entry
    codec.default_result = RetryableFailure();
    mte::OtlpLogExporterConfig cfg;
    cfg.retry_policy = ZeroDelayRetry(5);
    cfg.retry_policy.retry_budget = std::chrono::milliseconds{0};
    mte::OtlpLogExporter exporter{&encoder, &codec, cfg, &sink, &clock};

    (void)exporter.Export(MakeBatch());
    ASSERT_EQ(exporter.ForceFlush(kFlushTimeout), mt::Status::Failed);

    EXPECT_EQ(codec.send_call_count.load(), 1);
    EXPECT_EQ(DropCount(sink, mt::DropReason::RetryBudgetExhausted), 1U);
    EXPECT_EQ(sink.batches_failed, 1U);
}

TEST(OtlpLogExporterTest, Retry_NonRetryable_SingleAttemptAndRecordsNonRetryable)
{
    mtmk::MockLogEncoder encoder;
    mtmk::FakeWireCodec codec;
    mtmk::FakeDiagnosticsSink sink;
    codec.default_result = mti::WireResult{.success = false, .retryable = false};
    mte::OtlpLogExporterConfig cfg;
    cfg.retry_policy = ZeroDelayRetry(5);
    mte::OtlpLogExporter exporter{&encoder, &codec, cfg, &sink};

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
TEST(OtlpLogExporterTest, Retry_RetryAfterBeyondBudget_StopsRetrying)
{
    mtmk::MockLogEncoder encoder;
    mtmk::FakeWireCodec codec;
    mtmk::FakeDiagnosticsSink sink;
    mtmk::FakeSteadyClock clock;  // never advances
    codec.scripted_results.push_back(RetryableFailure());
    codec.scripted_results.push_back(mti::WireResult{
        .success = false, .retryable = true, .retry_after = std::chrono::seconds{10}});
    codec.default_result = mti::WireResult{.success = true};
    mte::OtlpLogExporterConfig cfg;
    cfg.retry_policy = ZeroDelayRetry(5);
    cfg.retry_policy.retry_budget = std::chrono::seconds{1};
    mte::OtlpLogExporter exporter{&encoder, &codec, cfg, &sink, &clock};

    (void)exporter.Export(MakeBatch());
    ASSERT_EQ(exporter.ForceFlush(kFlushTimeout), mt::Status::Failed);

    // Zero-delay backoff alone would have made the third (successful) attempt.
    EXPECT_EQ(codec.send_call_count.load(), 2);
    EXPECT_EQ(DropCount(sink, mt::DropReason::RetryBudgetExhausted), 1U);
}

TEST(OtlpLogExporterTest, Retry_RetryAfter_SleepsAtLeastThatLong)
{
    constexpr auto kRetryAfter = std::chrono::milliseconds{100};
    mtmk::MockLogEncoder encoder;
    mtmk::FakeWireCodec codec;
    mtmk::FakeDiagnosticsSink sink;
    codec.scripted_results.push_back(RetryableFailure());
    codec.scripted_results.push_back(
        mti::WireResult{.success = false, .retryable = true, .retry_after = kRetryAfter});
    codec.default_result = mti::WireResult{.success = true};
    mte::OtlpLogExporterConfig cfg;
    cfg.retry_policy = ZeroDelayRetry(5);
    mte::OtlpLogExporter exporter{&encoder, &codec, cfg, &sink};

    const auto started = std::chrono::steady_clock::now();
    (void)exporter.Export(MakeBatch());
    ASSERT_EQ(exporter.ForceFlush(std::chrono::seconds{5}), mt::Status::Completed);
    const auto elapsed = std::chrono::steady_clock::now() - started;

    EXPECT_EQ(codec.send_call_count.load(), 3);
    EXPECT_GE(elapsed, kRetryAfter) << "retry_after overrides a zero backoff";
    EXPECT_EQ(DropCount(sink, mt::DropReason::RetryableFailureRecovered), 1U);
}

TEST(OtlpLogExporterTest, Retry_PartialSuccess_NeverRetried)
{
    mtmk::MockLogEncoder encoder;
    mtmk::FakeWireCodec codec;
    mtmk::FakeDiagnosticsSink sink;
    codec.default_result = mti::WireResult{.success = true, .partial_success_rejected = 4};
    mte::OtlpLogExporterConfig cfg;
    cfg.retry_policy = ZeroDelayRetry(3);
    mte::OtlpLogExporter exporter{&encoder, &codec, cfg, &sink};

    (void)exporter.Export(MakeBatch());
    ASSERT_EQ(exporter.ForceFlush(kFlushTimeout), mt::Status::Completed);

    EXPECT_EQ(codec.send_call_count.load(), 1);
    EXPECT_EQ(DropCount(sink, mt::DropReason::PartialSuccessRejection), 4U);
    EXPECT_EQ(sink.batches_sent, 1U);
    EXPECT_EQ(sink.batches_failed, 0U);
}

// docs/sequences/shutdown-drain.md: "Shutdown during active retry — backoff
// sleep wakes; worker exits the retry loop". The batch is lost, and counted.
TEST(OtlpLogExporterTest, Retry_ShutdownDuringBackoff_ReturnsWithinTimeout)
{
    mtmk::MockLogEncoder encoder;
    mtmk::FakeWireCodec codec;
    mtmk::FakeDiagnosticsSink sink;
    codec.default_result = RetryableFailure();
    mte::OtlpLogExporterConfig cfg;
    cfg.retry_policy = mte::RetryPolicyConfig{
        .max_attempts = 3,
        .initial_backoff = kLongBackoff,
        .max_backoff = kLongBackoff,
        .backoff_multiplier = 1.0,
        .jitter_fraction = 0.0,
        .retry_budget = std::chrono::minutes(5),
    };
    mte::OtlpLogExporter exporter{&encoder, &codec, cfg, &sink};

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
TEST(OtlpLogExporterTest, Retry_FirstRetryWaitsForFanOutRetryAfter)
{
    constexpr auto kFanOutRetryAfter = std::chrono::milliseconds{60};
    mtmk::MockLogEncoder encoder;
    mtmk::FakeWireCodec codec;
    mtmk::FakeDiagnosticsSink sink;
    codec.scripted_results.push_back(
        mti::WireResult{.success = false, .retryable = true, .retry_after = kFanOutRetryAfter});
    codec.default_result = mti::WireResult{.success = true};
    mte::OtlpLogExporterConfig cfg;
    cfg.retry_policy = ZeroDelayRetry(5);
    mte::OtlpLogExporter exporter{&encoder, &codec, cfg, &sink};

    const auto started = std::chrono::steady_clock::now();
    (void)exporter.Export(MakeBatch());
    ASSERT_EQ(exporter.ForceFlush(std::chrono::seconds{5}), mt::Status::Completed);
    const auto elapsed = std::chrono::steady_clock::now() - started;

    EXPECT_EQ(codec.send_call_count.load(), 2);
    EXPECT_GE(elapsed, kFanOutRetryAfter) << "the fan-out's retry_after is slept before attempt 1";
    EXPECT_EQ(DropCount(sink, mt::DropReason::RetryableFailureRecovered), 1U);
}

}  // namespace

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

TEST(OtlpLogExporterTest, ShutdownTimeout_CallsTheHookBeforeJoiningTheWorker)
{
    mtmk::MockLogEncoder encoder;
    BlockingCodec codec;
    int hook_calls = 0;
    mte::OtlpLogExporterConfig cfg;
    cfg.on_shutdown_timeout = [&]
    {
        ++hook_calls;
        codec.Release();
    };
    mte::OtlpLogExporter exporter{&encoder, &codec, cfg};

    ASSERT_EQ(exporter.Export(MakeBatch()), mti::ExportResult::Success);
    codec.WaitUntilEntered();

    EXPECT_EQ(exporter.Shutdown(std::chrono::milliseconds(50)), mt::Status::TimedOut);
    EXPECT_EQ(hook_calls, 1);
}

TEST(OtlpLogExporterTest, ShutdownCompleted_DoesNotCallTheHook)
{
    mtmk::MockLogEncoder encoder;
    mtmk::MockWireCodec codec;
    codec.result_to_return.success = true;
    int hook_calls = 0;
    mte::OtlpLogExporterConfig cfg;
    cfg.on_shutdown_timeout = [&] { ++hook_calls; };
    mte::OtlpLogExporter exporter{&encoder, &codec, cfg};

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
TEST(OtlpLogExporterTest, ForceFlush_UnderSustainedLoad_CompletesForWhatWasQueuedBeforeIt)
{
    mtmk::MockLogEncoder encoder;
    mtmk::FakeWireCodec codec;
    codec.default_result = mti::WireResult{.success = true};
    codec.send_delay = kSustainedSendDelay;
    mte::OtlpLogExporter exporter{&encoder, &codec};

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

TEST(OtlpLogExporterTest, ForceFlush_ABatchThatFailedAfterItsFinalAttempt_ReturnsFailed)
{
    mtmk::MockLogEncoder encoder;
    mtmk::FakeWireCodec codec;
    codec.default_result = NonRetryableFailure();
    mte::OtlpLogExporter exporter{&encoder, &codec};

    (void)exporter.Export(MakeBatch());
    EXPECT_EQ(exporter.ForceFlush(kFlushTimeout), mt::Status::Failed);
}

TEST(OtlpLogExporterTest, ForceFlush_AFailureBeforeTheCall_DoesNotFailALaterFlush)
{
    mtmk::MockLogEncoder encoder;
    mtmk::FakeWireCodec codec;
    codec.scripted_results.push_back(NonRetryableFailure());
    codec.default_result = mti::WireResult{.success = true};
    mte::OtlpLogExporter exporter{&encoder, &codec};

    (void)exporter.Export(MakeBatch());
    ASSERT_EQ(exporter.ForceFlush(kFlushTimeout), mt::Status::Failed);
    (void)exporter.Export(MakeBatch());
    EXPECT_EQ(exporter.ForceFlush(kFlushTimeout), mt::Status::Completed);
}

// A batch in retry backoff is still pending: the flush waits for its retry.
TEST(OtlpLogExporterTest, ForceFlush_ABatchInRetryBackoff_IsStillPending)
{
    mtmk::MockLogEncoder encoder;
    mtmk::FakeWireCodec codec;
    codec.scripted_results.push_back(mti::WireResult{.success = false, .retryable = true});
    codec.default_result = mti::WireResult{.success = true};
    mte::OtlpLogExporterConfig cfg;
    cfg.retry_policy.initial_backoff = kRetryBackoff;
    cfg.retry_policy.jitter_fraction = 0.0;
    mte::OtlpLogExporter exporter{&encoder, &codec, cfg};

    (void)exporter.Export(MakeBatch());
    EXPECT_EQ(exporter.ForceFlush(kSustainedFlushTimeout), mt::Status::Completed);
    EXPECT_EQ(codec.send_call_count.load(), 2);
}

TEST(OtlpLogExporterTest, Shutdown_ABatchThatFailed_ReturnsFailed)
{
    mtmk::MockLogEncoder encoder;
    mtmk::FakeWireCodec codec;
    codec.default_result = NonRetryableFailure();
    mte::OtlpLogExporter exporter{&encoder, &codec};

    (void)exporter.Export(MakeBatch());
    EXPECT_EQ(exporter.Shutdown(kFlushTimeout), mt::Status::Failed);
}
