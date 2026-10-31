// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0
//
// OTLP/HTTP metrics (`POST /v1/metrics`) against a real collector.
//
// The scenarios live in conformance/support/metric_conformance.hpp and are
// shared with the gRPC suite; this file only decides how the Provider reaches
// the collector. Like every OTLP/HTTP test here except plaintext_http1_test.cpp,
// it uses the TLS receiver: that is where ALPN negotiates `h2`. The plaintext
// receiver is HTTP/1.1-only (plaintext_http1_test.cpp).
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

constexpr const char* kEndpointEnv = "MICROTEL_CONFORMANCE_HTTP_TLS_ENDPOINT";
constexpr const char* kCaEnv = "MICROTEL_CONFORMANCE_CA";

constexpr const char* kSkipReason =
    "no conformance collector configured — run ci/scripts/conformance.sh";

constexpr const char* kScopeName = "microtel.conformance.http.metrics";

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
    bool gzip = false;  ///< request `Content-Encoding: gzip`
};

/// @brief Builds a provider aimed at the collector's TLS OTLP/HTTP receiver.
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
        microtel::testing::ConfigureConformanceBuilder(builder, endpoint, microtel::Protocol::Http)
            .WithServiceName(metrics::kServiceName)
            .WithTls(microtel::TlsOptions{
                // The run's throwaway CA. System trust would reject
                // this server: verification is on, not bypassed.
                .ca_bundle = microtel::testing::GetEnv(kCaEnv).value_or(""),
                .client_cert = {},
                .client_key = {},
                .sni_override = {},
            })
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

TEST(HttpMetricsConformance, SyncInstruments)
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

TEST(HttpMetricsConformance, ObservableInstruments)
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

TEST(HttpMetricsConformance, CumulativeTemporality)
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

TEST(HttpMetricsConformance, DeltaTemporality)
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

TEST(HttpMetricsConformance, GzipAccepted)
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
