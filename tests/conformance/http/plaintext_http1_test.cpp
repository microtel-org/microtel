// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0
//
// Plaintext OTLP/HTTP against the stock collector's `otlp` receiver on 4318,
// which serves HTTP/1.1 only (ICP 0042, issue #166).
//
// This file used to be plaintext_gap_test.cpp, which pinned the opposite: that
// `http://collector:4318` could not be reached, because the transport was
// HTTP/2-only and this receiver does not wrap its handler in h2c. It said to
// invert it, not delete it, once that changed. Now the transport answers the
// receiver's HTTP/1.1 response to the HTTP/2 preface by falling back to
// HTTP/1.1, inside `Connect`, so the quick-start configuration works.
//
// The tests below are that inversion: the connect succeeds, and a span and a
// log record exported over `http://` come back out of the collector's file
// exporters intact. Like basic_export_test.cpp, nothing here trusts microtel's
// own accounting: the assertions read the collector's output, downstream of
// its protobuf decode. Several exports go out per provider, so the keep-alive
// connection carries more than one request.
//
// The last test is the other half of ICP 0042: OTLP/gRPC never falls back. The
// same HTTP/1.1-only port still refuses it, as a protocol mismatch.

#include "microtel/protocol.hpp"
#include "microtel/provider.hpp"
#include "microtel/sdk_builder.hpp"
#include "microtel/span.hpp"

#include "conformance/support/collector_output.hpp"
#include "conformance/support/conformance_env.hpp"
#include "conformance/support/log_conformance.hpp"
#include "conformance/support/provider_builder.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>

namespace
{

namespace logs = microtel::testing::logs;

constexpr const char* kPlaintextEndpointEnv = "MICROTEL_CONFORMANCE_HTTP_ENDPOINT";
constexpr const char* kTraceOutputFileEnv = "MICROTEL_CONFORMANCE_OUTPUT_FILE";

constexpr const char* kSkipReason =
    "no conformance collector configured — run ci/scripts/conformance.sh";

constexpr const char* kServiceName = "microtel-conformance";
constexpr const char* kScopeName = "microtel.conformance.http.plaintext";
constexpr const char* kLogScopeName = "microtel.conformance.http.plaintext.logs";
constexpr const char* kServiceNameJson =
    R"({"key":"service.name","value":{"stringValue":"microtel-conformance"}})";

constexpr auto kFlushTimeout = std::chrono::seconds(30);
constexpr auto kCollectorPollTimeout = std::chrono::seconds(15);
/// Exports per provider in the trace test: more than one, so the keep-alive
/// connection is reused.
constexpr int kTraceExports = 3;

/// The quick-start configuration: `http://collector:4318`, OTLP/HTTP, and
/// nothing else. No TLS options, no proxy.
std::shared_ptr<microtel::Provider> BuildPlaintextProvider(microtel::SdkBuilder& builder,
                                                           const std::string& endpoint)
{
    auto result =
        microtel::testing::ConfigureConformanceBuilder(builder, endpoint, microtel::Protocol::Http)
            .WithServiceName(kServiceName)
            .Build();
    if (!result.has_value())
    {
        ADD_FAILURE() << result.error().message;
        return nullptr;
    }
    return std::move(*result);
}

TEST(HttpPlaintextConformance, ConnectSucceedsOverHttp1)
{
    std::string endpoint;
    if (!microtel::testing::ConformanceEnabled(kPlaintextEndpointEnv, endpoint))
    {
        GTEST_SKIP() << kSkipReason;
    }
    microtel::SdkBuilder builder;
    const auto provider = BuildPlaintextProvider(builder, endpoint);
    ASSERT_NE(provider, nullptr);

    const auto connected = provider->Connect();
    ASSERT_TRUE(connected.has_value())
        << "plaintext OTLP/HTTP must reach the HTTP/1.1-only receiver (ICP 0042): "
        << connected.error().message;
    EXPECT_EQ(provider->GetExporterHealth().connection_state, microtel::ConnectionState::Connected);
}

TEST(HttpPlaintextConformance, TraceRoundTrip)
{
    std::string endpoint;
    std::string output_file;
    if (!microtel::testing::ConformanceEnabled(kPlaintextEndpointEnv, endpoint) ||
        !microtel::testing::ConformanceEnabled(kTraceOutputFileEnv, output_file))
    {
        GTEST_SKIP() << kSkipReason;
    }
    microtel::SdkBuilder builder;
    const auto provider = BuildPlaintextProvider(builder, endpoint);
    ASSERT_NE(provider, nullptr);
    const auto tracer = provider->GetTracer(kScopeName, "1.0");

    for (int i = 0; i < kTraceExports; ++i)
    {
        // A fresh marker per export: the output file is append-only across
        // tests and runs.
        const std::string marker = microtel::testing::UniqueMarker();
        microtel::SpanContext context;
        {
            auto span = tracer->StartSpan(marker);
            context = span->GetContext();
            span->End();
        }
        ASSERT_EQ(provider->ForceFlush(kFlushTimeout), microtel::Status::Completed);

        const std::string exported =
            microtel::testing::PollForLineContaining(output_file, marker, kCollectorPollTimeout)
                .value_or(std::string{});
        ASSERT_FALSE(exported.empty())
            << "export " << i << ": the collector never wrote '" << marker << "'";
        EXPECT_NE(exported.find(R"("traceId":")" + context.trace_id.ToHex() + R"(")"),
                  std::string::npos)
            << exported;
        EXPECT_NE(exported.find(kServiceNameJson), std::string::npos) << exported;
    }

    const microtel::HealthSnapshot health = provider->GetExporterHealth();
    EXPECT_GE(health.batches_sent, static_cast<std::uint64_t>(kTraceExports));
    EXPECT_EQ(health.batches_failed, 0U) << health.last_error_message;
}

TEST(HttpPlaintextConformance, LogRecordRoundTrip)
{
    std::string endpoint;
    std::string output_file;
    if (!microtel::testing::ConformanceEnabled(kPlaintextEndpointEnv, endpoint) ||
        !microtel::testing::ConformanceEnabled(logs::kLogsOutputFileEnv, output_file))
    {
        GTEST_SKIP() << kSkipReason;
    }
    microtel::SdkBuilder builder;
    const auto provider = BuildPlaintextProvider(builder, endpoint);
    ASSERT_NE(provider, nullptr);
    logs::RunRecordRoundTrip(*provider, output_file, kLogScopeName);
}

TEST(HttpPlaintextConformance, GrpcToTheHttp1OnlyReceiverStillFails)
{
    std::string endpoint;
    if (!microtel::testing::ConformanceEnabled(kPlaintextEndpointEnv, endpoint))
    {
        GTEST_SKIP() << kSkipReason;
    }
    microtel::SdkBuilder builder;
    auto result =
        microtel::testing::ConfigureConformanceBuilder(builder, endpoint, microtel::Protocol::Grpc)
            .WithServiceName(kServiceName)
            .Build();
    ASSERT_TRUE(result.has_value()) << result.error().message;
    const std::shared_ptr<microtel::Provider> provider = std::move(*result);

    const auto connected = provider->Connect();
    ASSERT_FALSE(connected.has_value()) << "gRPC needs HTTP/2 and must not fall back";
    EXPECT_EQ(connected.error().kind, microtel::Error::Kind::Protocol) << connected.error().message;
    EXPECT_NE(connected.error().message.find("HTTP/1.1-only"), std::string::npos)
        << connected.error().message;
}

}  // namespace
