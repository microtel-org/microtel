// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0
//
// Heap allocations made by one opentelemetry-cpp OtlpRecordable::SetIdentity
// call, and by each protobuf setter it makes (bench/parity/RESULTS.md).
// Built in the parity otelcpp image's build stage, against the same
// opentelemetry-cpp and libprotobuf as the SUT. Counts every malloc in the
// process by interposing malloc in the executable.

#include <opentelemetry/exporters/otlp/otlp_recordable.h>
#include <opentelemetry/trace/span_context.h>
#include <opentelemetry/trace/trace_state.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

extern "C" void* __libc_malloc(std::size_t);

namespace
{
unsigned long g_allocs = 0;
}

extern "C" void* malloc(std::size_t n)
{
    ++g_allocs;
    return __libc_malloc(n);
}

namespace trace = opentelemetry::trace;
namespace proto = opentelemetry::proto::trace::v1;

template <typename F>
unsigned long Count(F f)
{
    const unsigned long before = g_allocs;
    f();
    return g_allocs - before;
}

int main()
{
    std::uint8_t tid[16];
    std::uint8_t sid[8];
    std::uint8_t pid[8];
    std::memset(tid, 1, sizeof tid);
    std::memset(sid, 2, sizeof sid);
    std::memset(pid, 3, sizeof pid);
    const trace::SpanContext ctx(trace::TraceId(tid), trace::SpanId(sid), trace::TraceFlags(1), false);
    const trace::SpanId parent(pid);
    const trace::SpanId no_parent;

    for (int warm = 0; warm < 3; ++warm)
    {
        opentelemetry::exporter::otlp::OtlpRecordable r;
        r.SetIdentity(ctx, no_parent);
    }
    {
        opentelemetry::exporter::otlp::OtlpRecordable r;
        std::printf("SetIdentity, root span:  %lu\n", Count([&] { r.SetIdentity(ctx, no_parent); }));
    }
    {
        opentelemetry::exporter::otlp::OtlpRecordable r;
        std::printf("SetIdentity, child span: %lu\n", Count([&] { r.SetIdentity(ctx, parent); }));
    }
    {
        proto::Span s;
        std::printf("  set_trace_id (16 bytes):      %lu\n",
                    Count([&] { s.set_trace_id(reinterpret_cast<const char*>(tid), 16); }));
        std::printf("  set_span_id (8 bytes):        %lu\n",
                    Count([&] { s.set_span_id(reinterpret_cast<const char*>(sid), 8); }));
        std::printf("  set_parent_span_id (8 bytes): %lu\n",
                    Count([&] { s.set_parent_span_id(reinterpret_cast<const char*>(pid), 8); }));
        std::string empty_header;
        std::printf("  TraceState::ToHeader (empty): %lu\n",
                    Count([&] { empty_header = ctx.trace_state()->ToHeader(); }));
        std::printf("  set_trace_state(\"\"):          %lu\n",
                    Count([&] { s.set_trace_state(std::move(empty_header)); }));
    }
    {
        // Candidate fixes: skip an empty trace_state, and fill trace_id in
        // place instead of through set_trace_id(const char*, size_t).
        proto::Span s;
        std::printf("  mutable_trace_id()->assign (16 bytes): %lu\n",
                    Count([&] { s.mutable_trace_id()->assign(reinterpret_cast<const char*>(tid), 16); }));
        std::printf("  trace_state skipped when Empty():      %lu\n", Count([&] {
                        if (!ctx.trace_state()->Empty())
                        {
                            s.set_trace_state(ctx.trace_state()->ToHeader());
                        }
                    }));
    }
    std::printf("OtlpRecordable construction:  %lu\n",
                Count([&] { opentelemetry::exporter::otlp::OtlpRecordable x; }));
    return 0;
}
