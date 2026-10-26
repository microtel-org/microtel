// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0
//
// OTLP/gRPC metrics (`opentelemetry.proto.collector.metrics.v1.MetricsService/Export`)
// against a real collector.
//
// The scenarios live in conformance/support/metric_conformance.hpp and are
// shared with the OTLP/HTTP suite; this file only decides how the Provider
// reaches the collector. Like grpc/logs_test.cpp it uses the plaintext
// receiver: gRPC is h2c by definition.
//
// Assertions read the collector's metrics.jsonl, which is downstream of its
// protobuf decode.

#include "microtel/protocol.hpp"
#include "microtel/provider.hpp"
#include "microtel/sdk_builder.hpp"

#include "conformance/support/conformance_env.hpp"
#include "conformance/support/metric_conformance.hpp"
#include "conformance/support/provider_builder.hpp"

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <utility>

namespace
{

namespace metrics = microtel::testing::metrics;

constexpr const char* kEndpointEnv = "MICROTEL_CONFORMANCE_GRPC_ENDPOINT";

constexpr const char* kSkipReason =
    "no conformance collector configured — run ci/scripts/conformance.sh";

constexpr const char* kScopeName = "microtel.conformance.grpc.metrics";

/// @brief Where a test sends and where it reads back.
struct Target
{
    std::string endpoint;
    std::string output_file;
};

/// @brief Resolves the endpoint and metrics output file, or reports why not.
/// @return false when the caller must `GTEST_SKIP()`.
bool ResolveTarget(Target& target)
{
    return microtel::testing::ConformanceEnabled(kEndpointEnv, target.endpoint) &&
           microtel::testing::ConformanceEnabled(metrics::kMetricsOutputFileEnv,
                                                 target.output_file);
}

/// @brief How a test's provider differs from the default.
struct ProviderOptions
{
    microtel::TemporalityPreference temporality = microtel::TemporalityPreference::Cumulative;
    bool gzip = false;  ///< request per-message `grpc-encoding: gzip`
};

/// @brief Builds a provider aimed at the collector's plaintext OTLP/gRPC receiver.
///
/// @param builder  caller-owned; `SdkBuilder` cannot be returned by value.
/// @param endpoint resolved from @ref kEndpointEnv.
/// @param options  temporality and compression.
/// @return the provider, or null after recording a failure.
std::shared_ptr<microtel::Provider> BuildProvider(microtel::SdkBuilder& builder,
                                                  const std::string& endpoint,
                                                  const ProviderOptions& options = {})
{
    auto result =
        microtel::testing::ConfigureConformanceBuilder(builder, endpoint, microtel::Protocol::Grpc)
            .WithServiceName(metrics::kServiceName)
            .WithMetricTemporality(options.temporality)
            .WithCompressionGzip(options.gzip)
            .Build();
    if (!result.has_value())
    {
        ADD_FAILURE() << result.error().message;
        return nullptr;
    }
    return std::move(*result);
}

TEST(GrpcMetricsConformance, SyncInstruments)
{
    Target target;
    if (!ResolveTarget(target))
    {
        GTEST_SKIP() << kSkipReason;
    }
    microtel::SdkBuilder builder;
    const auto provider = BuildProvider(builder, target.endpoint);
    ASSERT_NE(provider, nullptr);
    metrics::RunSyncInstruments(*provider, target.output_file, kScopeName);
}

TEST(GrpcMetricsConformance, ObservableInstruments)
{
    Target target;
    if (!ResolveTarget(target))
    {
        GTEST_SKIP() << kSkipReason;
    }
    microtel::SdkBuilder builder;
    const auto provider = BuildProvider(builder, target.endpoint);
    ASSERT_NE(provider, nullptr);
    metrics::RunObservableInstruments(*provider, target.output_file, kScopeName);
}

TEST(GrpcMetricsConformance, CumulativeTemporality)
{
    Target target;
    if (!ResolveTarget(target))
    {
        GTEST_SKIP() << kSkipReason;
    }
    microtel::SdkBuilder builder;
    const auto provider = BuildProvider(builder, target.endpoint);
    ASSERT_NE(provider, nullptr);
    metrics::RunTemporality(*provider, target.output_file, kScopeName, /*delta=*/false);
}

TEST(GrpcMetricsConformance, DeltaTemporality)
{
    Target target;
    if (!ResolveTarget(target))
    {
        GTEST_SKIP() << kSkipReason;
    }
    microtel::SdkBuilder builder;
    const auto provider = BuildProvider(
        builder, target.endpoint, {.temporality = microtel::TemporalityPreference::Delta});
    ASSERT_NE(provider, nullptr);
    metrics::RunTemporality(*provider, target.output_file, kScopeName, /*delta=*/true);
}

TEST(GrpcMetricsConformance, GzipAccepted)
{
    Target target;
    if (!ResolveTarget(target))
    {
        GTEST_SKIP() << kSkipReason;
    }
    microtel::SdkBuilder builder;
    const auto provider = BuildProvider(builder, target.endpoint, {.gzip = true});
    ASSERT_NE(provider, nullptr);
    metrics::RunGzipDelivery(*provider, target.output_file, kScopeName);
}

}  // namespace
