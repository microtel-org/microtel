// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "microtel/internal/clock.hpp"
#include "microtel/internal/diagnostics_sink.hpp"
#include "microtel/internal/wire_result.hpp"

#include "exporter/retry_policy.hpp"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <random>
#include <string_view>

namespace microtel::exporter
{

/// @brief One export attempt: encode the batch afresh and send it.
///
/// Re-encoding per attempt is required, not a convenience: a failed
/// `EncodedPayload` is released with its arena (LOCKED — `memory-model.md`
/// §3.1).
using RetryAttempt = std::function<internal::WireResult()>;

/// @brief The retry loop and final-outcome accounting shared by the trace,
///        metric and log exporters.
///
/// Each exporter makes attempt 0 itself (a `SendAll` fan-out) and hands the
/// result to `Settle`, which retries a retryable failure per
/// `RetryPolicyConfig` and records exactly one outcome for the batch
/// (`docs/error-model.md` §3, `docs/sequences/retry-after-failure.md`).
///
/// **Dependencies (all non-owning):** `IDiagnosticsSink` and `ISteadyClock`
/// are optional; `nullptr` disables accounting and uses
/// `std::chrono::steady_clock` respectively. Both must outlive the engine.
///
/// @threadsafety `Settle` is called from the exporter's worker thread only.
///   `Abort` may be called from any thread.
class RetryEngine final
{
public:
    RetryEngine(const RetryPolicyConfig& policy,
                internal::IDiagnosticsSink* diag,
                internal::ISteadyClock* clock) noexcept;

    ~RetryEngine() = default;

    RetryEngine(const RetryEngine&) = delete;
    RetryEngine& operator=(const RetryEngine&) = delete;
    RetryEngine(RetryEngine&&) = delete;
    RetryEngine& operator=(RetryEngine&&) = delete;

    /// @brief Resolve one batch and record its outcome.
    ///
    /// A retryable `first_attempt` is retried through `retry` until success,
    /// a non-retryable result, `max_attempts`, the retry budget, or `Abort`.
    /// Success, partial success and non-retryable results are never retried.
    ///
    /// @param first_attempt the fan-out result for this batch (attempt 0).
    /// @param retry         makes one further attempt. May throw; the
    ///                      exception propagates and nothing is recorded.
    /// @param failure_stage the error message recorded when a failed result
    ///                      carries no `Error` of its own. Borrowed.
    /// @param batches       how many batches the request carried. The trace
    ///                      exporter sends several `BatchHandle`s as one
    ///                      request (`docs/leaf-concentrator-design.md`
    ///                      §3.6.1); the request is retried and resolved once,
    ///                      and its outcome is counted once per batch in it —
    ///                      `batches_sent`, `batches_failed`, and the
    ///                      `non_retryable_failure`, `retry_budget_exhausted`
    ///                      and `retryable_failure_recovered` counters — so the
    ///                      counts are what they were before requests were
    ///                      coalesced. A partial success's rejected count is
    ///                      the collector's for the whole request and is
    ///                      recorded once.
    /// @return whether the request was delivered (a partial success counts),
    ///         which is what an exporter's flush accounting needs (ICP 0041).
    bool Settle(const internal::WireResult& first_attempt,
                const RetryAttempt& retry,
                std::string_view failure_stage,
                std::uint64_t batches = 1);

    /// @brief Wake any backoff sleep and end every retry loop at its next
    ///        sleep, now and from now on. Called by `Shutdown`
    ///        (`docs/sequences/shutdown-drain.md`, edge cases).
    void Abort() noexcept;

private:
    [[nodiscard]] internal::WireResult Resolve(const internal::WireResult& first_attempt,
                                               const RetryAttempt& retry,
                                               std::uint64_t batches);
    /// @return The last retry's result, or `nullopt` when no retry was made
    ///         (the first backoff would reach the budget, or `Abort`).
    [[nodiscard]] std::optional<internal::WireResult> RunRetryLoop(
        const internal::WireResult& first_attempt, const RetryAttempt& retry);
    /// @brief Before retry `attempt` (1 = the first retry), sleep the backoff
    ///        computed from the previous result `last`.
    /// @return `true` to make the attempt; `false` when `last` needs no
    ///         retry, the sleep would reach the budget, or `Abort` ended it.
    [[nodiscard]] bool BackOffBeforeRetry(const internal::WireResult& last,
                                          std::uint32_t attempt,
                                          internal::TimePointSteady budget_deadline);
    [[nodiscard]] bool SleepUnlessAborted(std::chrono::milliseconds backoff);
    void RecordOutcome(const internal::WireResult& result,
                       std::string_view failure_stage,
                       std::uint64_t batches) noexcept;
    [[nodiscard]] internal::TimePointSteady ClockNow() const noexcept;
    [[nodiscard]] double DrawJitter01() noexcept;
    [[nodiscard]] static std::uint64_t ClockSeed() noexcept;

    RetryPolicyConfig m_policy;
    internal::IDiagnosticsSink* m_diag;
    internal::ISteadyClock* m_clock;
    // Backoff jitter only: spreading retries out needs no unpredictability, so
    // a non-cryptographic engine is right here. Same finding as the trace
    // exporter's former engine, accepted in SonarCloud there.
    std::mt19937_64 m_rng{ClockSeed()};  // NOSONAR(cpp:S2245) jitter only

    std::mutex m_mu;
    std::condition_variable m_cv;
    bool m_aborted = false;
};

}  // namespace microtel::exporter
