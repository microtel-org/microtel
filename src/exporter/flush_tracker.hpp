// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "microtel/status.hpp"

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <utility>
#include <vector>

namespace microtel::exporter
{

/// @brief The sequence accounting behind an exporter's `ForceFlush` and
///        `Shutdown` (ICP 0041, issue #484).
///
/// Every batch the exporter accepts takes the next number, in queue order. The
/// worker reports each drained round's outcomes in the same order. A flush
/// waits until every batch accepted before it has been processed — exported,
/// or failed after its final attempt — not until the queue is empty, which
/// under sustained load it never is.
///
/// A flush's range starts where the last *completed* flush's range ended
/// (the ICP 0041 amendment), so a failure is reported to every flush called
/// after it until one covering it completes — including a batch that failed
/// before this exporter's `ForceFlush` was reached, as happens between the
/// stages of `Provider::ForceFlush`. Overlapping flushes may both report the
/// same failure: a flush returns a status, and drops are counted elsewhere.
///
/// A flush is a `Waiter` on the caller's stack, linked into the tracker for as
/// long as it waits, so that the worker can mark it failed when a batch in its
/// range fails. No allocation: `ForceFlush` is `noexcept`.
///
/// @threadsafety None of its own. Every member, and every `Waiter`
///   constructor and destructor, runs under the owning exporter's `m_mu`; the
///   exporter's condition variable carries the wake-ups.
class FlushTracker final
{
public:
    /// @brief One flush's view of the batches queued before it.
    ///
    /// Registers with the tracker on construction and unregisters on
    /// destruction, so it must not outlive it, and is pinned in place.
    class Waiter final
    {
    public:
        explicit Waiter(FlushTracker& tracker) noexcept
            : m_tracker(tracker),
              m_after(tracker.m_completed_upto),
              m_upto(tracker.m_accepted),
              m_next(tracker.m_waiters),
              // Everything processed so far is at or below m_upto, so a
              // failure above m_after is in this range.
              m_failed(tracker.m_last_failed > m_after)
        {
            tracker.m_waiters = this;
        }

        ~Waiter() noexcept
        {
            // Covered its range: the next flush starts after it. A flush that
            // timed out has not, and leaves its failures to the next one.
            if (Reached() && m_upto > m_tracker.m_completed_upto)
            {
                m_tracker.m_completed_upto = m_upto;
            }
            Waiter** link = &m_tracker.m_waiters;
            while (*link != this)
            {
                link = &(*link)->m_next;
            }
            *link = m_next;
        }

        Waiter(const Waiter&) = delete;
        Waiter& operator=(const Waiter&) = delete;
        Waiter(Waiter&&) = delete;
        Waiter& operator=(Waiter&&) = delete;

        /// @brief Every batch accepted before this flush has been processed.
        [[nodiscard]] bool Reached() const noexcept
        {
            return m_tracker.m_processed >= m_upto;
        }

        /// @brief The flush's status: `Failed` if any of its batches failed,
        ///        else `Completed` if they were all processed in time, else
        ///        `TimedOut`.
        /// @param in_time whether the wait ended before its timeout.
        [[nodiscard]] microtel::Status StatusOf(bool in_time) const noexcept
        {
            if (m_failed)
            {
                return microtel::Status::Failed;
            }
            return in_time ? microtel::Status::Completed : microtel::Status::TimedOut;
        }

    private:
        friend class FlushTracker;

        FlushTracker& m_tracker;
        std::uint64_t m_after;
        std::uint64_t m_upto;
        Waiter* m_next;
        bool m_failed;
    };

    /// @brief How a wait ended.
    struct Outcome
    {
        /// @brief What `ForceFlush` or `Shutdown` returns.
        microtel::Status status = microtel::Status::TimedOut;
        /// @brief Every batch was processed before the timeout.
        bool in_time = false;
    };

    FlushTracker() noexcept = default;
    ~FlushTracker() = default;

    FlushTracker(const FlushTracker&) = delete;
    FlushTracker& operator=(const FlushTracker&) = delete;
    FlushTracker(FlushTracker&&) = delete;
    FlushTracker& operator=(FlushTracker&&) = delete;

    /// @brief `ForceFlush`: wait up to @p timeout for every batch accepted
    ///        before the call. The worker is already awake while the queue is
    ///        non-empty.
    /// @param lock holds the owning exporter's `m_mu`; released while waiting.
    /// @param cv the owning exporter's condition variable.
    Outcome Wait(std::unique_lock<std::mutex>& lock,
                 std::condition_variable& cv,
                 std::chrono::milliseconds timeout) noexcept
    {
        const Waiter waiter{*this};
        return WaitFor(waiter, lock, cv, timeout);
    }

    /// @brief `Shutdown`: as `Wait`, but first wakes the worker and runs
    ///        @p unlocked with @p lock released.
    ///
    /// The flush registers before the lock is released, so every batch
    /// queued now counts towards its status.
    /// @param unlocked a `noexcept` step that must not run under `m_mu`, such
    ///        as aborting the retry engine, whose lock is a leaf
    ///        (threading-model.md §4).
    template <typename Unlocked>
    Outcome WaitAfter(std::unique_lock<std::mutex>& lock,
                      std::condition_variable& cv,
                      std::chrono::milliseconds timeout,
                      Unlocked&& unlocked) noexcept
    {
        const Waiter waiter{*this};
        cv.notify_all();
        lock.unlock();
        std::forward<Unlocked>(unlocked)();
        lock.lock();
        return WaitFor(waiter, lock, cv, timeout);
    }

    /// @brief Count one batch into the queue. Call in queue order.
    void Accept() noexcept
    {
        ++m_accepted;
    }

    /// @brief Report the next @p count batches, in queue order, as processed.
    ///
    /// @param delivered one entry per batch, `true` if it was exported. A
    ///        batch with no entry — the round threw before every outcome was
    ///        known — counts as failed.
    void Complete(std::size_t count, const std::vector<bool>& delivered) noexcept
    {
        for (std::size_t i = 0; i < count; ++i)
        {
            if (i >= delivered.size() || !delivered[i])
            {
                MarkFailed(m_processed + i + 1U);
            }
        }
        m_processed += count;
    }

    /// @brief How many batches have been processed. For tests, which poll it
    ///        through the owning exporter rather than sleep.
    [[nodiscard]] std::uint64_t Processed() const noexcept
    {
        return m_processed;
    }

    /// @brief How many flushes are waiting now. For tests, as `Processed`.
    [[nodiscard]] std::size_t Waiting() const noexcept
    {
        std::size_t n = 0;
        for (const Waiter* w = m_waiters; w != nullptr; w = w->m_next)
        {
            ++n;
        }
        return n;
    }

private:
    static Outcome WaitFor(const Waiter& waiter,
                           std::unique_lock<std::mutex>& lock,
                           std::condition_variable& cv,
                           std::chrono::milliseconds timeout) noexcept
    {
        const bool in_time = cv.wait_for(lock, timeout, [&waiter] { return waiter.Reached(); });
        return Outcome{.status = waiter.StatusOf(in_time), .in_time = in_time};
    }

    void MarkFailed(std::uint64_t seq) noexcept
    {
        m_last_failed = seq;
        for (Waiter* w = m_waiters; w != nullptr; w = w->m_next)
        {
            if (seq > w->m_after && seq <= w->m_upto)
            {
                w->m_failed = true;
            }
        }
    }

    std::uint64_t m_accepted = 0;
    std::uint64_t m_processed = 0;
    /// @brief Where the last completed flush's range ended.
    std::uint64_t m_completed_upto = 0;
    /// @brief The highest sequence number that has failed; 0 for none.
    std::uint64_t m_last_failed = 0;
    /// @brief The flushes waiting now, newest first. Non-owning: each node is
    ///        a `Waiter` on its caller's stack.
    Waiter* m_waiters = nullptr;
};

}  // namespace microtel::exporter
