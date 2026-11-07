// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0
//
// Per-thread pacing to absolute deadlines.

#pragma once

#include <chrono>
#include <cstdint>
#include <thread>

namespace bench
{

/// Paces a loop to @p rate_hz iterations per second.  Iteration n starts no
/// earlier than start + n / rate_hz, a deadline fixed when the pacer is made,
/// so a late wake-up delays one iteration and the next ones catch up instead
/// of every oversleep adding to the run's length.  rate_hz == 0 means
/// unpaced: Wait() returns at once.
class Pacer
{
public:
    using Clock = std::chrono::steady_clock;
    static constexpr uint64_t kNsPerSec = 1'000'000'000ULL;

    Pacer(uint64_t rate_hz, Clock::time_point start) noexcept : m_rate_hz(rate_hz), m_start(start)
    {
    }

    /// The earliest start of iteration @p n.
    [[nodiscard]] Clock::time_point Deadline(uint64_t n) const noexcept
    {
        if (m_rate_hz == 0)
        {
            return m_start;
        }
        // n * 1e9 stays below 2^64 for n < 1.8e10, far past any run.
        return m_start + std::chrono::nanoseconds(n * kNsPerSec / m_rate_hz);
    }

    /// Sleeps until Deadline(n), if that is still ahead.
    void Wait(uint64_t n) const noexcept
    {
        if (m_rate_hz == 0)
        {
            return;
        }
        const auto deadline = Deadline(n);
        if (deadline > Clock::now())
        {
            std::this_thread::sleep_until(deadline);
        }
    }

private:
    uint64_t m_rate_hz{0};
    Clock::time_point m_start;
};

/// Where thread @p index of @p threads starts pacing, when each runs at
/// @p rate_hz_per_thread from a common @p start: start + index * interval /
/// threads, so the threads' iterations interleave evenly instead of all
/// waking at the same instant. Unpaced (rate 0) or one thread: @p start.
[[nodiscard]] inline Pacer::Clock::time_point StaggeredStart(Pacer::Clock::time_point start,
                                                             uint64_t rate_hz_per_thread,
                                                             uint64_t threads,
                                                             uint64_t index) noexcept
{
    if (rate_hz_per_thread == 0 || threads == 0)
    {
        return start;
    }
    return start +
           std::chrono::nanoseconds(index * Pacer::kNsPerSec / (rate_hz_per_thread * threads));
}

}  // namespace bench
