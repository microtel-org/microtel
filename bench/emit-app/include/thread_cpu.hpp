// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0
//
// The calling thread's own CPU time.

#pragma once

#include <cstdint>

namespace bench
{

/// CPU used by one thread: @c cpu_ns from CLOCK_THREAD_CPUTIME_ID, and its
/// user / system split from getrusage(RUSAGE_THREAD) (microsecond resolution).
struct ThreadCpu
{
    uint64_t cpu_ns{0};
    uint64_t user_ns{0};
    uint64_t sys_ns{0};

    ThreadCpu& operator+=(const ThreadCpu& o) noexcept
    {
        cpu_ns += o.cpu_ns;
        user_ns += o.user_ns;
        sys_ns += o.sys_ns;
        return *this;
    }
};

/// @p end minus @p start, field by field.
[[nodiscard]] inline ThreadCpu operator-(const ThreadCpu& end, const ThreadCpu& start) noexcept
{
    return ThreadCpu{
        .cpu_ns = end.cpu_ns - start.cpu_ns,
        .user_ns = end.user_ns - start.user_ns,
        .sys_ns = end.sys_ns - start.sys_ns,
    };
}

/// The calling thread's CPU so far; zeros for a reading that fails.
[[nodiscard]] ThreadCpu ReadThreadCpu() noexcept;

}  // namespace bench
