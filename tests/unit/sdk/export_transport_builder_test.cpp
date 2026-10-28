// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0
//
// SdkBuilder::WithExportTransport end to end through a real Provider
// (ICP 0036): Build() conflicts and warnings, no HTTP/2 transport or I/O
// thread, the three signals and their gating, retries and their bytes,
// connection_state, the request cap, and Cancel on a shutdown timeout.

#include "microtel/error.hpp"
#include "microtel/export_transport.hpp"
#include "microtel/log_record.hpp"
#include "microtel/log_sink.hpp"
#include "microtel/logger.hpp"
#include "microtel/meter.hpp"
#include "microtel/provider.hpp"
#include "microtel/sdk_builder.hpp"
#include "microtel/status.hpp"
#include "microtel/tracer.hpp"

#include "fakes/fake_export_transport.hpp"
#include "helpers/thread_ids.hpp"
#include "mocks/mock_export_transport.hpp"
#include "sdk/noop_logger.hpp"
#include "sdk/noop_meter.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace
{

namespace mt = microtel;
namespace mtt = microtel::testing;

using namespace std::chrono_literals;

constexpr std::string_view kTransportField = "exporter.transport";

/// Captures microtel's internal Warn lines for the scope's lifetime.
class LogCapture
{
public:
    LogCapture()
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
    ~LogCapture()
    {
        mt::ResetLogSink();
    }
    LogCapture(const LogCapture&) = delete;
    LogCapture& operator=(const LogCapture&) = delete;
    LogCapture(LogCapture&&) = delete;
    LogCapture& operator=(LogCapture&&) = delete;

    [[nodiscard]] std::size_t WarnsContaining(std::string_view needle) const
    {
        const std::scoped_lock lock{m_mu};
        return static_cast<std::size_t>(std::ranges::count_if(
            m_warns,
            [needle](const std::string& w) { return w.find(needle) != std::string::npos; }));
    }

private:
    mutable std::mutex m_mu;
    std::vector<std::string> m_warns;
};

/// Sets an environment variable for the scope's lifetime.
class EnvVar
{
public:
    EnvVar(const char* name, const char* value) : m_name(name)
    {
        (void)setenv(name, value, 1);
    }
    ~EnvVar()
    {
        (void)unsetenv(m_name);
    }
    EnvVar(const EnvVar&) = delete;
    EnvVar& operator=(const EnvVar&) = delete;
    EnvVar(EnvVar&&) = delete;
    EnvVar& operator=(EnvVar&&) = delete;

private:
    const char* m_name;
};

/// A fake the provider owns, with a borrowed pointer for the test.
struct Owned
{
    Owned() : owner(std::make_unique<mtt::FakeExportTransport>()), fake(owner.get()) {}
    std::unique_ptr<mtt::FakeExportTransport> owner;
    mtt::FakeExportTransport* fake;
};

std::shared_ptr<mt::Provider> BuildWith(std::unique_ptr<mt::ExportTransport> transport,
                                        mt::ExportTransportOptions opts = {})
{
    auto result = mt::SdkBuilder()
                      .WithServiceName("full-node")
                      .WithExportTransport(std::move(transport), opts)
                      .Build();
    EXPECT_TRUE(result.has_value()) << (result ? "" : result.error().message);
    return result ? *result : nullptr;
}

void EndOneSpan(mt::Provider& provider, std::string_view scope = "lib")
{
    auto span = provider.GetTracer(scope)->StartSpan("op");
    span->End();
}

std::size_t CountOf(const std::vector<mtt::FakeExportTransport::Recorded>& sent,
                    mt::ExportSignal signal)
{
    return static_cast<std::size_t>(
        std::ranges::count_if(sent, [signal](const auto& r) { return r.signal == signal; }));
}

}  // namespace

// ---------------------------------------------------------------------------
// The trace path
// ---------------------------------------------------------------------------

TEST(ExportTransportBuilderTest, SpansReachTheTransportWithNoEndpointConfigured)
{
    Owned t;
    const auto provider = BuildWith(std::move(t.owner));
    ASSERT_NE(provider, nullptr);

    EndOneSpan(*provider);
    ASSERT_EQ(provider->ForceFlush(5s), mt::Status::Completed);

    const auto sent = t.fake->Sent();
    ASSERT_EQ(sent.size(), 1U);
    EXPECT_EQ(sent[0].signal, mt::ExportSignal::Traces);
    EXPECT_FALSE(sent[0].bytes.empty());
    EXPECT_EQ(provider->GetExporterHealth().batches_sent, 1U);
}

TEST(ExportTransportBuilderTest, Connect_IsANoOpThatSucceeds)
{
    Owned t;
    const auto provider = BuildWith(std::move(t.owner));
    ASSERT_NE(provider, nullptr);

    EXPECT_TRUE(provider->Connect().has_value());
    EXPECT_TRUE(t.fake->Sent().empty());
}

TEST(ExportTransportBuilderTest, NoHttp2TransportIsBuilt_ThreeFewerThreadsThanTheHttpDefault)
{
    // The HTTP default starts an I/O thread and the metric and log exporter
    // workers; a traces-only custom transport starts none of them.
    const auto before_http = mtt::ThreadIds();
    auto http = mt::SdkBuilder().WithEndpoint("https://localhost:4318").Build();
    ASSERT_TRUE(http.has_value());
    const std::size_t http_threads = mtt::ThreadsStartedSince(before_http);
    http->reset();

    const auto before_custom = mtt::ThreadIds();
    const auto custom = BuildWith(std::make_unique<mtt::MockExportTransport>());
    ASSERT_NE(custom, nullptr);
    const std::size_t custom_threads = mtt::ThreadsStartedSince(before_custom);

    EXPECT_EQ(http_threads - custom_threads, 3U);
}

TEST(ExportTransportBuilderTest, ConnectionState_DisconnectedThenConnectedThenClosed)
{
    Owned t;
    const auto provider = BuildWith(std::move(t.owner));
    ASSERT_NE(provider, nullptr);

    EXPECT_EQ(provider->GetExporterHealth().connection_state, mt::ConnectionState::Disconnected);
    EndOneSpan(*provider);
    ASSERT_EQ(provider->ForceFlush(5s), mt::Status::Completed);
    EXPECT_EQ(provider->GetExporterHealth().connection_state, mt::ConnectionState::Connected);
    ASSERT_EQ(provider->Shutdown(5s), mt::Status::Completed);
    EXPECT_EQ(provider->GetExporterHealth().connection_state, mt::ConnectionState::Closed);
}

TEST(ExportTransportBuilderTest, NonRetryable_IsCountedWithTheTransportsMessage)
{
    Owned t;
    t.fake->default_result =
        mt::SendResult{.outcome = mt::SendOutcome::NonRetryable, .message = "frame refused"};
    const auto provider = BuildWith(std::move(t.owner));
    ASSERT_NE(provider, nullptr);

    EndOneSpan(*provider);
    ASSERT_EQ(provider->ForceFlush(5s), mt::Status::Failed);

    const auto health = provider->GetExporterHealth();
    EXPECT_EQ(health.batches_failed, 1U);
    EXPECT_EQ(health.drop_counters[static_cast<std::size_t>(mt::DropReason::NonRetryableFailure)],
              1U);
    EXPECT_EQ(health.last_error_message, "frame refused");
    EXPECT_EQ(t.fake->Sent().size(), 1U) << "never retried";
}

TEST(ExportTransportBuilderTest, PartialSuccess_CountsTheRejectedItems)
{
    Owned t;
    t.fake->default_result = mt::SendResult{.outcome = mt::SendOutcome::Success, .rejected = 2};
    const auto provider = BuildWith(std::move(t.owner));
    ASSERT_NE(provider, nullptr);

    EndOneSpan(*provider);
    ASSERT_EQ(provider->ForceFlush(5s), mt::Status::Completed);

    const auto health = provider->GetExporterHealth();
    EXPECT_EQ(
        health.drop_counters[static_cast<std::size_t>(mt::DropReason::PartialSuccessRejection)],
        2U);
    EXPECT_EQ(health.batches_sent, 1U);
}

TEST(ExportTransportBuilderTest, AThrowingSendCostsOneRequestNotTheProcess)
{
    Owned t;
    t.fake->throw_non_std = true;
    const auto provider = BuildWith(std::move(t.owner));
    ASSERT_NE(provider, nullptr);

    EndOneSpan(*provider);
    ASSERT_EQ(provider->ForceFlush(5s), mt::Status::Failed);

    const auto health = provider->GetExporterHealth();
    EXPECT_EQ(health.batches_failed, 1U);
    EXPECT_NE(health.last_error_message.find("threw"), std::string::npos);
}

// ---------------------------------------------------------------------------
// Retries: the bytes of every attempt are identical
// ---------------------------------------------------------------------------

TEST(ExportTransportBuilderTest, Retry_TraceRequestOfSeveralScopesIsByteIdentical)
{
    Owned t;
    t.fake->scripted_results.push_back(
        mt::SendResult{.outcome = mt::SendOutcome::Retryable, .retry_after = 1ms});
    const auto provider = BuildWith(std::move(t.owner));
    ASSERT_NE(provider, nullptr);

    // Three scopes in one drain: three BatchHandles joined into one request.
    auto a = provider->GetTracer("scope-a")->StartSpan("a");
    auto b = provider->GetTracer("scope-b")->StartSpan("b");
    auto c = provider->GetTracer("scope-c", "2.0")->StartSpan("c");
    a->End();
    b->End();
    c->End();
    ASSERT_EQ(provider->ForceFlush(10s), mt::Status::Completed);

    const auto sent = t.fake->Sent();
    ASSERT_EQ(sent.size(), 2U) << "attempt 0 and one retry";
    EXPECT_EQ(sent[1].bytes, sent[0].bytes);
    const auto health = provider->GetExporterHealth();
    EXPECT_EQ(
        health.drop_counters[static_cast<std::size_t>(mt::DropReason::RetryableFailureRecovered)],
        3U)
        << "one recovered request, counted per batch";
}

namespace
{

/// Sends one request of @p signal whose first attempt fails, and returns the
/// bytes of every attempt at it. One signal per provider, so the fake's
/// script is consumed by that signal alone.
std::vector<std::vector<std::byte>> AttemptsOfOneRetriedRequest(
    mt::ExportSignal signal, const std::function<void(mt::Provider&)>& produce)
{
    Owned t;
    t.fake->scripted_results.push_back(
        mt::SendResult{.outcome = mt::SendOutcome::Retryable, .retry_after = 1ms});
    const auto provider =
        BuildWith(std::move(t.owner),
                  mt::ExportTransportOptions{.metrics = signal == mt::ExportSignal::Metrics,
                                             .logs = signal == mt::ExportSignal::Logs});
    EXPECT_NE(provider, nullptr);
    if (provider == nullptr)
    {
        return {};
    }
    produce(*provider);
    EXPECT_EQ(provider->ForceFlush(10s), mt::Status::Completed);

    std::vector<std::vector<std::byte>> attempts;
    for (const auto& r : t.fake->Sent())
    {
        if (r.signal == signal)
        {
            attempts.push_back(r.bytes);
        }
    }
    return attempts;
}

/// @p attempts is one request per scope, and the first request, which failed,
/// is retried. The two scopes reach the exporter in two `Export` calls, so its
/// worker may send both before the retry or retry the first before it sees the
/// second: the order is not asserted, only that the retry repeats the first
/// attempt byte for byte and the other request differs.
void ExpectTheFirstRequestRetriedByteForByte(const std::vector<std::vector<std::byte>>& attempts)
{
    ASSERT_EQ(attempts.size(), 3U) << "two requests and the retry of the first";
    EXPECT_EQ(std::ranges::count(attempts, attempts[0]), 2)
        << "the first attempt and its byte-identical retry; the other request differs";
}

}  // namespace

TEST(ExportTransportBuilderTest, Retry_MetricRequestIsByteIdentical)
{
    const auto attempts = AttemptsOfOneRetriedRequest(
        mt::ExportSignal::Metrics,
        [](mt::Provider& p)
        {
            p.GetMeter("m")->CreateCounter<std::int64_t>("a")->Add(7, {});
            p.GetMeter("m2")->CreateCounter<double>("b")->Add(1.5, {});
        });
    ExpectTheFirstRequestRetriedByteForByte(attempts);
}

TEST(ExportTransportBuilderTest, Retry_LogRequestIsByteIdentical)
{
    const auto attempts = AttemptsOfOneRetriedRequest(mt::ExportSignal::Logs,
                                                      [](mt::Provider& p)
                                                      {
                                                          p.GetLogger("l")->Emit(mt::LogRecord{});
                                                          p.GetLogger("l2")->Emit(mt::LogRecord{});
                                                      });
    ExpectTheFirstRequestRetriedByteForByte(attempts);
}

// ---------------------------------------------------------------------------
// max_request_bytes
// ---------------------------------------------------------------------------

TEST(ExportTransportBuilderTest, MaxRequestBytes_StopsJoiningAndWarnsForAnOversizedBatch)
{
    const LogCapture capture;
    Owned t;
    const auto provider =
        BuildWith(std::move(t.owner), mt::ExportTransportOptions{.max_request_bytes = 1});
    ASSERT_NE(provider, nullptr);

    EndOneSpan(*provider, "scope-a");
    EndOneSpan(*provider, "scope-b");
    ASSERT_EQ(provider->ForceFlush(5s), mt::Status::Completed);

    EXPECT_EQ(t.fake->Sent().size(), 2U) << "each batch alone: joining any two crosses the cap";
    EXPECT_EQ(capture.WarnsContaining("max_request_bytes"), 1U) << "rate-limited";
}

TEST(ExportTransportBuilderTest, MaxRequestBytesZero_JoinsBySpanCountOnly)
{
    Owned t;
    const auto provider =
        BuildWith(std::move(t.owner), mt::ExportTransportOptions{.max_request_bytes = 0});
    ASSERT_NE(provider, nullptr);

    EndOneSpan(*provider, "scope-a");
    EndOneSpan(*provider, "scope-b");
    ASSERT_EQ(provider->ForceFlush(5s), mt::Status::Completed);

    EXPECT_EQ(t.fake->Sent().size(), 1U);
}

// ---------------------------------------------------------------------------
// Signals
// ---------------------------------------------------------------------------

TEST(ExportTransportBuilderTest, TracesOnlyDefault_GivesNoOpMeterAndLoggerWithOneWarnEach)
{
    const LogCapture capture;
    Owned t;
    const auto provider = BuildWith(std::move(t.owner));
    ASSERT_NE(provider, nullptr);

    const auto meter = provider->GetMeter("m");
    EXPECT_NE(dynamic_cast<mt::sdk::NoopMeter*>(meter.get()), nullptr);
    (void)provider->GetMeter("m2");
    const auto logger = provider->GetLogger("l");
    EXPECT_NE(dynamic_cast<mt::sdk::NoopLogger*>(logger.get()), nullptr);
    (void)provider->GetLogger("l2");

    meter->CreateCounter<std::int64_t>("c")->Add(1, {});
    meter->CreateHistogram<double>("h")->Record(1.0, {});
    logger->Emit(mt::LogRecord{});
    ASSERT_EQ(provider->ForceFlush(5s), mt::Status::Completed);

    EXPECT_TRUE(t.fake->Sent().empty());
    EXPECT_EQ(capture.WarnsContaining("metrics"), 1U);
    EXPECT_EQ(capture.WarnsContaining("logs"), 1U);
    const auto health = provider->GetExporterHealth();
    for (const auto count : health.drop_counters)
    {
        EXPECT_EQ(count, 0U) << "nothing was produced, so nothing was dropped";
    }
}

TEST(ExportTransportBuilderTest, MetricsAndLogsOn_ReachTheTransportTaggedBySignal)
{
    Owned t;
    const auto provider =
        BuildWith(std::move(t.owner), mt::ExportTransportOptions{.metrics = true, .logs = true});
    ASSERT_NE(provider, nullptr);

    EndOneSpan(*provider);
    provider->GetMeter("m")->CreateCounter<std::int64_t>("hits")->Add(1, {});
    provider->GetLogger("l")->Emit(mt::LogRecord{});
    ASSERT_EQ(provider->ForceFlush(5s), mt::Status::Completed);

    const auto sent = t.fake->Sent();
    EXPECT_EQ(CountOf(sent, mt::ExportSignal::Traces), 1U);
    EXPECT_EQ(CountOf(sent, mt::ExportSignal::Metrics), 1U);
    EXPECT_EQ(CountOf(sent, mt::ExportSignal::Logs), 1U);
}

TEST(ExportTransportBuilderTest, TracesOff_SpansAreNotRecordedAndNothingIsSent)
{
    const LogCapture capture;
    Owned t;
    const auto provider =
        BuildWith(std::move(t.owner), mt::ExportTransportOptions{.traces = false, .metrics = true});
    ASSERT_NE(provider, nullptr);

    auto span = provider->GetTracer("lib")->StartSpan("op");
    EXPECT_FALSE(span->IsSampled());
    span->End();
    ASSERT_EQ(provider->ForceFlush(5s), mt::Status::Completed);

    EXPECT_EQ(CountOf(t.fake->Sent(), mt::ExportSignal::Traces), 0U);
    EXPECT_EQ(capture.WarnsContaining("traces"), 1U);
}

// ---------------------------------------------------------------------------
// Build(): conflicts, null, and settings from the environment
// ---------------------------------------------------------------------------

namespace
{

void ExpectTransportConflict(mt::SdkBuilder& builder)
{
    const auto result =
        builder.WithExportTransport(std::make_unique<mtt::MockExportTransport>()).Build();
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, mt::ConfigError::Kind::InvalidValue);
    EXPECT_EQ(result.error().field, kTransportField);
}

}  // namespace

TEST(ExportTransportBuilderTest, Conflict_WithEndpoint)
{
    mt::SdkBuilder b;
    b.WithEndpoint("https://localhost:4318");
    ExpectTransportConflict(b);
}

TEST(ExportTransportBuilderTest, Conflict_WithProtocol)
{
    mt::SdkBuilder b;
    b.WithProtocol(mt::Protocol::Grpc);
    ExpectTransportConflict(b);
}

TEST(ExportTransportBuilderTest, Conflict_WithHeaders)
{
    mt::SdkBuilder b;
    b.WithHeaders({{.key = "x-tenant", .value = std::string{"acme"}}});
    ExpectTransportConflict(b);
}

TEST(ExportTransportBuilderTest, Conflict_WithTls)
{
    mt::SdkBuilder b;
    b.WithTls(mt::TlsOptions{});
    ExpectTransportConflict(b);
}

TEST(ExportTransportBuilderTest, Conflict_WithAuthProvider)
{
    mt::SdkBuilder b;
    b.WithAuthProvider([] { return microtel::Expected<std::string, mt::Error>{"Bearer x"}; });
    ExpectTransportConflict(b);
}

TEST(ExportTransportBuilderTest, Conflict_WithCompressionGzip)
{
    mt::SdkBuilder b;
    b.WithCompressionGzip(true);
    ExpectTransportConflict(b);
}

TEST(ExportTransportBuilderTest, NullTransport_IsRefused)
{
    const auto result = mt::SdkBuilder().WithExportTransport(nullptr).Build();
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, mt::ConfigError::Kind::InvalidValue);
    EXPECT_EQ(result.error().field, kTransportField);
}

TEST(ExportTransportBuilderTest, EnvironmentExporterSettings_AreIgnoredWithOneWarn)
{
    const LogCapture capture;
    // Malformed on purpose: an ignored setting is not validated either.
    const EnvVar endpoint{"OTEL_EXPORTER_OTLP_ENDPOINT", "not a url"};
    const EnvVar headers{"OTEL_EXPORTER_OTLP_HEADERS", "x-tenant=acme"};
    const EnvVar compression{"OTEL_EXPORTER_OTLP_COMPRESSION", "gzip"};

    const auto provider = BuildWith(std::make_unique<mtt::MockExportTransport>());
    ASSERT_NE(provider, nullptr);

    EXPECT_EQ(capture.WarnsContaining("ignored"), 1U);
    EXPECT_EQ(capture.WarnsContaining("endpoint"), 1U);
    EXPECT_EQ(capture.WarnsContaining("headers"), 1U);
    EXPECT_EQ(capture.WarnsContaining("compression"), 1U);
    EXPECT_EQ(capture.WarnsContaining("plaintext"), 0U) << "no HTTP warnings for a custom link";
}

TEST(ExportTransportBuilderTest, NoExporterSettingsAnywhere_NoWarn)
{
    const LogCapture capture;
    const auto provider = BuildWith(std::make_unique<mtt::MockExportTransport>());
    ASSERT_NE(provider, nullptr);

    EXPECT_EQ(capture.WarnsContaining("ignored"), 0U);
}

// ---------------------------------------------------------------------------
// Shutdown and Cancel
// ---------------------------------------------------------------------------

TEST(ExportTransportBuilderTest, Shutdown_SendsWithADeadlineClampedToTheShutdownTimeout)
{
    Owned t;
    auto result = mt::SdkBuilder()
                      .WithTimeouts(mt::TimeoutOptions{.per_export = 60s})
                      .WithExportTransport(std::move(t.owner))
                      .Build();
    ASSERT_TRUE(result.has_value());
    const auto provider = *result;

    EndOneSpan(*provider);
    const auto before = std::chrono::steady_clock::now();
    ASSERT_EQ(provider->Shutdown(2s), mt::Status::Completed);

    const auto sent = t.fake->Sent();
    ASSERT_EQ(sent.size(), 1U);
    EXPECT_LE(sent[0].deadline, before + 3s) << "clamped from 60 s to the 2 s shutdown";
}

TEST(ExportTransportBuilderTest, Shutdown_CancelsASendBlockedPastItsDeadline_InBoundedTime)
{
    Owned t;
    t.fake->block_until_cancel = true;
    const auto provider = BuildWith(std::move(t.owner));
    ASSERT_NE(provider, nullptr);

    EndOneSpan(*provider);
    (void)provider->ForceFlush(50ms);
    ASSERT_TRUE(t.fake->WaitForSends(1, 5s)) << "the worker is inside Send";

    const auto start = std::chrono::steady_clock::now();
    EXPECT_EQ(provider->Shutdown(200ms), mt::Status::TimedOut);
    const auto elapsed = std::chrono::steady_clock::now() - start;

    EXPECT_EQ(t.fake->CancelCalls(), 1);
    EXPECT_LT(elapsed, 5s) << "bounded: Cancel woke the Send the worker was joined inside";
}

TEST(ExportTransportBuilderTest, Shutdown_WithNothingInFlight_NeverCallsCancel)
{
    auto owner = std::make_unique<mtt::MockExportTransport>();
    owner->result_to_return = mt::SendResult{.outcome = mt::SendOutcome::Success};
    auto* const mock = owner.get();
    const auto provider = BuildWith(std::move(owner));
    ASSERT_NE(provider, nullptr);

    EndOneSpan(*provider);
    EXPECT_EQ(provider->Shutdown(5s), mt::Status::Completed);
    EXPECT_EQ(mock->cancel_call_count.load(), 0);
}

// #486: a trace stage that runs out of time no longer stops the log flush.
TEST(ExportTransportBuilderTest, ForceFlush_ASlowTraceSend_StillFlushesTheLogs)
{
    Owned t;
    t.fake->send_delay.at(static_cast<std::size_t>(mt::ExportSignal::Traces)) = 3s;
    auto result =
        mt::SdkBuilder()
            .WithBatch(mt::BatchOptions{.schedule_delay = 60s})
            .WithExportTransport(std::move(t.owner),
                                 mt::ExportTransportOptions{.metrics = false, .logs = true})
            .Build();
    ASSERT_TRUE(result.has_value());
    const auto provider = *result;

    EndOneSpan(*provider);
    provider->GetLogger("l")->Emit(mt::LogRecord{});
    const auto start = std::chrono::steady_clock::now();
    EXPECT_EQ(provider->ForceFlush(500ms), mt::Status::TimedOut);
    EXPECT_LT(std::chrono::steady_clock::now() - start, 1500ms) << "one deadline for every stage";

    // The 60 s schedule delay means only the flush can have sent the logs.
    EXPECT_TRUE(t.fake->WaitForSends(mt::ExportSignal::Logs, 1, 2s))
        << "the log stage was skipped after the trace stage timed out";
    (void)provider->Shutdown(1s);
}

// #484: with a metric interval shorter than one metric send, the metric
// exporter's queue never empties. ForceFlush waited for it to, and timed out
// every time; it now waits for what was queued before the call.
TEST(ExportTransportBuilderTest, ForceFlush_MetricIntervalShorterThanTheExport_Completes)
{
    constexpr auto kInterval = 20ms;
    constexpr auto kLetExportsBackUp = 300ms;
    constexpr auto kMetricSendTime = 50ms;
    Owned t;
    t.fake->send_delay.at(static_cast<std::size_t>(mt::ExportSignal::Metrics)) = kMetricSendTime;
    auto result =
        mt::SdkBuilder()
            .WithMetricInterval(kInterval)
            .WithBatch(mt::BatchOptions{.schedule_delay = 60s})
            .WithExportTransport(std::move(t.owner),
                                 mt::ExportTransportOptions{.metrics = true, .logs = true})
            .Build();
    ASSERT_TRUE(result.has_value());
    const auto provider = *result;
    provider->GetMeter("m")->CreateCounter<std::int64_t>("n")->Add(1, {});
    std::this_thread::sleep_for(kLetExportsBackUp);

    EndOneSpan(*provider);
    provider->GetLogger("l")->Emit(mt::LogRecord{});
    EXPECT_EQ(provider->ForceFlush(10s), mt::Status::Completed);

    const auto sent = t.fake->Sent();
    EXPECT_EQ(CountOf(sent, mt::ExportSignal::Traces), 1U);
    EXPECT_EQ(CountOf(sent, mt::ExportSignal::Logs), 1U);
    (void)provider->Shutdown(1s);
}

// ---------------------------------------------------------------------------
// Concurrency (run under TSAN): three signals through one transport
// ---------------------------------------------------------------------------

TEST(ExportTransportBuilderTest, ThreeSignalsConcurrently_ThroughOneTransport)
{
    Owned t;
    auto result =
        mt::SdkBuilder()
            .WithMetricInterval(5ms)
            .WithBatch(mt::BatchOptions{.schedule_delay = 5ms})
            .WithExportTransport(std::move(t.owner),
                                 mt::ExportTransportOptions{.metrics = true, .logs = true})
            .Build();
    ASSERT_TRUE(result.has_value());
    const auto provider = *result;
    const auto tracer = provider->GetTracer("t");
    const auto counter = provider->GetMeter("m")->CreateCounter<std::int64_t>("n");
    const auto logger = provider->GetLogger("l");

    constexpr int kIterations = 200;
    std::thread spans{[&]
                      {
                          for (int i = 0; i < kIterations; ++i)
                          {
                              tracer->StartSpan("s")->End();
                          }
                      }};
    std::thread metrics{[&]
                        {
                            for (int i = 0; i < kIterations; ++i)
                            {
                                counter->Add(1, {});
                            }
                        }};
    std::thread logs{[&]
                     {
                         for (int i = 0; i < kIterations; ++i)
                         {
                             logger->Emit(mt::LogRecord{});
                         }
                     }};
    spans.join();
    metrics.join();
    logs.join();
    ASSERT_EQ(provider->ForceFlush(10s), mt::Status::Completed);

    const auto sent = t.fake->Sent();
    EXPECT_GE(CountOf(sent, mt::ExportSignal::Traces), 1U);
    EXPECT_GE(CountOf(sent, mt::ExportSignal::Metrics), 1U);
    EXPECT_GE(CountOf(sent, mt::ExportSignal::Logs), 1U);
    EXPECT_EQ(provider->Shutdown(5s), mt::Status::Completed);
}
