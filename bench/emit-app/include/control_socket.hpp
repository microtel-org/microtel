// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0
//
// TCP control socket (port 19090).
// The Python driver connects, writes JSON commands, reads JSON responses.
// Protocol: newline-delimited JSON (ndjson) — one JSON object per line.

#pragma once

#include "backend.hpp"
#include "workload.hpp"

#include <array>
#include <cstdint>
#include <string>

namespace bench
{

struct RunResult
{
    uint64_t spans_emitted{0};
    DroppedCounts spans_dropped{};
    uint64_t bytes_sent{0};
    uint64_t duration_ns{0};
    uint64_t latency_p50_ns{0};
    uint64_t latency_p95_ns{0};
    uint64_t latency_p99_ns{0};
    uint64_t latency_min_ns{0};
    uint64_t latency_max_ns{0};
    /// Raw bucket counts — bucket i covers [2^i, 2^(i+1)) ns.
    std::array<uint64_t, 64> latency_histogram{};
    /// The requested rate after splitting it evenly across the threads.
    uint64_t rate_hz_effective{0};
    /// The workload threads' own CPU over their run, summed (thread_cpu.hpp).
    uint64_t emit_cpu_ns{0};
    uint64_t emit_user_ns{0};
    uint64_t emit_sys_ns{0};
};

/// Listen on TCP port 19090, accept one connection, then loop:
///   - read a JSON command line from the driver
///   - dispatch to the appropriate handler
///   - write a JSON response line back
///
/// Commands:
///   {"cmd":"run","spans":N,"threads":T,"rate_hz":R}
///                              → runs workload, replies with RunResult JSON
///                                T defaults to 1; R = 0 means unlimited
///   {"cmd":"flush"}            → flushes in-flight spans, replies {"flush_ns":N}
///   {"cmd":"quit"}             → replies {"ok":true} and returns
///
/// Blocks until "quit" is received or the connection is closed.
/// `mode` is determined once at startup via the EMIT_WORKLOAD env var.
void RunControlLoop(int port, IBackend& backend, WorkloadMode mode);

/// Serialize a RunResult to a single-line JSON string (no trailing newline).
[[nodiscard]] std::string SerializeRunResult(const RunResult& r);

}  // namespace bench
