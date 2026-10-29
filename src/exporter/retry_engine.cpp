// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#include "exporter/retry_engine.hpp"

#include "microtel/error.hpp"
#include "microtel/provider.hpp"

#include "exporter/retry_policy.hpp"

#include <chrono>
#include <cstdint>
#include <mutex>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <utility>

namespace microtel::exporter
{

RetryEngine::RetryEngine(const RetryPolicyConfig& policy,
                         internal::IDiagnosticsSink* diag,
                         internal::ISteadyClock* clock) noexcept
    : m_policy(policy), m_diag(diag), m_clock(clock)
{
}

bool RetryEngine::Settle(const internal::WireResult& first_attempt,
                         const RetryAttempt& retry,
                         std::string_view failure_stage,
                         std::uint64_t batches)
{
    const internal::WireResult outcome = Resolve(first_attempt, retry, batches);
    RecordOutcome(outcome, failure_stage, batches);
    return outcome.success;
}

void RetryEngine::Abort() noexcept
{
    {
        const std::scoped_lock lock{m_mu};
        m_aborted = true;
    }
    m_cv.notify_all();
}

internal::WireResult RetryEngine::Resolve(const internal::WireResult& first_attempt,
                                          const RetryAttempt& retry,
                                          std::uint64_t batches)
{
    if (first_attempt.success || !first_attempt.retryable)
    {
        return first_attempt;
    }
    auto retried = RunRetryLoop(first_attempt, retry);
    if (!retried.has_value())
    {
        // No retry was made — the first backoff would have reached the retry
        // budget, or Shutdown cut it short — so the fan-out result stands as
        // this batch's outcome.
        return first_attempt;
    }
    if (retried->success && m_diag != nullptr)
    {
        // Not a loss — the batch was delivered — but the export path is
        // unhealthy, and this counter is the only place that shows it.
        m_diag->RecordDrop(DropReason::RetryableFailureRecovered, batches);
    }
    return std::move(*retried);
}

std::optional<internal::WireResult> RetryEngine::RunRetryLoop(
    const internal::WireResult& first_attempt, const RetryAttempt& retry)
{
    const std::uint32_t max_attempts = (m_policy.max_attempts > 0U) ? m_policy.max_attempts : 1U;
    const auto budget_deadline = ClockNow() + m_policy.retry_budget;

    // The fan-out already made attempt 0. Every retry, the first included,
    // backs off on the result before it (issue #311): the fan-out's
    // `retry_after` and `initial_backoff` govern the wait before attempt 1.
    std::optional<internal::WireResult> last;
    for (std::uint32_t attempt = 1U; attempt < max_attempts; ++attempt)
    {
        const internal::WireResult& previous = last.has_value() ? *last : first_attempt;
        if (!BackOffBeforeRetry(previous, attempt, budget_deadline))
        {
            break;
        }
        last = retry();
    }
    return last;
}

bool RetryEngine::BackOffBeforeRetry(const internal::WireResult& last,
                                     std::uint32_t attempt,
                                     internal::TimePointSteady budget_deadline)
{
    if (last.success || !last.retryable)
    {
        return false;
    }
    // `ComputeBackoff` counts from 0 = the wait before the second attempt,
    // so the first retry (attempt 1) waits `initial_backoff`.
    const auto backoff = ComputeBackoff(attempt - 1U, m_policy, last.retry_after, DrawJitter01());
    // Look-ahead, per `docs/sequences/retry-after-failure.md` §4: exit when
    // the *upcoming* sleep would reach or pass the budget, not once the
    // budget is already spent (issue #195). `backoff` is never negative.
    if (ClockNow() + backoff >= budget_deadline)
    {
        return false;
    }
    return SleepUnlessAborted(backoff);
}

bool RetryEngine::SleepUnlessAborted(std::chrono::milliseconds backoff)
{
    // A condition-variable wait rather than `sleep_for`, so that `Shutdown`
    // can end it: an uninterruptible sleep held `Shutdown`'s join, and the
    // destructor's, for the rest of the backoff schedule (issue #310).
    std::unique_lock lock{m_mu};
    return !m_cv.wait_for(lock, backoff, [this] { return m_aborted; });
}

void RetryEngine::RecordOutcome(const internal::WireResult& result,
                                std::string_view failure_stage,
                                std::uint64_t batches) noexcept
{
    if (m_diag == nullptr)
    {
        return;
    }
    if (result.success)
    {
        if (result.partial_success_rejected > 0)
        {
            // Delivered, but the collector kept only some of it. Never
            // retried (error-model.md §6), so this is the only account of it.
            m_diag->RecordDrop(DropReason::PartialSuccessRejection,
                               result.partial_success_rejected);
        }
        for (std::uint64_t i = 0; i < batches; ++i)
        {
            m_diag->RecordBatchSent();
        }
        return;
    }
    // This funnel runs exactly once per batch, after every retry has been
    // resolved, so `retryable` here means "retried and still lost" rather
    // than "will be retried". Attempt exhaustion, budget exhaustion and a
    // retry cut short by Shutdown are the same outcome to an operator and
    // share one counter.
    m_diag->RecordDrop(result.retryable ? DropReason::RetryBudgetExhausted
                                        : DropReason::NonRetryableFailure,
                       batches);
    // A codec may report failure without populating `error`. Recording an
    // empty message would leave GetExporterHealth() saying a batch failed and
    // refusing to say why, so name the stage instead.
    const Error error = result.error.value_or(
        Error{.kind = Error::Kind::Network, .message = std::string{failure_stage}});
    for (std::uint64_t i = 0; i < batches; ++i)
    {
        m_diag->RecordBatchFailed(error);
    }
}

internal::TimePointSteady RetryEngine::ClockNow() const noexcept
{
    if (m_clock != nullptr)
    {
        return m_clock->Now();
    }
    return std::chrono::steady_clock::now();
}

std::uint64_t RetryEngine::ClockSeed() noexcept
{
    return static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
}

double RetryEngine::DrawJitter01() noexcept
{
    std::uniform_real_distribution dist{0.0, 1.0};
    return dist(m_rng);
}

}  // namespace microtel::exporter
