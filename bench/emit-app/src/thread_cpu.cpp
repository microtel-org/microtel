// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#include "thread_cpu.hpp"

#include <sys/resource.h>
#include <sys/time.h>
#include <time.h>

namespace bench
{

namespace
{

constexpr uint64_t kNsPerSec = 1'000'000'000ULL;
constexpr uint64_t kNsPerUsec = 1'000ULL;

[[nodiscard]] uint64_t ToNs(const timeval& tv) noexcept
{
    return static_cast<uint64_t>(tv.tv_sec) * kNsPerSec +
           static_cast<uint64_t>(tv.tv_usec) * kNsPerUsec;
}

}  // namespace

ThreadCpu ReadThreadCpu() noexcept
{
    ThreadCpu out;
    timespec ts{};
    if (::clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts) == 0)
    {
        out.cpu_ns =
            static_cast<uint64_t>(ts.tv_sec) * kNsPerSec + static_cast<uint64_t>(ts.tv_nsec);
    }
    rusage ru{};
    if (::getrusage(RUSAGE_THREAD, &ru) == 0)
    {
        out.user_ns = ToNs(ru.ru_utime);
        out.sys_ns = ToNs(ru.ru_stime);
    }
    return out;
}

}  // namespace bench
