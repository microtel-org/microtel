// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#include "control_socket.hpp"

#include "backend.hpp"
#include "histogram.hpp"
#include "pacer.hpp"
#include "thread_cpu.hpp"
#include "workload.hpp"

#include <chrono>
#include <functional>
#include <thread>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <sstream>
#include <stdexcept>
#include <string>

namespace bench
{

namespace
{

constexpr int kBacklog = 1;
constexpr int kBufSize = 4096;
constexpr int kMinThreads = 1;

[[nodiscard]] int CreateListenSocket(int port)
{
    const int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
    {
        throw std::runtime_error(std::string("socket: ") + std::strerror(errno));
    }

    const int one = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(static_cast<uint16_t>(port));

    if (::bind(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) < 0)
    {
        ::close(fd);
        throw std::runtime_error(std::string("bind: ") + std::strerror(errno));
    }

    if (::listen(fd, kBacklog) < 0)
    {
        ::close(fd);
        throw std::runtime_error(std::string("listen: ") + std::strerror(errno));
    }

    return fd;
}

// Minimal JSON field extractors — avoids pulling in a JSON library.

[[nodiscard]] std::string ExtractStringField(const std::string& json, const std::string& key)
{
    const std::string search = "\"" + key + "\"";
    const auto key_pos = json.find(search);
    if (key_pos == std::string::npos)
    {
        return {};
    }
    const auto colon = json.find(':', key_pos + search.size());
    if (colon == std::string::npos)
    {
        return {};
    }
    const auto q1 = json.find('"', colon + 1);
    if (q1 == std::string::npos)
    {
        return {};
    }
    const auto q2 = json.find('"', q1 + 1);
    if (q2 == std::string::npos)
    {
        return {};
    }
    return json.substr(q1 + 1, q2 - q1 - 1);
}

[[nodiscard]] uint64_t ExtractUint64Field(const std::string& json, const std::string& key)
{
    const std::string search = "\"" + key + "\"";
    const auto key_pos = json.find(search);
    if (key_pos == std::string::npos)
    {
        return 0;
    }
    const auto colon = json.find(':', key_pos + search.size());
    if (colon == std::string::npos)
    {
        return 0;
    }
    // skip whitespace
    auto val_pos = colon + 1;
    while (val_pos < json.size() && (json[val_pos] == ' ' || json[val_pos] == '\t'))
    {
        ++val_pos;
    }
    return static_cast<uint64_t>(std::stoull(json.substr(val_pos)));
}

void WorkerThread(uint64_t span_count, uint64_t rate_hz_per_thread,
                  WorkloadMode mode, IBackend& backend, Histogram& hist, ThreadCpu& cpu)
{
    // The driver's CPU accounting (bench/driver/cpu.py) tells these threads
    // from the SDK's own by this name.
    ::pthread_setname_np(::pthread_self(), "emit");
    const ThreadCpu cpu0 = ReadThreadCpu();
    using Clock = std::chrono::steady_clock;
    const Pacer pacer(rate_hz_per_thread, Clock::now());
    for (uint64_t i = 0; i < span_count; ++i)
    {
        pacer.Wait(i);
        const auto t0 = Clock::now();
        EmitOnce(backend, mode);
        const auto t1 = Clock::now();
        const uint64_t ns = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
        hist.Record(ns);
    }
    cpu = ReadThreadCpu() - cpu0;
}

/// Runs the workload on @p threads threads and returns their CPU, summed.
/// @p rate_hz is split evenly; *rate_hz_effective gets what the split leaves.
[[nodiscard]] ThreadCpu RunWorkload(uint64_t spans, int threads, uint64_t rate_hz,
                                    WorkloadMode mode, IBackend& backend, Histogram& hist,
                                    uint64_t& rate_hz_effective)
{
    const auto n = static_cast<uint64_t>(threads);
    const uint64_t per_thread = spans / n;
    const uint64_t remainder = spans % n;
    const uint64_t rate_per_thread = rate_hz > 0 ? rate_hz / n : 0;
    rate_hz_effective = rate_per_thread * n;

    std::vector<ThreadCpu> cpu(static_cast<std::size_t>(threads));
    {
        std::vector<std::jthread> workers;
        workers.reserve(static_cast<std::size_t>(threads));
        for (int t = 0; t < threads; ++t)
        {
            const uint64_t count = per_thread + (t == 0 ? remainder : 0);
            workers.emplace_back(WorkerThread, count, rate_per_thread, mode, std::ref(backend),
                                 std::ref(hist), std::ref(cpu[static_cast<std::size_t>(t)]));
        }
    }  // jthreads join here: every worker has finished and written its CPU
    ThreadCpu total;
    for (const auto& c : cpu)
    {
        total += c;
    }
    return total;
}

[[nodiscard]] RunResult HandleRunCommand(const std::string& line,
                                         WorkloadMode mode,
                                         IBackend& backend)
{
    const uint64_t spans = ExtractUint64Field(line, "spans");
    const uint64_t threads_raw = ExtractUint64Field(line, "threads");
    const int threads = threads_raw > 0 ? static_cast<int>(threads_raw) : kMinThreads;
    const uint64_t rate_hz = ExtractUint64Field(line, "rate_hz");

    Histogram run_hist;
    using Clock = std::chrono::steady_clock;
    const auto t0 = Clock::now();
    uint64_t rate_hz_effective = 0;
    const ThreadCpu cpu =
        RunWorkload(spans, threads, rate_hz, mode, backend, run_hist, rate_hz_effective);
    const auto t1 = Clock::now();

    const auto stats = backend.Stats();
    const uint64_t dur_ns = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());

    return RunResult{
        .spans_emitted     = run_hist.Count(),
        .spans_dropped     = stats.spans_dropped,
        .bytes_sent        = stats.bytes_sent_total,
        .duration_ns       = dur_ns,
        .latency_p50_ns    = run_hist.Percentile(0.50),
        .latency_p95_ns    = run_hist.Percentile(0.95),
        .latency_p99_ns    = run_hist.Percentile(0.99),
        .latency_min_ns    = run_hist.Min(),
        .latency_max_ns    = run_hist.Max(),
        .latency_histogram = run_hist.Buckets(),
        .rate_hz_effective = rate_hz_effective,
        .emit_cpu_ns       = cpu.cpu_ns,
        .emit_user_ns      = cpu.user_ns,
        .emit_sys_ns       = cpu.sys_ns,
    };
}

/// Process all complete lines in `line_buf`.  Returns false when "quit" was received.
[[nodiscard]] bool ProcessConnectionData(std::string& line_buf, int conn_fd,
                                         WorkloadMode mode, IBackend& backend)
{
    std::size_t newline = line_buf.find('\n');
    while (newline != std::string::npos)
    {
        const std::string line = line_buf.substr(0, newline);
        line_buf.erase(0, newline + 1);

        const std::string cmd = ExtractStringField(line, "cmd");

        if (cmd == "quit")
        {
            const std::string resp = "{\"ok\":true}\n";
            ::send(conn_fd, resp.data(), resp.size(), 0);
            return false;
        }

        if (cmd == "run")
        {
            const RunResult result = HandleRunCommand(line, mode, backend);
            const std::string resp = SerializeRunResult(result) + "\n";
            ::send(conn_fd, resp.data(), resp.size(), 0);
        }

        if (cmd == "flush")
        {
            const uint64_t flush_ns = backend.ForceFlush();
            const std::string resp = "{\"flush_ns\":" + std::to_string(flush_ns) + "}\n";
            ::send(conn_fd, resp.data(), resp.size(), 0);
        }

        newline = line_buf.find('\n');
    }
    return true;
}

}  // namespace

std::string SerializeRunResult(const RunResult& r)
{
    const auto& d = r.spans_dropped;
    std::ostringstream os;
    os << "{\"spans_emitted\":" << r.spans_emitted
       << ",\"spans_dropped\":{"
           << "\"queue_full\":"              << d.queue_full
           << ",\"record_too_large\":"       << d.record_too_large
           << ",\"span_attribute_limit\":"   << d.span_attribute_limit
           << ",\"attribute_value_truncated\":" << d.attribute_value_truncated
           << ",\"other\":"                  << d.other
           << ",\"total\":"                  << d.total
       << "}"
       << ",\"bytes_sent\":" << r.bytes_sent
       << ",\"duration_ns\":" << r.duration_ns
       << ",\"latency_p50_ns\":" << r.latency_p50_ns
       << ",\"latency_p95_ns\":" << r.latency_p95_ns
       << ",\"latency_p99_ns\":" << r.latency_p99_ns
       << ",\"latency_min_ns\":" << r.latency_min_ns
       << ",\"latency_max_ns\":" << r.latency_max_ns
       << ",\"latency_histogram\":[";
    for (std::size_t i = 0; i < r.latency_histogram.size(); ++i)
    {
        if (i > 0)
        {
            os << ',';
        }
        os << r.latency_histogram[i];
    }
    os << "],\"rate_hz_effective\":" << r.rate_hz_effective
       << ",\"emit_cpu_ns\":" << r.emit_cpu_ns
       << ",\"emit_user_ns\":" << r.emit_user_ns
       << ",\"emit_sys_ns\":" << r.emit_sys_ns << "}";
    return os.str();
}

void RunControlLoop(int port, IBackend& backend, WorkloadMode mode)
{
    const int listen_fd = CreateListenSocket(port);

    // Accept connections in a loop. The bench driver's wait_tcp probe
    // opens and immediately closes a connection before the real ControlClient
    // connects. By re-accepting after a zero-byte close we handle both.
    bool done = false;
    while (!done)
    {
        const int conn_fd = ::accept(listen_fd, nullptr, nullptr);
        if (conn_fd < 0)
        {
            break;
        }

        std::string line_buf;
        char buf[kBufSize];

        for (;;)
        {
            const ssize_t n = ::recv(conn_fd, buf, sizeof(buf) - 1, 0);
            if (n <= 0)
            {
                break;
            }
            buf[n] = '\0';  // NOLINT(cppcoreguidelines-pro-bounds-constant-array-index)
            line_buf += buf;

            if (!ProcessConnectionData(line_buf, conn_fd, mode, backend))
            {
                done = true;
                break;
            }
        }

        ::close(conn_fd);
    }

    ::close(listen_fd);
}

}  // namespace bench
