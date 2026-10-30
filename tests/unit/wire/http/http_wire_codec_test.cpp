// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0
//
// Unit tests for HttpWireCodec — every row of the HTTP status-code matrix
// from error-model.md §7.1, plus header construction and auth injection.
// Uses FakeTransport (synchronous, no real socket).

#include "wire/http/http_wire_codec.hpp"

#include "microtel/error.hpp"
#include "microtel/expected.hpp"
#include "microtel/internal/encoded_payload.hpp"
#include "microtel/internal/wire_result.hpp"
#include "microtel/provider.hpp"

#include "fakes/fake_auth_provider.hpp"
#include "fakes/fake_diagnostics_sink.hpp"
#include "fakes/fake_steady_clock.hpp"
#include "fakes/fake_transport.hpp"
#include "helpers/gunzip.hpp"
#include "wire/gzip.hpp"

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace mt = microtel;
namespace mti = microtel::internal;
namespace mtfk = microtel::testing;
namespace mtw = microtel::wire;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static mti::EncodedPayload MakePayload(std::size_t n = 4)
{
    auto buf = std::make_unique<std::byte[]>(n);
    return mti::EncodedPayload{std::move(buf), n};
}

static mti::TransportResult OkResponse(const std::string& status = "200")
{
    return mti::TransportResult{
        .success = true,
        .response_headers = {{.name = ":status", .value = status}},
        .response_trailers = {},
        .response_body = {},
        .error = {},
    };
}

static mtw::HttpWireCodecConfig MakeConfig()
{
    return mtw::HttpWireCodecConfig{
        .host = "localhost:4318",
        .scheme = "http",
        .path = "",
        .signal_path = {},
        .extra_headers = {},
    };
}

static std::string FindHeader(const std::vector<mti::HeaderField>& headers, const std::string& name)
{
    for (const auto& h : headers)
    {
        if (h.name == name)
        {
            return h.value;
        }
    }
    return {};
}

// ---------------------------------------------------------------------------
// 2xx success
// ---------------------------------------------------------------------------

TEST(HttpWireCodecTest, Send_200_Succeeds)
{
    mtfk::FakeTransport transport;
    transport.default_response = OkResponse("200");
    mtw::HttpWireCodec codec{&transport, MakeConfig()};

    const auto result = codec.Send(MakePayload(), std::chrono::milliseconds(1000));
    EXPECT_TRUE(result.success);
    EXPECT_FALSE(result.retryable);
}

TEST(HttpWireCodecTest, Send_206_Succeeds)
{
    mtfk::FakeTransport transport;
    transport.default_response = OkResponse("206");
    mtw::HttpWireCodec codec{&transport, MakeConfig()};

    const auto result = codec.Send(MakePayload(), std::chrono::milliseconds(1000));
    EXPECT_TRUE(result.success);
}

// ---------------------------------------------------------------------------
// Retryable 4xx/5xx
// ---------------------------------------------------------------------------

TEST(HttpWireCodecTest, Send_429_IsRetryable)
{
    mtfk::FakeTransport transport;
    transport.default_response = OkResponse("429");
    mtw::HttpWireCodec codec{&transport, MakeConfig()};

    const auto result = codec.Send(MakePayload(), std::chrono::milliseconds(1000));
    EXPECT_FALSE(result.success);
    EXPECT_TRUE(result.retryable);
}

TEST(HttpWireCodecTest, Send_429_WithRetryAfterHeader_PropagatesDelay)
{
    mtfk::FakeTransport transport;
    transport.default_response = mti::TransportResult{
        .success = true,
        .response_headers = {{.name = ":status", .value = "429"},
                             {.name = "retry-after", .value = "7"}},
        .response_trailers = {},
        .response_body = {},
        .error = {},
    };
    mtw::HttpWireCodec codec{&transport, MakeConfig()};

    const auto result = codec.Send(MakePayload(), std::chrono::milliseconds(1000));
    EXPECT_FALSE(result.success);
    EXPECT_TRUE(result.retryable);
    ASSERT_TRUE(result.retry_after.has_value());
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access) — guarded by ASSERT_TRUE above
    EXPECT_EQ(*result.retry_after, std::chrono::milliseconds(7000));
}

TEST(HttpWireCodecTest, Send_502_IsRetryable)
{
    mtfk::FakeTransport transport;
    transport.default_response = OkResponse("502");
    mtw::HttpWireCodec codec{&transport, MakeConfig()};

    const auto result = codec.Send(MakePayload(), std::chrono::milliseconds(1000));
    EXPECT_FALSE(result.success);
    EXPECT_TRUE(result.retryable);
}

TEST(HttpWireCodecTest, Send_503_IsRetryable)
{
    mtfk::FakeTransport transport;
    transport.default_response = OkResponse("503");
    mtw::HttpWireCodec codec{&transport, MakeConfig()};

    const auto result = codec.Send(MakePayload(), std::chrono::milliseconds(1000));
    EXPECT_FALSE(result.success);
    EXPECT_TRUE(result.retryable);
}

TEST(HttpWireCodecTest, Send_503_WithRetryAfterHeader_PropagatesDelay)
{
    mtfk::FakeTransport transport;
    transport.default_response = mti::TransportResult{
        .success = true,
        .response_headers = {{.name = ":status", .value = "503"},
                             {.name = "retry-after", .value = "30"}},
        .response_trailers = {},
        .response_body = {},
        .error = {},
    };
    mtw::HttpWireCodec codec{&transport, MakeConfig()};

    const auto result = codec.Send(MakePayload(), std::chrono::milliseconds(1000));
    EXPECT_TRUE(result.retryable);
    ASSERT_TRUE(result.retry_after.has_value());
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access) — guarded by ASSERT_TRUE above
    EXPECT_EQ(*result.retry_after, std::chrono::milliseconds(30000));
}

TEST(HttpWireCodecTest, Send_504_IsRetryable)
{
    mtfk::FakeTransport transport;
    transport.default_response = OkResponse("504");
    mtw::HttpWireCodec codec{&transport, MakeConfig()};

    const auto result = codec.Send(MakePayload(), std::chrono::milliseconds(1000));
    EXPECT_FALSE(result.success);
    EXPECT_TRUE(result.retryable);
}

// ---------------------------------------------------------------------------
// Non-retryable failures
// ---------------------------------------------------------------------------

TEST(HttpWireCodecTest, Send_404_IsNonRetryable)
{
    mtfk::FakeTransport transport;
    transport.default_response = OkResponse("404");
    mtw::HttpWireCodec codec{&transport, MakeConfig()};

    const auto result = codec.Send(MakePayload(), std::chrono::milliseconds(1000));
    EXPECT_FALSE(result.success);
    EXPECT_FALSE(result.retryable);
}

TEST(HttpWireCodecTest, Send_415_IsNonRetryable)
{
    mtfk::FakeTransport transport;
    transport.default_response = OkResponse("415");
    mtw::HttpWireCodec codec{&transport, MakeConfig()};

    const auto result = codec.Send(MakePayload(), std::chrono::milliseconds(1000));
    EXPECT_FALSE(result.success);
    EXPECT_FALSE(result.retryable);
}

TEST(HttpWireCodecTest, Send_Other4xx_IsNonRetryable)
{
    mtfk::FakeTransport transport;
    transport.default_response = OkResponse("401");
    mtw::HttpWireCodec codec{&transport, MakeConfig()};

    const auto result = codec.Send(MakePayload(), std::chrono::milliseconds(1000));
    EXPECT_FALSE(result.success);
    EXPECT_FALSE(result.retryable);
}

TEST(HttpWireCodecTest, Send_500_IsNonRetryable)
{
    mtfk::FakeTransport transport;
    transport.default_response = OkResponse("500");
    mtw::HttpWireCodec codec{&transport, MakeConfig()};

    const auto result = codec.Send(MakePayload(), std::chrono::milliseconds(1000));
    EXPECT_FALSE(result.success);
    EXPECT_FALSE(result.retryable);
}

// ---------------------------------------------------------------------------
// Transport-layer failure
// ---------------------------------------------------------------------------

TEST(HttpWireCodecTest, Send_TransportFailure_ReturnsError)
{
    mtfk::FakeTransport transport;
    transport.default_response = mti::TransportResult{
        .success = false,
        .response_headers = {},
        .response_trailers = {},
        .response_body = {},
        .error = mt::Error{.kind = mt::Error::Kind::Network, .message = "connection reset"},
    };
    mtw::HttpWireCodec codec{&transport, MakeConfig()};

    const auto result = codec.Send(MakePayload(), std::chrono::milliseconds(1000));
    EXPECT_FALSE(result.success);
    EXPECT_TRUE(result.error.has_value());
}

// The `Response > max_response_bytes` row of error-model.md §7.1. The transport
// enforces the cap (it owns the buffer); the codec's part is the row's
// classification — terminal, and counted, unlike every other transport failure,
// because the peer answers the retry with the same oversized response.
TEST(HttpWireCodecTest, Send_ResponseTooLarge_IsNonRetryableAndCounted)
{
    mtfk::FakeTransport transport;
    mtfk::FakeDiagnosticsSink sink;
    transport.default_response = mti::TransportResult{
        .success = false,
        .response_headers = {},
        .response_trailers = {},
        .response_body = {},
        .error = mt::Error{.kind = mt::Error::Kind::Malformed,
                           .message = "response exceeds max_response_bytes"},
        .response_too_large = true,
    };
    mtw::HttpWireCodec codec{&transport, MakeConfig(), nullptr, &sink};

    const auto result = codec.Send(MakePayload(), std::chrono::milliseconds(1000));
    EXPECT_FALSE(result.success);
    EXPECT_FALSE(result.retryable);
    ASSERT_TRUE(result.error.has_value());
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access) — guarded by ASSERT_TRUE above
    EXPECT_EQ(result.error->message, "response exceeds max_response_bytes");
    EXPECT_EQ(sink.drop_counters.at(static_cast<std::size_t>(mt::DropReason::ResponseTooLarge)),
              1U);
}

// The same shape when the trailers were what overran. `max_trailer_bytes` has
// no counter of its own — error-model.md §3 maps it onto `response_too_large`,
// with the message carrying which cap it was.
TEST(HttpWireCodecTest, Send_TrailersTooLarge_CountsResponseTooLarge)
{
    mtfk::FakeTransport transport;
    mtfk::FakeDiagnosticsSink sink;
    transport.default_response = mti::TransportResult{
        .success = false,
        .response_headers = {},
        .response_trailers = {},
        .response_body = {},
        .error = mt::Error{.kind = mt::Error::Kind::Malformed,
                           .message = "response trailers exceed max_trailer_bytes"},
        .response_too_large = true,
    };
    mtw::HttpWireCodec codec{&transport, MakeConfig(), nullptr, &sink};

    const auto result = codec.Send(MakePayload(), std::chrono::milliseconds(1000));
    EXPECT_FALSE(result.retryable);
    EXPECT_EQ(sink.drop_counters.at(static_cast<std::size_t>(mt::DropReason::ResponseTooLarge)),
              1U);
}

// The `transport_busy` row of error-model.md §3: the transport refused to
// queue the request because its request queue was at `max_pending_requests`.
// Counted, but *retryable* — unlike an oversized response, a full queue is
// transient and the retry engine is exactly what should handle it.
TEST(HttpWireCodecTest, Send_TransportBusy_IsRetryableAndCounted)
{
    mtfk::FakeTransport transport;
    mtfk::FakeDiagnosticsSink sink;
    transport.default_response = mti::TransportResult{
        .success = false,
        .response_headers = {},
        .response_trailers = {},
        .response_body = {},
        .error = mt::Error{.kind = mt::Error::Kind::ResourceExhausted,
                           .message = "transport request queue full"},
        .response_too_large = false,
        .transport_busy = true,
    };
    mtw::HttpWireCodec codec{&transport, MakeConfig(), nullptr, &sink};

    const auto result = codec.Send(MakePayload(), std::chrono::milliseconds(1000));
    EXPECT_FALSE(result.success);
    EXPECT_TRUE(result.retryable);
    EXPECT_EQ(sink.drop_counters.at(static_cast<std::size_t>(mt::DropReason::TransportBusy)), 1U);
}

// An ordinary transport failure stays retryable and uncounted — the cap flag,
// not the mere presence of an error, is what changes the classification.
TEST(HttpWireCodecTest, Send_OrdinaryTransportFailure_StaysRetryable)
{
    mtfk::FakeTransport transport;
    mtfk::FakeDiagnosticsSink sink;
    transport.default_response = mti::TransportResult{
        .success = false,
        .response_headers = {},
        .response_trailers = {},
        .response_body = {},
        .error = mt::Error{.kind = mt::Error::Kind::Network, .message = "connection reset"},
    };
    mtw::HttpWireCodec codec{&transport, MakeConfig(), nullptr, &sink};

    const auto result = codec.Send(MakePayload(), std::chrono::milliseconds(1000));
    EXPECT_TRUE(result.retryable);
    EXPECT_EQ(sink.drop_counters.at(static_cast<std::size_t>(mt::DropReason::ResponseTooLarge)),
              0U);
}

// ---------------------------------------------------------------------------
// Request header construction
// ---------------------------------------------------------------------------

TEST(HttpWireCodecTest, Send_BuildsCorrectPseudoHeaders)
{
    mtfk::FakeTransport transport;
    transport.default_response = OkResponse();
    mtw::HttpWireCodec codec{&transport, MakeConfig()};

    (void)codec.Send(MakePayload(), std::chrono::milliseconds(1000));

    ASSERT_EQ(transport.sent_specs.size(), 1U);
    const auto& headers = transport.sent_specs[0].headers;

    EXPECT_EQ(FindHeader(headers, ":method"), "POST");
    EXPECT_EQ(FindHeader(headers, ":scheme"), "http");
    EXPECT_EQ(FindHeader(headers, ":path"), "/v1/traces");
    EXPECT_EQ(FindHeader(headers, ":authority"), "localhost:4318");
    EXPECT_EQ(FindHeader(headers, "content-type"), "application/x-protobuf");
}

TEST(HttpWireCodecTest, Send_EmptyPath_ResolvesToV1Traces)
{
    mtfk::FakeTransport transport;
    transport.default_response = OkResponse();
    mtw::HttpWireCodecConfig cfg = MakeConfig();
    cfg.path = "";
    mtw::HttpWireCodec codec{&transport, cfg};

    (void)codec.Send(MakePayload(), std::chrono::milliseconds(1000));
    ASSERT_EQ(transport.sent_specs.size(), 1U);

    const auto& headers = transport.sent_specs[0].headers;
    for (const auto& h : headers)
    {
        if (h.name == ":path")
        {
            EXPECT_EQ(h.value, "/v1/traces");
            return;
        }
    }
    FAIL() << ":path header not found";
}

TEST(HttpWireCodecTest, Send_CustomBasePath_AppendsV1Traces)
{
    mtfk::FakeTransport transport;
    transport.default_response = OkResponse();
    mtw::HttpWireCodecConfig cfg = MakeConfig();
    cfg.path = "/prefix";
    mtw::HttpWireCodec codec{&transport, cfg};

    (void)codec.Send(MakePayload(), std::chrono::milliseconds(1000));
    ASSERT_EQ(transport.sent_specs.size(), 1U);

    for (const auto& h : transport.sent_specs[0].headers)
    {
        if (h.name == ":path")
        {
            EXPECT_EQ(h.value, "/prefix/v1/traces");
            return;
        }
    }
    FAIL() << ":path header not found";
}

TEST(HttpWireCodecTest, Send_ExtraHeaders_IncludedInRequest)
{
    mtfk::FakeTransport transport;
    transport.default_response = OkResponse();
    mtw::HttpWireCodecConfig cfg = MakeConfig();
    cfg.extra_headers.push_back({.name = "x-custom", .value = "value"});
    mtw::HttpWireCodec codec{&transport, cfg};

    (void)codec.Send(MakePayload(), std::chrono::milliseconds(1000));
    ASSERT_EQ(transport.sent_specs.size(), 1U);

    bool found = false;
    for (const auto& h : transport.sent_specs[0].headers)
    {
        if (h.name == "x-custom" && h.value == "value")
        {
            found = true;
        }
    }
    EXPECT_TRUE(found);
}

// ---------------------------------------------------------------------------
// Auth provider
// ---------------------------------------------------------------------------

TEST(HttpWireCodecTest, Send_WithAuth_AddsAuthorizationHeader)
{
    mtfk::FakeTransport transport;
    transport.default_response = OkResponse();
    mtfk::FakeAuthProvider auth;
    auth.static_value = std::optional<std::string>{"Bearer tok"};
    mtfk::FakeSteadyClock clock;
    mtw::HttpWireCodec codec{&transport, MakeConfig(), &auth, nullptr, &clock};

    (void)codec.Send(MakePayload(), std::chrono::milliseconds(1000));
    ASSERT_EQ(transport.sent_specs.size(), 1U);

    bool found = false;
    for (const auto& h : transport.sent_specs[0].headers)
    {
        if (h.name == "authorization" && h.value == "Bearer tok")
        {
            found = true;
        }
    }
    EXPECT_TRUE(found);
}

TEST(HttpWireCodecTest, Send_WithAuth_NoValue_NoAuthorizationHeader)
{
    mtfk::FakeTransport transport;
    transport.default_response = OkResponse();
    mtfk::FakeAuthProvider auth;
    auth.static_value = std::optional<std::string>{};  // nullopt inside the Expected
    mtfk::FakeSteadyClock clock;
    mtw::HttpWireCodec codec{&transport, MakeConfig(), &auth, nullptr, &clock};

    (void)codec.Send(MakePayload(), std::chrono::milliseconds(1000));
    ASSERT_EQ(transport.sent_specs.size(), 1U);

    for (const auto& h : transport.sent_specs[0].headers)
    {
        EXPECT_NE(h.name, "authorization");
    }
}

// Issue #250 / interfaces.md §4.9: an auth provider that fails drops the
// batch. Sending it without the header is worse than not sending it — the
// receiver either rejects it one hop later with a message that blames the
// credential, or accepts it unauthenticated and nothing is seen at all.
TEST(HttpWireCodecTest, Send_AuthProviderError_DropsBatchWithoutSending)
{
    mtfk::FakeTransport transport;
    transport.default_response = OkResponse();
    mtfk::FakeAuthProvider auth;
    auth.static_value = mt::make_unexpected(
        mt::Error{.kind = mt::Error::Kind::Network, .message = "token fetch failed"});
    mtfk::FakeSteadyClock clock;
    mtw::HttpWireCodec codec{&transport, MakeConfig(), &auth, nullptr, &clock};

    const auto result = codec.Send(MakePayload(), std::chrono::milliseconds(1000));
    EXPECT_FALSE(result.success);
    EXPECT_FALSE(result.retryable) << "non_retryable_failure per interfaces.md §4.9";
    ASSERT_TRUE(result.error.has_value());
    // NOLINTBEGIN(bugprone-unchecked-optional-access) — guarded by ASSERT_TRUE above
    EXPECT_EQ(result.error->kind, mt::Error::Kind::Network) << "the provider's kind survives";
    EXPECT_NE(result.error->message.find("authorization"), std::string::npos)
        << "last_error_message must name auth, not the receiver: " << result.error->message;
    EXPECT_NE(result.error->message.find("token fetch failed"), std::string::npos);
    // NOLINTEND(bugprone-unchecked-optional-access)
    EXPECT_TRUE(transport.sent_specs.empty())
        << "an unauthenticated request must never reach the wire";
}

TEST(HttpWireCodecTest, SendAll_AuthProviderError_DropsOnlyTheAffectedBatch)
{
    mtfk::FakeTransport transport;
    transport.default_response = OkResponse();
    mtfk::FakeAuthProvider auth;
    auth.scripted_responses.emplace_back(std::optional<std::string>{"Bearer tok"});
    auth.scripted_responses.emplace_back(mt::make_unexpected(
        mt::Error{.kind = mt::Error::Kind::InternalFailure, .message = "callback threw"}));
    auth.scripted_responses.emplace_back(std::optional<std::string>{"Bearer tok"});
    mtfk::FakeSteadyClock clock;
    mtw::HttpWireCodec codec{&transport, MakeConfig(), &auth, nullptr, &clock};

    std::vector<mti::EncodedPayload> payloads;
    payloads.push_back(MakePayload());
    payloads.push_back(MakePayload());
    payloads.push_back(MakePayload());

    const auto results = codec.SendAll(std::move(payloads), std::chrono::milliseconds(1000));
    ASSERT_EQ(results.size(), 3U);
    EXPECT_TRUE(results[0].success);
    EXPECT_FALSE(results[1].success);
    EXPECT_FALSE(results[1].retryable);
    EXPECT_TRUE(results[2].success) << "the fan-out continues past the failed batch";
    EXPECT_EQ(transport.sent_specs.size(), 2U);
}

// ---------------------------------------------------------------------------
// Request header validation (ICP 0042 §3): CR, LF and NUL are rejected in
// every field after assembly, so an auth token produced at send time cannot
// inject a header on HTTP/1.1 or be refused by nghttp2 on HTTP/2.
// ---------------------------------------------------------------------------

TEST(HttpWireCodecTest, Send_AuthTokenWithCrLfOrNul_DropsBatchWithoutSending)
{
    for (const char bad : {'\r', '\n', '\0'})
    {
        mtfk::FakeTransport transport;
        transport.default_response = OkResponse();
        mtfk::FakeAuthProvider auth;
        std::string token = "Bearer secret";
        token.push_back(bad);
        token += "x-injected: 1";
        auth.static_value = std::optional<std::string>{token};
        mtfk::FakeSteadyClock clock;
        mtw::HttpWireCodec codec{&transport, MakeConfig(), &auth, nullptr, &clock};

        const auto result = codec.Send(MakePayload(), std::chrono::milliseconds(1000));
        EXPECT_FALSE(result.success);
        EXPECT_FALSE(result.retryable) << "a bad header does not get better on retry";
        const std::string message = result.error ? result.error->message : std::string{};
        EXPECT_EQ(result.error ? result.error->kind : mt::Error::Kind::Unspecified,
                  mt::Error::Kind::Malformed);
        EXPECT_NE(message.find("\"authorization\""), std::string::npos)
            << "the message names the field: " << message;
        EXPECT_EQ(message.find("secret"), std::string::npos)
            << "the message never carries the value: " << message;
        EXPECT_TRUE(transport.sent_specs.empty()) << "the request must never reach the transport";
    }
}

TEST(HttpWireCodecTest, Send_ExtraHeaderValueWithLf_DropsBatchWithoutSending)
{
    mtfk::FakeTransport transport;
    transport.default_response = OkResponse();
    auto config = MakeConfig();
    config.extra_headers.push_back({.name = "x-tenant", .value = "a\nb"});
    mtw::HttpWireCodec codec{&transport, config};

    const auto result = codec.Send(MakePayload(), std::chrono::milliseconds(1000));
    EXPECT_FALSE(result.success);
    EXPECT_FALSE(result.retryable);
    const std::string message = result.error ? result.error->message : std::string{};
    EXPECT_NE(message.find("\"x-tenant\""), std::string::npos) << message;
    EXPECT_EQ(message.find("authorization header unavailable"), std::string::npos)
        << "a bad static header is not an auth failure: " << message;
    EXPECT_TRUE(transport.sent_specs.empty());
}

TEST(HttpWireCodecTest, Send_ExtraHeaderNameWithCr_DropsBatchWithoutSending)
{
    mtfk::FakeTransport transport;
    transport.default_response = OkResponse();
    auto config = MakeConfig();
    config.extra_headers.push_back({.name = "x-a\rx-b", .value = "1"});
    mtw::HttpWireCodec codec{&transport, config};

    const auto result = codec.Send(MakePayload(), std::chrono::milliseconds(1000));
    EXPECT_FALSE(result.success);
    EXPECT_FALSE(result.retryable);
    EXPECT_EQ(result.error ? result.error->kind : mt::Error::Kind::Unspecified,
              mt::Error::Kind::Malformed);
    EXPECT_TRUE(transport.sent_specs.empty());
}

TEST(HttpWireCodecTest, SendAll_AuthTokenWithLf_DropsOnlyTheAffectedBatch)
{
    mtfk::FakeTransport transport;
    transport.default_response = OkResponse();
    mtfk::FakeAuthProvider auth;
    auth.scripted_responses.emplace_back(std::optional<std::string>{"Bearer tok"});
    auth.scripted_responses.emplace_back(std::optional<std::string>{"Bearer tok\nx: y"});
    auth.scripted_responses.emplace_back(std::optional<std::string>{"Bearer tok"});
    mtfk::FakeSteadyClock clock;
    mtw::HttpWireCodec codec{&transport, MakeConfig(), &auth, nullptr, &clock};

    std::vector<mti::EncodedPayload> payloads;
    payloads.push_back(MakePayload());
    payloads.push_back(MakePayload());
    payloads.push_back(MakePayload());

    const auto results = codec.SendAll(std::move(payloads), std::chrono::milliseconds(1000));
    ASSERT_EQ(results.size(), 3U);
    EXPECT_TRUE(results[0].success);
    EXPECT_FALSE(results[1].success);
    EXPECT_FALSE(results[1].retryable);
    EXPECT_TRUE(results[2].success);
    EXPECT_EQ(transport.sent_specs.size(), 2U);
}

// ---------------------------------------------------------------------------
// M5-B: Partial-success parsing
// ---------------------------------------------------------------------------

// ExportTraceServiceResponse { partial_success { rejected_spans: 42 } }
// Outer: field 1 (partial_success), wt=2: tag=0x0A, len=2.
// Inner: field 1 (rejected_spans), wt=0: tag=0x08, varint(42)=0x2A.
static std::vector<std::byte> MakePartialSuccessBody(std::uint8_t rejected_varint)
{
    return {
        std::byte{0x0A},             // outer tag: field 1, wt=2
        std::byte{0x02},             // inner length = 2
        std::byte{0x08},             // inner tag: field 1, wt=0
        std::byte{rejected_varint},  // varint value
    };
}

TEST(HttpWireCodecTest, PartialSuccess_PopulatesRejectedSpans)
{
    mtfk::FakeTransport transport;
    auto resp = OkResponse("200");
    resp.response_body = MakePartialSuccessBody(42U);
    transport.default_response = resp;
    mtw::HttpWireCodec codec{&transport, MakeConfig()};

    const auto result = codec.Send(MakePayload(), std::chrono::milliseconds(1000));
    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.partial_success_rejected, 42U);
}

TEST(HttpWireCodecTest, PartialSuccess_EmptyBody_ZeroRejected)
{
    mtfk::FakeTransport transport;
    transport.default_response = OkResponse("200");
    mtw::HttpWireCodec codec{&transport, MakeConfig()};

    const auto result = codec.Send(MakePayload(), std::chrono::milliseconds(1000));
    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.partial_success_rejected, 0U);
}

TEST(HttpWireCodecTest, PartialSuccess_NonSuccessStatus_BodyIgnored)
{
    // 400 is non-retryable failure — body must not populate partial_success_rejected.
    mtfk::FakeTransport transport;
    const mti::TransportResult resp{
        .success = true,
        .response_headers = {{.name = ":status", .value = "400"}},
        .response_trailers = {},
        .response_body = MakePartialSuccessBody(7U),
        .error = {},
    };
    transport.default_response = resp;
    mtw::HttpWireCodec codec{&transport, MakeConfig()};

    const auto result = codec.Send(MakePayload(), std::chrono::milliseconds(1000));
    EXPECT_FALSE(result.success);
    EXPECT_EQ(result.partial_success_rejected, 0U);
}

// ---------------------------------------------------------------------------
// signal_path override (M12 metrics codec wiring)
// ---------------------------------------------------------------------------

TEST(HttpWireCodecTest, Send_SignalPath_OverridesDefaultTracesPath)
{
    mtfk::FakeTransport transport;
    transport.default_response = OkResponse();
    mtw::HttpWireCodecConfig cfg = MakeConfig();
    cfg.signal_path = "/v1/metrics";
    mtw::HttpWireCodec codec{&transport, cfg};

    (void)codec.Send(MakePayload(), std::chrono::milliseconds(1000));
    ASSERT_EQ(transport.sent_specs.size(), 1U);

    EXPECT_EQ(FindHeader(transport.sent_specs[0].headers, ":path"), "/v1/metrics");
}

TEST(HttpWireCodecTest, Send_SignalPath_WithBasePath_UsesSignalPathDirectly)
{
    // signal_path wins even when a base `path` prefix is also set.
    mtfk::FakeTransport transport;
    transport.default_response = OkResponse();
    mtw::HttpWireCodecConfig cfg = MakeConfig();
    cfg.path = "/prefix";
    cfg.signal_path = "/v1/metrics";
    mtw::HttpWireCodec codec{&transport, cfg};

    (void)codec.Send(MakePayload(), std::chrono::milliseconds(1000));
    ASSERT_EQ(transport.sent_specs.size(), 1U);

    EXPECT_EQ(FindHeader(transport.sent_specs[0].headers, ":path"), "/v1/metrics");
}

// ---------------------------------------------------------------------------
// Lazy connect (ICP 0017)
// ---------------------------------------------------------------------------

TEST(HttpWireCodecTest, Send_WhenDisconnected_ConnectsThenSucceeds)
{
    mtfk::FakeTransport transport;
    transport.state = mt::ConnectionState::Disconnected;
    transport.default_response = OkResponse();
    mtw::HttpWireCodec codec{&transport, MakeConfig()};

    const auto result = codec.Send(MakePayload(), std::chrono::milliseconds(1000));

    EXPECT_TRUE(result.success);
    EXPECT_EQ(transport.connect_calls.size(), 1U);
    EXPECT_EQ(transport.sent_specs.size(), 1U);
}

TEST(HttpWireCodecTest, Send_WhenAlreadyConnected_DoesNotCallConnect)
{
    mtfk::FakeTransport transport;  // default state: Connected
    transport.default_response = OkResponse();
    mtw::HttpWireCodec codec{&transport, MakeConfig()};

    const auto result = codec.Send(MakePayload(), std::chrono::milliseconds(1000));

    EXPECT_TRUE(result.success);
    EXPECT_EQ(transport.connect_calls.size(), 0U);
}

TEST(HttpWireCodecTest, Send_WhenDisconnectedAndConnectFails_ReturnsRetryableWithoutSending)
{
    mtfk::FakeTransport transport;
    transport.state = mt::ConnectionState::Disconnected;
    transport.connect_result =
        mt::make_unexpected(mt::Error{.kind = mt::Error::Kind::Network, .message = "refused"});
    mtw::HttpWireCodec codec{&transport, MakeConfig()};

    const auto result = codec.Send(MakePayload(), std::chrono::milliseconds(1000));

    EXPECT_FALSE(result.success);
    EXPECT_TRUE(result.retryable);
    ASSERT_TRUE(result.error.has_value());
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access) — guarded by ASSERT_TRUE above
    EXPECT_EQ(result.error->message, "refused");
    EXPECT_EQ(transport.connect_calls.size(), 1U);
    EXPECT_EQ(transport.sent_specs.size(), 0U);  // never got to the actual send
}

// A connect that failed because the peer speaks the wrong protocol — an
// HTTP/1.1-only receiver, or a TLS endpoint that would not negotiate h2
// (issue #166) — will fail the next attempt identically. Retrying it burns the
// whole retry budget on a misconfiguration no backoff can outlast, and buries
// the one error message that says what to change.
TEST(HttpWireCodecTest, Send_WhenConnectFailsWithProtocolError_IsNotRetryable)
{
    mtfk::FakeTransport transport;
    transport.state = mt::ConnectionState::Disconnected;
    transport.connect_result = mt::make_unexpected(
        mt::Error{.kind = mt::Error::Kind::Protocol, .message = "endpoint is HTTP/1.1-only"});
    mtw::HttpWireCodec codec{&transport, MakeConfig()};

    const auto result = codec.Send(MakePayload(), std::chrono::milliseconds(1000));

    EXPECT_FALSE(result.success);
    EXPECT_FALSE(result.retryable);
    ASSERT_TRUE(result.error.has_value());
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access) — guarded by ASSERT_TRUE above
    EXPECT_EQ(result.error->message, "endpoint is HTTP/1.1-only");
    EXPECT_EQ(transport.sent_specs.size(), 0U);
}

// ---------------------------------------------------------------------------
// Drop accounting — issue #169. connect_failure was documented as owned by
// "transport", which owns no diagnostics sink; the codec is where the failed
// connect is observed, so it is where the counter moves.
// ---------------------------------------------------------------------------

static std::uint64_t DropCount(const mtfk::FakeDiagnosticsSink& sink, mt::DropReason reason)
{
    return sink.drop_counters.at(static_cast<std::size_t>(reason));
}

TEST(HttpWireCodecTest, Diagnostics_ConnectFails_CountsConnectFailure)
{
    mtfk::FakeTransport transport;
    mtfk::FakeDiagnosticsSink sink;
    transport.state = mt::ConnectionState::Disconnected;
    transport.connect_result =
        mt::make_unexpected(mt::Error{.kind = mt::Error::Kind::Network, .message = "refused"});
    mtw::HttpWireCodec codec{&transport, MakeConfig(), nullptr, &sink};

    const auto result = codec.Send(MakePayload(), std::chrono::milliseconds(1000));

    EXPECT_FALSE(result.success);
    EXPECT_EQ(DropCount(sink, mt::DropReason::ConnectFailure), 1U);
}

// A Protocol connect failure is not retried, but it is still a lost batch:
// the counter must move for every kind the transport reports, not just the
// retryable ones.
TEST(HttpWireCodecTest, Diagnostics_ProtocolConnectFailure_StillCountsConnectFailure)
{
    mtfk::FakeTransport transport;
    mtfk::FakeDiagnosticsSink sink;
    transport.state = mt::ConnectionState::Disconnected;
    transport.connect_result = mt::make_unexpected(
        mt::Error{.kind = mt::Error::Kind::Protocol, .message = "endpoint is HTTP/1.1-only"});
    mtw::HttpWireCodec codec{&transport, MakeConfig(), nullptr, &sink};

    const auto result = codec.Send(MakePayload(), std::chrono::milliseconds(1000));

    EXPECT_FALSE(result.retryable);
    EXPECT_EQ(DropCount(sink, mt::DropReason::ConnectFailure), 1U);
}

TEST(HttpWireCodecTest, Diagnostics_SendAllConnectFails_CountsConnectFailureOnce)
{
    mtfk::FakeTransport transport;
    mtfk::FakeDiagnosticsSink sink;
    transport.state = mt::ConnectionState::Disconnected;
    transport.connect_result =
        mt::make_unexpected(mt::Error{.kind = mt::Error::Kind::Network, .message = "refused"});
    mtw::HttpWireCodec codec{&transport, MakeConfig(), nullptr, &sink};

    std::vector<mti::EncodedPayload> payloads;
    payloads.push_back(MakePayload());
    payloads.push_back(MakePayload());
    (void)codec.SendAll(std::move(payloads), std::chrono::milliseconds(1000));

    // SendAll makes one prologue connect attempt for the whole fan-out, so
    // one failed connect is one connect_failure however many payloads waited
    // behind it — those are accounted for by the exporter's batch counters.
    EXPECT_EQ(DropCount(sink, mt::DropReason::ConnectFailure), 1U);
}

TEST(HttpWireCodecTest, Diagnostics_ConnectSucceeds_CountsNothing)
{
    mtfk::FakeTransport transport;
    mtfk::FakeDiagnosticsSink sink;
    transport.state = mt::ConnectionState::Disconnected;
    transport.default_response = OkResponse();
    mtw::HttpWireCodec codec{&transport, MakeConfig(), nullptr, &sink};

    const auto result = codec.Send(MakePayload(), std::chrono::milliseconds(1000));

    EXPECT_TRUE(result.success);
    EXPECT_EQ(DropCount(sink, mt::DropReason::ConnectFailure), 0U);
}

TEST(HttpWireCodecTest, Diagnostics_NullSink_IsNotDereferenced)
{
    mtfk::FakeTransport transport;
    transport.state = mt::ConnectionState::Disconnected;
    transport.connect_result =
        mt::make_unexpected(mt::Error{.kind = mt::Error::Kind::Network, .message = "refused"});
    mtw::HttpWireCodec codec{&transport, MakeConfig()};  // no sink

    const auto result = codec.Send(MakePayload(), std::chrono::milliseconds(1000));
    EXPECT_FALSE(result.success);
}

TEST(HttpWireCodecTest, SendAll_WhenDisconnected_ConnectsOnceThenSendsAll)
{
    mtfk::FakeTransport transport;
    transport.state = mt::ConnectionState::Disconnected;
    transport.default_response = OkResponse();
    mtw::HttpWireCodec codec{&transport, MakeConfig()};

    std::vector<mti::EncodedPayload> payloads;
    payloads.push_back(MakePayload());
    payloads.push_back(MakePayload());
    const auto results = codec.SendAll(std::move(payloads), std::chrono::milliseconds(1000));

    ASSERT_EQ(results.size(), 2U);
    EXPECT_TRUE(results[0].success);
    EXPECT_TRUE(results[1].success);
    EXPECT_EQ(transport.connect_calls.size(), 1U);  // one prologue check, not per-payload
    EXPECT_EQ(transport.sent_specs.size(), 2U);
}

TEST(HttpWireCodecTest, SendAll_WhenDisconnectedAndConnectFails_EveryPayloadMarkedRetryable)
{
    mtfk::FakeTransport transport;
    transport.state = mt::ConnectionState::Disconnected;
    transport.connect_result =
        mt::make_unexpected(mt::Error{.kind = mt::Error::Kind::Network, .message = "refused"});
    mtw::HttpWireCodec codec{&transport, MakeConfig()};

    std::vector<mti::EncodedPayload> payloads;
    payloads.push_back(MakePayload());
    payloads.push_back(MakePayload());
    payloads.push_back(MakePayload());
    const auto results = codec.SendAll(std::move(payloads), std::chrono::milliseconds(1000));

    // results[i] must line up with the caller's original payloads[i]
    // (OtlpExporter::FanOutAndProcess indexes both by i) — every batch gets
    // its own retryable result even though none of them were actually sent.
    ASSERT_EQ(results.size(), 3U);
    for (const auto& result : results)
    {
        EXPECT_FALSE(result.success);
        EXPECT_TRUE(result.retryable);
    }
    EXPECT_EQ(transport.connect_calls.size(), 1U);
    EXPECT_EQ(transport.sent_specs.size(), 0U);
}

// ---------------------------------------------------------------------------
// Request compression (grpc-wire-protocol.md §5.1; configuration.md
// `exporter.compression`)
// ---------------------------------------------------------------------------

static mti::EncodedPayload MakePayloadFrom(const std::string& s)
{
    auto buf = std::make_unique<std::byte[]>(s.size());
    for (std::size_t i = 0; i < s.size(); ++i)
    {
        buf[i] = static_cast<std::byte>(s[i]);
    }
    return mti::EncodedPayload{std::move(buf), s.size()};
}

TEST(HttpWireCodecTest, Send_CompressionOff_OmitsContentEncoding)
{
    mtfk::FakeTransport transport;
    transport.default_response = OkResponse();
    mtw::HttpWireCodec codec{&transport, MakeConfig()};

    (void)codec.Send(MakePayloadFrom("hello"), std::chrono::milliseconds(1000));

    ASSERT_EQ(transport.sent_specs.size(), 1U);
    EXPECT_EQ(FindHeader(transport.sent_specs[0].headers, "content-encoding"), "");
    // Body goes out verbatim.
    ASSERT_EQ(transport.sent_payloads[0].size(), 5U);
}

TEST(HttpWireCodecTest, Send_CompressionOn_SetsContentEncodingAndCompressesBody)
{
    const std::string body(2048, 'x');

    mtfk::FakeTransport transport;
    transport.default_response = OkResponse();
    mtw::HttpWireCodecConfig cfg = MakeConfig();
    cfg.compression_gzip = true;
    mtw::HttpWireCodec codec{&transport, cfg};

    (void)codec.Send(MakePayloadFrom(body), std::chrono::milliseconds(1000));

    ASSERT_EQ(transport.sent_specs.size(), 1U);
    EXPECT_EQ(FindHeader(transport.sent_specs[0].headers, "content-encoding"), "gzip");

    const auto& sent = transport.sent_payloads[0];
    EXPECT_LT(sent.size(), body.size());

    const auto restored = mtfk::GunzipToString(sent);
    ASSERT_TRUE(restored.has_value());
    EXPECT_EQ(restored.value_or(""), body);
}

TEST(HttpWireCodecTest, Send_CompressionOn_ContentLengthMatchesCompressedSize)
{
    // content-length describing the uncompressed size is the classic mistake
    // here: the server reads the wrong number of bytes and the stream hangs.
    mtfk::FakeTransport transport;
    transport.default_response = OkResponse();
    mtw::HttpWireCodecConfig cfg = MakeConfig();
    cfg.compression_gzip = true;
    mtw::HttpWireCodec codec{&transport, cfg};

    (void)codec.Send(MakePayloadFrom(std::string(2048, 'y')), std::chrono::milliseconds(1000));

    ASSERT_EQ(transport.sent_specs.size(), 1U);
    const auto declared = FindHeader(transport.sent_specs[0].headers, "content-length");
    EXPECT_EQ(declared, std::to_string(transport.sent_payloads[0].size()));
}

TEST(HttpWireCodecTest, SendAll_CompressionOn_CompressesEveryPayloadInOrder)
{
    mtfk::FakeTransport transport;
    transport.default_response = OkResponse();
    mtw::HttpWireCodecConfig cfg = MakeConfig();
    cfg.compression_gzip = true;
    mtw::HttpWireCodec codec{&transport, cfg};

    std::vector<mti::EncodedPayload> payloads;
    payloads.push_back(MakePayloadFrom("first"));
    payloads.push_back(MakePayloadFrom("second"));

    const auto results = codec.SendAll(std::move(payloads), std::chrono::milliseconds(1000));

    ASSERT_EQ(results.size(), 2U);
    ASSERT_EQ(transport.sent_payloads.size(), 2U);
    EXPECT_EQ(mtfk::GunzipToString(transport.sent_payloads[0]).value_or(""), "first");
    EXPECT_EQ(mtfk::GunzipToString(transport.sent_payloads[1]).value_or(""), "second");
}

// ---------------------------------------------------------------------------
// Response decompression (error-model.md §7.1, grpc-wire-protocol.md §5.2)
// ---------------------------------------------------------------------------

static std::vector<std::byte> Gzipped(std::span<const std::byte> plain)
{
    auto compressed = mtw::GzipCompress(plain);
    EXPECT_TRUE(compressed.has_value());
    return compressed.value_or(std::vector<std::byte>{});
}

/// A 200 whose body carries @p encoding in `content-encoding`.
static mti::TransportResult EncodedResponse(const std::string& encoding,
                                            std::vector<std::byte> body)
{
    return mti::TransportResult{
        .success = true,
        .response_headers = {{.name = ":status", .value = "200"},
                             {.name = "content-encoding", .value = encoding}},
        .response_trailers = {},
        .response_body = std::move(body),
        .error = {},
    };
}

TEST(HttpWireCodecTest, Send_AdvertisesAcceptEncodingEvenWithCompressionOff)
{
    // The advertisement says what this client can decode, which has nothing to
    // do with whether it chose to compress the request.
    mtfk::FakeTransport transport;
    transport.default_response = OkResponse();
    mtw::HttpWireCodec codec{&transport, MakeConfig()};

    (void)codec.Send(MakePayload(), std::chrono::milliseconds(1000));

    ASSERT_EQ(transport.sent_specs.size(), 1U);
    EXPECT_EQ(FindHeader(transport.sent_specs[0].headers, "accept-encoding"), "gzip");
}

TEST(HttpWireCodecTest, SendAll_AdvertisesAcceptEncoding)
{
    mtfk::FakeTransport transport;
    transport.default_response = OkResponse();
    mtw::HttpWireCodec codec{&transport, MakeConfig()};

    std::vector<mti::EncodedPayload> payloads;
    payloads.push_back(MakePayload());
    (void)codec.SendAll(std::move(payloads), std::chrono::milliseconds(1000));

    ASSERT_EQ(transport.sent_specs.size(), 1U);
    EXPECT_EQ(FindHeader(transport.sent_specs[0].headers, "accept-encoding"), "gzip");
}

TEST(HttpWireCodecTest, Response_GzipBody_IsInflatedBeforeParsing)
{
    // Without this the still-gzipped bytes went to ParseRejectedSpans, which
    // reports 0 for noise exactly as for an absent body — a rejected batch
    // read as a clean success.
    const auto plain = MakePartialSuccessBody(42U);

    mtfk::FakeTransport transport;
    transport.default_response = EncodedResponse("gzip", Gzipped(plain));
    mtw::HttpWireCodec codec{&transport, MakeConfig()};

    const auto result = codec.Send(MakePayload(), std::chrono::milliseconds(1000));
    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.partial_success_rejected, 42U);
}

TEST(HttpWireCodecTest, Response_GzipBody_ExcerptIsTheInflatedText)
{
    // The excerpt is what an operator reads out of GetExporterHealth(). Binary
    // gzip there is worse than useless.
    std::vector<std::byte> plain;
    for (const char c : std::string{"rejected 3 spans"})
    {
        plain.push_back(static_cast<std::byte>(c));
    }

    mtfk::FakeTransport transport;
    transport.default_response = EncodedResponse("gzip", Gzipped(plain));
    mtw::HttpWireCodec codec{&transport, MakeConfig()};

    const auto result = codec.Send(MakePayload(), std::chrono::milliseconds(1000));
    EXPECT_EQ(result.response_excerpt, "rejected 3 spans");
}

TEST(HttpWireCodecTest, Response_IdentityEncoding_IsPassedThrough)
{
    mtfk::FakeTransport transport;
    transport.default_response = EncodedResponse("identity", MakePartialSuccessBody(7U));
    mtw::HttpWireCodec codec{&transport, MakeConfig()};

    const auto result = codec.Send(MakePayload(), std::chrono::milliseconds(1000));
    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.partial_success_rejected, 7U);
}

TEST(HttpWireCodecTest, Response_UnknownContentEncoding_IsMalformed)
{
    // We only ever ask for gzip. A server answering `deflate` or `br` is
    // sending bytes this codec cannot read, and a retry gets the same ones.
    mtfk::FakeTransport transport;
    mtfk::FakeDiagnosticsSink sink;
    transport.default_response = EncodedResponse("br", MakePartialSuccessBody(1U));
    mtw::HttpWireCodec codec{&transport, MakeConfig(), nullptr, &sink};

    const auto result = codec.Send(MakePayload(), std::chrono::milliseconds(1000));
    EXPECT_FALSE(result.success);
    EXPECT_FALSE(result.retryable);
    EXPECT_EQ(DropCount(sink, mt::DropReason::MalformedResponse), 1U);
}

TEST(HttpWireCodecTest, Response_CorruptGzipBody_IsMalformed)
{
    mtfk::FakeTransport transport;
    mtfk::FakeDiagnosticsSink sink;
    transport.default_response = EncodedResponse("gzip", MakePartialSuccessBody(1U));
    mtw::HttpWireCodec codec{&transport, MakeConfig(), nullptr, &sink};

    const auto result = codec.Send(MakePayload(), std::chrono::milliseconds(1000));
    EXPECT_FALSE(result.success);
    EXPECT_FALSE(result.retryable);
    EXPECT_EQ(DropCount(sink, mt::DropReason::MalformedResponse), 1U);
    EXPECT_EQ(DropCount(sink, mt::DropReason::DecompressionTooLarge), 0U);
}

TEST(HttpWireCodecTest, Response_DecompressionBomb_RecordsDecompressionTooLarge)
{
    const std::vector<std::byte> bomb(1U << 20U, std::byte{0x00});

    mtfk::FakeTransport transport;
    mtfk::FakeDiagnosticsSink sink;
    transport.default_response = EncodedResponse("gzip", Gzipped(bomb));
    mtw::HttpWireCodecConfig cfg = MakeConfig();
    cfg.max_decompressed_bytes = 4096U;
    mtw::HttpWireCodec codec{&transport, cfg, nullptr, &sink};

    const auto result = codec.Send(MakePayload(), std::chrono::milliseconds(1000));
    EXPECT_FALSE(result.success);
    EXPECT_FALSE(result.retryable);
    EXPECT_EQ(DropCount(sink, mt::DropReason::DecompressionTooLarge), 1U);
    EXPECT_EQ(DropCount(sink, mt::DropReason::MalformedResponse), 0U);
    ASSERT_TRUE(result.error.has_value());
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access) — guarded by ASSERT_TRUE above
    EXPECT_FALSE(result.error->message.empty());
}

TEST(HttpWireCodecTest, Response_GzipEmptyBody_IsNotTreatedAsCorrupt)
{
    // Zero bytes is not a gzip stream, but a header on an empty body is a
    // server quirk, not a protocol violation — there is nothing to misread.
    mtfk::FakeTransport transport;
    mtfk::FakeDiagnosticsSink sink;
    transport.default_response = EncodedResponse("gzip", {});
    mtw::HttpWireCodec codec{&transport, MakeConfig(), nullptr, &sink};

    const auto result = codec.Send(MakePayload(), std::chrono::milliseconds(1000));
    EXPECT_TRUE(result.success);
    EXPECT_EQ(DropCount(sink, mt::DropReason::MalformedResponse), 0U);
}

TEST(HttpWireCodecTest, Response_MalformedEncoding_NullSink_IsNotDereferenced)
{
    mtfk::FakeTransport transport;
    transport.default_response = EncodedResponse("br", MakePartialSuccessBody(1U));
    mtw::HttpWireCodec codec{&transport, MakeConfig()};  // no sink

    const auto result = codec.Send(MakePayload(), std::chrono::milliseconds(1000));
    EXPECT_FALSE(result.success);
}

TEST(HttpWireCodecTest, SendAll_GzipBody_IsInflatedBeforeParsing)
{
    // SendAll classifies responses on its own path; the fix has to reach both.
    const auto plain = MakePartialSuccessBody(9U);

    mtfk::FakeTransport transport;
    transport.default_response = EncodedResponse("gzip", Gzipped(plain));
    mtw::HttpWireCodec codec{&transport, MakeConfig()};

    std::vector<mti::EncodedPayload> payloads;
    payloads.push_back(MakePayload());
    payloads.push_back(MakePayload());
    const auto results = codec.SendAll(std::move(payloads), std::chrono::milliseconds(1000));

    ASSERT_EQ(results.size(), 2U);
    EXPECT_EQ(results[0].partial_success_rejected, 9U);
    EXPECT_EQ(results[1].partial_success_rejected, 9U);
}

// ---------------------------------------------------------------------------
// Issue #223: an unparseable partial-success body is a malformed response
// (error-model.md §7.1, last row), not a clean success. Each signal has its own
// codec, so every case runs against the traces, metrics and logs paths.
// ---------------------------------------------------------------------------

namespace
{

constexpr std::array<const char*, 3> kSignalPaths{"", "/v1/metrics", "/v1/logs"};

struct PartialSuccessRun
{
    mti::WireResult result;
    std::uint64_t malformed = 0;
    std::uint64_t rejected_counted_by_codec = 0;
};

PartialSuccessRun SendWithBody(const char* signal_path, std::vector<std::byte> body)
{
    mtfk::FakeTransport transport;
    mtfk::FakeDiagnosticsSink sink;
    auto resp = OkResponse("200");
    resp.response_body = std::move(body);
    transport.default_response = resp;
    auto cfg = MakeConfig();
    cfg.signal_path = signal_path;
    mtw::HttpWireCodec codec{&transport, cfg, nullptr, &sink};
    auto result = codec.Send(MakePayload(), std::chrono::milliseconds(1000));
    return {
        .result = std::move(result),
        .malformed = DropCount(sink, mt::DropReason::MalformedResponse),
        .rejected_counted_by_codec = DropCount(sink, mt::DropReason::PartialSuccessRejection),
    };
}

std::vector<std::byte> Bytes(std::initializer_list<std::uint8_t> raw)
{
    std::vector<std::byte> out;
    out.reserve(raw.size());
    for (const auto b : raw)
    {
        out.push_back(std::byte{b});
    }
    return out;
}

}  // namespace

TEST(HttpWireCodecTest, PartialSuccess_AbsentFromNonEmptyBody_CleanSuccess)
{
    for (const char* path : kSignalPaths)
    {
        SCOPED_TRACE(path);
        const auto run = SendWithBody(path, Bytes({0x10, 0x05}));  // unknown field only
        EXPECT_TRUE(run.result.success);
        EXPECT_EQ(run.result.partial_success_rejected, 0U);
        EXPECT_EQ(run.malformed, 0U);
    }
}

TEST(HttpWireCodecTest, PartialSuccess_ValidZero_CleanSuccess)
{
    for (const char* path : kSignalPaths)
    {
        SCOPED_TRACE(path);
        const auto run = SendWithBody(path, MakePartialSuccessBody(0U));
        EXPECT_TRUE(run.result.success);
        EXPECT_EQ(run.result.partial_success_rejected, 0U);
        EXPECT_EQ(run.malformed, 0U);
    }
}

TEST(HttpWireCodecTest, PartialSuccess_ValidN_SuccessCarryingCount)
{
    for (const char* path : kSignalPaths)
    {
        SCOPED_TRACE(path);
        const auto run = SendWithBody(path, MakePartialSuccessBody(9U));
        EXPECT_TRUE(run.result.success);
        EXPECT_FALSE(run.result.retryable);
        EXPECT_EQ(run.result.partial_success_rejected, 9U);
        EXPECT_EQ(run.malformed, 0U);
        // The exporter's final-outcome funnel counts it, not the codec (§3).
        EXPECT_EQ(run.rejected_counted_by_codec, 0U);
    }
}

TEST(HttpWireCodecTest, PartialSuccess_TruncatedBody_IsMalformed)
{
    for (const char* path : kSignalPaths)
    {
        SCOPED_TRACE(path);
        // partial_success claims 5 bytes, 2 follow.
        const auto run = SendWithBody(path, Bytes({0x0A, 0x05, 0x08, 0x2A}));
        EXPECT_FALSE(run.result.success);
        EXPECT_FALSE(run.result.retryable);
        EXPECT_EQ(run.result.partial_success_rejected, 0U);
        EXPECT_EQ(run.malformed, 1U);
        ASSERT_TRUE(run.result.error.has_value());
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access) — guarded by ASSERT_TRUE above
        EXPECT_EQ(run.result.error->kind, mt::Error::Kind::Malformed);
    }
}

TEST(HttpWireCodecTest, PartialSuccess_GarbageBody_IsMalformedWithExcerpt)
{
    for (const char* path : kSignalPaths)
    {
        SCOPED_TRACE(path);
        const auto run = SendWithBody(path, Bytes({'<', 'h', 't', 'm', 'l', '>'}));
        EXPECT_FALSE(run.result.success);
        EXPECT_FALSE(run.result.retryable);
        EXPECT_EQ(run.malformed, 1U);
        // The operator needs to see what came back instead of a response.
        EXPECT_EQ(run.result.response_excerpt, "<html>");
    }
}
