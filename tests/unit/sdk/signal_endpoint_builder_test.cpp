// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0
//
// SdkBuilder::Build and the per-signal OTLP exporter variables
// (`docs/configuration.md` §2.2, §3.3): the endpoints in effect must share one
// origin, a per-signal endpoint under OTLP/gRPC contributes only its origin,
// `WithEndpoint` in code outranks every endpoint variable, and every other
// per-signal exporter variable is reported with one Warn. The request paths
// themselves are checked in tests/unit/common/config/signal_endpoint_test.cpp
// and on the wire in tests/integration/sdk/per_signal_endpoint_test.cpp.

#include "microtel/error.hpp"
#include "microtel/log_sink.hpp"
#include "microtel/protocol.hpp"
#include "microtel/provider.hpp"
#include "microtel/sdk_builder.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <initializer_list>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace mt = microtel;

namespace
{

constexpr std::chrono::milliseconds kShutdown{200};

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

/// Captures microtel's Warn lines for the scope, then restores the default sink.
class WarnCapture
{
public:
    WarnCapture()
    {
        mt::SetLogSink(
            [this](mt::LogLevel level, std::string_view message)
            {
                if (level == mt::LogLevel::Warn)
                {
                    const std::scoped_lock lock{m_mu};
                    m_warns.emplace_back(message);
                }
            });
    }
    ~WarnCapture()
    {
        mt::ResetLogSink();
    }
    WarnCapture(const WarnCapture&) = delete;
    WarnCapture& operator=(const WarnCapture&) = delete;
    WarnCapture(WarnCapture&&) = delete;
    WarnCapture& operator=(WarnCapture&&) = delete;

    /// @brief How many Warn lines contain every one of @p needles (all, if none).
    [[nodiscard]] std::size_t Count(std::initializer_list<std::string_view> needles = {}) const
    {
        const std::scoped_lock lock{m_mu};
        return static_cast<std::size_t>(std::ranges::count_if(
            m_warns,
            [needles](const std::string& line)
            {
                return std::ranges::all_of(needles,
                                           [&line](std::string_view needle)
                                           { return line.find(needle) != std::string::npos; });
            }));
    }

private:
    mutable std::mutex m_mu;
    std::vector<std::string> m_warns;
};

/// Build, and shut the provider down again so no I/O thread outlives the test.
template <typename Builder>
[[nodiscard]] mt::Expected<std::shared_ptr<mt::Provider>, mt::ConfigError> BuildAndStop(
    Builder&& builder)
{
    auto result = std::forward<Builder>(builder).Build();
    if (result)
    {
        std::ignore = (*result)->Shutdown(kShutdown);
    }
    return result;
}

}  // namespace

// (a) A lone per-signal endpoint is the only endpoint configured.
TEST(SignalEndpointBuilderTest, LoneTracesEndpoint_BuildSucceedsWithoutWarning)
{
    const WarnCapture warns;
    const EnvVar t{"OTEL_EXPORTER_OTLP_TRACES_ENDPOINT", "http://collector:4318/v1/traces"};
    const auto result = BuildAndStop(mt::SdkBuilder{});
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(warns.Count(), 0U);
}

// (b) All three on one origin, each with its own path.
TEST(SignalEndpointBuilderTest, AllThreeOnOneOrigin_BuildSucceeds)
{
    const EnvVar t{"OTEL_EXPORTER_OTLP_TRACES_ENDPOINT", "http://collector:4318/a"};
    const EnvVar m{"OTEL_EXPORTER_OTLP_METRICS_ENDPOINT", "http://collector:4318/b"};
    const EnvVar l{"OTEL_EXPORTER_OTLP_LOGS_ENDPOINT", "http://collector:4318/c"};
    const auto result = BuildAndStop(mt::SdkBuilder{});
    ASSERT_TRUE(result.has_value()) << result.error().message;
}

// (c) The base endpoint and one per-signal endpoint on the same origin.
TEST(SignalEndpointBuilderTest, BasePlusOnePerSignalOnOneOrigin_BuildSucceeds)
{
    const EnvVar base{"OTEL_EXPORTER_OTLP_ENDPOINT", "http://collector:4318"};
    const EnvVar l{"OTEL_EXPORTER_OTLP_LOGS_ENDPOINT", "http://collector:4318/custom/logs"};
    const auto result = BuildAndStop(mt::SdkBuilder{});
    ASSERT_TRUE(result.has_value()) << result.error().message;
}

// (d) Different origins cannot share the one connection.
TEST(SignalEndpointBuilderTest, DifferentOrigins_BuildFailsNamingBothVariables)
{
    const EnvVar base{"OTEL_EXPORTER_OTLP_ENDPOINT", "http://collector:4318"};
    const EnvVar m{"OTEL_EXPORTER_OTLP_METRICS_ENDPOINT", "http://metrics-gw:4318/v1/metrics"};
    const auto result = BuildAndStop(mt::SdkBuilder{});
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, mt::ConfigError::Kind::InvalidValue);
    EXPECT_EQ(result.error().field, "OTEL_EXPORTER_OTLP_METRICS_ENDPOINT");
    const std::string& msg = result.error().message;
    EXPECT_NE(msg.find("OTEL_EXPORTER_OTLP_ENDPOINT (http://collector:4318)"), std::string::npos)
        << msg;
    EXPECT_NE(msg.find("OTEL_EXPORTER_OTLP_METRICS_ENDPOINT (http://metrics-gw:4318)"),
              std::string::npos)
        << msg;
    EXPECT_NE(msg.find("one connection"), std::string::npos) << msg;
}

// (e) Under OTLP/gRPC a per-signal endpoint contributes its origin only.
TEST(SignalEndpointBuilderTest, GrpcPerSignalEndpointWithPath_OneWarnAndBuildSucceeds)
{
    const WarnCapture warns;
    const EnvVar proto{"OTEL_EXPORTER_OTLP_PROTOCOL", "grpc"};
    const EnvVar t{"OTEL_EXPORTER_OTLP_TRACES_ENDPOINT", "http://collector:4317/v1/traces"};
    const auto result = BuildAndStop(mt::SdkBuilder{});
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(warns.Count(), 1U);
    EXPECT_EQ(warns.Count({"OTEL_EXPORTER_OTLP_TRACES_ENDPOINT", "/v1/traces", "ignored"}), 1U);
}

// (f) Every other per-signal exporter variable: one Warn naming it.
TEST(SignalEndpointBuilderTest, EachUnsupportedPerSignalVariable_OneWarnNamingIt)
{
    constexpr std::array<const char*, 3> kSignals{"TRACES", "METRICS", "LOGS"};
    constexpr std::array<const char*, 8> kSettings{"HEADERS",
                                                   "PROTOCOL",
                                                   "TIMEOUT",
                                                   "COMPRESSION",
                                                   "CERTIFICATE",
                                                   "CLIENT_KEY",
                                                   "CLIENT_CERTIFICATE",
                                                   "INSECURE"};
    for (const char* signal : kSignals)
    {
        for (const char* setting : kSettings)
        {
            const std::string name =
                std::string{"OTEL_EXPORTER_OTLP_"} + signal + "_" + std::string{setting};
            SCOPED_TRACE(name);
            const WarnCapture warns;
            const EnvVar v{name.c_str(), "x"};
            const auto result =
                BuildAndStop(mt::SdkBuilder{}.WithEndpoint("http://collector:4318"));
            ASSERT_TRUE(result.has_value()) << result.error().message;
            EXPECT_EQ(warns.Count(), 1U);
            EXPECT_EQ(warns.Count({name, "not supported"}), 1U);
        }
    }
}

// (g) WithEndpoint in code outranks the environment for every signal: the
// per-signal variable is not in effect, so its origin is not compared, and it
// is reported rather than dropped silently.
TEST(SignalEndpointBuilderTest, WithEndpoint_OutranksPerSignalVariablesWithOneWarnEach)
{
    const WarnCapture warns;
    const EnvVar t{"OTEL_EXPORTER_OTLP_TRACES_ENDPOINT", "http://elsewhere:4318/v1/traces"};
    const EnvVar l{"OTEL_EXPORTER_OTLP_LOGS_ENDPOINT", "not a url"};
    const auto result = BuildAndStop(mt::SdkBuilder{}.WithEndpoint("http://collector:4318"));
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(warns.Count(), 2U);
    EXPECT_EQ(warns.Count({"OTEL_EXPORTER_OTLP_TRACES_ENDPOINT", "WithEndpoint"}), 1U);
    EXPECT_EQ(warns.Count({"OTEL_EXPORTER_OTLP_LOGS_ENDPOINT", "WithEndpoint"}), 1U);
}

TEST(SignalEndpointBuilderTest, WithEndpointAndNoPerSignalVariable_NoWarn)
{
    const WarnCapture warns;
    const auto result = BuildAndStop(mt::SdkBuilder{}.WithEndpoint("http://collector:4318"));
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(warns.Count(), 0U);
}

// A per-signal variable outranks the file's base endpoint for its own signal,
// and therefore takes part in the origin check against it.
TEST(SignalEndpointBuilderTest, PerSignalVariableOverDifferentFileEndpoint_BuildFails)
{
    const EnvVar base{"OTEL_EXPORTER_OTLP_ENDPOINT", "https://collector:4318"};
    const EnvVar t{"OTEL_EXPORTER_OTLP_TRACES_ENDPOINT", "https://collector:443/v1/traces"};
    const auto result = BuildAndStop(mt::SdkBuilder{});
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, mt::ConfigError::Kind::InvalidValue);
}

// A gRPC per-signal endpoint selects gRPC, as the base endpoint's would.
TEST(SignalEndpointBuilderTest, GrpcSchemeOnLonePerSignalEndpoint_BuildSucceeds)
{
    const EnvVar l{"OTEL_EXPORTER_OTLP_LOGS_ENDPOINT", "grpc://collector"};
    const auto result = BuildAndStop(mt::SdkBuilder{});
    ASSERT_TRUE(result.has_value()) << result.error().message;
}
