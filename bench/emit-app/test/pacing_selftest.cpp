// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0
//
// Self-test for the pacer's deadlines and the thread CPU reading.  Plain
// asserts, no gtest, for the same reason as histogram_selftest.cpp.

#undef NDEBUG
#include "pacer.hpp"
#include "thread_cpu.hpp"

#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <thread>

namespace
{

using Clock = bench::Pacer::Clock;
using std::chrono::nanoseconds;

void TestDeadlinesAreAbsolute()
{
    const Clock::time_point start{};
    const bench::Pacer pacer(1'000, start);
    assert(pacer.Deadline(0) == start);
    assert(pacer.Deadline(1) == start + nanoseconds(1'000'000));
    assert(pacer.Deadline(60'000) == start + std::chrono::seconds(60));
}

void TestDeadlinesDoNotAccumulateRounding()
{
    // 1e9 / 3 is not a whole number of ns; a fixed interval would lose 1 ns
    // per iteration, deadlines computed from the start lose none.
    const Clock::time_point start{};
    const bench::Pacer pacer(3, start);
    assert(pacer.Deadline(3) == start + std::chrono::seconds(1));
    assert(pacer.Deadline(3'000) == start + std::chrono::seconds(1'000));
}

void TestUnpacedAndPastDeadlinesDoNotSleep()
{
    const bench::Pacer unpaced(0, Clock::now());
    const bench::Pacer behind(1, Clock::now() - std::chrono::hours(1));
    const auto t0 = Clock::now();
    unpaced.Wait(1'000'000);
    behind.Wait(10);
    assert(Clock::now() - t0 < std::chrono::milliseconds(100));
}

void TestStaggeredStartsInterleaveEvenly()
{
    // 4 threads at 83 Hz each: the per-thread interval is 1e9 / 83 ns, and
    // thread i starts i quarters of it later.
    const Clock::time_point start{};
    assert(bench::StaggeredStart(start, 83, 4, 0) == start);
    assert(bench::StaggeredStart(start, 83, 4, 1) == start + nanoseconds(1'000'000'000ULL / 332));
    assert(bench::StaggeredStart(start, 83, 4, 3) == start + nanoseconds(3'000'000'000ULL / 332));
    // Every offset is inside one interval, so no thread loses an iteration.
    assert(bench::StaggeredStart(start, 83, 4, 3) < bench::Pacer(83, start).Deadline(1));
}

void TestStaggeredStartUnpaced()
{
    const Clock::time_point start{};
    assert(bench::StaggeredStart(start, 0, 4, 3) == start);
    assert(bench::StaggeredStart(start, 100, 0, 0) == start);
}

void TestThreadCpuCountsWorkNotSleep()
{
    const bench::ThreadCpu c0 = bench::ReadThreadCpu();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    const bench::ThreadCpu c1 = bench::ReadThreadCpu();
    volatile uint64_t sink = 0;
    const auto until = Clock::now() + std::chrono::milliseconds(50);
    while (Clock::now() < until)
    {
        sink = sink + 1;
    }
    const bench::ThreadCpu c2 = bench::ReadThreadCpu();
    const bench::ThreadCpu slept = c1 - c0;
    const bench::ThreadCpu busy = c2 - c1;
    assert(slept.cpu_ns < 10'000'000);  // well under the 50 ms slept
    assert(busy.cpu_ns > 25'000'000);   // most of the 50 ms spun
    bench::ThreadCpu sum = slept;
    sum += busy;
    assert(sum.cpu_ns == (c2 - c0).cpu_ns);
}

}  // namespace

int main()
{
    TestDeadlinesAreAbsolute();
    TestDeadlinesDoNotAccumulateRounding();
    TestUnpacedAndPastDeadlinesDoNotSleep();
    TestStaggeredStartsInterleaveEvenly();
    TestStaggeredStartUnpaced();
    TestThreadCpuCountsWorkNotSleep();
    std::puts("pacing_selftest: OK");
    return 0;
}
