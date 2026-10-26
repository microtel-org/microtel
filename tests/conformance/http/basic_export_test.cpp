// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0
//
// The OTLP/HTTP acceptance floor: a real collector accepts the connection, and
// a span microtel exports comes back out of that collector's file exporter
// with its identity and payload intact.
//
// Unit and integration tests can only assert what microtel believes it sent.
// Nothing below trusts microtel's own accounting for the round trip — the
// assertions read the collector's output file, which is downstream of the
// collector's protobuf decode, so a payload it rejected or misread cannot
// pass. That is what makes this the Tier 1 conformance gate rather than a restatement
// of the exporter's unit tests.
//
// Why the TLS endpoint and not the plaintext one
// ----------------------------------------------
// These tests point at MICROTEL_CONFORMANCE_HTTP_TLS_ENDPOINT (receiver
// otlp/tls) even though a plaintext OTLP/HTTP receiver is running on 4318.
// microtel speaks HTTP/2 only — it is an nghttp2 transport by design — and on
// a plaintext socket that means h2c with prior knowledge. The collector's
// plaintext HTTP receiver is HTTP/1.1 only: it does not wrap its handler in
// h2c, so it answers the HTTP/2 connection preface with an HTTP/1.1 error and
// the SETTINGS exchange never completes. Over TLS the same receiver negotiates
// `h2` through ALPN and everything works.
//
// So this is the OTLP/HTTP path that can reach a stock collector today. The
// plaintext gap is real and tracked in docs/interop-matrix.md; the
// MICROTEL_CONFORMANCE_HTTP_ENDPOINT variable stays in the runner's contract
// for the negative test that will pin the behaviour deliberately.
//
// The expected JSON fragments are the collector's protojson rendering — hex
// ids, camelCase keys, attribute values in a typed envelope, int64 as a quoted
// string. Every one of them was read off a real run against the pinned image,
// not derived from the proto definitions.

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

constexpr const char* kEndpointEnv = "MICROTEL_CONFORMANCE_HTTP_TLS_ENDPOINT";
constexpr const char* kCaEnv = "MICROTEL_CONFORMANCE_CA";
constexpr const char* kOutputFileEnv = "MICROTEL_CONFORMANCE_OUTPUT_FILE";

constexpr const char* kSkipReason =
    "no conformance collector configured — run ci/scripts/conformance.sh";

constexpr const char* kServiceName = "microtel-conformance";
constexpr const char* kScopeName = "microtel.conformance.http";
constexpr const char* kScopeVersion = "1.0";

constexpr auto kFlushTimeout = std::chrono::seconds(30);
constexpr auto kCollectorPollTimeout = std::chrono::seconds(15);

// Expected protojson fragments, confirmed empirically. Each is one contiguous
// substring of a real collector output line, so a mangled key, a wrong value
// envelope, or a dropped field all fail here.
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
    R"("scope":{"name":"microtel.conformance.http","version":"1.0"})";

/// @brief Builds a provider aimed at the collector's TLS OTLP/HTTP receiver.
///
/// @param builder  caller-owned; `SdkBuilder` cannot be returned by value.
/// @param endpoint resolved from @ref kEndpointEnv.
/// @return the `Build()` result, for the caller to check.
auto BuildTlsProvider(microtel::SdkBuilder& builder, const std::string& endpoint)
{
    return microtel::testing::ConfigureConformanceBuilder(
               builder, endpoint, microtel::Protocol::Http)
        .WithServiceName(kServiceName)
        .WithTls(microtel::TlsOptions{
            // The run's throwaway CA. System trust would reject this server,
            // which is the point: verification is on, not bypassed.
            .ca_bundle = microtel::testing::GetEnv(kCaEnv).value_or(""),
            .client_cert = {},
            .client_key = {},
            .sni_override = {},
        })
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

TEST(HttpConformance, ConnectPreflightSucceeds)
{
    std::string endpoint;
    if (!microtel::testing::ConformanceEnabled(kEndpointEnv, endpoint))
    {
        GTEST_SKIP() << kSkipReason;
    }

    microtel::SdkBuilder builder;
    auto result = BuildTlsProvider(builder, endpoint);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    const std::shared_ptr<microtel::Provider> provider = std::move(*result);

    // Connect() is the eager-preflight half of ICP 0017: no telemetry, just
    // DNS / TCP / TLS / HTTP-2 SETTINGS. An operator who calls it expects a
    // reachability or TLS problem to surface here, not at first export.
    const auto connected = provider->Connect();
    ASSERT_TRUE(connected.has_value()) << connected.error().message;

    EXPECT_EQ(provider->GetExporterHealth().connection_state, microtel::ConnectionState::Connected);
}

TEST(HttpConformance, BasicExportRoundTrip)
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
    auto result = BuildTlsProvider(builder, endpoint);
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

    // Identity: the collector decoded the same ids microtel generated.
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

TEST(HttpConformance, RemoteParentTraceStateAndFlags)
{
    std::string endpoint;
    std::string output_file;
    if (!microtel::testing::ConformanceEnabled(kEndpointEnv, endpoint) ||
        !microtel::testing::ConformanceEnabled(kOutputFileEnv, output_file))
    {
        GTEST_SKIP() << kSkipReason;
    }
    microtel::SdkBuilder builder;
    auto result = BuildTlsProvider(builder, endpoint);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    microtel::testing::trace_context::RunRemoteParentTraceStateAndFlags(
        **result, output_file, kScopeName);
}

}  // namespace
