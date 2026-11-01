// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0
//
// Volume, compression and payload size over OTLP/HTTP against a real collector.
//
// basic_export_test.cpp proves one span survives the round trip. These tests
// ask the questions that only appear at scale, and that a mock transport
// structurally cannot answer:
//
//  - MultiBatchDelivery: when the batch processor splits a burst across several
//    HTTP requests, does *every* span arrive exactly once? A mock counts calls;
//    only a real receiver can say whether request number seven decoded.
//  - GzipAccepted: the collector is the only thing in the repo that actually
//    inflates our request bodies. A unit test can assert we set
//    `Content-Encoding: gzip` and produced a gzip stream; it cannot assert the
//    peer agreed.
//  - LargePayload: a body far past the 65 535-byte default HTTP/2 stream window
//    forces multiple DATA frames and real WINDOW_UPDATE handling by the peer.
//
// Every assertion about delivery reads the collector's output file, which is
// downstream of the collector's protobuf decode. Counting occurrences of the
// run's unique marker also makes duplicate delivery a failure, not a silent
// pass: a retry the collector accepted twice shows up as 501 where 500 was
// expected.
//
// These tests point at MICROTEL_CONFORMANCE_HTTP_TLS_ENDPOINT for the reason
// recorded in basic_export_test.cpp: TLS is where ALPN negotiates h2, and the
// multi-DATA-frame case is an HTTP/2 one. The collector's plaintext OTLP/HTTP
// receiver is HTTP/1.1-only and is covered by plaintext_http1_test.cpp.

#include "microtel/protocol.hpp"
#include "microtel/provider.hpp"
#include "microtel/sdk_builder.hpp"
#include "microtel/span.hpp"
#include "microtel/status.hpp"
#include "microtel/tracer.hpp"

#include "conformance/support/collector_output.hpp"
#include "conformance/support/conformance_env.hpp"
#include "conformance/support/provider_builder.hpp"

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <utility>

namespace
{

constexpr const char* kEndpointEnv = "MICROTEL_CONFORMANCE_HTTP_TLS_ENDPOINT";
constexpr const char* kCaEnv = "MICROTEL_CONFORMANCE_CA";
constexpr const char* kOutputFileEnv = "MICROTEL_CONFORMANCE_OUTPUT_FILE";

constexpr const char* kSkipReason =
    "no conformance collector configured — run ci/scripts/conformance.sh";

constexpr const char* kServiceName = "microtel-conformance";
constexpr const char* kScopeName = "microtel.conformance.http.batching";
constexpr const char* kScopeVersion = "1.0";

constexpr auto kFlushTimeout = std::chrono::seconds(30);
constexpr auto kCollectorPollTimeout = std::chrono::seconds(30);
constexpr auto kPollInterval = std::chrono::milliseconds(100);

/// Deliberately far below the 512 default so 500 spans cannot fit in one
/// request: this is what makes MultiBatchDelivery a multi-request test.
constexpr std::uint32_t kSmallBatchSize = 64;
constexpr auto kBatchScheduleDelay = std::chrono::milliseconds(100);

constexpr std::size_t kBurstSpanCount = 500;
constexpr std::size_t kGzipSpanCount = 50;
constexpr std::size_t kLargePayloadSpanCount = 200;

/// One byte under 4 KiB rather than exactly 4 KiB: `SpanLimitOptions`
/// defaults `attribute_value_length_limit` to 4096, and a value at the limit
/// risks being counted as truncation, which would make the test assert about
/// microtel's limit enforcement instead of about the wire. 4095 bytes is still
/// comfortably past every buffer size in the encode path.
constexpr std::size_t kLargeAttributeBytes = 4095;
constexpr char kFillerByte = 'x';

/// @brief Builds a provider aimed at the collector's TLS OTLP/HTTP receiver.
///
/// @param builder  caller-owned; `SdkBuilder` cannot be returned by value.
/// @param endpoint resolved from @ref kEndpointEnv.
/// @return @p builder, so the caller can add per-test options before `Build()`.
microtel::SdkBuilder& ConfigureTlsBuilder(microtel::SdkBuilder& builder,
                                          const std::string& endpoint)
{
    return microtel::testing::ConfigureConformanceBuilder(
               builder, endpoint, microtel::Protocol::Http)
        .WithServiceName(kServiceName)
        .WithTls(microtel::TlsOptions{
            // The run's throwaway CA. System trust would reject this server.
            .ca_bundle = microtel::testing::GetEnv(kCaEnv).value_or(""),
            .client_cert = {},
            .client_key = {},
            .sni_override = {},
        });
}

/// @brief Emits @p count spans named `<marker>.<index>`, ended immediately.
///
/// The index suffix is what lets the caller wait for the *last* span rather
/// than any span, which is the difference between "a batch arrived" and "every
/// batch arrived".
///
/// @param provider borrowed for the call.
/// @param marker   unique-per-run span-name prefix.
/// @param count    number of spans to emit.
void EmitNumberedSpans(microtel::Provider& provider, const std::string& marker, std::size_t count)
{
    const auto tracer = provider.GetTracer(kScopeName, kScopeVersion);
    for (std::size_t i = 0; i < count; ++i)
    {
        auto span = tracer->StartSpan(marker + "." + std::to_string(i));
        span->End();
    }
}

/// @brief Emits @p count spans, each carrying one @ref kLargeAttributeBytes
/// string attribute, so the serialised request body is hundreds of kilobytes.
///
/// @param provider borrowed for the call.
/// @param marker   unique-per-run span-name prefix.
/// @param count    number of spans to emit.
void EmitLargeSpans(microtel::Provider& provider, const std::string& marker, std::size_t count)
{
    const std::string payload(kLargeAttributeBytes, kFillerByte);
    const auto tracer = provider.GetTracer(kScopeName, kScopeVersion);
    for (std::size_t i = 0; i < count; ++i)
    {
        auto span = tracer->StartSpan(marker + "." + std::to_string(i));
        span->SetAttribute("conformance.payload", payload);
        span->End();
    }
}

/// @brief Polls the collector's output file until @p needle appears at least
/// @p expected times.
///
/// `PollForLineContaining` answers "did this one span land"; delivery of a
/// *burst* is only settled when the count stops being short. Returning the
/// observed count rather than a bool keeps over-delivery (a duplicate the
/// collector accepted twice) visible to the caller's assertion.
///
/// @param path     the collector's output file.
/// @param needle   substring to count; use a `UniqueMarker()`-derived prefix.
/// @param expected count at which polling stops early.
/// @param timeout  how long to keep polling.
/// @return the last observed count, which may be below or above @p expected.
std::size_t PollForOccurrences(const std::filesystem::path& path,
                               const std::string& needle,
                               std::size_t expected,
                               std::chrono::milliseconds timeout)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    std::size_t count = microtel::testing::CountOccurrences(path, needle);
    while (count < expected && std::chrono::steady_clock::now() < deadline)
    {
        std::this_thread::sleep_for(kPollInterval);
        count = microtel::testing::CountOccurrences(path, needle);
    }
    return count;
}

/// @brief Asserts the drop counters a clean delivery must leave at zero.
///
/// Not every counter: `AttributeValueTruncated` and the structural-limit
/// counters are about record shaping, and a delivery test should not be the
/// thing that fails when a limit changes. These are the ones that mean "a span
/// this test emitted never reached the collector".
///
/// @note Live coverage as of issue #181, which closed the last of the v1.0
///       limits gates: every counter in the list below now has a producer in
///       `src/` (`transport_busy` at the transport's bounded request queue was
///       the last). It used to be a tripwire — when this note was written
///       `RecordDrop` had two call sites, both on the metrics and logs paths
///       (issue #169). The real delivery evidence in each test is still the
///       occurrence count read back from the collector's output file; this
///       assertion says nothing was quietly shed on the way there.
///
/// @param health snapshot to inspect.
void ExpectNoDeliveryDrops(const microtel::HealthSnapshot& health)
{
    constexpr std::array kLossReasons = {
        microtel::DropReason::QueueFull,
        microtel::DropReason::RecordTooLarge,
        microtel::DropReason::PostShutdown,
        microtel::DropReason::PartialSuccessRejection,
        microtel::DropReason::NonRetryableFailure,
        microtel::DropReason::RetryBudgetExhausted,
        microtel::DropReason::TransportBusy,
        microtel::DropReason::ConnectFailure,
        microtel::DropReason::ForceFlushTimeout,
    };
    for (const microtel::DropReason reason : kLossReasons)
    {
        EXPECT_EQ(health.drop_counters.at(static_cast<std::size_t>(reason)), 0U)
            << "spans were dropped; DropReason index " << static_cast<unsigned>(reason);
    }
}

TEST(HttpBatchingConformance, MultiBatchDelivery)
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
    auto result = ConfigureTlsBuilder(builder, endpoint)
                      .WithBatch(microtel::BatchOptions{
                          .max_export_batch_size = kSmallBatchSize,
                          .schedule_delay = kBatchScheduleDelay,
                      })
                      .Build();
    ASSERT_TRUE(result.has_value()) << result.error().message;
    const std::shared_ptr<microtel::Provider> provider = std::move(*result);

    const std::string marker = microtel::testing::UniqueMarker();
    EmitNumberedSpans(*provider, marker, kBurstSpanCount);

    ASSERT_EQ(provider->ForceFlush(kFlushTimeout), microtel::Status::Completed);

    const microtel::HealthSnapshot health = provider->GetExporterHealth();
    // 500 spans at 64 per request cannot have been one request.
    EXPECT_GE(health.batches_sent, 2U);
    EXPECT_EQ(health.batches_failed, 0U) << health.last_error_message;
    ExpectNoDeliveryDrops(health);

    // The highest-numbered span is emitted last, so its arrival means the final
    // request was accepted rather than merely the first one.
    const std::string last_span = marker + "." + std::to_string(kBurstSpanCount - 1);
    const auto line =
        microtel::testing::PollForLineContaining(output_file, last_span, kCollectorPollTimeout);
    // value_or rather than assert-then-dereference: gtest's early return is
    // invisible to clang-tidy's dataflow, so a post-ASSERT access reads as
    // unchecked. A matched line is never empty.
    ASSERT_FALSE(line.value_or(std::string{}).empty())
        << "collector never wrote a line containing '" << last_span << "' to " << output_file;

    // Exactly once each: short means a lost batch, over means a retry the
    // collector accepted twice.
    const std::string span_prefix = marker + ".";
    const std::size_t delivered =
        PollForOccurrences(output_file, span_prefix, kBurstSpanCount, kCollectorPollTimeout);
    EXPECT_EQ(delivered, kBurstSpanCount);
}

TEST(HttpBatchingConformance, GzipAccepted)
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
    auto result = ConfigureTlsBuilder(builder, endpoint).WithCompressionGzip(true).Build();
    ASSERT_TRUE(result.has_value()) << result.error().message;
    const std::shared_ptr<microtel::Provider> provider = std::move(*result);

    const std::string marker = microtel::testing::UniqueMarker();
    EmitNumberedSpans(*provider, marker, kGzipSpanCount);

    ASSERT_EQ(provider->ForceFlush(kFlushTimeout), microtel::Status::Completed);

    const microtel::HealthSnapshot health = provider->GetExporterHealth();
    EXPECT_GE(health.batches_sent, 1U);
    EXPECT_EQ(health.batches_failed, 0U) << health.last_error_message;
    ExpectNoDeliveryDrops(health);

    // The collector rejects a body whose bytes do not match its
    // Content-Encoding, so every span arriving is the proof that it inflated
    // what we deflated.
    const std::string span_prefix = marker + ".";
    const std::size_t delivered =
        PollForOccurrences(output_file, span_prefix, kGzipSpanCount, kCollectorPollTimeout);
    EXPECT_EQ(delivered, kGzipSpanCount);
}

TEST(HttpBatchingConformance, LargePayload)
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
    auto result = ConfigureTlsBuilder(builder, endpoint).Build();
    ASSERT_TRUE(result.has_value()) << result.error().message;
    const std::shared_ptr<microtel::Provider> provider = std::move(*result);

    const std::string marker = microtel::testing::UniqueMarker();
    EmitLargeSpans(*provider, marker, kLargePayloadSpanCount);

    ASSERT_EQ(provider->ForceFlush(kFlushTimeout), microtel::Status::Completed);

    const microtel::HealthSnapshot health = provider->GetExporterHealth();
    EXPECT_GE(health.batches_sent, 1U);
    EXPECT_EQ(health.batches_failed, 0U) << health.last_error_message;
    ExpectNoDeliveryDrops(health);

    // ~800 KiB of span bodies: more than a dozen HTTP/2 DATA frames and several
    // WINDOW_UPDATE round trips past the 65 535-byte default stream window.
    const std::string span_prefix = marker + ".";
    const std::size_t delivered =
        PollForOccurrences(output_file, span_prefix, kLargePayloadSpanCount, kCollectorPollTimeout);
    EXPECT_EQ(delivered, kLargePayloadSpanCount);
}

}  // namespace
