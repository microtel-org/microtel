// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0
//
// Unit tests for OtlpExporter (M3 close + M5-A retry engine).

#include "exporter/otlp_exporter.hpp"

#include "microtel/internal/batch.hpp"
#include "microtel/internal/exporter.hpp"
#include "microtel/provider.hpp"
#include "microtel/resource.hpp"
#include "microtel/status.hpp"

#include "fakes/fake_diagnostics_sink.hpp"
#include "fakes/fake_steady_clock.hpp"
#include "fakes/fake_wire_codec.hpp"
#include "mocks/mock_otlp_encoder.hpp"
#include "mocks/mock_wire_codec.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

namespace mt = microtel;
namespace mti = microtel::internal;
namespace mtmk = microtel::testing;
namespace mte = microtel::exporter;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static mti::BatchHandle MakeBatchOf(std::size_t span_count)
{
    auto resource = std::make_shared<mt::Resource>();
    mti::InstrumentationScope scope{.name = "test", .version = "0.1"};
    std::vector<mti::SpanRecord> spans;
    spans.reserve(span_count);
    for (std::size_t i = 0; i < span_count; ++i)
    {
        spans.push_back(mti::SpanRecord{.name = "s"});
    }
    return mti::BatchHandle{std::move(spans), std::move(resource), std::move(scope)};
}

static mti::BatchHandle MakeBatch()
{
    return MakeBatchOf(1);
}

static std::uint64_t DropCount(const mtmk::FakeDiagnosticsSink& sink, mt::DropReason reason)
{
    return sink.drop_counters.at(static_cast<std::size_t>(reason));
}

static std::uint64_t TotalDrops(const mtmk::FakeDiagnosticsSink& sink)
{
    std::uint64_t total = 0;
    for (const auto count : sink.drop_counters)
    {
        total += count;
    }
    return total;
}

static constexpr auto kFlushTimeout = std::chrono::milliseconds(500);

// Zero-delay retry config: retries happen without sleeping, for fast tests.
static mte::RetryPolicyConfig ZeroDelayRetry(std::uint32_t max_attempts = 5)
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

// ---------------------------------------------------------------------------
// Basic encode + send
// ---------------------------------------------------------------------------

TEST(OtlpExporterTest, Export_CallsEncoderAndCodec)
{
    mtmk::MockOtlpEncoder encoder;
    mtmk::MockWireCodec codec;
    codec.result_to_return.success = true;
    mte::OtlpExporter exporter{&encoder, &codec};

    const auto result = exporter.Export(MakeBatch());
    EXPECT_EQ(result, mti::ExportResult::Success);

    ASSERT_EQ(exporter.ForceFlush(kFlushTimeout), mt::Status::Completed);
    EXPECT_EQ(encoder.encode_call_count.load(), 1);
    EXPECT_EQ(codec.send_call_count.load(), 1);
}

TEST(OtlpExporterTest, Export_MultipleExports_AllProcessed)
{
    mtmk::MockOtlpEncoder encoder;
    mtmk::MockWireCodec codec;
    codec.result_to_return.success = true;
    // One span per request, so no two batches can be coalesced into one
    // request (docs/leaf-concentrator-design.md §3.6.1) and every batch is
    // its own send whatever the worker's timing.
    mte::OtlpExporterConfig cfg;
    cfg.max_spans_per_request = 1;
    mte::OtlpExporter exporter{&encoder, &codec, cfg};

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

TEST(OtlpExporterTest, Export_AfterShutdown_ReturnsAlreadyShutDown)
{
    mtmk::MockOtlpEncoder encoder;
    mtmk::MockWireCodec codec;
    mte::OtlpExporter exporter{&encoder, &codec};

    ASSERT_EQ(exporter.Shutdown(kFlushTimeout), mt::Status::Completed);
    EXPECT_EQ(exporter.Export(MakeBatch()), mti::ExportResult::AlreadyShutDown);
}

TEST(OtlpExporterTest, Shutdown_Idempotent)
{
    mtmk::MockOtlpEncoder encoder;
    mtmk::MockWireCodec codec;
    mte::OtlpExporter exporter{&encoder, &codec};

    EXPECT_EQ(exporter.Shutdown(kFlushTimeout), mt::Status::Completed);
    EXPECT_EQ(exporter.Shutdown(kFlushTimeout), mt::Status::AlreadyShutDown);
}

TEST(OtlpExporterTest, Shutdown_WaitsForPendingBatch)
{
    mtmk::MockOtlpEncoder encoder;
    mtmk::MockWireCodec codec;
    codec.result_to_return.success = true;
    mte::OtlpExporter exporter{&encoder, &codec};

    (void)exporter.Export(MakeBatch());
    ASSERT_EQ(exporter.Shutdown(kFlushTimeout), mt::Status::Completed);
    EXPECT_EQ(encoder.encode_call_count.load(), 1);
}

// ---------------------------------------------------------------------------
// Destructor without Shutdown
//
// CLAUDE.md rule 15: the destructor invokes Shutdown with a small finite
// timeout if not already shut down. Every lifecycle test above calls Shutdown
// explicitly, so nothing exercised the destructor-only path — the one an
// application takes when an exporter simply goes out of scope.
// ---------------------------------------------------------------------------

// Generous by design: the assertion is that teardown *terminates*, not a
// latency budget. The real bound is the destructor's own Shutdown timeout
// (`kDestructorShutdownTimeout`, 5s in `otlp_exporter.cpp`); a sanitizer build
// runs several times slower, and ctest's per-test timeout is the backstop for a
// destructor that wedges rather than merely running late.
static constexpr auto kDestructorTeardownBound = std::chrono::seconds(30);

TEST(OtlpExporterTest, DestroyedWithoutShutdown_DrainsAndJoinsWithinBound)
{
    mtmk::MockOtlpEncoder encoder;
    mtmk::MockWireCodec codec;
    codec.result_to_return.success = true;

    const auto start = std::chrono::steady_clock::now();
    {
        // Declared inside, so encoder and codec outlive the exporter that
        // borrows them — the same ordering the SDK guarantees by member
        // declaration order.
        mte::OtlpExporter exporter{&encoder, &codec};
        (void)exporter.Export(MakeBatch());
        // No Shutdown(), no ForceFlush(). Scope exit is the whole teardown.
    }
    const auto elapsed = std::chrono::steady_clock::now() - start;

    EXPECT_LT(elapsed, kDestructorTeardownBound);
    // Not merely "it returned": the queued batch was drained by the
    // destructor's Shutdown rather than abandoned with the worker thread.
    EXPECT_EQ(encoder.encode_call_count.load(), 1);
    EXPECT_EQ(codec.send_call_count.load(), 1);
}

TEST(OtlpExporterTest, DestroyedImmediatelyAfterConstruction_JoinsWithinBound)
{
    mtmk::MockOtlpEncoder encoder;
    mtmk::MockWireCodec codec;

    const auto start = std::chrono::steady_clock::now();
    {
        // `const` because nothing is called on it — construction and
        // destruction are the whole test, and the destructor runs regardless.
        const mte::OtlpExporter exporter{&encoder, &codec};
    }
    const auto elapsed = std::chrono::steady_clock::now() - start;

    EXPECT_LT(elapsed, kDestructorTeardownBound);
    EXPECT_EQ(encoder.encode_call_count.load(), 0);
}

// ---------------------------------------------------------------------------
// Queue capacity
// ---------------------------------------------------------------------------

TEST(OtlpExporterTest, Export_QueueFull_ReturnsDropped)
{
    mtmk::MockOtlpEncoder encoder;
    mtmk::MockWireCodec codec;
    mte::OtlpExporterConfig cfg;
    cfg.max_queue_size = 0;
    mte::OtlpExporter exporter{&encoder, &codec, cfg};

    EXPECT_EQ(exporter.Export(MakeBatch()), mti::ExportResult::Dropped);
}

// ---------------------------------------------------------------------------
// ForceFlush
// ---------------------------------------------------------------------------

TEST(OtlpExporterTest, ForceFlush_EmptyQueue_ReturnsCompleted)
{
    mtmk::MockOtlpEncoder encoder;
    mtmk::MockWireCodec codec;
    mte::OtlpExporter exporter{&encoder, &codec};

    EXPECT_EQ(exporter.ForceFlush(kFlushTimeout), mt::Status::Completed);
}

// ---------------------------------------------------------------------------
// Failure classification — single-attempt scenarios
// ---------------------------------------------------------------------------

TEST(OtlpExporterTest, Export_NonRetryableFailure_SingleAttempt)
{
    mtmk::MockOtlpEncoder encoder;
    mtmk::MockWireCodec codec;
    // default: success=false, retryable=false
    mte::OtlpExporterConfig cfg;
    cfg.retry_policy = ZeroDelayRetry(3);
    mte::OtlpExporter exporter{&encoder, &codec, cfg};

    (void)exporter.Export(MakeBatch());
    ASSERT_EQ(exporter.ForceFlush(kFlushTimeout), mt::Status::Failed);
    EXPECT_EQ(codec.send_call_count.load(), 1);
}

// ---------------------------------------------------------------------------
// M5-A: Retry engine
// ---------------------------------------------------------------------------

TEST(OtlpExporterTest, Retry_RetryableFailure_ExhaustsMaxAttempts)
{
    mtmk::MockOtlpEncoder encoder;
    mtmk::FakeWireCodec codec;
    codec.default_result = mti::WireResult{.success = false, .retryable = true};

    mte::OtlpExporterConfig cfg;
    cfg.retry_policy = ZeroDelayRetry(3);
    mte::OtlpExporter exporter{&encoder, &codec, cfg};

    (void)exporter.Export(MakeBatch());
    ASSERT_EQ(exporter.ForceFlush(kFlushTimeout), mt::Status::Failed);
    // 3 attempts total (1 initial + 2 retries)
    EXPECT_EQ(codec.send_call_count.load(), 3);
    EXPECT_EQ(encoder.encode_call_count.load(), 3);
}

TEST(OtlpExporterTest, Retry_SuccessOnSecondAttempt_StopsRetrying)
{
    mtmk::MockOtlpEncoder encoder;
    mtmk::FakeWireCodec codec;
    codec.scripted_results.push_back(mti::WireResult{.success = false, .retryable = true});
    codec.default_result = mti::WireResult{.success = true};

    mte::OtlpExporterConfig cfg;
    cfg.retry_policy = ZeroDelayRetry(5);
    mte::OtlpExporter exporter{&encoder, &codec, cfg};

    (void)exporter.Export(MakeBatch());
    ASSERT_EQ(exporter.ForceFlush(kFlushTimeout), mt::Status::Completed);
    EXPECT_EQ(codec.send_call_count.load(), 2);
    EXPECT_EQ(encoder.encode_call_count.load(), 2);
}

TEST(OtlpExporterTest, Retry_NonRetryableAfterRetryable_StopsImmediately)
{
    mtmk::MockOtlpEncoder encoder;
    mtmk::FakeWireCodec codec;
    codec.scripted_results.push_back(mti::WireResult{.success = false, .retryable = true});
    codec.default_result = mti::WireResult{.success = false, .retryable = false};

    mte::OtlpExporterConfig cfg;
    cfg.retry_policy = ZeroDelayRetry(5);
    mte::OtlpExporter exporter{&encoder, &codec, cfg};

    (void)exporter.Export(MakeBatch());
    ASSERT_EQ(exporter.ForceFlush(kFlushTimeout), mt::Status::Failed);
    EXPECT_EQ(codec.send_call_count.load(), 2);
}

TEST(OtlpExporterTest, Retry_MaxAttemptsOne_NeverRetries)
{
    mtmk::MockOtlpEncoder encoder;
    mtmk::FakeWireCodec codec;
    codec.default_result = mti::WireResult{.success = false, .retryable = true};

    mte::OtlpExporterConfig cfg;
    cfg.retry_policy = ZeroDelayRetry(1);
    mte::OtlpExporter exporter{&encoder, &codec, cfg};

    (void)exporter.Export(MakeBatch());
    ASSERT_EQ(exporter.ForceFlush(kFlushTimeout), mt::Status::Failed);
    EXPECT_EQ(codec.send_call_count.load(), 1);
}

TEST(OtlpExporterTest, Retry_BudgetExhausted_StopsAfterFirstAttempt)
{
    mtmk::MockOtlpEncoder encoder;
    mtmk::FakeWireCodec codec;
    codec.default_result = mti::WireResult{.success = false, .retryable = true};
    mtmk::FakeSteadyClock clock;  // time = epoch

    mte::OtlpExporterConfig cfg;
    cfg.retry_policy = mte::RetryPolicyConfig{
        .max_attempts = 5,
        .initial_backoff = std::chrono::milliseconds{0},
        .max_backoff = std::chrono::milliseconds{0},
        .backoff_multiplier = 1.0,
        .jitter_fraction = 0.0,
        .retry_budget = std::chrono::milliseconds{0},  // budget = 0ms → exhausted immediately
    };
    mte::OtlpExporter exporter{&encoder, &codec, cfg, nullptr, &clock};

    (void)exporter.Export(MakeBatch());
    ASSERT_EQ(exporter.ForceFlush(kFlushTimeout), mt::Status::Failed);
    EXPECT_EQ(codec.send_call_count.load(), 1);
}

// Issue #195 — the retry-budget look-ahead.
// `docs/sequences/retry-after-failure.md` §4 says the loop exits when the
// *upcoming* sleep would push elapsed time past `retry_budget`; the code
// checked only whether the budget was already spent, so a failure path that
// returns quickly still bought one full backoff sleep beyond the budget.
//
// The fake clock never advances on its own, so "already spent" is never true
// once the loop is running: without the look-ahead every attempt is made and
// every backoff slept. With it, the loop exits rather than sleeping a full
// second against a 1 ms budget. Since issue #311 the first retry backs off
// too, so that exit comes before attempt 1.
TEST(OtlpExporterTest, Retry_BackoffWouldOutlastBudget_ExitsWithoutSleeping)
{
    mtmk::MockOtlpEncoder encoder;
    mtmk::FakeWireCodec codec;
    mtmk::FakeDiagnosticsSink sink;
    codec.default_result = mti::WireResult{.success = false, .retryable = true};
    mtmk::FakeSteadyClock clock;  // time = epoch, never advances

    mte::OtlpExporterConfig cfg;
    cfg.retry_policy = mte::RetryPolicyConfig{
        .max_attempts = 4,
        .initial_backoff = std::chrono::seconds{1},
        .max_backoff = std::chrono::seconds{1},
        .backoff_multiplier = 1.0,
        .jitter_fraction = 0.0,
        .retry_budget = std::chrono::milliseconds{1},
    };
    mte::OtlpExporter exporter{&encoder, &codec, cfg, &sink, &clock};

    const auto started = std::chrono::steady_clock::now();
    (void)exporter.Export(MakeBatch());
    // Long enough that a loop still sleeping its way through the backoffs
    // drains rather than times out — the assertions below are the diagnosis,
    // not a flush timeout.
    ASSERT_EQ(exporter.ForceFlush(std::chrono::milliseconds{5000}), mt::Status::Failed);
    const auto elapsed = std::chrono::steady_clock::now() - started;

    // Attempt 0 is the fan-out. The backoff before attempt 1 costs 1 s
    // against a 1 ms budget, so the loop exits instead of taking it and the
    // fan-out result stands (issue #311).
    EXPECT_EQ(codec.send_call_count.load(), 1);
    // And it exits *before* the sleep: without the look-ahead the loop spends
    // two whole seconds of backoff on a budget of one millisecond.
    EXPECT_LT(elapsed, std::chrono::milliseconds{400});
    // Budget exhaustion is still the terminal outcome the operator sees.
    EXPECT_EQ(DropCount(sink, mt::DropReason::RetryBudgetExhausted), 1U);
}

TEST(OtlpExporterTest, Retry_SuccessOnFirstAttempt_NeverRetries)
{
    mtmk::MockOtlpEncoder encoder;
    mtmk::FakeWireCodec codec;
    codec.default_result = mti::WireResult{.success = true};

    mte::OtlpExporterConfig cfg;
    cfg.retry_policy = ZeroDelayRetry(5);
    mte::OtlpExporter exporter{&encoder, &codec, cfg};

    (void)exporter.Export(MakeBatch());
    ASSERT_EQ(exporter.ForceFlush(kFlushTimeout), mt::Status::Completed);
    EXPECT_EQ(codec.send_call_count.load(), 1);
}

// ---------------------------------------------------------------------------
// Diagnostics wiring: batches_sent / batches_failed / last_error_message.
//
// Before this, OtlpExporter stored its IDiagnosticsSink* as [[maybe_unused]]
// and never called it, so every counter behind Provider::GetExporterHealth()
// read zero no matter what the exporter did. Reads below happen after
// ForceFlush, which synchronises with the worker through m_mu/m_cv.
// ---------------------------------------------------------------------------

TEST(OtlpExporterTest, Diagnostics_SuccessfulExport_RecordsBatchSent)
{
    mtmk::MockOtlpEncoder encoder;
    mtmk::FakeWireCodec codec;
    mtmk::FakeDiagnosticsSink sink;
    codec.default_result = mti::WireResult{.success = true};

    mte::OtlpExporterConfig cfg;
    cfg.retry_policy = ZeroDelayRetry(3);
    mte::OtlpExporter exporter{&encoder, &codec, cfg, &sink};

    (void)exporter.Export(MakeBatch());
    ASSERT_EQ(exporter.ForceFlush(kFlushTimeout), mt::Status::Completed);

    EXPECT_EQ(sink.batches_sent, 1U);
    EXPECT_EQ(sink.batches_failed, 0U);
    // A clean delivery attributes nothing: no drop counter may move.
    EXPECT_EQ(TotalDrops(sink), 0U);
}

TEST(OtlpExporterTest, Diagnostics_NonRetryableFailure_RecordsFailureAndMessage)
{
    mtmk::MockOtlpEncoder encoder;
    mtmk::FakeWireCodec codec;
    mtmk::FakeDiagnosticsSink sink;
    codec.default_result = mti::WireResult{
        .success = false,
        .retryable = false,
        .error = mt::Error{.kind = mt::Error::Kind::Network, .message = "connection refused"},
    };

    mte::OtlpExporterConfig cfg;
    cfg.retry_policy = ZeroDelayRetry(3);
    mte::OtlpExporter exporter{&encoder, &codec, cfg, &sink};

    (void)exporter.Export(MakeBatch());
    ASSERT_EQ(exporter.ForceFlush(kFlushTimeout), mt::Status::Failed);

    EXPECT_EQ(sink.batches_failed, 1U);
    EXPECT_EQ(sink.batches_sent, 0U);
    // The message is what an operator actually reads out of GetExporterHealth().
    EXPECT_EQ(sink.last_error_message, "connection refused");
    EXPECT_TRUE(sink.last_error_time.has_value());
    // batches_failed says a batch was lost; the counter says why (issue #169).
    EXPECT_EQ(DropCount(sink, mt::DropReason::NonRetryableFailure), 1U);
    EXPECT_EQ(DropCount(sink, mt::DropReason::RetryBudgetExhausted), 0U);
}

TEST(OtlpExporterTest, Diagnostics_RetriesThenSucceeds_RecordsSentExactlyOnce)
{
    mtmk::MockOtlpEncoder encoder;
    mtmk::FakeWireCodec codec;
    mtmk::FakeDiagnosticsSink sink;
    codec.scripted_results.push_back(mti::WireResult{.success = false, .retryable = true});
    codec.scripted_results.push_back(mti::WireResult{.success = false, .retryable = true});
    codec.default_result = mti::WireResult{.success = true};

    mte::OtlpExporterConfig cfg;
    cfg.retry_policy = ZeroDelayRetry(5);
    mte::OtlpExporter exporter{&encoder, &codec, cfg, &sink};

    (void)exporter.Export(MakeBatch());
    ASSERT_EQ(exporter.ForceFlush(kFlushTimeout), mt::Status::Completed);

    // One batch, one outcome: intermediate retryable failures are attempts,
    // not failed batches. Counting each attempt would make batches_failed
    // meaningless as a batch counter.
    EXPECT_EQ(sink.batches_sent, 1U);
    EXPECT_EQ(sink.batches_failed, 0U);
    // The recovery is the visibility this counter exists for: nothing was
    // lost, but the export path is unhealthy. Once per recovered batch, not
    // once per retried attempt.
    EXPECT_EQ(DropCount(sink, mt::DropReason::RetryableFailureRecovered), 1U);
    EXPECT_EQ(DropCount(sink, mt::DropReason::RetryBudgetExhausted), 0U);
}

TEST(OtlpExporterTest, Diagnostics_ExhaustedRetries_RecordsFailedExactlyOnce)
{
    mtmk::MockOtlpEncoder encoder;
    mtmk::FakeWireCodec codec;
    mtmk::FakeDiagnosticsSink sink;
    codec.default_result = mti::WireResult{
        .success = false,
        .retryable = true,
        .error = mt::Error{.kind = mt::Error::Kind::Network, .message = "timed out"},
    };

    mte::OtlpExporterConfig cfg;
    cfg.retry_policy = ZeroDelayRetry(3);
    mte::OtlpExporter exporter{&encoder, &codec, cfg, &sink};

    (void)exporter.Export(MakeBatch());
    ASSERT_EQ(exporter.ForceFlush(kFlushTimeout), mt::Status::Failed);

    EXPECT_EQ(codec.send_call_count.load(), 3);  // all attempts made
    EXPECT_EQ(sink.batches_failed, 1U);          // but one failed batch
    EXPECT_EQ(sink.last_error_message, "timed out");
    // Running out of attempts and running out of budget are the same outcome
    // for an operator — the batch was retried and still lost. One increment
    // either way, on the exit path the loop actually took.
    EXPECT_EQ(DropCount(sink, mt::DropReason::RetryBudgetExhausted), 1U);
    EXPECT_EQ(DropCount(sink, mt::DropReason::NonRetryableFailure), 0U);
}

TEST(OtlpExporterTest, Diagnostics_BudgetExhaustedOnEntry_RecordsExhaustedExactlyOnce)
{
    mtmk::MockOtlpEncoder encoder;
    mtmk::FakeWireCodec codec;
    mtmk::FakeDiagnosticsSink sink;
    codec.default_result = mti::WireResult{.success = false, .retryable = true};
    mtmk::FakeSteadyClock clock;  // never advances: budget of 0ms is spent on entry

    mte::OtlpExporterConfig cfg;
    cfg.retry_policy = mte::RetryPolicyConfig{
        .max_attempts = 5,
        .initial_backoff = std::chrono::milliseconds{0},
        .max_backoff = std::chrono::milliseconds{0},
        .backoff_multiplier = 1.0,
        .jitter_fraction = 0.0,
        .retry_budget = std::chrono::milliseconds{0},
    };
    mte::OtlpExporter exporter{&encoder, &codec, cfg, &sink, &clock};

    (void)exporter.Export(MakeBatch());
    ASSERT_EQ(exporter.ForceFlush(kFlushTimeout), mt::Status::Failed);

    // The budget-on-entry early return takes a different code path from
    // attempt exhaustion, and must still land on exactly one counter.
    EXPECT_EQ(codec.send_call_count.load(), 1);
    EXPECT_EQ(DropCount(sink, mt::DropReason::RetryBudgetExhausted), 1U);
}

TEST(OtlpExporterTest, Diagnostics_PartialSuccessRejection_CountsRejectedRecords)
{
    mtmk::MockOtlpEncoder encoder;
    mtmk::FakeWireCodec codec;
    mtmk::FakeDiagnosticsSink sink;
    // The partial-success shape: the request succeeded, but the collector
    // rejected some of the records in it (error-model.md §6). Both codecs
    // populate this field; until now nothing read it.
    codec.default_result = mti::WireResult{.success = true, .partial_success_rejected = 3};

    mte::OtlpExporterConfig cfg;
    cfg.retry_policy = ZeroDelayRetry(3);
    mte::OtlpExporter exporter{&encoder, &codec, cfg, &sink};

    (void)exporter.Export(MakeBatch());
    ASSERT_EQ(exporter.ForceFlush(kFlushTimeout), mt::Status::Completed);

    // Still a sent batch — partial success is never retried and never a
    // failed batch — but the rejected records must be attributable.
    //
    // The send count is the direct assertion of "never retried": the retry
    // policy allows three attempts and the result carries rejections, so an
    // exporter that treated partial success as a failure would show 3 here.
    // Without it the test proved only that the counters were right, which a
    // retrying implementation could also manage.
    EXPECT_EQ(codec.send_call_count.load(), 1);
    EXPECT_EQ(sink.batches_sent, 1U);
    EXPECT_EQ(sink.batches_failed, 0U);
    EXPECT_EQ(DropCount(sink, mt::DropReason::PartialSuccessRejection), 3U);
}

TEST(OtlpExporterTest, Diagnostics_QueueFull_CountsEveryRecordInTheRejectedBatch)
{
    mtmk::MockOtlpEncoder encoder;
    mtmk::MockWireCodec codec;
    mtmk::FakeDiagnosticsSink sink;
    mte::OtlpExporterConfig cfg;
    cfg.max_queue_size = 0;  // every Export is rejected
    mte::OtlpExporter exporter{&encoder, &codec, cfg, &sink};

    EXPECT_EQ(exporter.Export(MakeBatchOf(4)), mti::ExportResult::Dropped);

    // The unit of loss is the span, not the batch: a rejected 4-span batch
    // loses 4 spans, and that is the number an operator needs.
    EXPECT_EQ(DropCount(sink, mt::DropReason::QueueFull), 4U);
    EXPECT_EQ(DropCount(sink, mt::DropReason::PostShutdown), 0U);
}

TEST(OtlpExporterTest, Diagnostics_ExportAfterShutdown_CountsPostShutdown)
{
    mtmk::MockOtlpEncoder encoder;
    mtmk::MockWireCodec codec;
    mtmk::FakeDiagnosticsSink sink;
    mte::OtlpExporter exporter{&encoder, &codec, {}, &sink};

    ASSERT_EQ(exporter.Shutdown(kFlushTimeout), mt::Status::Completed);
    EXPECT_EQ(exporter.Export(MakeBatchOf(2)), mti::ExportResult::AlreadyShutDown);

    EXPECT_EQ(DropCount(sink, mt::DropReason::PostShutdown), 2U);
    EXPECT_EQ(DropCount(sink, mt::DropReason::QueueFull), 0U);
}

TEST(OtlpExporterTest, Diagnostics_FailureWithNoErrorPayload_StillCountsAndNamesTheStage)
{
    mtmk::MockOtlpEncoder encoder;
    mtmk::FakeWireCodec codec;
    mtmk::FakeDiagnosticsSink sink;
    // A codec may report failure without populating `error`; the counter must
    // still move, and the message must not be empty for an operator reading it.
    codec.default_result = mti::WireResult{.success = false, .retryable = false};

    mte::OtlpExporterConfig cfg;
    cfg.retry_policy = ZeroDelayRetry(1);
    mte::OtlpExporter exporter{&encoder, &codec, cfg, &sink};

    (void)exporter.Export(MakeBatch());
    ASSERT_EQ(exporter.ForceFlush(kFlushTimeout), mt::Status::Failed);

    EXPECT_EQ(sink.batches_failed, 1U);
    EXPECT_FALSE(sink.last_error_message.empty());
    EXPECT_EQ(DropCount(sink, mt::DropReason::NonRetryableFailure), 1U);
}

// ---------------------------------------------------------------------------
// Drain-path exception accounting — issue #224.
//
// `FanOutAndProcess` runs with the queue lock released, inside a `noexcept`
// worker, so `DrainQueue` catches everything it can throw. The catch was empty
// under a comment claiming the diag hook had been added in M3-C; it had not,
// so a batch lost this way left `GetExporterHealth()` reporting a clean
// pipeline against error-model.md §5.1 ("recorded as a diagnostic, and the
// worker continues"). The batch is still lost — there is nowhere to put it —
// but the loss is countable.
// ---------------------------------------------------------------------------

namespace
{

/// An encoder that always throws — the only way to reach `DrainQueue`'s catch
/// from a test, every other collaborator on that path being `noexcept`.
class ThrowingOtlpEncoder final : public mti::IOtlpEncoder
{
public:
    [[nodiscard]] mti::EncodedPayload Encode(const mti::BatchHandle& /*batch*/) override
    {
        throw std::runtime_error("trace encode blew up");
    }
};

}  // namespace

TEST(OtlpExporterTest, Diagnostics_DrainThrows_RecordsBatchFailed)
{
    ThrowingOtlpEncoder encoder;
    mtmk::FakeWireCodec codec;
    mtmk::FakeDiagnosticsSink sink;
    mte::OtlpExporter exporter{&encoder, &codec, {}, &sink};

    EXPECT_EQ(exporter.Export(MakeBatch()), mti::ExportResult::Success);
    ASSERT_EQ(exporter.ForceFlush(kFlushTimeout), mt::Status::Failed);

    EXPECT_EQ(sink.batches_failed, 1U) << "a swallowed drain failure must still be countable";
    EXPECT_EQ(sink.batches_sent, 0U);
    EXPECT_FALSE(sink.last_error_message.empty())
        << "GetExporterHealth() must be able to say why the batch was lost";
    EXPECT_TRUE(sink.last_error_time.has_value());
}

TEST(OtlpExporterTest, Diagnostics_DrainThrows_WithoutSink_StillDrains)
{
    ThrowingOtlpEncoder encoder;
    mtmk::FakeWireCodec codec;
    mte::OtlpExporter exporter{&encoder, &codec};  // no sink

    (void)exporter.Export(MakeBatch());
    EXPECT_EQ(exporter.ForceFlush(kFlushTimeout), mt::Status::Failed);
}

TEST(OtlpExporterTest, Diagnostics_NullSink_IsNotDereferenced)
{
    mtmk::MockOtlpEncoder encoder;
    mtmk::FakeWireCodec codec;
    codec.default_result = mti::WireResult{.success = true};

    mte::OtlpExporterConfig cfg;
    cfg.retry_policy = ZeroDelayRetry(3);
    mte::OtlpExporter exporter{&encoder, &codec, cfg};  // no sink

    (void)exporter.Export(MakeBatch());
    EXPECT_EQ(exporter.ForceFlush(kFlushTimeout), mt::Status::Completed);
}

TEST(OtlpExporterTest, Diagnostics_QueueDepthPublishedOnEnqueueAndDrain)
{
    mtmk::MockOtlpEncoder encoder;
    mtmk::FakeWireCodec codec;
    mtmk::FakeDiagnosticsSink sink;
    codec.default_result = mti::WireResult{.success = true};

    mte::OtlpExporterConfig cfg;
    cfg.retry_policy = ZeroDelayRetry(3);
    mte::OtlpExporter exporter{&encoder, &codec, cfg, &sink};

    (void)exporter.Export(MakeBatch());
    ASSERT_EQ(exporter.ForceFlush(kFlushTimeout), mt::Status::Completed);

    // Depth must actually be published (not merely left at its zero default),
    // and must settle back to 0 after the drain rather than staying at its
    // high-water mark.
    EXPECT_GT(sink.queue_depth_call_count, 0U);
    EXPECT_EQ(sink.queue_depth_now, 0U);
}

// ---------------------------------------------------------------------------
// Shutdown during a retry backoff. docs/sequences/shutdown-drain.md: "backoff
// sleep wakes; worker exits the retry loop". The sleep was a plain
// `sleep_for`, so Shutdown's join waited out every remaining backoff — far
// past its own timeout, and the destructor's (CLAUDE.md rule 15).
// ---------------------------------------------------------------------------

namespace
{

// Far longer than kShutdownReturnBound, so an uninterruptible sleep shows.
constexpr auto kLongBackoff = std::chrono::seconds(10);
constexpr auto kShutdownTimeout = std::chrono::milliseconds(500);
// Generous for sanitizer builds, and still a fraction of kLongBackoff.
constexpr auto kShutdownReturnBound = std::chrono::seconds(3);
constexpr auto kWaitForSendBound = std::chrono::seconds(5);
constexpr auto kPollInterval = std::chrono::milliseconds(1);

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

TEST(OtlpExporterTest, Retry_ShutdownDuringBackoff_ReturnsWithinTimeout)
{
    mtmk::MockOtlpEncoder encoder;
    mtmk::FakeWireCodec codec;
    mtmk::FakeDiagnosticsSink sink;
    codec.default_result = mti::WireResult{.success = false, .retryable = true};

    mte::OtlpExporterConfig cfg;
    cfg.retry_policy = mte::RetryPolicyConfig{
        .max_attempts = 3,
        .initial_backoff = kLongBackoff,
        .max_backoff = kLongBackoff,
        .backoff_multiplier = 1.0,
        .jitter_fraction = 0.0,
        .retry_budget = std::chrono::minutes(5),
    };
    mte::OtlpExporter exporter{&encoder, &codec, cfg, &sink};

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
TEST(OtlpExporterTest, Retry_FirstRetryWaitsForFanOutRetryAfter)
{
    constexpr auto kFanOutRetryAfter = std::chrono::milliseconds{60};
    mtmk::MockOtlpEncoder encoder;
    mtmk::FakeWireCodec codec;
    mtmk::FakeDiagnosticsSink sink;
    codec.scripted_results.push_back(
        mti::WireResult{.success = false, .retryable = true, .retry_after = kFanOutRetryAfter});
    codec.default_result = mti::WireResult{.success = true};
    mte::OtlpExporterConfig cfg;
    cfg.retry_policy = ZeroDelayRetry(5);
    mte::OtlpExporter exporter{&encoder, &codec, cfg, &sink};

    const auto started = std::chrono::steady_clock::now();
    (void)exporter.Export(MakeBatch());
    ASSERT_EQ(exporter.ForceFlush(std::chrono::seconds{5}), mt::Status::Completed);
    const auto elapsed = std::chrono::steady_clock::now() - started;

    EXPECT_EQ(codec.send_call_count.load(), 2);
    EXPECT_GE(elapsed, kFanOutRetryAfter) << "the fan-out's retry_after is slept before attempt 1";
    EXPECT_EQ(DropCount(sink, mt::DropReason::RetryableFailureRecovered), 1U);
}

// ---------------------------------------------------------------------------
// Multi-Resource requests (docs/leaf-concentrator-design.md §3.6.1): the
// batches the worker drains together go out as one request, up to
// max_spans_per_request spans, by concatenating their encodings.
// ---------------------------------------------------------------------------

namespace
{

/// A FakeWireCodec whose first Send waits until the test releases it, so the
/// test can queue several batches behind a busy worker and know they will be
/// drained together.
class GatedCodec : public mtmk::FakeWireCodec
{
public:
    [[nodiscard]] mti::WireResult Send(mti::EncodedPayload&& payload,
                                       std::chrono::milliseconds deadline) override
    {
        {
            std::unique_lock lock{m_gate_mu};
            m_entered = true;
            m_gate_cv.notify_all();
            m_gate_cv.wait(lock, [this] { return m_released; });
        }
        return FakeWireCodec::Send(std::move(payload), deadline);
    }

    /// Block until the worker is inside the first Send.
    void WaitUntilEntered()
    {
        std::unique_lock lock{m_gate_mu};
        m_gate_cv.wait(lock, [this] { return m_entered; });
    }

    void Release()
    {
        const std::scoped_lock lock{m_gate_mu};
        m_released = true;
        m_gate_cv.notify_all();
    }

private:
    std::mutex m_gate_mu;
    std::condition_variable m_gate_cv;
    bool m_entered = false;
    bool m_released = false;
};

/// What the mock encoder returns in the coalescing tests.
std::vector<std::byte> Encoded()
{
    return {std::byte{0x0a}, std::byte{0x02}, std::byte{0x08}, std::byte{0x01}};
}

std::vector<std::byte> Repeated(const std::vector<std::byte>& part, std::size_t n)
{
    std::vector<std::byte> out;
    for (std::size_t i = 0; i < n; ++i)
    {
        out.insert(out.end(), part.begin(), part.end());
    }
    return out;
}

/// Occupies the worker with one gated batch, queues @p batches behind it, and
/// releases the gate. The gated batch is always sent alone and succeeds.
/// @p flushed is the flush's status: `Failed` when the batches behind the
/// gate are scripted to fail (ICP 0041).
void SendBehindTheGate(mte::OtlpExporter& exporter,
                       GatedCodec& codec,
                       std::vector<mti::BatchHandle> batches,
                       mt::Status flushed = mt::Status::Completed)
{
    codec.scripted_results.push_front(mti::WireResult{.success = true});
    ASSERT_EQ(exporter.Export(MakeBatch()), mti::ExportResult::Success);
    codec.WaitUntilEntered();
    for (auto& batch : batches)
    {
        ASSERT_EQ(exporter.Export(std::move(batch)), mti::ExportResult::Success);
    }
    codec.Release();
    ASSERT_EQ(exporter.ForceFlush(std::chrono::seconds(5)), flushed);
}

std::vector<mti::BatchHandle> Batches(std::initializer_list<std::size_t> sizes)
{
    std::vector<mti::BatchHandle> out;
    for (const auto n : sizes)
    {
        out.push_back(MakeBatchOf(n));
    }
    return out;
}

}  // namespace

TEST(OtlpExporterTest, Coalescing_BatchesDrainedTogether_GoOutAsOneConcatenatedRequest)
{
    mtmk::MockOtlpEncoder encoder;
    encoder.bytes_to_return = Encoded();
    GatedCodec codec;
    codec.default_result = mti::WireResult{.success = true};
    mtmk::FakeDiagnosticsSink sink;
    mte::OtlpExporter exporter{&encoder, &codec, mte::OtlpExporterConfig{}, &sink};

    SendBehindTheGate(exporter, codec, Batches({1, 1, 1}));

    const auto sent = codec.SentPayloads();
    ASSERT_EQ(sent.size(), 2U) << "the gated batch, then one request for the three behind it";
    EXPECT_EQ(sent[1], Repeated(Encoded(), 3))
        << "the request is the three encodings concatenated, in queue order";
    EXPECT_EQ(encoder.encode_call_count.load(), 4);
    EXPECT_EQ(sink.batches_sent, 4U) << "batches_sent still counts BatchHandles";
    EXPECT_EQ(TotalDrops(sink), 0U);
}

TEST(OtlpExporterTest, Coalescing_SplitsAtMaxSpansPerRequest)
{
    mtmk::MockOtlpEncoder encoder;
    encoder.bytes_to_return = Encoded();
    GatedCodec codec;
    codec.default_result = mti::WireResult{.success = true};
    mte::OtlpExporterConfig cfg;
    cfg.max_spans_per_request = 4;
    mte::OtlpExporter exporter{&encoder, &codec, cfg};

    // 2 + 2 fills a request exactly; the third batch starts the next one.
    SendBehindTheGate(exporter, codec, Batches({2, 2, 2}));

    const auto sent = codec.SentPayloads();
    ASSERT_EQ(sent.size(), 3U);
    EXPECT_EQ(sent[1], Repeated(Encoded(), 2));
    EXPECT_EQ(sent[2], Encoded());
}

TEST(OtlpExporterTest, Coalescing_ABatchOverTheLimitGoesAloneAndWhole)
{
    mtmk::MockOtlpEncoder encoder;
    encoder.bytes_to_return = Encoded();
    GatedCodec codec;
    codec.default_result = mti::WireResult{.success = true};
    mte::OtlpExporterConfig cfg;
    cfg.max_spans_per_request = 4;
    mte::OtlpExporter exporter{&encoder, &codec, cfg};

    SendBehindTheGate(exporter, codec, Batches({1, 5, 1}));

    const auto sent = codec.SentPayloads();
    ASSERT_EQ(sent.size(), 4U) << "[1] [5] [1]: a batch is never split";
    EXPECT_EQ(sent[1], Encoded());
    EXPECT_EQ(sent[2], Encoded());
    EXPECT_EQ(sent[3], Encoded());
}

TEST(OtlpExporterTest, Coalescing_TerminalFailureIsOneOutcomeCountedPerBatch)
{
    mtmk::MockOtlpEncoder encoder;
    GatedCodec codec;
    codec.default_result = mti::WireResult{
        .success = false,
        .retryable = false,
        .error = mt::Error{.kind = mt::Error::Kind::Protocol, .message = "HTTP 400"},
    };
    mtmk::FakeDiagnosticsSink sink;
    mte::OtlpExporterConfig cfg;
    cfg.retry_policy = ZeroDelayRetry(3);
    mte::OtlpExporter exporter{&encoder, &codec, cfg, &sink};

    SendBehindTheGate(exporter, codec, Batches({1, 1, 1}), mt::Status::Failed);

    EXPECT_EQ(codec.send_call_count.load(), 2) << "one request, never retried";
    EXPECT_EQ(sink.batches_sent, 1U) << "the gated batch";
    EXPECT_EQ(sink.batches_failed, 3U);
    EXPECT_EQ(DropCount(sink, mt::DropReason::NonRetryableFailure), 3U)
        << "the request's one outcome is counted in batches, as it was before coalescing";
    EXPECT_EQ(sink.last_error_message, "HTTP 400");
}

TEST(OtlpExporterTest, Coalescing_RetryResendsTheWholeRequest)
{
    mtmk::MockOtlpEncoder encoder;
    encoder.bytes_to_return = Encoded();
    GatedCodec codec;
    codec.scripted_results.push_back(mti::WireResult{.success = false, .retryable = true});
    codec.default_result = mti::WireResult{.success = true};
    mtmk::FakeDiagnosticsSink sink;
    mte::OtlpExporterConfig cfg;
    cfg.retry_policy = ZeroDelayRetry(3);
    mte::OtlpExporter exporter{&encoder, &codec, cfg, &sink};

    SendBehindTheGate(exporter, codec, Batches({1, 1}));

    const auto sent = codec.SentPayloads();
    ASSERT_EQ(sent.size(), 3U) << "gated, the request, its one retry";
    EXPECT_EQ(sent[2], Repeated(Encoded(), 2)) << "the retry re-encodes every batch in the request";
    EXPECT_EQ(encoder.encode_call_count.load(), 5);
    EXPECT_EQ(sink.batches_sent, 3U);
    EXPECT_EQ(sink.batches_failed, 0U);
    EXPECT_EQ(DropCount(sink, mt::DropReason::RetryableFailureRecovered), 2U);
}

TEST(OtlpExporterTest, Coalescing_PartialSuccessRejectedCountIsRecordedOncePerRequest)
{
    mtmk::MockOtlpEncoder encoder;
    GatedCodec codec;
    codec.scripted_results.push_back(
        mti::WireResult{.success = true, .partial_success_rejected = 5});
    codec.default_result = mti::WireResult{.success = true};
    mtmk::FakeDiagnosticsSink sink;
    mte::OtlpExporter exporter{&encoder, &codec, mte::OtlpExporterConfig{}, &sink};

    SendBehindTheGate(exporter, codec, Batches({2, 2, 2}));

    EXPECT_EQ(codec.send_call_count.load(), 2);
    EXPECT_EQ(DropCount(sink, mt::DropReason::PartialSuccessRejection), 5U)
        << "the collector's count is for the request, not for each batch in it";
    EXPECT_EQ(sink.batches_sent, 4U);
}

TEST(OtlpExporterTest, ExportGroup_QueuesTheWholeGroupAtOnce_SoItLeavesAsOneRequest)
{
    mtmk::MockOtlpEncoder encoder;
    encoder.bytes_to_return = Encoded();
    mtmk::FakeWireCodec codec;
    codec.default_result = mti::WireResult{.success = true};
    mtmk::FakeDiagnosticsSink sink;
    mte::OtlpExporter exporter{&encoder, &codec, mte::OtlpExporterConfig{}, &sink};

    // No gate: the one lock ExportGroup takes is what keeps the worker from
    // draining the first batch alone.
    exporter.ExportGroup(Batches({1, 1, 1}));
    ASSERT_EQ(exporter.ForceFlush(std::chrono::seconds(5)), mt::Status::Completed);

    const auto sent = codec.SentPayloads();
    ASSERT_EQ(sent.size(), 1U);
    EXPECT_EQ(sent[0], Repeated(Encoded(), 3));
    EXPECT_EQ(sink.batches_sent, 3U);
}

TEST(OtlpExporterTest, ExportGroup_RefusesWhatDoesNotFitAndCountsItAsExportWould)
{
    mtmk::MockOtlpEncoder encoder;
    GatedCodec codec;
    codec.default_result = mti::WireResult{.success = true};
    mtmk::FakeDiagnosticsSink sink;
    mte::OtlpExporterConfig cfg;
    cfg.max_queue_size = 2;
    mte::OtlpExporter exporter{&encoder, &codec, cfg, &sink};

    // Hold the worker so the queue cannot drain while the group arrives.
    codec.scripted_results.push_front(mti::WireResult{.success = true});
    ASSERT_EQ(exporter.Export(MakeBatch()), mti::ExportResult::Success);
    codec.WaitUntilEntered();
    exporter.ExportGroup(Batches({1, 2, 3}));
    EXPECT_EQ(DropCount(sink, mt::DropReason::QueueFull), 3U) << "the third batch, in spans";
    codec.Release();
    ASSERT_EQ(exporter.ForceFlush(std::chrono::seconds(5)), mt::Status::Completed);
    ASSERT_EQ(exporter.Shutdown(std::chrono::seconds(5)), mt::Status::Completed);

    exporter.ExportGroup(Batches({4}));
    EXPECT_EQ(DropCount(sink, mt::DropReason::PostShutdown), 4U);
}

// ---------------------------------------------------------------------------
// Queue bound in spans (issue #345). A processor drain from N Resources is N
// batches, so a bound in batches alone let a many-leaf concentrator queue a
// fraction of the spans a one-leaf one could. `max_queued_spans` bounds what
// the batches carry; its default holds as many spans as the batch bound did
// when every batch was a full one.
// ---------------------------------------------------------------------------

TEST(OtlpExporterTest, QueueBound_DefaultHoldsManySmallBatchesThatAFewFullOnesWould)
{
    mtmk::MockOtlpEncoder encoder;
    GatedCodec codec;
    codec.default_result = mti::WireResult{.success = true};
    mtmk::FakeDiagnosticsSink sink;
    const mte::OtlpExporterConfig cfg;
    mte::OtlpExporter exporter{&encoder, &codec, cfg, &sink};

    codec.scripted_results.push_front(mti::WireResult{.success = true});
    ASSERT_EQ(exporter.Export(MakeBatch()), mti::ExportResult::Success);
    codec.WaitUntilEntered();
    // Four drains of 512 one-span batches, as four drains from 512 leaves
    // would be: 2,048 spans, far inside the span budget, but eight times the
    // 256 batches the queue used to be limited to.
    constexpr std::size_t kDrains = 4;
    for (std::size_t d = 0; d < kDrains; ++d)
    {
        std::vector<mti::BatchHandle> drain;
        drain.reserve(cfg.max_spans_per_request);
        for (std::size_t i = 0; i < cfg.max_spans_per_request; ++i)
        {
            drain.push_back(MakeBatch());
        }
        exporter.ExportGroup(std::move(drain));
    }
    EXPECT_EQ(DropCount(sink, mt::DropReason::QueueFull), 0U);
    codec.Release();
    ASSERT_EQ(exporter.ForceFlush(std::chrono::seconds(5)), mt::Status::Completed);
    EXPECT_EQ(sink.batches_sent, 1U + (kDrains * cfg.max_spans_per_request));
}

TEST(OtlpExporterTest, QueueBound_RefusesTheBatchThatWouldPassTheSpanBudget)
{
    mtmk::MockOtlpEncoder encoder;
    GatedCodec codec;
    codec.default_result = mti::WireResult{.success = true};
    mtmk::FakeDiagnosticsSink sink;
    mte::OtlpExporterConfig cfg;
    cfg.max_queued_spans = 5;
    mte::OtlpExporter exporter{&encoder, &codec, cfg, &sink};

    codec.scripted_results.push_front(mti::WireResult{.success = true});
    ASSERT_EQ(exporter.Export(MakeBatch()), mti::ExportResult::Success);
    codec.WaitUntilEntered();
    ASSERT_EQ(exporter.Export(MakeBatchOf(3)), mti::ExportResult::Success);
    EXPECT_EQ(exporter.Export(MakeBatchOf(3)), mti::ExportResult::Dropped) << "3 + 3 > 5";
    EXPECT_EQ(DropCount(sink, mt::DropReason::QueueFull), 3U);
    EXPECT_EQ(exporter.Export(MakeBatchOf(2)), mti::ExportResult::Success)
        << "the budget is a ceiling the queue may reach";
    exporter.ExportGroup(Batches({1, 1}));
    EXPECT_EQ(DropCount(sink, mt::DropReason::QueueFull), 5U)
        << "a group is refused batch by batch, as Export would refuse each";

    codec.Release();
    ASSERT_EQ(exporter.ForceFlush(std::chrono::seconds(5)), mt::Status::Completed);
    EXPECT_EQ(exporter.Export(MakeBatchOf(5)), mti::ExportResult::Success)
        << "the drain gave the spans it took back to the budget";
    ASSERT_EQ(exporter.ForceFlush(std::chrono::seconds(5)), mt::Status::Completed);
    EXPECT_EQ(DropCount(sink, mt::DropReason::QueueFull), 5U);
}

TEST(OtlpExporterTest, QueueBound_SpanBudgetScalesWithTheProcessorBatch)
{
    // SdkBuilder sizes the trace exporter's queue from the batch size it gives
    // the processor, so a larger batch keeps the same number of full drains.
    const mte::OtlpExporterConfig defaults;
    EXPECT_EQ(mte::QueuedSpanBudget(defaults.max_spans_per_request), defaults.max_queued_spans);
    EXPECT_EQ(mte::QueuedSpanBudget(4 * defaults.max_spans_per_request),
              4 * defaults.max_queued_spans);
}

// ---------------------------------------------------------------------------
// max_request_bytes (ICP 0036): joining stops at a byte cap too
// ---------------------------------------------------------------------------

namespace
{

/// Encodes a batch as `kBytesPerSpan` bytes per span, all equal to the span
/// count, so request sizes and their parts can be told apart.
class SizedByBatchEncoder final : public mti::IOtlpEncoder
{
public:
    static constexpr std::size_t kBytesPerSpan = 10;

    [[nodiscard]] mti::EncodedPayload Encode(const mti::BatchHandle& batch) override
    {
        const std::size_t spans = batch.Spans().size();
        const std::size_t n = spans * kBytesPerSpan;
        auto buf = std::make_unique<std::byte[]>(n);
        for (std::size_t i = 0; i < n; ++i)
        {
            buf[i] = static_cast<std::byte>(spans);
        }
        return mti::EncodedPayload{std::move(buf), n};
    }
};

std::vector<std::size_t> Sizes(const std::vector<std::vector<std::byte>>& sent)
{
    std::vector<std::size_t> out;
    out.reserve(sent.size());
    for (const auto& p : sent)
    {
        out.push_back(p.size());
    }
    return out;
}

}  // namespace

TEST(OtlpExporterTest, RequestBytesCap_StopsAJoinThatWouldCrossIt)
{
    SizedByBatchEncoder encoder;
    GatedCodec codec;
    codec.default_result = mti::WireResult{.success = true};
    mtmk::FakeDiagnosticsSink sink;
    mte::OtlpExporterConfig cfg;
    cfg.max_request_bytes = 25;
    mte::OtlpExporter exporter{&encoder, &codec, cfg, &sink};

    // 10 + 10 fits; a third would make 30. The handle that would cross the
    // cap starts the next request.
    SendBehindTheGate(exporter, codec, Batches({1, 1, 1, 1, 1}));

    EXPECT_EQ(Sizes(codec.SentPayloads()), (std::vector<std::size_t>{10, 20, 20, 10}));
    EXPECT_EQ(sink.batches_sent, 6U) << "still counted per BatchHandle";
}

TEST(OtlpExporterTest, RequestBytesCap_AtExactlyTheCapStillJoins)
{
    SizedByBatchEncoder encoder;
    GatedCodec codec;
    codec.default_result = mti::WireResult{.success = true};
    mte::OtlpExporterConfig cfg;
    cfg.max_request_bytes = 20;
    mte::OtlpExporter exporter{&encoder, &codec, cfg};

    SendBehindTheGate(exporter, codec, Batches({1, 1, 1}));

    EXPECT_EQ(Sizes(codec.SentPayloads()), (std::vector<std::size_t>{10, 20, 10}));
}

TEST(OtlpExporterTest, RequestBytesCap_ZeroMeansJoinBySpanCountOnly)
{
    SizedByBatchEncoder encoder;
    GatedCodec codec;
    codec.default_result = mti::WireResult{.success = true};
    mte::OtlpExporterConfig cfg;
    cfg.max_request_bytes = 0;
    mte::OtlpExporter exporter{&encoder, &codec, cfg};

    SendBehindTheGate(exporter, codec, Batches({1, 1, 1, 1, 1}));

    EXPECT_EQ(Sizes(codec.SentPayloads()), (std::vector<std::size_t>{10, 50}));
}

TEST(OtlpExporterTest, RequestBytesCap_ABatchOverTheCapGoesAloneAndWhole)
{
    SizedByBatchEncoder encoder;
    GatedCodec codec;
    codec.default_result = mti::WireResult{.success = true};
    mte::OtlpExporterConfig cfg;
    cfg.max_request_bytes = 25;
    mte::OtlpExporter exporter{&encoder, &codec, cfg};

    SendBehindTheGate(exporter, codec, Batches({1, 5, 1}));

    const auto sent = codec.SentPayloads();
    EXPECT_EQ(Sizes(sent), (std::vector<std::size_t>{10, 10, 50, 10}));
    EXPECT_EQ(sent[2], std::vector<std::byte>(50, std::byte{5})) << "unchanged, not split";
}

TEST(OtlpExporterTest, RequestBytesCap_SpanCountStillSplitsBelowTheByteCap)
{
    SizedByBatchEncoder encoder;
    GatedCodec codec;
    codec.default_result = mti::WireResult{.success = true};
    mte::OtlpExporterConfig cfg;
    cfg.max_request_bytes = 1000;
    cfg.max_spans_per_request = 2;
    mte::OtlpExporter exporter{&encoder, &codec, cfg};

    SendBehindTheGate(exporter, codec, Batches({1, 1, 1}));

    EXPECT_EQ(Sizes(codec.SentPayloads()), (std::vector<std::size_t>{10, 20, 10}));
}

TEST(OtlpExporterTest, RequestBytesCap_ARetryReencodesTheSameGroupByteForByte)
{
    SizedByBatchEncoder encoder;
    GatedCodec codec;
    // SendBehindTheGate scripts the gated batch's success; then [1,2]
    // succeeds and [3] fails once.
    codec.scripted_results.push_back(mti::WireResult{.success = true});
    codec.scripted_results.push_back(mti::WireResult{.success = false, .retryable = true});
    codec.default_result = mti::WireResult{.success = true};
    mte::OtlpExporterConfig cfg;
    cfg.max_request_bytes = 30;
    cfg.retry_policy = ZeroDelayRetry(3);
    mte::OtlpExporter exporter{&encoder, &codec, cfg};

    SendBehindTheGate(exporter, codec, Batches({1, 2, 3}));

    const auto sent = codec.SentPayloads();
    ASSERT_EQ(sent.size(), 4U) << "gated, [1,2], [3], the retry of [3]";
    EXPECT_EQ(sent[2].size(), 30U);
    EXPECT_EQ(sent[3], sent[2]) << "the retry re-encodes the same group, not a regrouping";
}

// ---------------------------------------------------------------------------
// on_shutdown_timeout (ICP 0036 Decision 2)
// ---------------------------------------------------------------------------

TEST(OtlpExporterTest, ShutdownTimeout_CallsTheHookBeforeJoiningTheWorker)
{
    mtmk::MockOtlpEncoder encoder;
    GatedCodec codec;
    codec.default_result = mti::WireResult{.success = true};
    int hook_calls = 0;
    mte::OtlpExporterConfig cfg;
    // The hook is what wakes the Send the worker is stuck in; without it the
    // join below would never return.
    cfg.on_shutdown_timeout = [&]
    {
        ++hook_calls;
        codec.Release();
    };
    mte::OtlpExporter exporter{&encoder, &codec, cfg};

    ASSERT_EQ(exporter.Export(MakeBatch()), mti::ExportResult::Success);
    codec.WaitUntilEntered();

    EXPECT_EQ(exporter.Shutdown(std::chrono::milliseconds(50)), mt::Status::TimedOut);
    EXPECT_EQ(hook_calls, 1);
}

TEST(OtlpExporterTest, ShutdownCompleted_DoesNotCallTheHook)
{
    mtmk::MockOtlpEncoder encoder;
    mtmk::MockWireCodec codec;
    codec.result_to_return.success = true;
    int hook_calls = 0;
    mte::OtlpExporterConfig cfg;
    cfg.on_shutdown_timeout = [&] { ++hook_calls; };
    mte::OtlpExporter exporter{&encoder, &codec, cfg};

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
TEST(OtlpExporterTest, ForceFlush_UnderSustainedLoad_CompletesForWhatWasQueuedBeforeIt)
{
    mtmk::MockOtlpEncoder encoder;
    mtmk::FakeWireCodec codec;
    codec.default_result = mti::WireResult{.success = true};
    codec.send_delay = kSustainedSendDelay;
    mte::OtlpExporterConfig cfg;
    cfg.max_spans_per_request = 1;  // one send per batch
    mte::OtlpExporter exporter{&encoder, &codec, cfg};

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

TEST(OtlpExporterTest, ForceFlush_ABatchThatFailedAfterItsFinalAttempt_ReturnsFailed)
{
    mtmk::MockOtlpEncoder encoder;
    mtmk::FakeWireCodec codec;
    codec.default_result = NonRetryableFailure();
    mte::OtlpExporter exporter{&encoder, &codec};

    (void)exporter.Export(MakeBatch());
    EXPECT_EQ(exporter.ForceFlush(kFlushTimeout), mt::Status::Failed);
}

TEST(OtlpExporterTest, ForceFlush_AFailureBeforeTheCall_DoesNotFailALaterFlush)
{
    mtmk::MockOtlpEncoder encoder;
    mtmk::FakeWireCodec codec;
    codec.scripted_results.push_back(NonRetryableFailure());
    codec.default_result = mti::WireResult{.success = true};
    mte::OtlpExporter exporter{&encoder, &codec};

    (void)exporter.Export(MakeBatch());
    ASSERT_EQ(exporter.ForceFlush(kFlushTimeout), mt::Status::Failed);
    (void)exporter.Export(MakeBatch());
    EXPECT_EQ(exporter.ForceFlush(kFlushTimeout), mt::Status::Completed);
}

// A batch in retry backoff is still pending: the flush waits for its retry.
TEST(OtlpExporterTest, ForceFlush_ABatchInRetryBackoff_IsStillPending)
{
    mtmk::MockOtlpEncoder encoder;
    mtmk::FakeWireCodec codec;
    codec.scripted_results.push_back(mti::WireResult{.success = false, .retryable = true});
    codec.default_result = mti::WireResult{.success = true};
    mte::OtlpExporterConfig cfg;
    cfg.retry_policy.initial_backoff = kRetryBackoff;
    cfg.retry_policy.jitter_fraction = 0.0;
    mte::OtlpExporter exporter{&encoder, &codec, cfg};

    (void)exporter.Export(MakeBatch());
    EXPECT_EQ(exporter.ForceFlush(kSustainedFlushTimeout), mt::Status::Completed);
    EXPECT_EQ(codec.send_call_count.load(), 2);
}

TEST(OtlpExporterTest, Shutdown_ABatchThatFailed_ReturnsFailed)
{
    mtmk::MockOtlpEncoder encoder;
    mtmk::FakeWireCodec codec;
    codec.default_result = NonRetryableFailure();
    mte::OtlpExporter exporter{&encoder, &codec};

    (void)exporter.Export(MakeBatch());
    EXPECT_EQ(exporter.Shutdown(kFlushTimeout), mt::Status::Failed);
}

namespace
{

/// Time for the worker to finish the failed round after its Send returned.
constexpr auto kRoundSettles = std::chrono::milliseconds{50};
/// Time for both flush threads to start waiting.
constexpr auto kFlushesRegister = std::chrono::milliseconds{200};

/// Answers the first Send at once from its script; holds the second until
/// `Release`, so that flushes can be made to overlap.
class SecondSendGatedCodec : public mtmk::FakeWireCodec
{
public:
    [[nodiscard]] mti::WireResult Send(mti::EncodedPayload&& payload,
                                       std::chrono::milliseconds deadline) override
    {
        CountAndHoldTheSecond();
        return mtmk::FakeWireCodec::Send(std::move(payload), deadline);
    }

    [[nodiscard]] bool WaitForSends(int n)
    {
        std::unique_lock lock{m_gate_mu};
        return m_gate_cv.wait_for(lock, kSustainedFlushTimeout, [this, n] { return m_sends >= n; });
    }

    void Release()
    {
        const std::scoped_lock lock{m_gate_mu};
        m_released = true;
        m_gate_cv.notify_all();
    }

private:
    void CountAndHoldTheSecond()
    {
        std::unique_lock lock{m_gate_mu};
        ++m_sends;
        m_gate_cv.notify_all();
        if (m_sends == 2)
        {
            m_gate_cv.wait(lock, [this] { return m_released; });
        }
    }

    std::mutex m_gate_mu;
    std::condition_variable m_gate_cv;
    int m_sends = 0;
    bool m_released = false;
};

}  // namespace

// ICP 0041 amendment: a failure is reported to every flush called after it
// until a flush covering it completes. Overlapping flushes both report it.
TEST(OtlpExporterTest, ForceFlush_OverlappingFlushesAfterAFailure_BothFail_ThenTheNextCompletes)
{
    mtmk::MockOtlpEncoder encoder;
    SecondSendGatedCodec codec;
    codec.scripted_results.push_back(NonRetryableFailure());
    codec.default_result = mti::WireResult{.success = true};
    mte::OtlpExporter exporter{&encoder, &codec};

    // The first batch fails in a round of its own, finished before either
    // flush is called: no flush has covered it yet, so both must report it.
    (void)exporter.Export(MakeBatch());
    ASSERT_TRUE(codec.WaitForSends(1));
    std::this_thread::sleep_for(kRoundSettles);
    (void)exporter.Export(MakeBatch());
    if (!codec.WaitForSends(2))  // the second is held
    {
        codec.Release();
        FAIL() << "the second send never started";
    }

    mt::Status first = mt::Status::Completed;
    mt::Status second = mt::Status::Completed;
    std::thread a{[&] { first = exporter.ForceFlush(kSustainedFlushTimeout); }};
    std::thread b{[&] { second = exporter.ForceFlush(kSustainedFlushTimeout); }};
    // Both flushes must be waiting before the held send ends, or the first
    // completes before the second starts and they never overlap. Neither can
    // return while the send is held. FlushTrackerTest covers the rule itself
    // without threads.
    std::this_thread::sleep_for(kFlushesRegister);
    codec.Release();
    a.join();
    b.join();

    EXPECT_EQ(first, mt::Status::Failed);
    EXPECT_EQ(second, mt::Status::Failed);
    EXPECT_EQ(exporter.ForceFlush(kFlushTimeout), mt::Status::Completed);
}
