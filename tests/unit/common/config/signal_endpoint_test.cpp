// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0
//
// The per-signal OTLP exporter variables (`docs/configuration.md` §2.2, §3.3):
//   - OverlayEnv reads OTEL_EXPORTER_OTLP_{TRACES,METRICS,LOGS}_ENDPOINT and
//     records one warning per other per-signal exporter variable it does not
//     implement;
//   - Validate resolves the endpoints in effect to one connection origin and a
//     request path per signal, or refuses origins that differ.

#include "microtel/error.hpp"
#include "microtel/export_transport.hpp"
#include "microtel/protocol.hpp"

#include "common/config/config.hpp"
#include "common/config/config_validator.hpp"
#include "common/config/env_resolver.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdlib>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mc = microtel::config;
namespace mt = microtel;

namespace
{

constexpr std::size_t kTraces = mc::SignalIndex(mt::ExportSignal::Traces);
constexpr std::size_t kMetrics = mc::SignalIndex(mt::ExportSignal::Metrics);
constexpr std::size_t kLogs = mc::SignalIndex(mt::ExportSignal::Logs);

/// Sets an environment variable for the scope's lifetime, then unsets it.
class EnvVar
{
public:
    EnvVar(const char* name, const char* value) : m_name(name)
    {
        (void)::setenv(name, value, 1);
    }
    ~EnvVar()
    {
        (void)::unsetenv(m_name);
    }
    EnvVar(const EnvVar&) = delete;
    EnvVar& operator=(const EnvVar&) = delete;
    EnvVar(EnvVar&&) = delete;
    EnvVar& operator=(EnvVar&&) = delete;

private:
    const char* m_name;
};

[[nodiscard]] std::size_t CountContaining(const std::vector<std::string>& lines,
                                          std::string_view needle)
{
    return static_cast<std::size_t>(std::ranges::count_if(
        lines,
        [needle](const std::string& line) { return line.find(needle) != std::string::npos; }));
}

/// A Config with only the given per-signal endpoints set (empty = unset).
[[nodiscard]] mc::Config WithSignalEndpoints(std::string base,
                                             std::string traces,
                                             std::string metrics = {},
                                             std::string logs = {})
{
    mc::Config cfg;
    cfg.endpoint_url = std::move(base);
    cfg.signal_endpoint_urls.at(kTraces) = std::move(traces);
    cfg.signal_endpoint_urls.at(kMetrics) = std::move(metrics);
    cfg.signal_endpoint_urls.at(kLogs) = std::move(logs);
    return cfg;
}

}  // namespace

// ---------------------------------------------------------------------------
// OverlayEnv — the three endpoint variables
// ---------------------------------------------------------------------------

TEST(SignalEndpointEnvTest, TracesEndpoint_IsRead)
{
    const EnvVar v{"OTEL_EXPORTER_OTLP_TRACES_ENDPOINT", "http://collector:4318/v1/traces"};
    mc::Config cfg;
    ASSERT_TRUE(mc::OverlayEnv(cfg).has_value());
    EXPECT_EQ(cfg.signal_endpoint_urls.at(kTraces), "http://collector:4318/v1/traces");
    EXPECT_TRUE(cfg.signal_endpoint_urls.at(kMetrics).empty());
    EXPECT_TRUE(cfg.signal_endpoint_urls.at(kLogs).empty());
    EXPECT_TRUE(cfg.endpoint_url.empty()) << "a per-signal variable is not the base endpoint";
}

TEST(SignalEndpointEnvTest, MetricsEndpoint_IsRead)
{
    const EnvVar v{"OTEL_EXPORTER_OTLP_METRICS_ENDPOINT", "http://collector:4318/m"};
    mc::Config cfg;
    ASSERT_TRUE(mc::OverlayEnv(cfg).has_value());
    EXPECT_EQ(cfg.signal_endpoint_urls.at(kMetrics), "http://collector:4318/m");
    EXPECT_TRUE(cfg.signal_endpoint_urls.at(kTraces).empty());
}

TEST(SignalEndpointEnvTest, LogsEndpoint_IsRead)
{
    const EnvVar v{"OTEL_EXPORTER_OTLP_LOGS_ENDPOINT", "http://collector:4318/l"};
    mc::Config cfg;
    ASSERT_TRUE(mc::OverlayEnv(cfg).has_value());
    EXPECT_EQ(cfg.signal_endpoint_urls.at(kLogs), "http://collector:4318/l");
    EXPECT_TRUE(cfg.signal_endpoint_urls.at(kTraces).empty());
}

TEST(SignalEndpointEnvTest, EndpointVariables_RecordNoWarning)
{
    const EnvVar t{"OTEL_EXPORTER_OTLP_TRACES_ENDPOINT", "http://collector:4318/t"};
    const EnvVar m{"OTEL_EXPORTER_OTLP_METRICS_ENDPOINT", "http://collector:4318/m"};
    const EnvVar l{"OTEL_EXPORTER_OTLP_LOGS_ENDPOINT", "http://collector:4318/l"};
    mc::Config cfg;
    ASSERT_TRUE(mc::OverlayEnv(cfg).has_value());
    EXPECT_TRUE(cfg.config_warnings.empty());
}

TEST(SignalEndpointEnvTest, BaseEndpointFromEnv_RecordsItsSource)
{
    const EnvVar v{"OTEL_EXPORTER_OTLP_ENDPOINT", "http://collector:4318"};
    mc::Config cfg;
    EXPECT_EQ(cfg.endpoint_source, "exporter.endpoint");
    ASSERT_TRUE(mc::OverlayEnv(cfg).has_value());
    EXPECT_EQ(cfg.endpoint_source, "OTEL_EXPORTER_OTLP_ENDPOINT");
}

// ---------------------------------------------------------------------------
// OverlayEnv — every other per-signal exporter variable warns, once each
// ---------------------------------------------------------------------------

namespace
{

struct UnsupportedVar
{
    const char* name;
    const char* shared;  ///< the setting the warning points at instead
};

class UnsupportedSignalVarTest : public ::testing::TestWithParam<UnsupportedVar>
{
};

}  // namespace

TEST_P(UnsupportedSignalVarTest, Set_RecordsOneWarningNamingItAndTheSharedSetting)
{
    const UnsupportedVar& param = GetParam();
    const EnvVar v{param.name, "x"};
    mc::Config cfg;
    ASSERT_TRUE(mc::OverlayEnv(cfg).has_value());
    ASSERT_EQ(cfg.config_warnings.size(), 1U);
    const std::string& line = cfg.config_warnings.front();
    EXPECT_NE(line.find(param.name), std::string::npos) << line;
    EXPECT_NE(line.find("not supported"), std::string::npos) << line;
    EXPECT_NE(line.find(param.shared), std::string::npos) << line;
}

TEST_P(UnsupportedSignalVarTest, Set_DoesNotChangeTheSharedSetting)
{
    const EnvVar v{GetParam().name, "gzip"};
    mc::Config cfg;
    const mc::Config before;
    ASSERT_TRUE(mc::OverlayEnv(cfg).has_value());
    EXPECT_EQ(cfg.compression_gzip, before.compression_gzip);
    EXPECT_EQ(cfg.protocol_explicit, before.protocol_explicit);
    EXPECT_TRUE(cfg.headers.empty());
    EXPECT_EQ(cfg.tls.ca_bundle, before.tls.ca_bundle);
    EXPECT_EQ(cfg.timeouts.per_export, before.timeouts.per_export);
}

namespace
{

constexpr std::array<UnsupportedVar, 24> kUnsupportedVars{{
    {.name = "OTEL_EXPORTER_OTLP_TRACES_HEADERS", .shared = "OTEL_EXPORTER_OTLP_HEADERS"},
    {.name = "OTEL_EXPORTER_OTLP_METRICS_HEADERS", .shared = "OTEL_EXPORTER_OTLP_HEADERS"},
    {.name = "OTEL_EXPORTER_OTLP_LOGS_HEADERS", .shared = "OTEL_EXPORTER_OTLP_HEADERS"},
    {.name = "OTEL_EXPORTER_OTLP_TRACES_PROTOCOL", .shared = "OTEL_EXPORTER_OTLP_PROTOCOL"},
    {.name = "OTEL_EXPORTER_OTLP_METRICS_PROTOCOL", .shared = "OTEL_EXPORTER_OTLP_PROTOCOL"},
    {.name = "OTEL_EXPORTER_OTLP_LOGS_PROTOCOL", .shared = "OTEL_EXPORTER_OTLP_PROTOCOL"},
    {.name = "OTEL_EXPORTER_OTLP_TRACES_TIMEOUT", .shared = "OTEL_EXPORTER_OTLP_TIMEOUT"},
    {.name = "OTEL_EXPORTER_OTLP_METRICS_TIMEOUT", .shared = "OTEL_EXPORTER_OTLP_TIMEOUT"},
    {.name = "OTEL_EXPORTER_OTLP_LOGS_TIMEOUT", .shared = "OTEL_EXPORTER_OTLP_TIMEOUT"},
    {.name = "OTEL_EXPORTER_OTLP_TRACES_COMPRESSION", .shared = "OTEL_EXPORTER_OTLP_COMPRESSION"},
    {.name = "OTEL_EXPORTER_OTLP_METRICS_COMPRESSION", .shared = "OTEL_EXPORTER_OTLP_COMPRESSION"},
    {.name = "OTEL_EXPORTER_OTLP_LOGS_COMPRESSION", .shared = "OTEL_EXPORTER_OTLP_COMPRESSION"},
    {.name = "OTEL_EXPORTER_OTLP_TRACES_CERTIFICATE", .shared = "OTEL_EXPORTER_OTLP_CERTIFICATE"},
    {.name = "OTEL_EXPORTER_OTLP_METRICS_CERTIFICATE", .shared = "OTEL_EXPORTER_OTLP_CERTIFICATE"},
    {.name = "OTEL_EXPORTER_OTLP_LOGS_CERTIFICATE", .shared = "OTEL_EXPORTER_OTLP_CERTIFICATE"},
    {.name = "OTEL_EXPORTER_OTLP_TRACES_CLIENT_KEY", .shared = "tls.client_key"},
    {.name = "OTEL_EXPORTER_OTLP_METRICS_CLIENT_KEY", .shared = "tls.client_key"},
    {.name = "OTEL_EXPORTER_OTLP_LOGS_CLIENT_KEY", .shared = "tls.client_key"},
    {.name = "OTEL_EXPORTER_OTLP_TRACES_CLIENT_CERTIFICATE", .shared = "tls.client_cert"},
    {.name = "OTEL_EXPORTER_OTLP_METRICS_CLIENT_CERTIFICATE", .shared = "tls.client_cert"},
    {.name = "OTEL_EXPORTER_OTLP_LOGS_CLIENT_CERTIFICATE", .shared = "tls.client_cert"},
    {.name = "OTEL_EXPORTER_OTLP_TRACES_INSECURE", .shared = "tls.insecure"},
    {.name = "OTEL_EXPORTER_OTLP_METRICS_INSECURE", .shared = "tls.insecure"},
    {.name = "OTEL_EXPORTER_OTLP_LOGS_INSECURE", .shared = "tls.insecure"},
}};

}  // namespace

INSTANTIATE_TEST_SUITE_P(AllTwentyFour,
                         UnsupportedSignalVarTest,
                         ::testing::ValuesIn(kUnsupportedVars),
                         [](const ::testing::TestParamInfo<UnsupportedVar>& info)
                         { return std::string{info.param.name}; });

TEST(SignalEndpointEnvTest, SeveralUnsupportedVars_OneWarningEach)
{
    const EnvVar a{"OTEL_EXPORTER_OTLP_TRACES_HEADERS", "a=b"};
    const EnvVar b{"OTEL_EXPORTER_OTLP_LOGS_TIMEOUT", "100"};
    mc::Config cfg;
    ASSERT_TRUE(mc::OverlayEnv(cfg).has_value());
    ASSERT_EQ(cfg.config_warnings.size(), 2U);
    EXPECT_EQ(CountContaining(cfg.config_warnings, "OTEL_EXPORTER_OTLP_TRACES_HEADERS"), 1U);
    EXPECT_EQ(CountContaining(cfg.config_warnings, "OTEL_EXPORTER_OTLP_LOGS_TIMEOUT"), 1U);
}

TEST(SignalEndpointEnvTest, MetricsTemporalityPreference_IsSupportedAndDoesNotWarn)
{
    const EnvVar v{"OTEL_EXPORTER_OTLP_METRICS_TEMPORALITY_PREFERENCE", "delta"};
    mc::Config cfg;
    ASSERT_TRUE(mc::OverlayEnv(cfg).has_value());
    EXPECT_TRUE(cfg.config_warnings.empty());
}

// ---------------------------------------------------------------------------
// Validate — one origin, a path per signal (OTLP/HTTP)
// ---------------------------------------------------------------------------

// (a) A lone per-signal endpoint picks the origin; the other signals keep
// their default paths on it.
TEST(SignalEndpointValidateTest, LoneTracesEndpoint_PicksTheOriginAndIsUsedAsIs)
{
    mc::Config cfg = WithSignalEndpoints("", "http://collector:4318/custom/traces");
    const auto r = mc::Validate(cfg);
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_EQ(cfg.endpoint.scheme, "http");
    EXPECT_EQ(cfg.endpoint.host, "collector");
    EXPECT_EQ(cfg.endpoint.port, 4318);
    EXPECT_EQ(cfg.signal_paths.at(kTraces), "/custom/traces");
    EXPECT_EQ(cfg.signal_paths.at(kMetrics), "/v1/metrics");
    EXPECT_EQ(cfg.signal_paths.at(kLogs), "/v1/logs");
    EXPECT_TRUE(cfg.config_warnings.empty());
}

TEST(SignalEndpointValidateTest, LoneMetricsEndpoint_PicksTheOrigin)
{
    mc::Config cfg = WithSignalEndpoints("", "", "https://metrics.example:9000/ingest");
    const auto r = mc::Validate(cfg);
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_EQ(cfg.endpoint.scheme, "https");
    EXPECT_EQ(cfg.endpoint.host, "metrics.example");
    EXPECT_EQ(cfg.endpoint.port, 9000);
    EXPECT_EQ(cfg.signal_paths.at(kTraces), "/v1/traces");
    EXPECT_EQ(cfg.signal_paths.at(kMetrics), "/ingest");
    EXPECT_EQ(cfg.signal_paths.at(kLogs), "/v1/logs");
}

// The OTel spec's Example 2: no path means the root path, nothing appended.
TEST(SignalEndpointValidateTest, PerSignalEndpointWithoutPath_UsesTheRootPath)
{
    mc::Config cfg = WithSignalEndpoints("", "http://collector:4318");
    ASSERT_TRUE(mc::Validate(cfg).has_value());
    EXPECT_EQ(cfg.signal_paths.at(kTraces), "/");
}

TEST(SignalEndpointValidateTest, PerSignalEndpointWithRootPath_UsesTheRootPath)
{
    mc::Config cfg = WithSignalEndpoints("", "http://collector:4318/");
    ASSERT_TRUE(mc::Validate(cfg).has_value());
    EXPECT_EQ(cfg.signal_paths.at(kTraces), "/");
}

TEST(SignalEndpointValidateTest, PerSignalEndpointTrailingSlash_IsKept)
{
    mc::Config cfg = WithSignalEndpoints("", "", "http://collector:4318/v1/metrics/");
    ASSERT_TRUE(mc::Validate(cfg).has_value());
    EXPECT_EQ(cfg.signal_paths.at(kMetrics), "/v1/metrics/");
}

// (b) All three set, one origin, custom paths.
TEST(SignalEndpointValidateTest, AllThreeOnOneOrigin_EachPathIsUsedAsIs)
{
    mc::Config cfg = WithSignalEndpoints("",
                                         "https://otel.example:4318/a/traces",
                                         "https://otel.example:4318/b/metrics",
                                         "https://otel.example:4318/c/logs");
    const auto r = mc::Validate(cfg);
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_EQ(cfg.endpoint.host, "otel.example");
    EXPECT_EQ(cfg.signal_paths.at(kTraces), "/a/traces");
    EXPECT_EQ(cfg.signal_paths.at(kMetrics), "/b/metrics");
    EXPECT_EQ(cfg.signal_paths.at(kLogs), "/c/logs");
}

// With all three set no signal uses the base endpoint, so it is not in effect:
// its origin is not compared, and it is not even parsed.
TEST(SignalEndpointValidateTest, AllThreeSet_BaseEndpointIsNotInEffect)
{
    mc::Config cfg = WithSignalEndpoints("http://elsewhere:1234/x",
                                         "http://collector:4318/t",
                                         "http://collector:4318/m",
                                         "http://collector:4318/l");
    const auto r = mc::Validate(cfg);
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_EQ(cfg.endpoint.host, "collector");
    EXPECT_EQ(cfg.endpoint.port, 4318);
    EXPECT_TRUE(cfg.endpoint.path.empty());
}

// (c) Base plus one per-signal endpoint on the same origin.
TEST(SignalEndpointValidateTest, BasePlusOnePerSignal_SameOrigin)
{
    mc::Config cfg = WithSignalEndpoints("http://collector:4318", "", "http://collector:4318/m");
    const auto r = mc::Validate(cfg);
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_EQ(cfg.signal_paths.at(kTraces), "/v1/traces");
    EXPECT_EQ(cfg.signal_paths.at(kMetrics), "/m");
    EXPECT_EQ(cfg.signal_paths.at(kLogs), "/v1/logs");
}

// The spec's Example 3 shape on one origin: the base's path prefix applies to
// every signal on the base, metrics and logs included.
TEST(SignalEndpointValidateTest, BaseWithPathPrefix_AppliesToEverySignalOnTheBase)
{
    mc::Config cfg = WithSignalEndpoints(
        "http://collector:4318/mycollector/", "", "http://collector:4318/v1/metrics/");
    const auto r = mc::Validate(cfg);
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_EQ(cfg.signal_paths.at(kTraces), "/mycollector/v1/traces");
    EXPECT_EQ(cfg.signal_paths.at(kMetrics), "/v1/metrics/");
    EXPECT_EQ(cfg.signal_paths.at(kLogs), "/mycollector/v1/logs");
}

// No per-signal endpoint at all: the base path prefix reaches metrics and logs
// too (before this change it reached traces only).
TEST(SignalEndpointValidateTest, BaseOnlyWithPrefix_EverySignalGetsThePrefix)
{
    mc::Config cfg = WithSignalEndpoints("https://gw.example/otlp", "");
    ASSERT_TRUE(mc::Validate(cfg).has_value());
    EXPECT_EQ(cfg.endpoint.path, "/otlp");
    EXPECT_EQ(cfg.signal_paths.at(kTraces), "/otlp/v1/traces");
    EXPECT_EQ(cfg.signal_paths.at(kMetrics), "/otlp/v1/metrics");
    EXPECT_EQ(cfg.signal_paths.at(kLogs), "/otlp/v1/logs");
}

TEST(SignalEndpointValidateTest, BaseOnlyNoPath_DefaultPaths)
{
    mc::Config cfg = WithSignalEndpoints("https://gw.example", "");
    ASSERT_TRUE(mc::Validate(cfg).has_value());
    EXPECT_EQ(cfg.signal_paths.at(kTraces), "/v1/traces");
    EXPECT_EQ(cfg.signal_paths.at(kMetrics), "/v1/metrics");
    EXPECT_EQ(cfg.signal_paths.at(kLogs), "/v1/logs");
}

// The origin compares with the default port filled in.
TEST(SignalEndpointValidateTest, OmittedPortEqualsTheDefaultPort)
{
    mc::Config cfg = WithSignalEndpoints("http://collector:4318", "http://collector/v1/traces");
    const auto r = mc::Validate(cfg);
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_EQ(cfg.endpoint.port, 4318);
}

TEST(SignalEndpointValidateTest, NothingSet_StillFailsAsEndpointMalformed)
{
    mc::Config cfg;
    const auto r = mc::Validate(cfg);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().kind, mt::ConfigError::Kind::EndpointMalformed);
    EXPECT_EQ(r.error().field, "exporter.endpoint");
}

TEST(SignalEndpointValidateTest, MalformedPerSignalEndpoint_NamesTheVariable)
{
    mc::Config cfg = WithSignalEndpoints("http://collector:4318", "", "", "collector:4318/l");
    const auto r = mc::Validate(cfg);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().kind, mt::ConfigError::Kind::EndpointMalformed);
    EXPECT_EQ(r.error().field, "OTEL_EXPORTER_OTLP_LOGS_ENDPOINT");
}

// ---------------------------------------------------------------------------
// Validate — different origins are refused
// ---------------------------------------------------------------------------

// (d) Two per-signal endpoints on different hosts.
TEST(SignalEndpointValidateTest, TwoPerSignalEndpointsOnDifferentHosts_InvalidValue)
{
    mc::Config cfg = WithSignalEndpoints("", "http://a:4318/t", "http://b:4318/m");
    const auto r = mc::Validate(cfg);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().kind, mt::ConfigError::Kind::InvalidValue);
    EXPECT_EQ(r.error().field, "OTEL_EXPORTER_OTLP_METRICS_ENDPOINT");
    const std::string& msg = r.error().message;
    EXPECT_NE(msg.find("OTEL_EXPORTER_OTLP_TRACES_ENDPOINT"), std::string::npos) << msg;
    EXPECT_NE(msg.find("OTEL_EXPORTER_OTLP_METRICS_ENDPOINT"), std::string::npos) << msg;
    EXPECT_NE(msg.find("http://a:4318"), std::string::npos) << msg;
    EXPECT_NE(msg.find("http://b:4318"), std::string::npos) << msg;
    EXPECT_NE(msg.find("one connection"), std::string::npos) << msg;
}

TEST(SignalEndpointValidateTest, BaseAndPerSignalOnDifferentPorts_NamesTheBaseSource)
{
    mc::Config cfg = WithSignalEndpoints("http://collector:4318", "http://collector:4319/t");
    cfg.endpoint_source = "OTEL_EXPORTER_OTLP_ENDPOINT";
    const auto r = mc::Validate(cfg);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().kind, mt::ConfigError::Kind::InvalidValue);
    EXPECT_EQ(r.error().field, "OTEL_EXPORTER_OTLP_TRACES_ENDPOINT");
    const std::string& msg = r.error().message;
    EXPECT_NE(msg.find("OTEL_EXPORTER_OTLP_ENDPOINT (http://collector:4318)"), std::string::npos)
        << msg;
    EXPECT_NE(msg.find("OTEL_EXPORTER_OTLP_TRACES_ENDPOINT (http://collector:4319)"),
              std::string::npos)
        << msg;
}

TEST(SignalEndpointValidateTest, BaseFromFileAndPerSignalOnDifferentSchemes_NamesTheTomlKey)
{
    mc::Config cfg = WithSignalEndpoints("https://collector:4318", "", "", "http://collector:4318");
    const auto r = mc::Validate(cfg);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().kind, mt::ConfigError::Kind::InvalidValue);
    EXPECT_EQ(r.error().field, "OTEL_EXPORTER_OTLP_LOGS_ENDPOINT");
    EXPECT_NE(r.error().message.find("exporter.endpoint (https://collector:4318)"),
              std::string::npos)
        << r.error().message;
}

// ---------------------------------------------------------------------------
// Validate — OTLP/gRPC
// ---------------------------------------------------------------------------

TEST(SignalEndpointValidateTest, GrpcSchemeOnAPerSignalEndpoint_SelectsGrpcAndItsDefaultPort)
{
    mc::Config cfg = WithSignalEndpoints("", "grpc://collector");
    const auto r = mc::Validate(cfg);
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_EQ(cfg.protocol, mt::Protocol::Grpc);
    EXPECT_EQ(cfg.endpoint.scheme, "http");
    EXPECT_EQ(cfg.endpoint.port, 4317);
    EXPECT_TRUE(cfg.config_warnings.empty());
}

TEST(SignalEndpointValidateTest, GrpcShorthandAndHttpSchemeShareAnOrigin)
{
    mc::Config cfg = WithSignalEndpoints("http://collector:4317", "grpc://collector:4317");
    const auto r = mc::Validate(cfg);
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_EQ(cfg.protocol, mt::Protocol::Grpc);
}

TEST(SignalEndpointValidateTest, GrpcSchemeOnAPerSignalEndpoint_ExplicitHttp_ProtocolMismatch)
{
    mc::Config cfg = WithSignalEndpoints("", "", "grpcs://collector:4317");
    cfg.protocol = mt::Protocol::Http;
    cfg.protocol_explicit = true;
    const auto r = mc::Validate(cfg);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().kind, mt::ConfigError::Kind::ProtocolMismatch);
}

// (e) A gRPC per-signal endpoint contributes only its origin.
TEST(SignalEndpointValidateTest, GrpcPerSignalEndpointWithPath_PathIgnoredWithOneWarning)
{
    mc::Config cfg = WithSignalEndpoints("", "http://collector:4317/v1/traces");
    cfg.protocol = mt::Protocol::Grpc;
    cfg.protocol_explicit = true;
    const auto r = mc::Validate(cfg);
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_EQ(cfg.endpoint.host, "collector");
    EXPECT_EQ(cfg.endpoint.port, 4317);
    EXPECT_TRUE(cfg.endpoint.path.empty());
    ASSERT_EQ(cfg.config_warnings.size(), 1U);
    EXPECT_NE(cfg.config_warnings.front().find("OTEL_EXPORTER_OTLP_TRACES_ENDPOINT"),
              std::string::npos);
    EXPECT_NE(cfg.config_warnings.front().find("/v1/traces"), std::string::npos);
    EXPECT_NE(cfg.config_warnings.front().find("ignored"), std::string::npos);
}

TEST(SignalEndpointValidateTest, GrpcPerSignalEndpointWithRootPath_DoesNotWarn)
{
    mc::Config cfg = WithSignalEndpoints("", "grpc://collector:4317/");
    ASSERT_TRUE(mc::Validate(cfg).has_value());
    EXPECT_TRUE(cfg.config_warnings.empty());
}

// The base endpoint keeps its stricter rule: a path on it is refused.
TEST(SignalEndpointValidateTest, GrpcBaseEndpointWithPath_StillProtocolMismatch)
{
    mc::Config cfg = WithSignalEndpoints("grpc://collector:4317/x", "grpc://collector:4317");
    const auto r = mc::Validate(cfg);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().kind, mt::ConfigError::Kind::ProtocolMismatch);
    EXPECT_EQ(r.error().field, "exporter.endpoint");
}

TEST(SignalEndpointValidateTest, Grpc_SignalPathsAreLeftEmpty)
{
    mc::Config cfg = WithSignalEndpoints("grpc://collector:4317", "");
    ASSERT_TRUE(mc::Validate(cfg).has_value());
    for (const std::string& path : cfg.signal_paths)
    {
        EXPECT_TRUE(path.empty()) << path;
    }
}
