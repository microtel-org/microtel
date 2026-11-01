// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0
//
// Unit tests for M6-C: SdkBuilder::Build() and SdkProvider lifecycle.

#include "microtel/sdk_builder.hpp"

#include "microtel/attribute.hpp"
#include "microtel/error.hpp"
#include "microtel/log_sink.hpp"
#include "microtel/meter.hpp"
#include "microtel/provider.hpp"
#include "microtel/resource.hpp"
#include "microtel/resource_detectors.hpp"
#include "microtel/sampler.hpp"
#include "microtel/status.hpp"

#include "fakes/fake_resource_detector.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <unistd.h>

// ---------------------------------------------------------------------------
// Build — validation / consumed guard
// ---------------------------------------------------------------------------

TEST(SdkBuilderTest, Build_MinimalConfig_Succeeds)
{
    auto result = microtel::SdkBuilder().WithEndpoint("https://localhost:4318").Build();
    ASSERT_TRUE(result.has_value());
    EXPECT_NE(*result, nullptr);
}

TEST(SdkBuilderTest, Build_GrpcProtocol_Succeeds)
{
    auto result = microtel::SdkBuilder()
                      .WithEndpoint("https://localhost:4317")
                      .WithProtocol(microtel::Protocol::Grpc)
                      .Build();
    ASSERT_TRUE(result.has_value());
}

TEST(SdkBuilderTest, Build_AllOptions_Succeeds)
{
    auto result = microtel::SdkBuilder()
                      .WithEndpoint("https://localhost:4318")
                      .WithProtocol(microtel::Protocol::Http)
                      .WithCompressionGzip(false)
                      .WithServiceName("svc")
                      .WithServiceVersion("2.0")
                      .WithBatch({.max_queue_size = 512, .max_export_batch_size = 64})
                      .WithSampler(microtel::MakeAlwaysOnSampler())
                      .Build();
    ASSERT_TRUE(result.has_value());
}

TEST(SdkBuilderTest, Build_CodeTablesMergeOverEnvTables_Succeeds)
{
    // Issue #257: WithResource / WithHeaders merge per key over the env layer
    // rather than replacing it. The per-key results are asserted in
    // config_test's TableMergeTest, through the same config::MergeResourceAttrs
    // / MergeHeaders calls; Build() exposes no resolved Config to assert on
    // here, so this drives the builder's code-layer path end to end.
    struct EnvGuard
    {
        EnvGuard() = default;
        EnvGuard(const EnvGuard&) = delete;
        EnvGuard& operator=(const EnvGuard&) = delete;
        EnvGuard(EnvGuard&&) = delete;
        EnvGuard& operator=(EnvGuard&&) = delete;
        ~EnvGuard() noexcept
        {
            (void)unsetenv("OTEL_RESOURCE_ATTRIBUTES");
            (void)unsetenv("OTEL_EXPORTER_OTLP_HEADERS");
        }
    } const guard;

    (void)setenv("OTEL_RESOURCE_ATTRIBUTES", "a=env-a,b=env-b", 1);
    (void)setenv("OTEL_EXPORTER_OTLP_HEADERS", "X-Tenant=acme,X-Region=eu", 1);

    auto result = microtel::SdkBuilder()
                      .WithEndpoint("https://localhost:4318")
                      .WithResource({{.key = "b", .value = std::string{"code-b"}}})
                      .WithHeaders({{.key = "x-tenant", .value = std::string{"globex"}}})
                      .Build();
    ASSERT_TRUE(result.has_value());
}

TEST(SdkBuilderTest, Build_CalledTwice_SecondCallReturnsConsumedError)
{
    microtel::SdkBuilder builder;
    builder.WithEndpoint("https://localhost:4318");
    (void)builder.Build();
    const auto result = builder.Build();
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, microtel::ConfigError::Kind::BuildAlreadyConsumed);
}

TEST(SdkBuilderTest, Build_EmptyEndpoint_ReturnsEndpointMalformed)
{
    const auto result = microtel::SdkBuilder().Build();
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, microtel::ConfigError::Kind::EndpointMalformed);
}

TEST(SdkBuilderTest, Build_InvalidEndpointUrl_ReturnsEndpointMalformed)
{
    const auto result = microtel::SdkBuilder().WithEndpoint("not-a-url").Build();
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, microtel::ConfigError::Kind::EndpointMalformed);
}

TEST(SdkBuilderTest, Build_GrpcWithPath_ReturnsProtocolMismatch)
{
    const auto result = microtel::SdkBuilder()
                            .WithEndpoint("https://localhost:4317/v1/traces")
                            .WithProtocol(microtel::Protocol::Grpc)
                            .Build();
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, microtel::ConfigError::Kind::ProtocolMismatch);
}

// Issue #203: the `grpc://` shorthand selects OTLP/gRPC, so the gRPC rule that
// an endpoint carries no path applies to it. Before the fix this built an
// OTLP/HTTP pipeline aimed at a gRPC port, and succeeded.
TEST(SdkBuilderTest, Build_GrpcSchemeWithPath_ReturnsProtocolMismatch)
{
    const auto result =
        microtel::SdkBuilder().WithEndpoint("grpc://localhost:4317/v1/traces").Build();
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, microtel::ConfigError::Kind::ProtocolMismatch);
}

TEST(SdkBuilderTest, Build_GrpcSchemeWithExplicitHttpProtocol_ReturnsProtocolMismatch)
{
    const auto result = microtel::SdkBuilder()
                            .WithEndpoint("grpc://localhost:4317")
                            .WithProtocol(microtel::Protocol::Http)
                            .Build();
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, microtel::ConfigError::Kind::ProtocolMismatch);
    EXPECT_EQ(result.error().field, "exporter.protocol");
}

// ---------------------------------------------------------------------------
// Provider lifecycle
// ---------------------------------------------------------------------------

TEST(SdkBuilderTest, Provider_GetTracer_ReturnsNonNull)
{
    auto result = microtel::SdkBuilder().WithEndpoint("https://localhost:4318").Build();
    ASSERT_TRUE(result.has_value());
    const auto tracer = (*result)->GetTracer("test.lib", "1.0");
    EXPECT_NE(tracer, nullptr);
}

TEST(SdkBuilderTest, Provider_GetTracer_EmptyVersion_ReturnsNonNull)
{
    auto result = microtel::SdkBuilder().WithEndpoint("https://localhost:4318").Build();
    ASSERT_TRUE(result.has_value());
    const auto tracer = (*result)->GetTracer("test.lib");
    EXPECT_NE(tracer, nullptr);
}

TEST(SdkBuilderTest, Provider_ForceFlush_EmptyQueue_ReturnsCompleted)
{
    auto result = microtel::SdkBuilder().WithEndpoint("https://localhost:4318").Build();
    ASSERT_TRUE(result.has_value());
    const auto status = (*result)->ForceFlush(std::chrono::milliseconds(500));
    EXPECT_EQ(status, microtel::Status::Completed);
}

TEST(SdkBuilderTest, Provider_Shutdown_ReturnsCompleted)
{
    auto result = microtel::SdkBuilder().WithEndpoint("https://localhost:4318").Build();
    ASSERT_TRUE(result.has_value());
    const auto status = (*result)->Shutdown(std::chrono::milliseconds(500));
    EXPECT_EQ(status, microtel::Status::Completed);
}

TEST(SdkBuilderTest, Provider_Shutdown_Idempotent_SecondCallReturnsAlreadyShutDown)
{
    auto result = microtel::SdkBuilder().WithEndpoint("https://localhost:4318").Build();
    ASSERT_TRUE(result.has_value());
    (void)(*result)->Shutdown(std::chrono::milliseconds(500));
    const auto status = (*result)->Shutdown(std::chrono::milliseconds(500));
    EXPECT_EQ(status, microtel::Status::AlreadyShutDown);
}

TEST(SdkBuilderTest, Provider_GetExporterHealth_ReturnsSnapshot)
{
    auto result = microtel::SdkBuilder().WithEndpoint("https://localhost:4318").Build();
    ASSERT_TRUE(result.has_value());
    // Just verify it doesn't crash and returns a snapshot with a valid state.
    const auto health = (*result)->GetExporterHealth();
    EXPECT_EQ(health.batches_sent, 0U);
}

// ---------------------------------------------------------------------------
// WithMetricInterval (M12)
// ---------------------------------------------------------------------------

TEST(SdkBuilderTest, WithMetricInterval_BuildSucceeds)
{
    auto result = microtel::SdkBuilder()
                      .WithEndpoint("https://localhost:4318")
                      .WithMetricInterval(std::chrono::seconds(5))
                      .Build();
    ASSERT_TRUE(result.has_value());
}

// ---------------------------------------------------------------------------
// WithMetricTemporality (M12 — increment 21)
// ---------------------------------------------------------------------------

TEST(SdkBuilderTest, WithMetricTemporality_Delta_BuildSucceeds)
{
    auto result = microtel::SdkBuilder()
                      .WithEndpoint("https://localhost:4318")
                      .WithMetricTemporality(microtel::TemporalityPreference::Delta)
                      .Build();
    ASSERT_TRUE(result.has_value());
}

TEST(SdkBuilderTest, WithMetricTemporality_LowMemory_BuildSucceeds)
{
    auto result = microtel::SdkBuilder()
                      .WithEndpoint("https://localhost:4318")
                      .WithMetricTemporality(microtel::TemporalityPreference::LowMemory)
                      .Build();
    ASSERT_TRUE(result.has_value());
}

// ---------------------------------------------------------------------------
// WithMetricLimits (M12 increment 27)
// ---------------------------------------------------------------------------

TEST(SdkBuilderTest, WithMetricLimits_BuildSucceeds)
{
    auto result = microtel::SdkBuilder()
                      .WithEndpoint("https://localhost:4318")
                      .WithMetricLimits(microtel::MetricLimitOptions{.max_cardinality = 5})
                      .Build();
    ASSERT_TRUE(result.has_value());
}

TEST(SdkBuilderTest, EnvVarOverridesDefaultCardinality)
{
    // RAII guard: ensure the env var is removed even if the test fails early.
    struct EnvGuard
    {
        EnvGuard() = default;
        EnvGuard(const EnvGuard&) = delete;
        EnvGuard& operator=(const EnvGuard&) = delete;
        EnvGuard(EnvGuard&&) = delete;
        EnvGuard& operator=(EnvGuard&&) = delete;
        ~EnvGuard() noexcept
        {
            (void)unsetenv("MICROTEL_METRIC_CARDINALITY_LIMIT");
        }
    } const guard;

    (void)setenv("MICROTEL_METRIC_CARDINALITY_LIMIT", "3", 1);

    auto result = microtel::SdkBuilder().WithEndpoint("https://localhost:4318").Build();
    ASSERT_TRUE(result.has_value());

    auto meter = (*result)->GetMeter("test.lib");
    const auto counter = meter->CreateCounter<std::int64_t>("c", "", "");

    // 4 distinct attribute sets with cap=3: the 4th must overflow.
    for (std::size_t i = 0; i < 4; ++i)
    {
        const microtel::KeyValue kv{.key = "i", .value = static_cast<std::int64_t>(i)};
        counter->Add(1, {&kv, 1});
    }

    constexpr auto kIdx = static_cast<std::size_t>(microtel::DropReason::CardinalityOverflow);
    const auto health = (*result)->GetExporterHealth();
    EXPECT_EQ(health.drop_counters[kIdx], 1U);
}

TEST(SdkBuilderTest, WithMetricLimitsOverridesEnvVar)
{
    // WithMetricLimits has higher precedence than the env var.
    struct EnvGuard
    {
        EnvGuard() = default;
        EnvGuard(const EnvGuard&) = delete;
        EnvGuard& operator=(const EnvGuard&) = delete;
        EnvGuard(EnvGuard&&) = delete;
        EnvGuard& operator=(EnvGuard&&) = delete;
        ~EnvGuard() noexcept
        {
            (void)unsetenv("MICROTEL_METRIC_CARDINALITY_LIMIT");
        }
    } const guard;

    (void)setenv("MICROTEL_METRIC_CARDINALITY_LIMIT", "100", 1);

    auto result = microtel::SdkBuilder()
                      .WithEndpoint("https://localhost:4318")
                      .WithMetricLimits(microtel::MetricLimitOptions{.max_cardinality = 3})
                      .Build();
    ASSERT_TRUE(result.has_value());

    auto meter = (*result)->GetMeter("test.lib");
    const auto counter = meter->CreateCounter<std::int64_t>("c", "", "");

    // cap is 3 (from WithMetricLimits, not 100 from env); 4th set overflows.
    for (std::size_t i = 0; i < 4; ++i)
    {
        const microtel::KeyValue kv{.key = "i", .value = static_cast<std::int64_t>(i)};
        counter->Add(1, {&kv, 1});
    }

    constexpr auto kIdx = static_cast<std::size_t>(microtel::DropReason::CardinalityOverflow);
    const auto health = (*result)->GetExporterHealth();
    EXPECT_EQ(health.drop_counters[kIdx], 1U);
}

// ---------------------------------------------------------------------------
// End-to-end drop accounting — issue #169. Each of these reaches the counter
// the way an application does: through the public builder, tracer and
// GetExporterHealth(), with none of the SDK's seams mocked out.
// ---------------------------------------------------------------------------

namespace
{

std::uint64_t DropCount(const microtel::HealthSnapshot& health, microtel::DropReason reason)
{
    return health.drop_counters.at(static_cast<std::size_t>(reason));
}

/// Short deadlines so a provider pointed at a dead endpoint tears down in
/// milliseconds instead of burning the default 10s export deadline.
constexpr microtel::TimeoutOptions kFailFastTimeouts{
    .connect = std::chrono::milliseconds(200),
    .tls_handshake = std::chrono::milliseconds(200),
    .per_export = std::chrono::milliseconds(200),
    .retry_budget = std::chrono::milliseconds(200),
    .flush = std::chrono::milliseconds(500),
    .shutdown = std::chrono::milliseconds(500),
};

}  // namespace

TEST(SdkBuilderTest, EndToEnd_SpanLimitAndPostShutdownDropsReachGetExporterHealth)
{
    auto result = microtel::SdkBuilder()
                      .WithEndpoint("http://127.0.0.1:1")  // nothing listening; never connects
                      .WithTimeouts(kFailFastTimeouts)
                      .WithSpanLimits(microtel::SpanLimitOptions{
                          .attribute_count_limit = 1,
                          .event_count_limit = 1,
                          .link_count_limit = 1,
                      })
                      .Build();
    ASSERT_TRUE(result.has_value());

    const auto tracer = (*result)->GetTracer("test.lib", "1.0");
    // Shut the pipeline down first, so the finished span is refused at the
    // processor and never reaches the wire. The record-shaping counters are
    // recorded in the Span API, well before the processor sees the record, so
    // this exercises the full path without the test waiting out a retry loop
    // against a dead endpoint.
    ASSERT_NE((*result)->Shutdown(std::chrono::seconds(2)), microtel::Status::Failed);

    {
        auto span = tracer->StartSpan("op");
        span->SetAttribute("a", std::int64_t{1});
        span->SetAttribute("b", std::int64_t{2});  // over the limit
        span->AddEvent("e1");
        span->AddEvent("e2");  // over the limit
        span->AddLink(span->GetContext());
        span->AddLink(span->GetContext());  // over the limit
        span->End();
    }

    const auto health = (*result)->GetExporterHealth();
    EXPECT_EQ(DropCount(health, microtel::DropReason::SpanAttributeLimit), 1U);
    EXPECT_EQ(DropCount(health, microtel::DropReason::SpanEventLimit), 1U);
    EXPECT_EQ(DropCount(health, microtel::DropReason::SpanLinkLimit), 1U);
    EXPECT_EQ(DropCount(health, microtel::DropReason::PostShutdown), 1U);
}

TEST(SdkBuilderTest, EndToEnd_ConnectFailureReachesGetExporterHealth)
{
    // Port 1 is reserved and never listening, so the connect fails fast
    // without depending on a collector being absent from a common port.
    auto result = microtel::SdkBuilder()
                      .WithEndpoint("http://127.0.0.1:1")
                      .WithTimeouts(kFailFastTimeouts)
                      .Build();
    ASSERT_TRUE(result.has_value());

    ASSERT_FALSE((*result)->Connect().has_value());

    EXPECT_EQ(DropCount((*result)->GetExporterHealth(), microtel::DropReason::ConnectFailure), 1U);
}

// ---------------------------------------------------------------------------
// Destructor without Shutdown
//
// CLAUDE.md rule 15 and threading-model.md §6.2: the destructor invokes
// Shutdown with a small finite timeout if not already shut down, is noexcept,
// and does not block indefinitely. Every other lifecycle test in this file
// calls Shutdown explicitly first, so until now nothing exercised the path
// where the destructor is the only thing that stops the batch-processor worker
// and the transport's I/O thread. An application that simply drops its
// provider on the way out takes exactly this path.
// ---------------------------------------------------------------------------

namespace
{

/// Deliberately generous: the assertion is that teardown *terminates*, not a
/// latency budget. The real bound is the destructor's own Shutdown timeout
/// (`kProviderDestructorTimeout`, 5s in `sdk_provider.cpp`); a TSAN or ASAN
/// build runs several times slower, and ctest's per-test timeout is the
/// backstop for a destructor that wedges rather than merely running late.
constexpr auto kDestructorTeardownBound = std::chrono::seconds(30);

}  // namespace

TEST(SdkBuilderTest, Provider_DestroyedWithoutShutdown_TearsDownWithinBound)
{
    const auto start = std::chrono::steady_clock::now();
    {
        auto result = microtel::SdkBuilder()
                          .WithEndpoint("http://127.0.0.1:1")  // nothing listening
                          .WithTimeouts(kFailFastTimeouts)
                          .Build();
        ASSERT_TRUE(result.has_value());

        // A live provider, not a freshly built and untouched one: a tracer
        // handed out and a span ended, so the worker has real work queued and
        // the export path has been entered when the destructor runs.
        //
        // Declared after `result` so it is destroyed before it — the tracer
        // holds borrowed pointers into provider-owned state.
        const auto tracer = (*result)->GetTracer("dtor.test", "1.0");
        ASSERT_NE(tracer, nullptr);
        {
            auto span = tracer->StartSpan("never-explicitly-flushed");
            span->SetAttribute("k", std::int64_t{1});
        }
        // No Shutdown(), no ForceFlush(). Scope exit is the whole teardown.
    }

    EXPECT_LT(std::chrono::steady_clock::now() - start, kDestructorTeardownBound);
}

TEST(SdkBuilderTest, Provider_DestroyedImmediatelyAfterBuild_TearsDownWithinBound)
{
    // The other half of the gap: threads exist from construction (the batch
    // processor's worker, and the transport's I/O thread from
    // Http2Transport::Create), so a provider that is built and dropped without
    // a single API call still has to join two threads.
    const auto start = std::chrono::steady_clock::now();
    {
        auto result = microtel::SdkBuilder()
                          .WithEndpoint("http://127.0.0.1:1")
                          .WithTimeouts(kFailFastTimeouts)
                          .Build();
        ASSERT_TRUE(result.has_value());
    }

    EXPECT_LT(std::chrono::steady_clock::now() - start, kDestructorTeardownBound);
}

// ---------------------------------------------------------------------------
// Build()-time warnings
//
// `insecure = true` is permitted by configuration.md §3.5 but is very likely
// to be a mistake, and `config::Validate` returns `Expected<void, ConfigError>`
// — it can reject but it cannot warn. Plaintext OTLP/HTTP used to warn too;
// since ICP 0042 it reaches HTTP/1.1-only receivers, so it no longer does.
// ---------------------------------------------------------------------------

namespace
{

/// Captures microtel's internal log emissions for the duration of a scope, and
/// restores the default routing on the way out.
class LogCapture
{
public:
    LogCapture()
    {
        microtel::SetLogSink([this](microtel::LogLevel level, std::string_view message)
                             { m_entries.emplace_back(level, std::string{message}); });
    }

    ~LogCapture()
    {
        microtel::ResetLogSink();
    }

    LogCapture(const LogCapture&) = delete;
    LogCapture& operator=(const LogCapture&) = delete;
    LogCapture(LogCapture&&) = delete;
    LogCapture& operator=(LogCapture&&) = delete;

    [[nodiscard]] bool WarnedAbout(std::string_view needle) const
    {
        return std::ranges::any_of(m_entries,
                                   [needle](const auto& entry)
                                   {
                                       return entry.first == microtel::LogLevel::Warn &&
                                              entry.second.find(needle) != std::string::npos;
                                   });
    }

    /// @brief How many Info lines contain every one of `needles`.
    [[nodiscard]] std::size_t InfoCount(std::initializer_list<std::string_view> needles) const
    {
        return Count(microtel::LogLevel::Info, needles);
    }

    /// @brief How many Warn lines contain every one of `needles` (all, if none).
    [[nodiscard]] std::size_t WarnCount(std::initializer_list<std::string_view> needles = {}) const
    {
        return Count(microtel::LogLevel::Warn, needles);
    }

private:
    [[nodiscard]] std::size_t Count(microtel::LogLevel level,
                                    std::initializer_list<std::string_view> needles) const
    {
        return static_cast<std::size_t>(std::ranges::count_if(
            m_entries,
            [level, needles](const auto& entry)
            {
                return entry.first == level &&
                       std::ranges::all_of(
                           needles,
                           [&entry](std::string_view needle)
                           { return entry.second.find(needle) != std::string::npos; });
            }));
    }

    std::vector<std::pair<microtel::LogLevel, std::string>> m_entries;
};

constexpr std::string_view kPlaintextNeedle = "plaintext OTLP/HTTP";
constexpr std::string_view kInsecureNeedle = "insecure";

}  // namespace

// ICP 0042: plaintext OTLP/HTTP falls back to HTTP/1.1, so the quick-start
// configuration works and there is nothing left to warn about.
TEST(SdkBuilderTest, Build_PlaintextHttpEndpoint_DoesNotWarn)
{
    const LogCapture capture;
    const auto result = microtel::SdkBuilder()
                            .WithEndpoint("http://localhost:4318")
                            .WithProtocol(microtel::Protocol::Http)
                            .Build();

    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_FALSE(capture.WarnedAbout(kPlaintextNeedle));
    EXPECT_EQ(capture.WarnCount(), 0U);
}

TEST(SdkBuilderTest, Build_PlaintextGrpcEndpoint_DoesNotWarn)
{
    const LogCapture capture;
    const auto result = microtel::SdkBuilder()
                            .WithEndpoint("http://localhost:4317")
                            .WithProtocol(microtel::Protocol::Grpc)
                            .Build();

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(capture.WarnCount(), 0U);
}

TEST(SdkBuilderTest, Build_HttpsEndpoint_DoesNotWarn)
{
    const LogCapture capture;
    const auto result = microtel::SdkBuilder()
                            .WithEndpoint("https://localhost:4318")
                            .WithProtocol(microtel::Protocol::Http)
                            .Build();

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(capture.WarnCount(), 0U);
}

// Issue #284: configuration.md §5 — each Build() logs its resolved Resource once, at
// Info, naming the profile, so two named providers give two distinct lines.
TEST(SdkBuilderTest, Build_MultiProfile_LogsOneResolvedResourceLinePerProvider)
{
    const LogCapture capture;
    const auto first = microtel::SdkBuilder()
                           .WithEndpoint("https://localhost:4318")
                           .WithServiceName("svc-one")
                           .WithProfileName("resource-log-one")
                           .Build();
    const auto second = microtel::SdkBuilder()
                            .WithEndpoint("https://localhost:4318")
                            .WithServiceName("svc-two")
                            .WithProfileName("resource-log-two")
                            .Build();

    ASSERT_TRUE(first.has_value()) << first.error().message;
    ASSERT_TRUE(second.has_value()) << second.error().message;
    EXPECT_EQ(capture.InfoCount({"resolved resource"}), 2U);
    EXPECT_EQ(capture.InfoCount({"resolved resource", "\"resource-log-one\"", "svc-one"}), 1U);
    EXPECT_EQ(capture.InfoCount({"resolved resource", "\"resource-log-two\"", "svc-two"}), 1U);
}

TEST(SdkBuilderTest, Build_InsecureTls_Warns)
{
    const LogCapture capture;
    const auto result = microtel::SdkBuilder()
                            .WithEndpoint("https://localhost:4318")
                            .WithTls(microtel::TlsOptions{.insecure = true,
                                                          .ca_bundle = {},
                                                          .client_cert = {},
                                                          .client_key = {},
                                                          .sni_override = {}})
                            .Build();

    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(capture.WarnedAbout(kInsecureNeedle))
        << "configuration.md §3.5 promises a Warn line for insecure = true";
}

TEST(SdkBuilderTest, Build_VerifiedTls_DoesNotWarnAboutInsecure)
{
    const LogCapture capture;
    const auto result = microtel::SdkBuilder().WithEndpoint("https://localhost:4318").Build();

    ASSERT_TRUE(result.has_value());
    EXPECT_FALSE(capture.WarnedAbout(kInsecureNeedle));
}

// ---------------------------------------------------------------------------
// [config] unknown_keys = "warn" | "ignore"
//
// "warn" promises one Warn line per unknown key and a successful Build();
// "ignore" promises silence. Before the fix "warn" was silent too.
// ---------------------------------------------------------------------------

namespace
{

/// Writes a `microtel.toml` with the given body, and removes it on destruction.
class UnknownKeysToml
{
public:
    UnknownKeysToml(std::string_view tag, std::string_view body)
        : m_path(std::filesystem::temp_directory_path() /
                 ("microtel_unknown_keys_" + std::string{tag} + "_" + std::to_string(::getpid()) +
                  ".toml"))
    {
        std::ofstream f{m_path};
        f << body;
    }

    ~UnknownKeysToml()
    {
        std::error_code ec;
        std::filesystem::remove(m_path, ec);
    }

    UnknownKeysToml(const UnknownKeysToml&) = delete;
    UnknownKeysToml& operator=(const UnknownKeysToml&) = delete;
    UnknownKeysToml(UnknownKeysToml&&) = delete;
    UnknownKeysToml& operator=(UnknownKeysToml&&) = delete;

    [[nodiscard]] const std::filesystem::path& Path() const
    {
        return m_path;
    }

private:
    std::filesystem::path m_path;
};

/// Three unknown keys at three depths: a section key, a nested key, a section.
constexpr std::string_view kThreeUnknownKeys = R"toml(
[exporter]
endpoint = "https://localhost:4318"
endpiont = "https://typo:4318"

[sdk]
max_queue_szie = 10

[bogus]
x = 1
)toml";

}  // namespace

TEST(SdkBuilderTest, Build_UnknownKeysWarn_LogsOneWarnPerKeyAndSucceeds)
{
    const UnknownKeysToml file{
        "warn", "[config]\nunknown_keys = \"warn\"\n" + std::string{kThreeUnknownKeys}};
    const LogCapture capture;
    const auto result = microtel::SdkBuilder().FromFile(file.Path()).Build();

    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(capture.WarnCount(), 3U);
    EXPECT_EQ(capture.WarnCount({"unknown configuration key", "\"exporter.endpiont\""}), 1U);
    EXPECT_EQ(capture.WarnCount({"unknown configuration key", "\"sdk.max_queue_szie\""}), 1U);
    EXPECT_EQ(capture.WarnCount({"unknown configuration key", "\"bogus\""}), 1U);
}

TEST(SdkBuilderTest, Build_UnknownKeysIgnore_LogsNothingAndSucceeds)
{
    const UnknownKeysToml file{
        "ignore", "[config]\nunknown_keys = \"ignore\"\n" + std::string{kThreeUnknownKeys}};
    const LogCapture capture;
    const auto result = microtel::SdkBuilder().FromFile(file.Path()).Build();

    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(capture.WarnCount(), 0U);
}

// ---------------------------------------------------------------------------
// WithResourceDetector (v1.1)
// ---------------------------------------------------------------------------

namespace
{

/// RAII guard for the strict-detector env var.
struct StrictEnvGuard
{
    StrictEnvGuard() = default;
    StrictEnvGuard(const StrictEnvGuard&) = delete;
    StrictEnvGuard& operator=(const StrictEnvGuard&) = delete;
    StrictEnvGuard(StrictEnvGuard&&) = delete;
    StrictEnvGuard& operator=(StrictEnvGuard&&) = delete;
    ~StrictEnvGuard() noexcept
    {
        (void)unsetenv("MICROTEL_RESOURCE_DETECTORS_STRICT");
    }
};

[[nodiscard]] std::unique_ptr<microtel::testing::FakeResourceDetector> MakeFailingDetector()
{
    auto fake = std::make_unique<microtel::testing::FakeResourceDetector>();
    fake->name = "broken";
    fake->failure = microtel::ConfigError{.kind = microtel::ConfigError::Kind::Unspecified,
                                          .field = "resource.detectors.broken",
                                          .message = "detector is deliberately broken"};
    return fake;
}

}  // namespace

TEST(SdkBuilderTest, WithResourceDetector_SucceedingDetector_BuildSucceeds)
{
    auto detector = std::make_unique<microtel::testing::FakeResourceDetector>();
    auto* const observer = detector.get();
    detector->resource_to_return = microtel::Resource{
        std::vector<microtel::KeyValue>{{.key = "host.name", .value = std::string{"node-7"}}}};

    // The builder is a named local, not a temporary: it owns the detector
    // (interfaces.md §4.10) and destroys it with itself, so reading `observer`
    // after a `SdkBuilder{}...Build()` one-liner would be a use-after-free.
    microtel::SdkBuilder builder;
    builder.WithEndpoint("https://localhost:4318").WithResourceDetector(std::move(detector));
    const auto result = builder.Build();

    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(observer->detect_call_count, 1) << "detection is one-shot per interfaces.md §4.10";
}

TEST(SdkBuilderTest, WithResourceDetector_BuiltInDetectors_BuildSucceeds)
{
    const auto result = microtel::SdkBuilder()
                            .WithEndpoint("https://localhost:4318")
                            .WithResourceDetector(microtel::MakeProcessDetector())
                            .WithResourceDetector(microtel::MakeHostDetector())
                            .Build();

    ASSERT_TRUE(result.has_value()) << result.error().message;
}

TEST(SdkBuilderTest, WithResourceDetector_FailingDetector_LenientByDefault_BuildSucceeds)
{
    const auto result = microtel::SdkBuilder()
                            .WithEndpoint("https://localhost:4318")
                            .WithResourceDetector(MakeFailingDetector())
                            .Build();

    ASSERT_TRUE(result.has_value()) << result.error().message;
}

TEST(SdkBuilderTest, WithResourceDetector_FailingDetector_StrictViaEnv_BuildFails)
{
    const StrictEnvGuard guard;
    (void)setenv("MICROTEL_RESOURCE_DETECTORS_STRICT", "true", 1);

    const auto result = microtel::SdkBuilder()
                            .WithEndpoint("https://localhost:4318")
                            .WithResourceDetector(MakeFailingDetector())
                            .Build();

    ASSERT_FALSE(result.has_value());
    EXPECT_NE(result.error().message.find("broken"), std::string::npos);
}

TEST(SdkBuilderTest, WithResourceDetector_NullDetector_IsIgnored)
{
    // A moved-from or failed factory result must not crash Build().
    const auto result = microtel::SdkBuilder()
                            .WithEndpoint("https://localhost:4318")
                            .WithResourceDetector(nullptr)
                            .Build();

    ASSERT_TRUE(result.has_value()) << result.error().message;
}
