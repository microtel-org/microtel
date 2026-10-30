// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0
//
// Unit tests for GrpcWireCodec — every row of the gRPC status-code matrix
// from error-model.md §7.2, plus header construction, gRPC framing,
// trailer-only detection, and RetryInfo decoding.
// Uses FakeTransport (synchronous, no real socket).

#include "wire/grpc/grpc_wire_codec.hpp"

#include "microtel/error.hpp"
#include "microtel/expected.hpp"
#include "microtel/internal/encoded_payload.hpp"
#include "microtel/internal/wire_result.hpp"
#include "microtel/log_sink.hpp"
#include "microtel/provider.hpp"
#include "microtel/version.hpp"

#include "fakes/fake_auth_provider.hpp"
#include "fakes/fake_diagnostics_sink.hpp"
#include "fakes/fake_transport.hpp"
#include "helpers/gunzip.hpp"
#include "wire/gzip.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
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

static mtw::GrpcWireCodecConfig MakeConfig()
{
    return mtw::GrpcWireCodecConfig{
        .host = "localhost:4317",
        .scheme = "https",
        .extra_headers = {},
        .service_path = {},
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

// Minimal base64 URL-safe encoder (no padding) — test-only fixture builder.
// NOLINTBEGIN(cppcoreguidelines-pro-bounds-constant-array-index)
static std::string Base64UrlEncode(const std::vector<std::uint8_t>& data)
{
    static constexpr char kAlphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::string result;
    std::size_t i = 0;
    while (i + 2 < data.size())
    {
        const auto d0 = static_cast<unsigned>(data[i]);
        const auto d1 = static_cast<unsigned>(data[i + 1U]);
        const auto d2 = static_cast<unsigned>(data[i + 2U]);
        result += kAlphabet[(d0 >> 2U) & 0x3FU];
        result += kAlphabet[((d0 & 0x3U) << 4U) | (d1 >> 4U)];
        result += kAlphabet[((d1 & 0xFU) << 2U) | (d2 >> 6U)];
        result += kAlphabet[d2 & 0x3FU];
        i += 3;
    }
    if (i < data.size())
    {
        const auto d0 = static_cast<unsigned>(data[i]);
        result += kAlphabet[(d0 >> 2U) & 0x3FU];
        if (i + 1 < data.size())
        {
            const auto d1 = static_cast<unsigned>(data[i + 1U]);
            result += kAlphabet[((d0 & 0x3U) << 4U) | (d1 >> 4U)];
            result += kAlphabet[(d1 & 0xFU) << 2U];
        }
        else
        {
            result += kAlphabet[(d0 & 0x3U) << 4U];
        }
    }
    return result;
}
// NOLINTEND(cppcoreguidelines-pro-bounds-constant-array-index)

static void EncodeVarint(std::uint64_t v, std::vector<std::uint8_t>& out)
{
    while (v >= 0x80U)
    {
        out.push_back(static_cast<std::uint8_t>((v & 0x7FU) | 0x80U));
        v >>= 7U;
    }
    out.push_back(static_cast<std::uint8_t>(v));
}

// Builds a grpc-status-details-bin value encoding:
//   google.rpc.Status { details: [ Any { type_url: <RetryInfo>, value: RetryInfo{delay=seconds} } ]
//   }
static std::string MakeRetryInfoDetailsBin(std::int64_t seconds)
{
    // Duration{seconds}
    std::vector<std::uint8_t> duration;
    duration.push_back(0x08U);  // field 1 (seconds), wire type 0
    EncodeVarint(static_cast<std::uint64_t>(seconds), duration);

    // RetryInfo{retry_delay: duration}
    std::vector<std::uint8_t> retry_info;
    retry_info.push_back(0x0AU);  // field 1 (retry_delay), wire type 2
    retry_info.push_back(static_cast<std::uint8_t>(duration.size()));
    retry_info.insert(retry_info.end(), duration.begin(), duration.end());

    // Any{type_url, value: retry_info}
    static constexpr std::string_view kTypeUrl = "type.googleapis.com/google.rpc.RetryInfo";
    std::vector<std::uint8_t> any_msg;
    any_msg.push_back(0x0AU);  // field 1 (type_url), wire type 2
    any_msg.push_back(static_cast<std::uint8_t>(kTypeUrl.size()));
    for (char c : kTypeUrl)
    {
        any_msg.push_back(static_cast<std::uint8_t>(c));
    }
    any_msg.push_back(0x12U);  // field 2 (value), wire type 2
    any_msg.push_back(static_cast<std::uint8_t>(retry_info.size()));
    any_msg.insert(any_msg.end(), retry_info.begin(), retry_info.end());

    // Status{details: [any]}
    std::vector<std::uint8_t> status;
    status.push_back(0x1AU);  // field 3 (details), wire type 2
    status.push_back(static_cast<std::uint8_t>(any_msg.size()));
    status.insert(status.end(), any_msg.begin(), any_msg.end());

    return Base64UrlEncode(status);
}

static mti::TransportResult GrpcSuccessResponse()
{
    return mti::TransportResult{
        .success = true,
        .response_headers = {{.name = ":status", .value = "200"}},
        .response_trailers = {{.name = "grpc-status", .value = "0"}},
        .response_body = {},
        .error = {},
    };
}

static mti::TransportResult GrpcStatusResponse(const std::string& code)
{
    return mti::TransportResult{
        .success = true,
        .response_headers = {{.name = ":status", .value = "200"}},
        .response_trailers = {{.name = "grpc-status", .value = code}},
        .response_body = {},
        .error = {},
    };
}

static mti::TransportResult TrailerOnlyResponse(const std::string& code)
{
    // grpc-status in the initial HEADERS frame (END_STREAM on first HEADERS)
    return mti::TransportResult{
        .success = true,
        .response_headers = {{.name = ":status", .value = "200"},
                             {.name = "grpc-status", .value = code}},
        .response_trailers = {},
        .response_body = {},
        .error = {},
    };
}

// ---------------------------------------------------------------------------
// gRPC success — grpc-status: 0
// ---------------------------------------------------------------------------

TEST(GrpcWireCodecTest, Send_GrpcStatus0_InTrailers_Succeeds)
{
    mtfk::FakeTransport transport;
    transport.default_response = GrpcSuccessResponse();
    mtw::GrpcWireCodec codec{&transport, MakeConfig()};

    const auto result = codec.Send(MakePayload(), std::chrono::milliseconds(500));
    EXPECT_TRUE(result.success);
    EXPECT_FALSE(result.retryable);
}

TEST(GrpcWireCodecTest, Send_TrailerOnly_GrpcStatus0_Succeeds)
{
    mtfk::FakeTransport transport;
    transport.default_response = TrailerOnlyResponse("0");
    mtw::GrpcWireCodec codec{&transport, MakeConfig()};

    const auto result = codec.Send(MakePayload(), std::chrono::milliseconds(500));
    EXPECT_TRUE(result.success);
    EXPECT_FALSE(result.retryable);
}

// ---------------------------------------------------------------------------
// Retryable gRPC status codes
// ---------------------------------------------------------------------------

TEST(GrpcWireCodecTest, Send_Cancelled_Retryable)
{
    mtfk::FakeTransport transport;
    transport.default_response = GrpcStatusResponse("1");
    mtw::GrpcWireCodec codec{&transport, MakeConfig()};

    const auto result = codec.Send(MakePayload(), std::chrono::milliseconds(500));
    EXPECT_FALSE(result.success);
    EXPECT_TRUE(result.retryable);
    ASSERT_TRUE(result.error.has_value());
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access) — guarded by ASSERT_TRUE above
    EXPECT_EQ(result.error->message, "CANCELLED (1)");
}

TEST(GrpcWireCodecTest, Send_DeadlineExceeded_Retryable)
{
    mtfk::FakeTransport transport;
    transport.default_response = GrpcStatusResponse("4");
    mtw::GrpcWireCodec codec{&transport, MakeConfig()};

    const auto result = codec.Send(MakePayload(), std::chrono::milliseconds(500));
    EXPECT_FALSE(result.success);
    EXPECT_TRUE(result.retryable);
    ASSERT_TRUE(result.error.has_value());
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access) — guarded by ASSERT_TRUE above
    EXPECT_EQ(result.error->message, "DEADLINE_EXCEEDED (4)");
}

TEST(GrpcWireCodecTest, Send_Unavailable_Retryable)
{
    mtfk::FakeTransport transport;
    transport.default_response = GrpcStatusResponse("14");
    mtw::GrpcWireCodec codec{&transport, MakeConfig()};

    const auto result = codec.Send(MakePayload(), std::chrono::milliseconds(500));
    EXPECT_FALSE(result.success);
    EXPECT_TRUE(result.retryable);
    ASSERT_TRUE(result.error.has_value());
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access) — guarded by ASSERT_TRUE above
    EXPECT_EQ(result.error->message, "UNAVAILABLE (14)");
}

TEST(GrpcWireCodecTest, Send_DataLoss_Retryable)
{
    mtfk::FakeTransport transport;
    transport.default_response = GrpcStatusResponse("15");
    mtw::GrpcWireCodec codec{&transport, MakeConfig()};

    const auto result = codec.Send(MakePayload(), std::chrono::milliseconds(500));
    EXPECT_FALSE(result.success);
    EXPECT_TRUE(result.retryable);
    ASSERT_TRUE(result.error.has_value());
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access) — guarded by ASSERT_TRUE above
    EXPECT_EQ(result.error->message, "DATA_LOSS (15)");
}

TEST(GrpcWireCodecTest, Send_TrailerOnly_Unavailable_Retryable)
{
    mtfk::FakeTransport transport;
    transport.default_response = TrailerOnlyResponse("14");
    mtw::GrpcWireCodec codec{&transport, MakeConfig()};

    const auto result = codec.Send(MakePayload(), std::chrono::milliseconds(500));
    EXPECT_FALSE(result.success);
    EXPECT_TRUE(result.retryable);
    ASSERT_TRUE(result.error.has_value());
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access) — guarded by ASSERT_TRUE above
    EXPECT_EQ(result.error->message, "UNAVAILABLE (14)");
}

// ---------------------------------------------------------------------------
// Non-retryable gRPC status codes
// ---------------------------------------------------------------------------

TEST(GrpcWireCodecTest, Send_InvalidArgument_NotRetryable)
{
    mtfk::FakeTransport transport;
    transport.default_response = GrpcStatusResponse("3");
    mtw::GrpcWireCodec codec{&transport, MakeConfig()};

    const auto result = codec.Send(MakePayload(), std::chrono::milliseconds(500));
    EXPECT_FALSE(result.success);
    EXPECT_FALSE(result.retryable);
    ASSERT_TRUE(result.error.has_value());
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access) — guarded by ASSERT_TRUE above
    EXPECT_EQ(result.error->message, "INVALID_ARGUMENT (3)");
}

TEST(GrpcWireCodecTest, Send_Internal_NotRetryable)
{
    mtfk::FakeTransport transport;
    transport.default_response = GrpcStatusResponse("13");
    mtw::GrpcWireCodec codec{&transport, MakeConfig()};

    const auto result = codec.Send(MakePayload(), std::chrono::milliseconds(500));
    EXPECT_FALSE(result.success);
    EXPECT_FALSE(result.retryable);
    ASSERT_TRUE(result.error.has_value());
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access) — guarded by ASSERT_TRUE above
    EXPECT_EQ(result.error->message, "INTERNAL (13)");
}

TEST(GrpcWireCodecTest, Send_Unimplemented_NotRetryable)
{
    mtfk::FakeTransport transport;
    transport.default_response = GrpcStatusResponse("12");
    mtw::GrpcWireCodec codec{&transport, MakeConfig()};

    const auto result = codec.Send(MakePayload(), std::chrono::milliseconds(500));
    EXPECT_FALSE(result.success);
    EXPECT_FALSE(result.retryable);
    ASSERT_TRUE(result.error.has_value());
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access) — guarded by ASSERT_TRUE above
    EXPECT_EQ(result.error->message, "UNIMPLEMENTED (12)");
}

// ---------------------------------------------------------------------------
// RESOURCE_EXHAUSTED (8) — the RetryInfo gate
// ---------------------------------------------------------------------------

TEST(GrpcWireCodecTest, Send_ResourceExhausted_WithRetryInfo_RetryableWithDelay)
{
    constexpr std::int64_t kDelaySec = 5;
    mtfk::FakeTransport transport;
    transport.default_response = mti::TransportResult{
        .success = true,
        .response_headers = {{.name = ":status", .value = "200"}},
        .response_trailers = {{.name = "grpc-status", .value = "8"},
                              {.name = "grpc-status-details-bin",
                               .value = MakeRetryInfoDetailsBin(kDelaySec)}},
        .response_body = {},
        .error = {},
    };
    mtw::GrpcWireCodec codec{&transport, MakeConfig()};

    const auto result = codec.Send(MakePayload(), std::chrono::milliseconds(500));
    EXPECT_FALSE(result.success);
    EXPECT_TRUE(result.retryable);
    ASSERT_TRUE(result.retry_after.has_value());
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access) — guarded by ASSERT_TRUE above
    EXPECT_EQ(*result.retry_after, std::chrono::milliseconds(kDelaySec * 1000));
}

TEST(GrpcWireCodecTest, Send_ResourceExhausted_WithoutRetryInfo_NotRetryable)
{
    mtfk::FakeTransport transport;
    transport.default_response = GrpcStatusResponse("8");
    mtw::GrpcWireCodec codec{&transport, MakeConfig()};

    const auto result = codec.Send(MakePayload(), std::chrono::milliseconds(500));
    EXPECT_FALSE(result.success);
    EXPECT_FALSE(result.retryable);
    EXPECT_FALSE(result.retry_after.has_value());
}

TEST(GrpcWireCodecTest, Send_ResourceExhausted_RetryInfo_InInitialHeaders_Retryable)
{
    // Trailer-only: grpc-status-details-bin in the initial HEADERS frame
    constexpr std::int64_t kDelaySec = 3;
    mtfk::FakeTransport transport;
    transport.default_response = mti::TransportResult{
        .success = true,
        .response_headers = {{.name = ":status", .value = "200"},
                             {.name = "grpc-status", .value = "8"},
                             {.name = "grpc-status-details-bin",
                              .value = MakeRetryInfoDetailsBin(kDelaySec)}},
        .response_trailers = {},
        .response_body = {},
        .error = {},
    };
    mtw::GrpcWireCodec codec{&transport, MakeConfig()};

    const auto result = codec.Send(MakePayload(), std::chrono::milliseconds(500));
    EXPECT_FALSE(result.success);
    EXPECT_TRUE(result.retryable);
    ASSERT_TRUE(result.retry_after.has_value());
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access) — guarded by ASSERT_TRUE above
    EXPECT_EQ(*result.retry_after, std::chrono::milliseconds(kDelaySec * 1000));
}

// ---------------------------------------------------------------------------
// Missing grpc-status — HTTP :status fallback (§4.2)
// ---------------------------------------------------------------------------

TEST(GrpcWireCodecTest, Send_MissingGrpcStatus_Http429_Retryable)
{
    mtfk::FakeTransport transport;
    transport.default_response = mti::TransportResult{
        .success = true,
        .response_headers = {{.name = ":status", .value = "429"}},
        .response_trailers = {},
        .response_body = {},
        .error = {},
    };
    mtw::GrpcWireCodec codec{&transport, MakeConfig()};

    const auto result = codec.Send(MakePayload(), std::chrono::milliseconds(500));
    EXPECT_FALSE(result.success);
    EXPECT_TRUE(result.retryable);
}

TEST(GrpcWireCodecTest, Send_MissingGrpcStatus_Http503_Retryable)
{
    mtfk::FakeTransport transport;
    transport.default_response = mti::TransportResult{
        .success = true,
        .response_headers = {{.name = ":status", .value = "503"}},
        .response_trailers = {},
        .response_body = {},
        .error = {},
    };
    mtw::GrpcWireCodec codec{&transport, MakeConfig()};

    const auto result = codec.Send(MakePayload(), std::chrono::milliseconds(500));
    EXPECT_FALSE(result.success);
    EXPECT_TRUE(result.retryable);
}

TEST(GrpcWireCodecTest, Send_MissingGrpcStatus_Http200_NotRetryable)
{
    mtfk::FakeTransport transport;
    transport.default_response = mti::TransportResult{
        .success = true,
        .response_headers = {{.name = ":status", .value = "200"}},
        .response_trailers = {},
        .response_body = {},
        .error = {},
    };
    mtw::GrpcWireCodec codec{&transport, MakeConfig()};

    const auto result = codec.Send(MakePayload(), std::chrono::milliseconds(500));
    EXPECT_FALSE(result.success);
    EXPECT_FALSE(result.retryable);
}

// A 5xx HTTP status under a `grpc-status: 0` trailer is an unusual but legal
// combination — an intermediary stamping its own status onto a stream the
// server completed successfully. §4.1 is LOCKED on which one wins:
// `grpc-status` classifies, the HTTP status is diagnostic only. Dropping the
// batch here would discard spans the collector already accepted.
//
// (§4.3 also calls for a diagnostic on this row. The codec emits none today;
// classification is the normative half and is what this pins.)
TEST(GrpcWireCodecTest, Send_Http503WithGrpcStatus0_SucceedsPerGrpcStatus)
{
    mtfk::FakeTransport transport;
    transport.default_response = mti::TransportResult{
        .success = true,
        .response_headers = {{.name = ":status", .value = "503"}},
        .response_trailers = {{.name = "grpc-status", .value = "0"}},
        .response_body = {},
        .error = {},
    };
    mtw::GrpcWireCodec codec{&transport, MakeConfig()};

    const auto result = codec.Send(MakePayload(), std::chrono::milliseconds(500));
    EXPECT_TRUE(result.success);
    EXPECT_FALSE(result.retryable);
}

// ---------------------------------------------------------------------------
// Transport-level error
// ---------------------------------------------------------------------------

// A transport-level failure is exactly what the retry engine exists for:
// collector restarts, rolling deploys, load balancers draining. This used to
// return retryable=false, so a gRPC deployment dropped the batch on the first
// connection reset while an HTTP deployment retried the identical failure
// (`http_wire_codec.cpp`, "transport-level failure: connection reset, etc.").
TEST(GrpcWireCodecTest, Send_TransportError_IsRetryable)
{
    mtfk::FakeTransport transport;
    transport.default_response = mti::TransportResult{
        .success = false,
        .response_headers = {},
        .response_trailers = {},
        .response_body = {},
        .error = mt::Error{.kind = mt::Error::Kind::Network, .message = "connection reset"},
    };
    mtw::GrpcWireCodec codec{&transport, MakeConfig()};

    const auto result = codec.Send(MakePayload(), std::chrono::milliseconds(500));
    EXPECT_FALSE(result.success);
    EXPECT_TRUE(result.retryable);
    ASSERT_TRUE(result.error.has_value());
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access) — guarded by ASSERT_TRUE above
    EXPECT_EQ(result.error->message, "connection reset");
}

// The codec used to disagree with itself: a connection that failed while being
// established was retryable (ICP 0017's EnsureConnected), but one that failed
// after being established was not. Same Error::Kind, same recovery, opposite
// verdicts.
TEST(GrpcWireCodecTest, Send_ConnectFailureAndMidStreamFailure_AgreeOnRetryability)
{
    mtfk::FakeTransport connect_fails;
    connect_fails.state = mt::ConnectionState::Disconnected;
    connect_fails.connect_result =
        mt::make_unexpected(mt::Error{.kind = mt::Error::Kind::Network, .message = "refused"});
    mtw::GrpcWireCodec connect_codec{&connect_fails, MakeConfig()};
    const auto connect_result = connect_codec.Send(MakePayload(), std::chrono::milliseconds(500));

    mtfk::FakeTransport send_fails;
    send_fails.default_response = mti::TransportResult{
        .success = false,
        .response_headers = {},
        .response_trailers = {},
        .response_body = {},
        .error = mt::Error{.kind = mt::Error::Kind::Network, .message = "connection reset"},
    };
    mtw::GrpcWireCodec send_codec{&send_fails, MakeConfig()};
    const auto send_result = send_codec.Send(MakePayload(), std::chrono::milliseconds(500));

    EXPECT_EQ(connect_result.retryable, send_result.retryable);
    EXPECT_TRUE(send_result.retryable);
}


// ---------------------------------------------------------------------------
// Drop accounting — issue #169. The codec owns classification (ICP 0001) but
// wrote none of the counters the classification names in error-model.md §7.2.
// ---------------------------------------------------------------------------

static std::uint64_t DropCount(const mtfk::FakeDiagnosticsSink& sink, mt::DropReason reason)
{
    return sink.drop_counters.at(static_cast<std::size_t>(reason));
}

// The one transport failure that is *not* retryable: a response the transport
// refused to buffer past `max_response_bytes`. The peer answers the retry with
// the same oversized response, so retrying only spends the budget
// (`error-model.md` §7.1's `response_too_large` row, which §7.2 now mirrors).
TEST(GrpcWireCodecTest, Send_ResponseTooLarge_IsNonRetryableAndCounted)
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
    mtw::GrpcWireCodec codec{&transport, MakeConfig(), nullptr, &sink};

    const auto result = codec.Send(MakePayload(), std::chrono::milliseconds(500));
    EXPECT_FALSE(result.success);
    EXPECT_FALSE(result.retryable);
    EXPECT_EQ(DropCount(sink, mt::DropReason::ResponseTooLarge), 1U);
}

// Trailers have no counter of their own — error-model.md §3 maps
// `max_trailer_bytes` onto `response_too_large`, with the message naming the
// cap. Relevant to gRPC in particular: `grpc-status` rides in the trailers.
TEST(GrpcWireCodecTest, Send_TrailersTooLarge_CountsResponseTooLarge)
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
    mtw::GrpcWireCodec codec{&transport, MakeConfig(), nullptr, &sink};

    const auto result = codec.Send(MakePayload(), std::chrono::milliseconds(500));
    EXPECT_FALSE(result.retryable);
    EXPECT_EQ(DropCount(sink, mt::DropReason::ResponseTooLarge), 1U);
}

// The `transport_busy` row of error-model.md §3: the transport refused to
// queue the request because its request queue was at `max_pending_requests`.
// Counted, but *retryable* — a full queue drains, unlike an oversized response
// the peer will send again.
TEST(GrpcWireCodecTest, Send_TransportBusy_IsRetryableAndCounted)
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
    mtw::GrpcWireCodec codec{&transport, MakeConfig(), nullptr, &sink};

    const auto result = codec.Send(MakePayload(), std::chrono::milliseconds(500));
    EXPECT_FALSE(result.success);
    EXPECT_TRUE(result.retryable);
    EXPECT_EQ(DropCount(sink, mt::DropReason::TransportBusy), 1U);
}

TEST(GrpcWireCodecTest, Diagnostics_ConnectFails_CountsConnectFailure)
{
    mtfk::FakeTransport transport;
    mtfk::FakeDiagnosticsSink sink;
    transport.state = mt::ConnectionState::Disconnected;
    transport.connect_result =
        mt::make_unexpected(mt::Error{.kind = mt::Error::Kind::Network, .message = "refused"});
    mtw::GrpcWireCodec codec{&transport, MakeConfig(), nullptr, &sink};

    const auto result = codec.Send(MakePayload(), std::chrono::milliseconds(500));

    EXPECT_FALSE(result.success);
    EXPECT_EQ(DropCount(sink, mt::DropReason::ConnectFailure), 1U);
}

TEST(GrpcWireCodecTest, Diagnostics_ConnectSucceeds_CountsNothing)
{
    mtfk::FakeTransport transport;
    mtfk::FakeDiagnosticsSink sink;
    transport.state = mt::ConnectionState::Disconnected;
    transport.default_response = GrpcSuccessResponse();
    mtw::GrpcWireCodec codec{&transport, MakeConfig(), nullptr, &sink};

    (void)codec.Send(MakePayload(), std::chrono::milliseconds(500));

    EXPECT_EQ(DropCount(sink, mt::DropReason::ConnectFailure), 0U);
}

TEST(GrpcWireCodecTest, Diagnostics_MissingGrpcStatusNonRetryable_CountsMalformedResponse)
{
    mtfk::FakeTransport transport;
    mtfk::FakeDiagnosticsSink sink;
    transport.default_response = mti::TransportResult{
        .success = true,
        .response_headers = {{.name = ":status", .value = "200"}},
        .response_trailers = {},
        .response_body = {},
        .error = {},
    };
    mtw::GrpcWireCodec codec{&transport, MakeConfig(), nullptr, &sink};

    const auto result = codec.Send(MakePayload(), std::chrono::milliseconds(500));

    EXPECT_FALSE(result.retryable);
    EXPECT_EQ(DropCount(sink, mt::DropReason::MalformedResponse), 1U);
}

TEST(GrpcWireCodecTest, Diagnostics_MissingGrpcStatusRetryable_CountsNothing)
{
    mtfk::FakeTransport transport;
    mtfk::FakeDiagnosticsSink sink;
    transport.default_response = mti::TransportResult{
        .success = true,
        .response_headers = {{.name = ":status", .value = "503"}},
        .response_trailers = {},
        .response_body = {},
        .error = {},
    };
    mtw::GrpcWireCodec codec{&transport, MakeConfig(), nullptr, &sink};

    const auto result = codec.Send(MakePayload(), std::chrono::milliseconds(500));

    // A 503 without grpc-status is a load balancer talking, not a malformed
    // gRPC response: §7.2 makes it retryable and names no counter.
    EXPECT_TRUE(result.retryable);
    EXPECT_EQ(DropCount(sink, mt::DropReason::MalformedResponse), 0U);
}

TEST(GrpcWireCodecTest, Diagnostics_NullSink_IsNotDereferenced)
{
    mtfk::FakeTransport transport;
    transport.state = mt::ConnectionState::Disconnected;
    transport.connect_result =
        mt::make_unexpected(mt::Error{.kind = mt::Error::Kind::Network, .message = "refused"});
    mtw::GrpcWireCodec codec{&transport, MakeConfig()};  // no sink

    const auto result = codec.Send(MakePayload(), std::chrono::milliseconds(500));
    EXPECT_FALSE(result.success);
}

// ---------------------------------------------------------------------------
// Header construction
// ---------------------------------------------------------------------------

TEST(GrpcWireCodecTest, Send_BuildsRequiredGrpcHeaders)
{
    mtfk::FakeTransport transport;
    transport.default_response = GrpcSuccessResponse();
    mtw::GrpcWireCodec codec{&transport, MakeConfig()};

    (void)codec.Send(MakePayload(), std::chrono::milliseconds(500));

    ASSERT_EQ(transport.sent_specs.size(), 1U);
    const auto& headers = transport.sent_specs.front().headers;
    EXPECT_EQ(FindHeader(headers, ":method"), "POST");
    EXPECT_EQ(FindHeader(headers, ":scheme"), "https");
    EXPECT_EQ(FindHeader(headers, ":authority"), "localhost:4317");
    EXPECT_EQ(FindHeader(headers, ":path"),
              "/opentelemetry.proto.collector.trace.v1.TraceService/Export");
    EXPECT_EQ(FindHeader(headers, "te"), "trailers");
    EXPECT_EQ(FindHeader(headers, "content-type"), "application/grpc+proto");
    EXPECT_FALSE(FindHeader(headers, "user-agent").empty());
}

// The user-agent is the release version as collectors see it (grpc-wire-protocol.md §2.1), and a
// release bump edits the literal by hand. `kUserAgent` carries a static_assert
// against `kVersionString`, but that only proves the *constant* is coherent —
// nothing proved the header actually emitted carries it, so a codec that built
// the header from anything else would have compiled and shipped silently.
TEST(GrpcWireCodecTest, Send_UserAgentCarriesVersionString)
{
    mtfk::FakeTransport transport;
    transport.default_response = GrpcSuccessResponse();
    mtw::GrpcWireCodec codec{&transport, MakeConfig()};

    (void)codec.Send(MakePayload(), std::chrono::milliseconds(500));

    ASSERT_EQ(transport.sent_specs.size(), 1U);
    const auto& headers = transport.sent_specs.front().headers;
    const std::string expected = "microtel-cpp/" + std::string{microtel::kVersionString};
    EXPECT_EQ(FindHeader(headers, "user-agent"), expected);
}

// ---------------------------------------------------------------------------
// Auth provider (issue #250, interfaces.md §4.9)
// ---------------------------------------------------------------------------

TEST(GrpcWireCodecTest, Send_WithAuth_AddsAuthorizationHeader)
{
    mtfk::FakeTransport transport;
    transport.default_response = GrpcSuccessResponse();
    mtfk::FakeAuthProvider auth;
    auth.static_value = std::optional<std::string>{"Bearer tok"};
    mtw::GrpcWireCodec codec{&transport, MakeConfig(), &auth};

    (void)codec.Send(MakePayload(), std::chrono::milliseconds(500));
    ASSERT_EQ(transport.sent_specs.size(), 1U);
    EXPECT_EQ(FindHeader(transport.sent_specs.front().headers, "authorization"), "Bearer tok");
}

// The batch is dropped, not shipped bare: sending without auth is worse than
// not sending (interfaces.md §4.9).
TEST(GrpcWireCodecTest, Send_AuthProviderError_DropsBatchWithoutSending)
{
    mtfk::FakeTransport transport;
    transport.default_response = GrpcSuccessResponse();
    mtfk::FakeAuthProvider auth;
    auth.static_value = mt::make_unexpected(
        mt::Error{.kind = mt::Error::Kind::Network, .message = "token fetch failed"});
    mtw::GrpcWireCodec codec{&transport, MakeConfig(), &auth};

    const auto result = codec.Send(MakePayload(), std::chrono::milliseconds(500));
    EXPECT_FALSE(result.success);
    EXPECT_FALSE(result.retryable) << "non_retryable_failure per interfaces.md §4.9";
    ASSERT_TRUE(result.error.has_value());
    // NOLINTBEGIN(bugprone-unchecked-optional-access) — guarded by ASSERT_TRUE above
    EXPECT_EQ(result.error->kind, mt::Error::Kind::Network);
    EXPECT_NE(result.error->message.find("authorization"), std::string::npos)
        << "last_error_message must name auth: " << result.error->message;
    EXPECT_NE(result.error->message.find("token fetch failed"), std::string::npos);
    // NOLINTEND(bugprone-unchecked-optional-access)
    EXPECT_TRUE(transport.sent_specs.empty())
        << "an unauthenticated request must never reach the wire";
}

// ---------------------------------------------------------------------------
// Request header validation (ICP 0042 §3): CR, LF and NUL are rejected in
// every field after assembly, the auth header included.
// ---------------------------------------------------------------------------

TEST(GrpcWireCodecTest, Send_AuthTokenWithCrLfOrNul_DropsBatchWithoutSending)
{
    for (const char bad : {'\r', '\n', '\0'})
    {
        mtfk::FakeTransport transport;
        transport.default_response = GrpcSuccessResponse();
        mtfk::FakeAuthProvider auth;
        std::string token = "Bearer secret";
        token.push_back(bad);
        token += "x-injected: 1";
        auth.static_value = std::optional<std::string>{token};
        mtw::GrpcWireCodec codec{&transport, MakeConfig(), &auth};

        const auto result = codec.Send(MakePayload(), std::chrono::milliseconds(500));
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

TEST(GrpcWireCodecTest, Send_ExtraHeaderValueWithLf_DropsBatchWithoutSending)
{
    mtfk::FakeTransport transport;
    transport.default_response = GrpcSuccessResponse();
    auto config = MakeConfig();
    config.extra_headers.push_back({.name = "x-tenant", .value = "a\nb"});
    mtw::GrpcWireCodec codec{&transport, config};

    const auto result = codec.Send(MakePayload(), std::chrono::milliseconds(500));
    EXPECT_FALSE(result.success);
    EXPECT_FALSE(result.retryable);
    const std::string message = result.error ? result.error->message : std::string{};
    EXPECT_NE(message.find("\"x-tenant\""), std::string::npos) << message;
    EXPECT_EQ(message.find("authorization header unavailable"), std::string::npos)
        << "a bad static header is not an auth failure: " << message;
    EXPECT_TRUE(transport.sent_specs.empty());
}

// ---------------------------------------------------------------------------
// gRPC framing — 5-byte length prefix
// ---------------------------------------------------------------------------

TEST(GrpcWireCodecTest, Send_GrpcFrame_HasCorrectPrefix)
{
    constexpr std::size_t kPayloadSize = 7;
    mtfk::FakeTransport transport;
    transport.default_response = GrpcSuccessResponse();
    mtw::GrpcWireCodec codec{&transport, MakeConfig()};

    (void)codec.Send(MakePayload(kPayloadSize), std::chrono::milliseconds(500));

    ASSERT_EQ(transport.sent_payloads.size(), 1U);
    const auto& payload = transport.sent_payloads.front();
    ASSERT_GE(payload.size(), 5U);

    // Byte 0: compression flag (0 = uncompressed)
    EXPECT_EQ(std::to_integer<int>(payload[0]), 0);
    // Bytes 1-4: big-endian uint32 message length
    const std::uint32_t length = (std::to_integer<std::uint32_t>(payload[1]) << 24U) |
                                 (std::to_integer<std::uint32_t>(payload[2]) << 16U) |
                                 (std::to_integer<std::uint32_t>(payload[3]) << 8U) |
                                 std::to_integer<std::uint32_t>(payload[4]);
    EXPECT_EQ(length, kPayloadSize);
    EXPECT_EQ(payload.size(), 5U + kPayloadSize);
}

// ---------------------------------------------------------------------------
// M5-B: Partial-success parsing
// ---------------------------------------------------------------------------

// Build a gRPC DATA frame (5-byte prefix + proto) from raw proto bytes.
// Compression flag = 0x00, length as big-endian uint32.
static std::vector<std::byte> GrpcFrame(std::span<const std::uint8_t> proto_bytes)
{
    const auto n = static_cast<std::uint32_t>(proto_bytes.size());
    std::vector<std::byte> frame;
    frame.reserve(5U + proto_bytes.size());
    frame.push_back(std::byte{0x00U});
    frame.push_back(std::byte{static_cast<std::uint8_t>((n >> 24U) & 0xFFU)});
    frame.push_back(std::byte{static_cast<std::uint8_t>((n >> 16U) & 0xFFU)});
    frame.push_back(std::byte{static_cast<std::uint8_t>((n >> 8U) & 0xFFU)});
    frame.push_back(std::byte{static_cast<std::uint8_t>(n & 0xFFU)});
    for (const auto b : proto_bytes)
    {
        frame.push_back(static_cast<std::byte>(b));
    }
    return frame;
}

// ExportTraceServiceResponse { partial_success { rejected_spans: 42 } }
// [0x0A, 0x02, 0x08, 0x2A]
static constexpr std::array<std::uint8_t, 4> kRejected42Proto{0x0A, 0x02, 0x08, 0x2A};

TEST(GrpcWireCodecTest, PartialSuccess_PopulatesRejectedSpans)
{
    mtfk::FakeTransport transport;
    auto resp = GrpcSuccessResponse();
    resp.response_body = GrpcFrame(kRejected42Proto);
    transport.default_response = resp;
    mtw::GrpcWireCodec codec{&transport, MakeConfig()};

    const auto result = codec.Send(MakePayload(), std::chrono::milliseconds(500));
    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.partial_success_rejected, 42U);
}

TEST(GrpcWireCodecTest, PartialSuccess_EmptyBody_ZeroRejected)
{
    mtfk::FakeTransport transport;
    transport.default_response = GrpcSuccessResponse();
    mtw::GrpcWireCodec codec{&transport, MakeConfig()};

    const auto result = codec.Send(MakePayload(), std::chrono::milliseconds(500));
    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.partial_success_rejected, 0U);
}

TEST(GrpcWireCodecTest, PartialSuccess_NonZeroGrpcStatus_BodyIgnored)
{
    // grpc-status=8 (RESOURCE_EXHAUSTED without RetryInfo) → failure, no partial success.
    mtfk::FakeTransport transport;
    const mti::TransportResult resp{
        .success = true,
        .response_headers = {{.name = ":status", .value = "200"}},
        .response_trailers = {{.name = "grpc-status", .value = "8"}},
        .response_body = GrpcFrame(kRejected42Proto),
        .error = {},
    };
    transport.default_response = resp;
    mtw::GrpcWireCodec codec{&transport, MakeConfig()};

    const auto result = codec.Send(MakePayload(), std::chrono::milliseconds(500));
    EXPECT_FALSE(result.success);
    EXPECT_EQ(result.partial_success_rejected, 0U);
}

// ---------------------------------------------------------------------------
// service_path override (M12 metrics codec wiring)
// ---------------------------------------------------------------------------

TEST(GrpcWireCodecTest, Send_ServicePath_OverridesDefaultTracesPath)
{
    mtfk::FakeTransport transport;
    const mti::TransportResult resp{
        .success = true,
        .response_headers = {{.name = ":status", .value = "200"}},
        .response_trailers = {{.name = "grpc-status", .value = "0"}},
        .response_body = {},
        .error = {},
    };
    transport.default_response = resp;

    mtw::GrpcWireCodecConfig cfg = MakeConfig();
    cfg.service_path = "/opentelemetry.proto.collector.metrics.v1.MetricsService/Export";
    mtw::GrpcWireCodec codec{&transport, cfg};

    (void)codec.Send(MakePayload(), std::chrono::milliseconds(500));
    ASSERT_EQ(transport.sent_specs.size(), 1U);

    EXPECT_EQ(FindHeader(transport.sent_specs[0].headers, ":path"),
              "/opentelemetry.proto.collector.metrics.v1.MetricsService/Export");
}

// ---------------------------------------------------------------------------
// Lazy connect (ICP 0017)
// ---------------------------------------------------------------------------

TEST(GrpcWireCodecTest, Send_WhenDisconnected_ConnectsThenSucceeds)
{
    mtfk::FakeTransport transport;
    transport.state = mt::ConnectionState::Disconnected;
    transport.default_response = mti::TransportResult{
        .success = true,
        .response_headers = {{.name = ":status", .value = "200"}},
        .response_trailers = {{.name = "grpc-status", .value = "0"}},
        .response_body = {},
        .error = {},
    };
    mtw::GrpcWireCodec codec{&transport, MakeConfig()};

    const auto result = codec.Send(MakePayload(), std::chrono::milliseconds(500));

    EXPECT_TRUE(result.success);
    EXPECT_EQ(transport.connect_calls.size(), 1U);
    EXPECT_EQ(transport.sent_specs.size(), 1U);
}

TEST(GrpcWireCodecTest, Send_WhenAlreadyConnected_DoesNotCallConnect)
{
    mtfk::FakeTransport transport;  // default state: Connected
    transport.default_response = mti::TransportResult{
        .success = true,
        .response_headers = {{.name = ":status", .value = "200"}},
        .response_trailers = {{.name = "grpc-status", .value = "0"}},
        .response_body = {},
        .error = {},
    };
    mtw::GrpcWireCodec codec{&transport, MakeConfig()};

    const auto result = codec.Send(MakePayload(), std::chrono::milliseconds(500));

    EXPECT_TRUE(result.success);
    EXPECT_EQ(transport.connect_calls.size(), 0U);
}

TEST(GrpcWireCodecTest, Send_WhenDisconnectedAndConnectFails_ReturnsRetryableWithoutSending)
{
    mtfk::FakeTransport transport;
    transport.state = mt::ConnectionState::Disconnected;
    transport.connect_result =
        mt::make_unexpected(mt::Error{.kind = mt::Error::Kind::Network, .message = "refused"});
    mtw::GrpcWireCodec codec{&transport, MakeConfig()};

    const auto result = codec.Send(MakePayload(), std::chrono::milliseconds(500));

    EXPECT_FALSE(result.success);
    EXPECT_TRUE(result.retryable);
    ASSERT_TRUE(result.error.has_value());
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access) — guarded by ASSERT_TRUE above
    EXPECT_EQ(result.error->message, "refused");
    EXPECT_EQ(transport.connect_calls.size(), 1U);
    EXPECT_EQ(transport.sent_specs.size(), 0U);  // never got to the actual send
}

// A connect that failed because the peer speaks the wrong protocol — a TLS
// endpoint that would not negotiate h2 (issue #166) — will fail the next
// attempt identically. Retrying it burns the whole retry budget on a
// misconfiguration no backoff can outlast, and buries the one error message
// that says what to change.
TEST(GrpcWireCodecTest, Send_WhenConnectFailsWithProtocolError_IsNotRetryable)
{
    mtfk::FakeTransport transport;
    transport.state = mt::ConnectionState::Disconnected;
    transport.connect_result = mt::make_unexpected(
        mt::Error{.kind = mt::Error::Kind::Protocol, .message = "ALPN negotiated http/1.1"});
    mtw::GrpcWireCodec codec{&transport, MakeConfig()};

    const auto result = codec.Send(MakePayload(), std::chrono::milliseconds(500));

    EXPECT_FALSE(result.success);
    EXPECT_FALSE(result.retryable);
    ASSERT_TRUE(result.error.has_value());
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access) — guarded by ASSERT_TRUE above
    EXPECT_EQ(result.error->message, "ALPN negotiated http/1.1");
    EXPECT_EQ(transport.sent_specs.size(), 0U);
}

// ── Abandoned promise: the mid-connection drop hang (ICP 0018) ───────────────
//
// The transport's drop path left every in-flight promise unfulfilled, and this
// codec waited on it with an unbounded Future().get(). A dropped connection
// therefore wedged a gRPC exporter thread permanently — the process had to be
// restarted. HttpWireCodec was never affected because it always waited with a
// deadline.
//
// The drop path now fulfils (see Http2Transport::AbandonInFlight); this test
// covers the codec half, so that a future bug which abandons a promise costs
// one deadline instead of the thread.

TEST(GrpcWireCodecTest, Send_PromiseNeverFulfilled_TimesOutInsteadOfHanging)
{
    mtfk::FakeTransport transport;
    transport.abandon_promises = true;
    mtw::GrpcWireCodec codec{&transport, MakeConfig()};

    const auto start = std::chrono::steady_clock::now();
    const auto result = codec.Send(MakePayload(), std::chrono::milliseconds(50));
    const auto elapsed = std::chrono::steady_clock::now() - start;

    EXPECT_FALSE(result.success);
    EXPECT_TRUE(result.retryable) << "a timed-out request must be retried, not dropped";
    ASSERT_TRUE(result.error.has_value());
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access) — guarded above
    EXPECT_EQ(result.error->message, "request deadline exceeded");
    // Returned near the deadline rather than blocking indefinitely.
    EXPECT_LT(elapsed, std::chrono::seconds(5));
    EXPECT_EQ(transport.cancel_call_count, 1) << "the abandoned request must be cancelled";
}

// ---------------------------------------------------------------------------
// Request compression (grpc-wire-protocol.md §2.2 and §5.1)
// ---------------------------------------------------------------------------

static mti::EncodedPayload MakeGrpcPayloadFrom(const std::string& s)
{
    auto buf = std::make_unique<std::byte[]>(s.size());
    for (std::size_t i = 0; i < s.size(); ++i)
    {
        buf[i] = static_cast<std::byte>(s[i]);
    }
    return mti::EncodedPayload{std::move(buf), s.size()};
}

/// Decodes the big-endian uint32 message length from a gRPC frame prefix.
static std::uint32_t FrameLength(const std::vector<std::byte>& framed)
{
    return (static_cast<std::uint32_t>(framed[1]) << 24U) |
           (static_cast<std::uint32_t>(framed[2]) << 16U) |
           (static_cast<std::uint32_t>(framed[3]) << 8U) | static_cast<std::uint32_t>(framed[4]);
}

TEST(GrpcWireCodecTest, Send_CompressionOff_UsesUncompressedFlag)
{
    mtfk::FakeTransport transport;
    transport.default_response = GrpcSuccessResponse();
    mtw::GrpcWireCodec codec{&transport, MakeConfig()};

    (void)codec.Send(MakeGrpcPayloadFrom("hello"), std::chrono::milliseconds(500));

    ASSERT_EQ(transport.sent_payloads.size(), 1U);
    const auto& framed = transport.sent_payloads[0];
    ASSERT_EQ(framed.size(), 5U + 5U);
    EXPECT_EQ(framed[0], std::byte{0x00});
    EXPECT_EQ(FrameLength(framed), 5U);
    EXPECT_EQ(FindHeader(transport.sent_specs[0].headers, "grpc-encoding"), "");
}

TEST(GrpcWireCodecTest, Send_CompressionOn_SetsGrpcEncodingHeader)
{
    mtfk::FakeTransport transport;
    transport.default_response = GrpcSuccessResponse();
    mtw::GrpcWireCodecConfig cfg = MakeConfig();
    cfg.compression_gzip = true;
    mtw::GrpcWireCodec codec{&transport, cfg};

    (void)codec.Send(MakeGrpcPayloadFrom("hello"), std::chrono::milliseconds(500));

    ASSERT_EQ(transport.sent_specs.size(), 1U);
    EXPECT_EQ(FindHeader(transport.sent_specs[0].headers, "grpc-encoding"), "gzip");
}

TEST(GrpcWireCodecTest, Send_CompressionOn_SetsCompressedFlagAndCompressedLength)
{
    const std::string body(2048, 'z');

    mtfk::FakeTransport transport;
    transport.default_response = GrpcSuccessResponse();
    mtw::GrpcWireCodecConfig cfg = MakeConfig();
    cfg.compression_gzip = true;
    mtw::GrpcWireCodec codec{&transport, cfg};

    (void)codec.Send(MakeGrpcPayloadFrom(body), std::chrono::milliseconds(500));

    ASSERT_EQ(transport.sent_payloads.size(), 1U);
    const auto& framed = transport.sent_payloads[0];
    ASSERT_GT(framed.size(), 5U);

    // §2.2: the prefix is computed from the compressed bytes, not the input.
    EXPECT_EQ(framed[0], std::byte{0x01});
    EXPECT_EQ(FrameLength(framed), framed.size() - 5U);
    EXPECT_LT(framed.size(), body.size());

    const std::span<const std::byte> message{framed.data() + 5, framed.size() - 5};
    const auto restored = mtfk::GunzipToString(message);
    ASSERT_TRUE(restored.has_value());
    EXPECT_EQ(restored.value_or(""), body);
}

// ---------------------------------------------------------------------------
// Response frame validation and decompression (grpc-wire-protocol.md §2.3, §5.2)
// ---------------------------------------------------------------------------

/// Builds a response DATA frame with an explicit compression flag and an
/// explicit declared length, so a test can produce frames the request framer
/// would never emit — that is the whole point of validating the response.
static std::vector<std::byte> RawGrpcFrame(std::uint8_t flag,
                                           std::uint32_t declared_length,
                                           std::span<const std::byte> message)
{
    std::vector<std::byte> frame;
    frame.reserve(5U + message.size());
    frame.push_back(std::byte{flag});
    frame.push_back(std::byte{static_cast<std::uint8_t>((declared_length >> 24U) & 0xFFU)});
    frame.push_back(std::byte{static_cast<std::uint8_t>((declared_length >> 16U) & 0xFFU)});
    frame.push_back(std::byte{static_cast<std::uint8_t>((declared_length >> 8U) & 0xFFU)});
    frame.push_back(std::byte{static_cast<std::uint8_t>(declared_length & 0xFFU)});
    for (const std::byte b : message)
    {
        frame.push_back(b);
    }
    return frame;
}

static std::vector<std::byte> Gzipped(std::span<const std::byte> plain)
{
    auto compressed = mtw::GzipCompress(plain);
    EXPECT_TRUE(compressed.has_value());
    return compressed.value_or(std::vector<std::byte>{});
}

static std::vector<std::byte> AsBytes(std::span<const std::uint8_t> in)
{
    std::vector<std::byte> out;
    out.reserve(in.size());
    for (const std::uint8_t b : in)
    {
        out.push_back(static_cast<std::byte>(b));
    }
    return out;
}

/// A success response carrying @p body as its single DATA frame.
static mti::TransportResult GrpcBodyResponse(std::vector<std::byte> body)
{
    auto resp = GrpcSuccessResponse();
    resp.response_body = std::move(body);
    return resp;
}

TEST(GrpcWireCodecTest, Send_AdvertisesGrpcAcceptEncodingEvenWithCompressionOff)
{
    // §5.2: the advertisement is a statement about what this client can
    // *decode*, which is now unconditional. Tying it to request compression
    // would leave a gzip-capable collector sending identity for no reason.
    mtfk::FakeTransport transport;
    transport.default_response = GrpcSuccessResponse();
    mtw::GrpcWireCodec codec{&transport, MakeConfig()};

    (void)codec.Send(MakePayload(), std::chrono::milliseconds(500));

    ASSERT_EQ(transport.sent_specs.size(), 1U);
    EXPECT_EQ(FindHeader(transport.sent_specs[0].headers, "grpc-accept-encoding"), "gzip");
}

TEST(GrpcWireCodecTest, Response_CompressedFrame_IsDecompressedBeforeParsing)
{
    // The bug this fixes: the codec skipped the 5-byte prefix and handed the
    // still-gzipped bytes to the protobuf parser, which returned 0 rejected
    // spans — a silent clean success that hid a partial-success rejection.
    const auto plain = AsBytes(kRejected42Proto);
    const auto compressed = Gzipped(plain);
    ASSERT_FALSE(compressed.empty());

    mtfk::FakeTransport transport;
    transport.default_response = GrpcBodyResponse(
        RawGrpcFrame(0x01U, static_cast<std::uint32_t>(compressed.size()), compressed));
    mtw::GrpcWireCodec codec{&transport, MakeConfig()};

    const auto result = codec.Send(MakePayload(), std::chrono::milliseconds(500));
    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.partial_success_rejected, 42U);
}

TEST(GrpcWireCodecTest, Response_UncompressedFrame_StillParses)
{
    const auto plain = AsBytes(kRejected42Proto);

    mtfk::FakeTransport transport;
    transport.default_response =
        GrpcBodyResponse(RawGrpcFrame(0x00U, static_cast<std::uint32_t>(plain.size()), plain));
    mtw::GrpcWireCodec codec{&transport, MakeConfig()};

    const auto result = codec.Send(MakePayload(), std::chrono::milliseconds(500));
    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.partial_success_rejected, 42U);
}

TEST(GrpcWireCodecTest, Response_ShortBody_IsMalformed)
{
    // Fewer than the five prefix bytes. Previously the body was ignored and
    // the response passed as a clean success.
    mtfk::FakeTransport transport;
    mtfk::FakeDiagnosticsSink sink;
    transport.default_response =
        GrpcBodyResponse(std::vector<std::byte>{std::byte{0x00}, std::byte{0x00}, std::byte{0x02}});
    mtw::GrpcWireCodec codec{&transport, MakeConfig(), nullptr, &sink};

    const auto result = codec.Send(MakePayload(), std::chrono::milliseconds(500));
    EXPECT_FALSE(result.success);
    EXPECT_FALSE(result.retryable);
    EXPECT_EQ(DropCount(sink, mt::DropReason::MalformedResponse), 1U);
}

TEST(GrpcWireCodecTest, Response_UnknownCompressionFlag_IsMalformed)
{
    // CF values 0x02..0xFF are reserved (§2.2). A peer using one is speaking a
    // protocol this codec does not, and no retry changes that.
    const auto plain = AsBytes(kRejected42Proto);

    mtfk::FakeTransport transport;
    mtfk::FakeDiagnosticsSink sink;
    transport.default_response =
        GrpcBodyResponse(RawGrpcFrame(0x02U, static_cast<std::uint32_t>(plain.size()), plain));
    mtw::GrpcWireCodec codec{&transport, MakeConfig(), nullptr, &sink};

    const auto result = codec.Send(MakePayload(), std::chrono::milliseconds(500));
    EXPECT_FALSE(result.success);
    EXPECT_FALSE(result.retryable);
    EXPECT_EQ(DropCount(sink, mt::DropReason::MalformedResponse), 1U);
}

TEST(GrpcWireCodecTest, Response_DeclaredLengthLongerThanBody_IsMalformed)
{
    // A truncated message: the prefix promises more bytes than arrived.
    const auto plain = AsBytes(kRejected42Proto);

    mtfk::FakeTransport transport;
    mtfk::FakeDiagnosticsSink sink;
    transport.default_response =
        GrpcBodyResponse(RawGrpcFrame(0x00U, static_cast<std::uint32_t>(plain.size()) + 1U, plain));
    mtw::GrpcWireCodec codec{&transport, MakeConfig(), nullptr, &sink};

    const auto result = codec.Send(MakePayload(), std::chrono::milliseconds(500));
    EXPECT_FALSE(result.success);
    EXPECT_FALSE(result.retryable);
    EXPECT_EQ(DropCount(sink, mt::DropReason::MalformedResponse), 1U);
}

TEST(GrpcWireCodecTest, Response_TrailingBytesAfterMessage_IsMalformed)
{
    // §2.3: OTLP unary carries exactly one message. Bytes past its declared
    // length are a second message, and the codec rejects those cleanly rather
    // than parsing the first and ignoring the rest.
    const auto plain = AsBytes(kRejected42Proto);
    auto body = RawGrpcFrame(0x00U, static_cast<std::uint32_t>(plain.size()), plain);
    body.push_back(std::byte{0x00});

    mtfk::FakeTransport transport;
    mtfk::FakeDiagnosticsSink sink;
    transport.default_response = GrpcBodyResponse(std::move(body));
    mtw::GrpcWireCodec codec{&transport, MakeConfig(), nullptr, &sink};

    const auto result = codec.Send(MakePayload(), std::chrono::milliseconds(500));
    EXPECT_FALSE(result.success);
    EXPECT_FALSE(result.retryable);
    EXPECT_EQ(DropCount(sink, mt::DropReason::MalformedResponse), 1U);
}

TEST(GrpcWireCodecTest, Response_CorruptCompressedFrame_IsMalformed)
{
    // CF says gzip, the bytes are not gzip. Distinct from the overflow case
    // below: this one is malformed, not a bomb.
    const auto garbage = AsBytes(kRejected42Proto);

    mtfk::FakeTransport transport;
    mtfk::FakeDiagnosticsSink sink;
    transport.default_response =
        GrpcBodyResponse(RawGrpcFrame(0x01U, static_cast<std::uint32_t>(garbage.size()), garbage));
    mtw::GrpcWireCodec codec{&transport, MakeConfig(), nullptr, &sink};

    const auto result = codec.Send(MakePayload(), std::chrono::milliseconds(500));
    EXPECT_FALSE(result.success);
    EXPECT_FALSE(result.retryable);
    EXPECT_EQ(DropCount(sink, mt::DropReason::MalformedResponse), 1U);
    EXPECT_EQ(DropCount(sink, mt::DropReason::DecompressionTooLarge), 0U);
}

TEST(GrpcWireCodecTest, Response_DecompressionBomb_RecordsDecompressionTooLarge)
{
    // 1 MiB of zeroes behind a kilobyte of gzip, against a 4 KiB ceiling.
    const std::vector<std::byte> bomb(1U << 20U, std::byte{0x00});
    const auto compressed = Gzipped(bomb);
    ASSERT_FALSE(compressed.empty());

    mtfk::FakeTransport transport;
    mtfk::FakeDiagnosticsSink sink;
    transport.default_response = GrpcBodyResponse(
        RawGrpcFrame(0x01U, static_cast<std::uint32_t>(compressed.size()), compressed));
    mtw::GrpcWireCodecConfig cfg = MakeConfig();
    cfg.max_decompressed_bytes = 4096U;
    mtw::GrpcWireCodec codec{&transport, cfg, nullptr, &sink};

    const auto result = codec.Send(MakePayload(), std::chrono::milliseconds(500));
    EXPECT_FALSE(result.success);
    EXPECT_FALSE(result.retryable) << "a bomb is a property of the peer, not of this attempt";
    EXPECT_EQ(DropCount(sink, mt::DropReason::DecompressionTooLarge), 1U);
    EXPECT_EQ(DropCount(sink, mt::DropReason::MalformedResponse), 0U);
    ASSERT_TRUE(result.error.has_value());
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access) — guarded by ASSERT_TRUE above
    EXPECT_FALSE(result.error->message.empty());
}

TEST(GrpcWireCodecTest, Response_DecompressedExactlyAtTheCap_IsAccepted)
{
    // The other side of the cap. An off-by-one here would reject every legal
    // response whose size happens to equal the configured ceiling.
    const auto plain = AsBytes(kRejected42Proto);
    const auto compressed = Gzipped(plain);

    mtfk::FakeTransport transport;
    transport.default_response = GrpcBodyResponse(
        RawGrpcFrame(0x01U, static_cast<std::uint32_t>(compressed.size()), compressed));
    mtw::GrpcWireCodecConfig cfg = MakeConfig();
    cfg.max_decompressed_bytes = static_cast<std::uint32_t>(plain.size());
    mtw::GrpcWireCodec codec{&transport, cfg};

    const auto result = codec.Send(MakePayload(), std::chrono::milliseconds(500));
    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.partial_success_rejected, 42U);
}

TEST(GrpcWireCodecTest, Response_MalformedFrame_NullSink_IsNotDereferenced)
{
    mtfk::FakeTransport transport;
    transport.default_response =
        GrpcBodyResponse(std::vector<std::byte>{std::byte{0x00}, std::byte{0x01}});
    mtw::GrpcWireCodec codec{&transport, MakeConfig()};  // no sink

    const auto result = codec.Send(MakePayload(), std::chrono::milliseconds(500));
    EXPECT_FALSE(result.success);
}

TEST(GrpcWireCodecTest, Response_NonZeroStatus_DoesNotValidateTheFrame)
{
    // The status is already the verdict; a broken body on a failure response
    // must not turn a retryable status into a non-retryable malformed one.
    mtfk::FakeTransport transport;
    mtfk::FakeDiagnosticsSink sink;
    mti::TransportResult resp{
        .success = true,
        .response_headers = {{.name = ":status", .value = "200"}},
        .response_trailers = {{.name = "grpc-status", .value = "14"}},
        .response_body = {std::byte{0xFF}, std::byte{0xFF}},
        .error = {},
    };
    transport.default_response = std::move(resp);
    mtw::GrpcWireCodec codec{&transport, MakeConfig(), nullptr, &sink};

    const auto result = codec.Send(MakePayload(), std::chrono::milliseconds(500));
    EXPECT_FALSE(result.success);
    EXPECT_TRUE(result.retryable);
    EXPECT_EQ(DropCount(sink, mt::DropReason::MalformedResponse), 0U);
}

// ---------------------------------------------------------------------------
// grpc-message surfacing (issue #171, grpc-wire-protocol.md §4.4)
//
// Every non-zero status used to collapse to the literal "grpc error": the code
// was parsed and thrown away, and grpc-message was never read anywhere in src/.
// last_error_message is the whole operator-visible explanation, so a rejected
// credential, a wrong path and a server bug all read identically.
// ---------------------------------------------------------------------------

/// A failure response carrying @p message in the `grpc-message` trailer.
static mti::TransportResult GrpcStatusWithMessage(const std::string& code,
                                                  const std::string& message)
{
    return mti::TransportResult{
        .success = true,
        .response_headers = {{.name = ":status", .value = "200"}},
        .response_trailers = {{.name = "grpc-status", .value = code},
                              {.name = "grpc-message", .value = message}},
        .response_body = {},
        .error = {},
    };
}

static std::string ErrorMessage(const mti::WireResult& result)
{
    return result.error.has_value() ? result.error->message : std::string{"<no error>"};
}

TEST(GrpcWireCodecTest, Send_Unauthenticated_NamesTheStatusAndCarriesTheMessage)
{
    // The exact exchange from issue #171, as captured from the collector's
    // bearer-auth receiver with a wrong token.
    mtfk::FakeTransport transport;
    transport.default_response =
        GrpcStatusWithMessage("16", "provided authorization does not match expected scheme");
    mtw::GrpcWireCodec codec{&transport, MakeConfig()};

    const auto result = codec.Send(MakePayload(), std::chrono::milliseconds(500));
    EXPECT_FALSE(result.success);
    EXPECT_FALSE(result.retryable);
    EXPECT_EQ(ErrorMessage(result),
              "UNAUTHENTICATED (16): provided authorization does not match expected scheme");
}

TEST(GrpcWireCodecTest, Send_GrpcMessage_IsPercentDecoded)
{
    mtfk::FakeTransport transport;
    transport.default_response = GrpcStatusWithMessage("3", "bad%20field%3A%20name");
    mtw::GrpcWireCodec codec{&transport, MakeConfig()};

    const auto result = codec.Send(MakePayload(), std::chrono::milliseconds(500));
    EXPECT_EQ(ErrorMessage(result), "INVALID_ARGUMENT (3): bad field: name");
}

TEST(GrpcWireCodecTest, Send_GrpcMessage_InvalidEscapeSurvivesVerbatim)
{
    // A bare '%' in a human-readable sentence must not cost the whole message.
    mtfk::FakeTransport transport;
    transport.default_response = GrpcStatusWithMessage("13", "queue 100% full");
    mtw::GrpcWireCodec codec{&transport, MakeConfig()};

    const auto result = codec.Send(MakePayload(), std::chrono::milliseconds(500));
    EXPECT_EQ(ErrorMessage(result), "INTERNAL (13): queue 100% full");
}

TEST(GrpcWireCodecTest, Send_GrpcMessage_EmptyValueLeavesNoDanglingSeparator)
{
    // A present-but-empty trailer is not the same as a message; it must not
    // produce "INTERNAL (13): " with nothing after the colon.
    mtfk::FakeTransport transport;
    transport.default_response = GrpcStatusWithMessage("13", "");
    mtw::GrpcWireCodec codec{&transport, MakeConfig()};

    const auto result = codec.Send(MakePayload(), std::chrono::milliseconds(500));
    EXPECT_EQ(ErrorMessage(result), "INTERNAL (13)");
}

TEST(GrpcWireCodecTest, Send_GrpcMessage_ReadFromInitialHeadersOnTrailerOnly)
{
    // Trailer-only responses carry the status — and the message — in the first
    // HEADERS frame (§2.5). Same fallback the status lookup already does.
    mtfk::FakeTransport transport;
    transport.default_response = mti::TransportResult{
        .success = true,
        .response_headers = {{.name = ":status", .value = "200"},
                             {.name = "grpc-status", .value = "7"},
                             {.name = "grpc-message", .value = "no%20access"}},
        .response_trailers = {},
        .response_body = {},
        .error = {},
    };
    mtw::GrpcWireCodec codec{&transport, MakeConfig()};

    const auto result = codec.Send(MakePayload(), std::chrono::milliseconds(500));
    EXPECT_EQ(ErrorMessage(result), "PERMISSION_DENIED (7): no access");
}

TEST(GrpcWireCodecTest, Send_UnrecognizedStatusCode_IsNamedAndNonRetryable)
{
    // A code outside 0..16 cannot be classified, but the number is the only
    // lead an operator has.
    mtfk::FakeTransport transport;
    transport.default_response = GrpcStatusWithMessage("42", "from the future");
    mtw::GrpcWireCodec codec{&transport, MakeConfig()};

    const auto result = codec.Send(MakePayload(), std::chrono::milliseconds(500));
    EXPECT_FALSE(result.success);
    EXPECT_FALSE(result.retryable);
    EXPECT_EQ(ErrorMessage(result), "UNRECOGNIZED (42): from the future");
}

TEST(GrpcWireCodecTest, Send_UnparseableStatusValue_IsNamedAndNonRetryable)
{
    // `grpc-status: banana`. from_chars leaves the sentinel in place; the
    // sentinel must not read as some real status.
    mtfk::FakeTransport transport;
    transport.default_response = GrpcStatusWithMessage("banana", "");
    mtw::GrpcWireCodec codec{&transport, MakeConfig()};

    const auto result = codec.Send(MakePayload(), std::chrono::milliseconds(500));
    EXPECT_FALSE(result.success);
    EXPECT_FALSE(result.retryable);
    EXPECT_EQ(ErrorMessage(result), "UNRECOGNIZED (-1)");
}

TEST(GrpcWireCodecTest, Send_Failure_PopulatesResponseExcerpt)
{
    // response_excerpt was left empty on every gRPC failure. It is the
    // diagnostics field the HTTP codec fills from the body, and the decoded
    // grpc-message is this protocol's equivalent.
    mtfk::FakeTransport transport;
    transport.default_response = GrpcStatusWithMessage("13", "disk%20on%20fire");
    mtw::GrpcWireCodec codec{&transport, MakeConfig()};

    const auto result = codec.Send(MakePayload(), std::chrono::milliseconds(500));
    EXPECT_EQ(result.response_excerpt, "disk on fire");
}

TEST(GrpcWireCodecTest, Send_OverlongGrpcMessage_IsTruncated)
{
    // Bounded by max_trailer_bytes on the wire — 64 KiB — but the health
    // snapshot keeps 256 chars, so the codec must not assemble the rest.
    const std::string huge(4096, 'x');
    mtfk::FakeTransport transport;
    transport.default_response = GrpcStatusWithMessage("13", huge);
    mtw::GrpcWireCodec codec{&transport, MakeConfig()};

    const auto result = codec.Send(MakePayload(), std::chrono::milliseconds(500));
    EXPECT_LT(ErrorMessage(result).size(), huge.size());
    EXPECT_LT(result.response_excerpt.size(), huge.size());
    EXPECT_EQ(ErrorMessage(result).rfind("INTERNAL (13): ", 0), 0U);
}

TEST(GrpcWireCodecTest, Send_Success_LeavesNoErrorOrExcerpt)
{
    // The new formatting must not leak onto the success path.
    mtfk::FakeTransport transport;
    transport.default_response = GrpcSuccessResponse();
    mtw::GrpcWireCodec codec{&transport, MakeConfig()};

    const auto result = codec.Send(MakePayload(), std::chrono::milliseconds(500));
    EXPECT_TRUE(result.success);
    EXPECT_FALSE(result.error.has_value());
    EXPECT_TRUE(result.response_excerpt.empty());
}

// ---------------------------------------------------------------------------
// RESOURCE_EXHAUSTED and missing-status messages — the siblings
// ---------------------------------------------------------------------------

TEST(GrpcWireCodecTest, Send_ResourceExhausted_WithoutRetryInfo_NamesTheStatus)
{
    mtfk::FakeTransport transport;
    transport.default_response = GrpcStatusWithMessage("8", "too%20many%20spans");
    mtw::GrpcWireCodec codec{&transport, MakeConfig()};

    const auto result = codec.Send(MakePayload(), std::chrono::milliseconds(500));
    EXPECT_FALSE(result.retryable);
    // The RetryInfo note is on the message deliberately: "not retried" is the
    // non-obvious half of this row (error-model.md §7.2, LOCKED), and an
    // operator seeing a dropped batch under load needs to know the server was
    // asked and gave no delay.
    EXPECT_EQ(ErrorMessage(result),
              "RESOURCE_EXHAUSTED (8): too many spans - no RetryInfo, not retried");
    EXPECT_EQ(result.response_excerpt, "too many spans");
}

TEST(GrpcWireCodecTest, Send_ResourceExhausted_UndecodableDetails_NamesTheStatus)
{
    // details-bin present but not a parseable RetryInfo: still terminal, and
    // the message must say which status it was rather than describing only the
    // missing RetryInfo.
    mtfk::FakeTransport transport;
    transport.default_response = mti::TransportResult{
        .success = true,
        .response_headers = {{.name = ":status", .value = "200"}},
        .response_trailers = {{.name = "grpc-status", .value = "8"},
                              {.name = "grpc-status-details-bin", .value = "!!!not-base64!!!"}},
        .response_body = {},
        .error = {},
    };
    mtw::GrpcWireCodec codec{&transport, MakeConfig()};

    const auto result = codec.Send(MakePayload(), std::chrono::milliseconds(500));
    EXPECT_FALSE(result.retryable);
    EXPECT_NE(ErrorMessage(result).find("RESOURCE_EXHAUSTED (8)"), std::string::npos)
        << ErrorMessage(result);
    EXPECT_NE(ErrorMessage(result).find("RetryInfo"), std::string::npos) << ErrorMessage(result);
}

TEST(GrpcWireCodecTest, Send_MissingGrpcStatus_MessageNamesTheHttpStatus)
{
    // "missing grpc-status" alone does not tell an operator whether a proxy
    // returned 502 or the endpoint answered 404.
    mtfk::FakeTransport transport;
    transport.default_response = mti::TransportResult{
        .success = true,
        .response_headers = {{.name = ":status", .value = "404"}},
        .response_trailers = {},
        .response_body = {},
        .error = {},
    };
    mtw::GrpcWireCodec codec{&transport, MakeConfig()};

    const auto result = codec.Send(MakePayload(), std::chrono::milliseconds(500));
    EXPECT_FALSE(result.retryable);
    EXPECT_NE(ErrorMessage(result).find("404"), std::string::npos) << ErrorMessage(result);
    EXPECT_NE(ErrorMessage(result).find("grpc-status"), std::string::npos) << ErrorMessage(result);
}

// ---------------------------------------------------------------------------
// Missing grpc-status — the warn diagnostic (grpc-wire-protocol.md §4.2)
//
// §4.2: "The codec emits a `warn`-level diagnostic on the first occurrence per
// (connection) so operators can see 'your proxy is terminating gRPC streams
// without trailers.'" First occurrence, not every occurrence: a proxy doing
// this does it to every export, and a per-batch warn would bury the signal it
// is trying to raise.
// ---------------------------------------------------------------------------

namespace
{

struct CapturedLog
{
    mt::LogLevel level;
    std::string message;
};

/// Installs a capturing log sink for the duration of a test and restores the
/// default afterwards, so a leaked sink cannot affect a later suite.
class GrpcCodecLogTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        mt::SetLogSink([this](mt::LogLevel level, std::string_view message)
                       { m_logs.push_back({.level = level, .message = std::string{message}}); });
    }

    void TearDown() override
    {
        mt::ResetLogSink();
    }

    [[nodiscard]] std::size_t WarnCount() const
    {
        return static_cast<std::size_t>(std::ranges::count_if(
            m_logs, [](const CapturedLog& e) { return e.level == mt::LogLevel::Warn; }));
    }

    std::vector<CapturedLog> m_logs;
};

mti::TransportResult MissingStatusResponse(const std::string& http_status)
{
    return mti::TransportResult{
        .success = true,
        .response_headers = {{.name = ":status", .value = http_status}},
        .response_trailers = {},
        .response_body = {},
        .error = {},
    };
}

}  // namespace

TEST_F(GrpcCodecLogTest, MissingGrpcStatus_WarnsOnce)
{
    mtfk::FakeTransport transport;
    transport.default_response = MissingStatusResponse("404");
    mtw::GrpcWireCodec codec{&transport, MakeConfig()};

    (void)codec.Send(MakePayload(), std::chrono::milliseconds(500));
    (void)codec.Send(MakePayload(), std::chrono::milliseconds(500));
    (void)codec.Send(MakePayload(), std::chrono::milliseconds(500));

    EXPECT_EQ(WarnCount(), 1U) << "§4.2 says first occurrence per connection, not per batch";
    ASSERT_FALSE(m_logs.empty());
    EXPECT_NE(m_logs.front().message.find("grpc-status"), std::string::npos)
        << m_logs.front().message;
}

TEST_F(GrpcCodecLogTest, MissingGrpcStatus_WarnsAgainOnANewConnection)
{
    // "Per connection" is the spec's unit. A codec that warned once for its
    // whole lifetime would go silent across a reconnect, which is exactly when
    // an operator restarting a proxy wants to see it again.
    mtfk::FakeTransport transport;
    transport.default_response = MissingStatusResponse("404");
    mtw::GrpcWireCodec codec{&transport, MakeConfig()};

    (void)codec.Send(MakePayload(), std::chrono::milliseconds(500));
    EXPECT_EQ(WarnCount(), 1U);

    // Drop the connection; the next Send reconnects through EnsureConnected.
    transport.state = mt::ConnectionState::Disconnected;
    (void)codec.Send(MakePayload(), std::chrono::milliseconds(500));
    EXPECT_EQ(WarnCount(), 2U);
}

TEST_F(GrpcCodecLogTest, RetryableMissingGrpcStatus_AlsoWarns)
{
    // A 503 without trailers is still a peer that is not speaking gRPC; it is
    // retryable, but the operator should still learn why.
    mtfk::FakeTransport transport;
    transport.default_response = MissingStatusResponse("503");
    mtw::GrpcWireCodec codec{&transport, MakeConfig()};

    const auto result = codec.Send(MakePayload(), std::chrono::milliseconds(500));
    EXPECT_TRUE(result.retryable);
    EXPECT_EQ(WarnCount(), 1U);
}

TEST_F(GrpcCodecLogTest, OrdinaryGrpcFailure_DoesNotWarn)
{
    // The warn is about malformed peers, not about ordinary rejections.
    mtfk::FakeTransport transport;
    transport.default_response = GrpcStatusResponse("13");
    mtw::GrpcWireCodec codec{&transport, MakeConfig()};

    (void)codec.Send(MakePayload(), std::chrono::milliseconds(500));
    EXPECT_EQ(WarnCount(), 0U);
}

// ---------------------------------------------------------------------------
// Issue #223: a well-framed message that does not parse as the Export
// response is a malformed response (error-model.md §7.2, "Multi-frame parse
// failure / truncated message"), not a clean OK. Each signal has its own codec,
// so every case runs against the traces, metrics and logs service paths.
// ---------------------------------------------------------------------------

namespace
{

constexpr std::array<const char*, 3> kServicePaths{
    "",
    "/opentelemetry.proto.collector.metrics.v1.MetricsService/Export",
    "/opentelemetry.proto.collector.logs.v1.LogsService/Export",
};

struct PartialSuccessRun
{
    mti::WireResult result;
    std::uint64_t malformed = 0;
    std::uint64_t rejected_counted_by_codec = 0;
};

PartialSuccessRun SendWithMessage(const char* service_path,
                                  std::initializer_list<std::uint8_t> proto)
{
    const std::vector<std::uint8_t> bytes{proto};
    mtfk::FakeTransport transport;
    mtfk::FakeDiagnosticsSink sink;
    auto resp = GrpcSuccessResponse();
    resp.response_body = GrpcFrame(bytes);
    transport.default_response = resp;
    auto cfg = MakeConfig();
    cfg.service_path = service_path;
    mtw::GrpcWireCodec codec{&transport, cfg, nullptr, &sink};
    auto result = codec.Send(MakePayload(), std::chrono::milliseconds(500));
    return {
        .result = std::move(result),
        .malformed = DropCount(sink, mt::DropReason::MalformedResponse),
        .rejected_counted_by_codec = DropCount(sink, mt::DropReason::PartialSuccessRejection),
    };
}

}  // namespace

TEST(GrpcWireCodecTest, PartialSuccess_AbsentFromNonEmptyMessage_CleanSuccess)
{
    for (const char* path : kServicePaths)
    {
        SCOPED_TRACE(path);
        const auto run = SendWithMessage(path, {0x10, 0x05});  // unknown field only
        EXPECT_TRUE(run.result.success);
        EXPECT_EQ(run.result.partial_success_rejected, 0U);
        EXPECT_EQ(run.malformed, 0U);
    }
}

TEST(GrpcWireCodecTest, PartialSuccess_ValidZero_CleanSuccess)
{
    for (const char* path : kServicePaths)
    {
        SCOPED_TRACE(path);
        const auto run = SendWithMessage(path, {0x0A, 0x02, 0x08, 0x00});
        EXPECT_TRUE(run.result.success);
        EXPECT_EQ(run.result.partial_success_rejected, 0U);
        EXPECT_EQ(run.malformed, 0U);
    }
}

TEST(GrpcWireCodecTest, PartialSuccess_ValidN_SuccessCarryingCount)
{
    for (const char* path : kServicePaths)
    {
        SCOPED_TRACE(path);
        const auto run = SendWithMessage(path, {0x0A, 0x02, 0x08, 0x09});
        EXPECT_TRUE(run.result.success);
        EXPECT_FALSE(run.result.retryable);
        EXPECT_EQ(run.result.partial_success_rejected, 9U);
        EXPECT_EQ(run.malformed, 0U);
        // The exporter's final-outcome funnel counts it, not the codec (§3).
        EXPECT_EQ(run.rejected_counted_by_codec, 0U);
    }
}

TEST(GrpcWireCodecTest, PartialSuccess_TruncatedMessage_IsMalformed)
{
    for (const char* path : kServicePaths)
    {
        SCOPED_TRACE(path);
        // partial_success claims 5 bytes, 2 follow; the frame itself is well-formed.
        const auto run = SendWithMessage(path, {0x0A, 0x05, 0x08, 0x2A});
        EXPECT_FALSE(run.result.success);
        EXPECT_FALSE(run.result.retryable);
        EXPECT_EQ(run.result.partial_success_rejected, 0U);
        EXPECT_EQ(run.malformed, 1U);
        ASSERT_TRUE(run.result.error.has_value());
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access) — guarded by ASSERT_TRUE above
        EXPECT_EQ(run.result.error->kind, mt::Error::Kind::Malformed);
    }
}

TEST(GrpcWireCodecTest, PartialSuccess_GarbageMessage_IsMalformed)
{
    for (const char* path : kServicePaths)
    {
        SCOPED_TRACE(path);
        const auto run = SendWithMessage(path, {0x0A, 0x02, 0x08, 0x2A, 0xFF});
        EXPECT_FALSE(run.result.success);
        EXPECT_FALSE(run.result.retryable);
        EXPECT_EQ(run.malformed, 1U);
    }
}
