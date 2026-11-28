// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "microtel/internal/reactor.hpp"

#include <atomic>
#include <chrono>
#include <concepts>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <utility>

namespace microtel::transport
{

/// @brief A transport's I/O thread: the thread, its stop flag, and the timed
///        stop that `Close` uses.
///
/// The thread calls one turn of the transport's loop until a stop is
/// requested, then publishes that it has exited. `Stop` waits for that
/// signal, bounded by the caller's timeout, and then joins. `std::thread` has
/// no timed join, and detaching is not an option because the loop touches
/// members of the transport — so the wait is what the timeout actually
/// governs, and a timeout is reported honestly rather than silently ignored
/// (`Provider::Shutdown` contract, CLAUDE.md rule 15). Same shape as
/// `OtlpExporter::Shutdown`.
///
/// Shared by `Http2Transport` and `Http1Transport`. Neither movable nor
/// copyable: the thread holds `this`.
///
/// @threadsafety `Start` and `Stop` are called by the owning transport, once
///               each, never concurrently with each other. The turn runs on
///               the I/O thread.
class IoThreadRunner
{
public:
    /// @brief Spawn the I/O thread. Call at most once.
    ///
    /// @param turn One pass of the loop: `void() noexcept`. Called repeatedly
    ///             on the I/O thread until `Stop`. It must not outlive what it
    ///             refers to: the owner calls `Stop` before destroying it.
    /// @throws std::system_error when no thread can be spawned (EAGAIN), as
    ///         `std::thread`'s constructor does.
    template <std::invocable Turn>
    void Start(Turn turn)
    {
        m_thread = std::thread([this, turn = std::move(turn)]() noexcept { Run(turn); });
    }

    /// @brief Stop the I/O thread: request the stop, wake the loop, wait up to
    ///        `timeout` for it to exit, then join.
    ///
    /// @param timeout How long to wait for the loop to exit before reporting
    ///                a timeout. The join happens either way.
    /// @param reactor The reactor the loop waits in, woken so the loop sees
    ///                the stop without waiting out its poll interval.
    /// @return true if the loop exited within `timeout`, false if it did not
    ///         (the caller reports `Status::TimedOut`). Also false when
    ///         `Start` never spawned a thread, after waiting the full timeout.
    [[nodiscard]] bool Stop(std::chrono::milliseconds timeout, internal::IReactor& reactor) noexcept
    {
        m_stop.store(true, std::memory_order_release);
        reactor.Wake();

        // Wait for the loop to publish its exit, bounded by the caller's
        // timeout. Previously the timeout parameter was not even named and
        // this was a bare join(), so a wedged I/O thread hung
        // Provider::Shutdown -- and through it the host application's exit --
        // with no way for the caller to find out.
        const bool exited_in_time = [&]
        {
            std::unique_lock lk{m_io_done_mu};
            return m_io_done_cv.wait_for(lk, timeout, [this] { return m_io_done; });
        }();

        // Joined either way: the loop touches members of the transport, so
        // detaching a thread that has not finished would leave it running
        // against a destroyed transport. The timeout governs how long the
        // caller waits before being told the truth, not whether the join
        // happens.
        if (m_thread.joinable())
        {
            m_thread.join();
        }
        return exited_in_time;
    }

private:
    template <typename Turn>
    void Run(Turn& turn) noexcept
    {
        while (!m_stop.load(std::memory_order_acquire))
        {
            turn();
        }
        // Publish loop exit so Stop's wait can be bounded by its timeout.
        {
            const std::scoped_lock lk{m_io_done_mu};
            m_io_done = true;
        }
        m_io_done_cv.notify_all();
    }

    std::atomic<bool> m_stop{false};
    std::thread m_thread;
    /// Signalled by the I/O thread as its loop exits, so `Stop` can bound how
    /// long it waits.
    std::mutex m_io_done_mu;
    std::condition_variable m_io_done_cv;
    bool m_io_done{false};
};

}  // namespace microtel::transport
