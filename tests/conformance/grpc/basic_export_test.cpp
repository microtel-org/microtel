// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0
//
// The OTLP/gRPC acceptance floor: a real collector accepts the connection, and
// a span microtel exports comes back out of that collector's file exporter
// with its identity and payload intact.
//
// This is the first end-to-end gRPC validation in the repo. microtel speaks
// gRPC without linking the gRPC library — it is a unary-RPC protocol layer over
// the same nghttp2 transport the HTTP path uses (CLAUDE.md rule 13) — so every
// other test of that layer has the same author on both sides of the wire. Here
// the receiver is the gRPC project's, which is the whole point: length-prefixed
// framing, `application/grpc` content negotiation, and trailer-carried status
// are all things a hand-rolled peer would agree with us about for the wrong
// reasons.
//
// Nothing below trusts microtel's own accounting for the round trip — the
// assertions read the collector's output file, which is downstream of the
// collector's protobuf decode, so a payload it rejected or misread cannot pass.
// That is what makes this the Tier 1 conformance gate rather than a restatement of the
// codec's unit tests.
//
// Why the PLAINTEXT endpoint here, where the HTTP suite needs TLS
// ---------------------------------------------------------------
// This file points at MICROTEL_CONFORMANCE_GRPC_ENDPOINT — `http://127.0.0.1:4317`,
// no TLS — and that is deliberate. The collector's plaintext OTLP/HTTP receiver
// is HTTP/1.1-only, so plaintext OTLP/HTTP reaches it over HTTP/1.1 (ICP 0042,
// tests/conformance/http/plaintext_http1_test.cpp), and the HTTP suite uses the
// TLS receiver to stay on HTTP/2. gRPC needs no such split — gRPC *is* h2c by
// definition and the collector's gRPC receiver speaks it — so plaintext gRPC is
// HTTP/2, and this test pins that.
//
// The expected JSON fragments are the collector's protojson rendering — hex
// ids, camelCase keys, attribute values in a typed envelope, int64 as a quoted
// string. They are character-identical to the ones in
// tests/conformance/http/basic_export_test.cpp, and that was checked rather
// than assumed: the two output lines this test and its HTTP twin produce were
// diffed after normalising ids, timestamps and the unique marker, and they are
// byte-identical — same key order, same `doubleValue:2.5` (unquoted) against
// `intValue:"42"` (quoted), same event and status shape. Which is the expected
// result, because the file exporter serialises the decoded ResourceSpans and by
// that point the receiving protocol has been discarded; a fragment that
// differed by protocol would mean microtel's gRPC encoding disagreed with its
// HTTP encoding about the payload, which is a finding rather than a reason to
// relax the assertion.

#include "microtel/attribute.hpp"
#include "microtel/protocol.hpp"
#include "microtel/provider.hpp"
#include "microtel/sdk_builder.hpp"
#include "microtel/span.hpp"
#include "microtel/status.hpp"
#include "microtel/trace.hpp"
#include "microtel/tracer.hpp"

#include "conformance/support/collector_output.hpp"
#include "conformance/support/conformance_env.hpp"
#include "conformance/support/provider_builder.hpp"
#include "conformance/support/trace_context_conformance.hpp"

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>

namespace
{

constexpr const char* kEndpointEnv = "MICROTEL_CONFORMANCE_GRPC_ENDPOINT";
constexpr const char* kOutputFileEnv = "MICROTEL_CONFORMANCE_OUTPUT_FILE";

constexpr const char* kSkipReason =
    "no conformance collector configured — run ci/scripts/conformance.sh";

constexpr const char* kServiceName = "microtel-conformance";
constexpr const char* kScopeName = "microtel.conformance.grpc";
constexpr const char* kScopeVersion = "1.0";

constexpr auto kFlushTimeout = std::chrono::seconds(30);
constexpr auto kCollectorPollTimeout = std::chrono::seconds(15);

// Expected protojson fragments, confirmed empirically against the pinned
// collector over gRPC. Each is one contiguous substring of a real collector
// output line, so a mangled key, a wrong value envelope, or a dropped field all
// fail here.
constexpr const char* kServiceNameJson =
    R"({"key":"service.name","value":{"stringValue":"microtel-conformance"}})";
constexpr const char* kStringAttrJson =
    R"({"key":"conformance.attr.string","value":{"stringValue":"string-value"}})";
constexpr const char* kInt64AttrJson =
    R"({"key":"conformance.attr.int64","value":{"intValue":"42"}})";
constexpr const char* kDoubleAttrJson =
    R"({"key":"conformance.attr.double","value":{"doubleValue":2.5}})";
constexpr const char* kBoolAttrJson =
    R"({"key":"conformance.attr.bool","value":{"boolValue":true}})";
constexpr const char* kEventNameJson = R"("name":"conformance.event")";
constexpr const char* kEventAttrJson =
    R"({"key":"conformance.event.attr","value":{"stringValue":"event-value"}})";
constexpr const char* kStatusJson = R"("status":{"message":"conformance-error","code":2})";
// The instrumentation scope: kScopeName / kScopeVersion as handed to
// GetTracer, not the service name. Asserting it is what issue #167 was open
// for — the collector used to decode `"scope":{"name":"microtel-conformance"}`
// here, with no version at all (ICP 0023).
constexpr const char* kScopeJson =
    R"("scope":{"name":"microtel.conformance.grpc","version":"1.0"})";

/// @brief Builds a provider aimed at the collector's plaintext OTLP/gRPC receiver.
///
/// No `WithTls`: this is the quick-start configuration, and unlike its
/// OTLP/HTTP twin it reaches a stock collector. See the file comment.
///
/// @param builder  caller-owned; `SdkBuilder` cannot be returned by value.
/// @param endpoint resolved from @ref kEndpointEnv.
/// @return the `Build()` result, for the caller to check.
auto BuildPlaintextProvider(microtel::SdkBuilder& builder, const std::string& endpoint)
{
    return microtel::testing::ConfigureConformanceBuilder(
               builder, endpoint, microtel::Protocol::Grpc)
        .WithServiceName(kServiceName)
        .Build();
}

/// @brief Asserts @p line contains @p fragment, reporting the whole line if not.
///
/// Extracted so the round-trip test body reads as a flat list of claims rather
/// than a wall of EXPECT_NE(find(...), npos).
void ExpectLineContains(const std::string& line, const std::string& fragment)
{
    EXPECT_NE(line.find(fragment), std::string::npos)
        << "collector output is missing " << fragment << "\n  line: " << line;
}

TEST(GrpcConformance, ConnectPreflightSucceeds)
{
    std::string endpoint;
    if (!microtel::testing::ConformanceEnabled(kEndpointEnv, endpoint))
    {
        GTEST_SKIP() << kSkipReason;
    }

    microtel::SdkBuilder builder;
    auto result = BuildPlaintextProvider(builder, endpoint);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    const std::shared_ptr<microtel::Provider> provider = std::move(*result);

    // Connect() is the eager-preflight half of ICP 0017: no telemetry, just
    // DNS / TCP / HTTP-2 SETTINGS. Over plaintext gRPC that is h2c with prior
    // knowledge, which the collector's gRPC receiver accepts — the same
    // exchange the HTTP/1.1-only HTTP receiver rejects on its plaintext port,
    // where OTLP/HTTP falls back and gRPC cannot (issue #166, ICP 0042).
    const auto connected = provider->Connect();
    ASSERT_TRUE(connected.has_value()) << connected.error().message;

    EXPECT_EQ(provider->GetExporterHealth().connection_state, microtel::ConnectionState::Connected);
}

TEST(GrpcConformance, BasicExportRoundTrip)
{
    std::string endpoint;
    if (!microtel::testing::ConformanceEnabled(kEndpointEnv, endpoint))
    {
        GTEST_SKIP() << kSkipReason;
    }
    std::string output_file;
    if (!microtel::testing::ConformanceEnabled(kOutputFileEnv, output_file))
    {
        GTEST_SKIP() << kSkipReason;
    }

    microtel::SdkBuilder builder;
    auto result = BuildPlaintextProvider(builder, endpoint);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    const std::shared_ptr<microtel::Provider> provider = std::move(*result);

    // The span name doubles as the needle: the collector's output file is
    // append-only across tests and runs, so a fixed name would let a previous
    // run's line satisfy this run's assertions.
    const std::string marker = microtel::testing::UniqueMarker();

    microtel::SpanContext context;
    {
        const auto tracer = provider->GetTracer(kScopeName, kScopeVersion);
        auto span = tracer->StartSpan(marker);

        // One of each OTLP scalar type: they take different protojson value
        // envelopes, so a single string attribute would not prove the encoder
        // handles the others.
        span->SetAttribute("conformance.attr.string", std::string("string-value"));
        span->SetAttribute("conformance.attr.int64", std::int64_t{42});
        span->SetAttribute("conformance.attr.double", 2.5);
        span->SetAttribute("conformance.attr.bool", true);

        const std::array<microtel::KeyValue, 1> event_attributes{microtel::KeyValue{
            .key = "conformance.event.attr", .value = std::string("event-value")}};
        span->AddEvent("conformance.event", event_attributes);

        span->SetStatus(microtel::StatusCode::Error, "conformance-error");

        // Captured before End(): the ids tie this span to the collector's
        // output line, and the handle is spent afterwards.
        context = span->GetContext();
        span->End();
    }

    ASSERT_EQ(provider->ForceFlush(kFlushTimeout), microtel::Status::Completed);

    const microtel::HealthSnapshot health = provider->GetExporterHealth();
    EXPECT_GE(health.batches_sent, 1U);
    EXPECT_EQ(health.batches_failed, 0U) << health.last_error_message;

    const auto line =
        microtel::testing::PollForLineContaining(output_file, marker, kCollectorPollTimeout);
    // Unwrapped with value_or rather than asserted-then-dereferenced: gtest's
    // early return is invisible to clang-tidy's dataflow, so every access after
    // an ASSERT_TRUE(has_value()) reads as unchecked. A line that matched the
    // marker is never empty, so the empty string stands in for "not found".
    const std::string exported = line.value_or(std::string{});
    ASSERT_FALSE(exported.empty())
        << "collector never wrote a line containing '" << marker << "' to " << output_file;

    // Identity: the collector decoded the same ids microtel generated, out of a
    // length-prefixed gRPC message rather than an HTTP body.
    const std::string trace_id_json = R"("traceId":")" + context.trace_id.ToHex() + R"(")";
    const std::string span_id_json = R"("spanId":")" + context.span_id.ToHex() + R"(")";
    ExpectLineContains(exported, trace_id_json);
    ExpectLineContains(exported, span_id_json);

    // Payload: resource, scope, every attribute type, the event, and the status.
    ExpectLineContains(exported, kServiceNameJson);
    ExpectLineContains(exported, kScopeJson);
    ExpectLineContains(exported, kStringAttrJson);
    ExpectLineContains(exported, kInt64AttrJson);
    ExpectLineContains(exported, kDoubleAttrJson);
    ExpectLineContains(exported, kBoolAttrJson);
    ExpectLineContains(exported, kEventNameJson);
    ExpectLineContains(exported, kEventAttrJson);
    ExpectLineContains(exported, kStatusJson);
}

TEST(GrpcConformance, RemoteParentTraceStateAndFlags)
{
    std::string endpoint;
    std::string output_file;
    if (!microtel::testing::ConformanceEnabled(kEndpointEnv, endpoint) ||
        !microtel::testing::ConformanceEnabled(kOutputFileEnv, output_file))
    {
        GTEST_SKIP() << kSkipReason;
    }
    microtel::SdkBuilder builder;
    auto result = BuildPlaintextProvider(builder, endpoint);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    microtel::testing::trace_context::RunRemoteParentTraceStateAndFlags(
        **result, output_file, kScopeName);
}

}  // namespace
