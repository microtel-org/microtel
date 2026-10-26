// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0
//
// Span and link `traceState` and `flags` against the collector (issue #451),
// shared by tests/conformance/{http,grpc}/basic_export_test.cpp.
//
// A span with a sampled remote parent carrying a W3C tracestate, and a link to
// a sampled remote context with its own, must come out of the collector with
// both trace states and with `flags` = sampled | parent-is-remote-known |
// parent-is-remote (0x301 = 769). The fragments are the pinned collector's
// protojson rendering, read off a real run.

#pragma once

#include "microtel/provider.hpp"
#include "microtel/span.hpp"
#include "microtel/status.hpp"
#include "microtel/trace.hpp"
#include "microtel/tracer.hpp"

#include "conformance/support/collector_output.hpp"
#include "conformance/support/conformance_env.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>

namespace microtel::testing::trace_context
{

inline constexpr auto kFlushTimeout = std::chrono::seconds(30);
inline constexpr auto kCollectorPollTimeout = std::chrono::seconds(15);

/// Nesting from a span's `name` to the span object.
inline constexpr std::size_t kSpanLevel = 1;

inline constexpr const char* kParentTraceState = "conformance=1";
inline constexpr const char* kLinkTraceState = "linked=1";
inline constexpr const char* kSpanTraceStateJson = R"("traceState":"conformance=1")";
inline constexpr const char* kLinkTraceStateJson = R"("traceState":"linked=1")";
/// sampled (0x01) | CONTEXT_HAS_IS_REMOTE (0x100) | CONTEXT_IS_REMOTE (0x200).
inline constexpr const char* kRemoteSampledFlagsJson = R"("flags":769)";

/// A sampled, remote span context with @p trace_state, ids filled with @p fill.
inline SpanContext RemoteContext(std::uint8_t fill, const char* trace_state)
{
    TraceId::Bytes trace_bytes{};
    trace_bytes.fill(fill);
    SpanId::Bytes span_bytes{};
    span_bytes.fill(fill);
    return SpanContext{
        .trace_id = TraceId{trace_bytes},
        .span_id = SpanId{span_bytes},
        .trace_flags = TraceFlags{TraceFlags::kSampled},
        .trace_state = TraceState::FromHeader(trace_state),
        .remote = true,
    };
}

/// @brief Asserts @p span (one span's protojson object) carries both trace
///        states, and the remote-sampled flags on the span and on its link.
inline void ExpectTraceContext(const std::string& span)
{
    for (const char* fragment : {kSpanTraceStateJson, kLinkTraceStateJson})
    {
        EXPECT_NE(span.find(fragment), std::string::npos)
            << "collector output is missing " << fragment << "\n  in: " << span;
    }
    const auto first = span.find(kRemoteSampledFlagsJson);
    ASSERT_NE(first, std::string::npos) << span;
    EXPECT_NE(span.find(kRemoteSampledFlagsJson, first + 1), std::string::npos)
        << "expected flags on the span and on its link\n  in: " << span;
}

/// @brief Exports one child of a remote parent, with one remote link, and
///        asserts the collector decoded both trace states and both flags.
inline void RunRemoteParentTraceStateAndFlags(Provider& provider,
                                              const std::string& output_file,
                                              const std::string& scope_name)
{
    constexpr std::uint8_t kParentFill = 0x5a;
    constexpr std::uint8_t kLinkFill = 0x6b;
    const std::string marker = UniqueMarker();
    {
        const auto tracer = provider.GetTracer(scope_name, "1.0");
        StartSpanOptions options;
        options.parent = RemoteContext(kParentFill, kParentTraceState);
        auto span = tracer->StartSpan(marker, options);
        span->AddLink(RemoteContext(kLinkFill, kLinkTraceState));
        span->End();
    }
    ASSERT_EQ(provider.ForceFlush(kFlushTimeout), Status::Completed);

    const std::string needle = R"("name":")" + marker + R"(")";
    const auto line = PollForLineContaining(output_file, needle, kCollectorPollTimeout);
    const std::string exported = line.value_or(std::string{});
    ASSERT_FALSE(exported.empty()) << "span never reached " << output_file;
    ExpectTraceContext(EnclosingObject(exported, needle, kSpanLevel).value_or(std::string{}));
}

}  // namespace microtel::testing::trace_context
