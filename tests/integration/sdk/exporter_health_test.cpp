// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0
//
// Proves the diagnostics sink actually reaches the export pipeline through a
// real SdkBuilder::Build().
//
// Two independent bugs used to make this impossible: OtlpExporter stored its
// IDiagnosticsSink* as [[maybe_unused]] and never called it, and SdkBuilder
// never passed one in at all. Every counter behind GetExporterHealth() read
// zero no matter what the pipeline did, so an operator debugging a failing
// export got silence. Unit coverage of the recording logic lives in
// tests/unit/exporter/otlp_exporter_test.cpp; this test exists to catch the
// *wiring* regressing, which a unit test with a hand-injected sink cannot see.
//
// Integration tier because it opens a real socket and takes seconds.

#include "microtel/protocol.hpp"
#include "microtel/provider.hpp"
#include "microtel/sdk_builder.hpp"
#include "microtel/status.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>

namespace
{

// Port 1 is reliably closed for a non-root process, so the lazy connect
// (ICP 0017) fails rather than hanging on an unroutable address.
constexpr const char* kClosedPortEndpoint = "http://127.0.0.1:1";
constexpr const char* kClosedPortGrpcEndpoint = "grpc://127.0.0.1:1";

// The protocol is named rather than left to the endpoint scheme (issue #203):
// which branch of BuildWireCodec runs is the whole point of the gRPC case
// below, so this test states it instead of depending on scheme inference to
// supply it.
[[nodiscard]] std::shared_ptr<microtel::Provider> BuildAgainstClosedPort(
    const char* endpoint, microtel::Protocol protocol)
{
    auto result = microtel::SdkBuilder()
                      .WithEndpoint(endpoint)
                      .WithProtocol(protocol)
                      .WithTimeouts(microtel::TimeoutOptions{
                          .connect = std::chrono::milliseconds(200),
                          .tls_handshake = std::chrono::milliseconds(200),
                          .per_export = std::chrono::milliseconds(200),
                          // Near-zero budget: stop after the first retry rather
                          // than sleeping through the full backoff schedule.
                          .retry_budget = std::chrono::milliseconds(1),
                          .flush = std::chrono::seconds(30),
                          .shutdown = std::chrono::seconds(10),
                      })
                      .Build();
    EXPECT_TRUE(result.has_value()) << (result.has_value() ? "" : result.error().message);
    if (!result.has_value())
    {
        return nullptr;
    }
    return std::move(*result);
}

/// Emits one span and flushes it, so the export path runs end to end.
void ExportOneDoomedSpan(const std::shared_ptr<microtel::Provider>& provider)
{
    // Deliberately no Connect() — the export path connects lazily (ICP 0017),
    // which is what puts the failure inside the wire codec rather than inside
    // Provider::Connect.
    {
        auto tracer = provider->GetTracer("diag-wiring", "1.0");
        auto span = tracer->StartSpan("doomed-export");
        span->End();
    }
    ASSERT_EQ(provider->ForceFlush(std::chrono::seconds(30)), microtel::Status::Failed);
}

[[nodiscard]] std::uint64_t DropCount(const microtel::HealthSnapshot& health,
                                      microtel::DropReason reason)
{
    return health.drop_counters.at(static_cast<std::size_t>(reason));
}

TEST(ExporterHealthIntegrationTest, FailedExportIsVisibleInHealthSnapshot)
{
    auto result = microtel::SdkBuilder()
                      .WithEndpoint(kClosedPortEndpoint)
                      .WithTimeouts(microtel::TimeoutOptions{
                          .connect = std::chrono::milliseconds(200),
                          .tls_handshake = std::chrono::milliseconds(200),
                          .per_export = std::chrono::milliseconds(200),
                          // Near-zero budget: stop after the first retry rather
                          // than sleeping through the full backoff schedule.
                          .retry_budget = std::chrono::milliseconds(1),
                          .flush = std::chrono::seconds(30),
                          .shutdown = std::chrono::seconds(10),
                      })
                      .Build();
    ASSERT_TRUE(result.has_value()) << result.error().message;
    const std::shared_ptr<microtel::Provider> provider = std::move(*result);

    // Deliberately no Connect() — the export path connects lazily (ICP 0017).
    {
        auto tracer = provider->GetTracer("diag-wiring", "1.0");
        auto span = tracer->StartSpan("doomed-export");
        span->End();
    }
    ASSERT_EQ(provider->ForceFlush(std::chrono::seconds(30)), microtel::Status::Failed);

    const microtel::HealthSnapshot health = provider->GetExporterHealth();
    EXPECT_GT(health.batches_failed, 0U)
        << "an export to a closed port must be visible in GetExporterHealth()";
    EXPECT_FALSE(health.last_error_message.empty())
        << "an operator reading health must be told why the export failed";
    EXPECT_EQ(health.batches_sent, 0U);
    EXPECT_EQ(health.queue_depth_now, 0U) << "queue must drain, not stay at high-water mark";
}

// ---------------------------------------------------------------------------
// Codec-recorded counters
//
// The exporter's own counters (batches_failed above) reached health because
// SdkBuilder passes the sink to OtlpExporter. The wire codecs were constructed
// with /*diag=*/nullptr, so every RecordDrop inside them — connect_failure,
// malformed_response, and now decompression_too_large — went nowhere in a real
// build. Unit tests could not see it: they inject the sink by hand.
// ---------------------------------------------------------------------------

TEST(ExporterHealthIntegrationTest, HttpCodecRecordedDropReachesHealthSnapshot)
{
    const auto provider = BuildAgainstClosedPort(kClosedPortEndpoint, microtel::Protocol::Http);
    ASSERT_NE(provider, nullptr);
    ExportOneDoomedSpan(provider);

    const microtel::HealthSnapshot health = provider->GetExporterHealth();
    EXPECT_GT(DropCount(health, microtel::DropReason::ConnectFailure), 0U)
        << "connect_failure is recorded inside HttpWireCodec::EnsureConnected; "
           "a codec built with a null sink drops it on the floor";
}

TEST(ExporterHealthIntegrationTest, GrpcCodecRecordedDropReachesHealthSnapshot)
{
    // Same wiring, the other branch of BuildWireCodec.
    const auto provider = BuildAgainstClosedPort(kClosedPortGrpcEndpoint, microtel::Protocol::Grpc);
    ASSERT_NE(provider, nullptr);
    ExportOneDoomedSpan(provider);

    const microtel::HealthSnapshot health = provider->GetExporterHealth();
    EXPECT_GT(DropCount(health, microtel::DropReason::ConnectFailure), 0U)
        << "connect_failure is recorded inside GrpcWireCodec::EnsureConnected; "
           "a codec built with a null sink drops it on the floor";
}

}  // namespace
