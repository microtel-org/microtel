// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0
//
// Real OtlpExporter + real HttpWireCodec + real CallbackAuthProvider against a
// FakeTransport. What a unit test on any one of them cannot see is the blast
// radius of an auth failure: which batches survive it, and what an operator
// reads out of the diagnostics afterwards.
//
// Issues #250 and #251, and `docs/interfaces.md` §4.9:
//   - a callback that returns an error drops *its* batch (sending without auth
//     is worse than not sending) — it does not ship the batch bare;
//   - a callback that throws is converted to `InternalFailure` at the provider
//     boundary and costs the same one batch, not the whole drain.

#include "microtel/error.hpp"
#include "microtel/expected.hpp"
#include "microtel/internal/auth_provider.hpp"
#include "microtel/internal/batch.hpp"
#include "microtel/internal/transport.hpp"
#include "microtel/provider.hpp"
#include "microtel/resource.hpp"
#include "microtel/status.hpp"

#include "common/config/auth_providers.hpp"
#include "exporter/otlp_exporter.hpp"
#include "fakes/fake_diagnostics_sink.hpp"
#include "fakes/fake_transport.hpp"
#include "mocks/mock_otlp_encoder.hpp"
#include "wire/http/http_wire_codec.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace mt = microtel;
namespace mtc = microtel::config;
namespace mte = microtel::exporter;
namespace mti = microtel::internal;
namespace mtfk = microtel::testing;
namespace mtw = microtel::wire;

namespace
{

constexpr auto kFlushTimeout = std::chrono::seconds(5);

mti::BatchHandle MakeBatch()
{
    auto resource = std::make_shared<mt::Resource>();
    mti::InstrumentationScope scope{.name = "auth-failure", .version = "1.0"};
    std::vector<mti::SpanRecord> spans;
    spans.push_back(mti::SpanRecord{.name = "s"});
    return mti::BatchHandle{std::move(spans), std::move(resource), std::move(scope)};
}

mti::TransportResult OkResponse()
{
    return mti::TransportResult{
        .success = true,
        .response_headers = {{.name = ":status", .value = "200"}},
        .response_trailers = {},
        .response_body = {},
        .error = {},
    };
}

mtw::HttpWireCodecConfig MakeCodecConfig()
{
    return mtw::HttpWireCodecConfig{
        .host = "localhost:4318",
        .scheme = "http",
        .path = "",
        .signal_path = {},
        .extra_headers = {},
    };
}

std::uint64_t DropCount(const mtfk::FakeDiagnosticsSink& sink, mt::DropReason reason)
{
    return sink.drop_counters.at(static_cast<std::size_t>(reason));
}

bool HasAuthHeader(const mti::RequestSpec& spec)
{
    return std::ranges::any_of(spec.headers,
                               [](const mti::HeaderField& h) { return h.name == "authorization"; });
}

/// Exports three batches through the pipeline and waits for the drain.
/// `provider` supplies the header; the middle call is the one each test fails.
void ExportThreeBatches(mti::IAuthProvider& auth,
                        mtfk::FakeTransport& transport,
                        mtfk::FakeDiagnosticsSink& sink)
{
    mtfk::MockOtlpEncoder encoder;
    transport.default_response = OkResponse();
    mtw::HttpWireCodec codec{&transport, MakeCodecConfig(), &auth, &sink, nullptr};
    // One span per request: each batch is its own request, and so its own
    // auth callback. Without it the exporter may join drained batches into one
    // request (docs/leaf-concentrator-design.md §3.6.1), which fetches one
    // header for all of them, and which batch a failure costs would depend on
    // the worker's timing.
    mte::OtlpExporterConfig config;
    config.max_spans_per_request = 1;
    mte::OtlpExporter exporter{&encoder, &codec, config, &sink, nullptr};

    for (int i = 0; i < 3; ++i)
    {
        ASSERT_EQ(exporter.Export(MakeBatch()), mti::ExportResult::Success);
    }
    ASSERT_EQ(exporter.ForceFlush(kFlushTimeout), mt::Status::Failed);
    ASSERT_EQ(exporter.Shutdown(kFlushTimeout), mt::Status::Completed);
}

}  // namespace

// Issue #250. Pre-fix this shipped all three batches, the middle one with no
// `authorization` header at all, and no counter moved.
TEST(AuthFailureExportIntegrationTest, CallbackError_DropsOnlyItsBatch_AndIsCounted)
{
    std::atomic<int> calls{0};
    // Zero TTL: one callback invocation per batch, so the failure lands on
    // exactly one of them.
    mtc::CallbackAuthProvider auth{[&calls]() -> mt::Expected<std::string, mt::Error>
                                   {
                                       if (++calls == 2)
                                       {
                                           return mt::make_unexpected(
                                               mt::Error{.kind = mt::Error::Kind::Network,
                                                         .message = "token endpoint refused"});
                                       }
                                       return std::string{"Bearer tok"};
                                   },
                                   std::chrono::milliseconds(0)};

    mtfk::FakeTransport transport;
    mtfk::FakeDiagnosticsSink sink;
    ExportThreeBatches(auth, transport, sink);

    EXPECT_EQ(transport.sent_specs.size(), 2U)
        << "the batch whose auth failed must not reach the wire at all";
    for (const auto& spec : transport.sent_specs)
    {
        EXPECT_TRUE(HasAuthHeader(spec)) << "no batch may ship unauthenticated";
    }
    EXPECT_EQ(sink.batches_sent, 2U);
    EXPECT_EQ(sink.batches_failed, 1U);
    EXPECT_EQ(DropCount(sink, mt::DropReason::NonRetryableFailure), 1U)
        << "interfaces.md §4.9: the batch is dropped as a non-retryable failure";
    EXPECT_NE(sink.last_error_message.find("authorization"), std::string::npos)
        << "the operator must be told auth failed, not left reading a 401: "
        << sink.last_error_message;
    EXPECT_NE(sink.last_error_message.find("token endpoint refused"), std::string::npos);
}

// Issue #251. Pre-fix the throw unwound into OtlpExporter's drain handler and
// every batch in the drain was lost, not the one whose header was being built.
TEST(AuthFailureExportIntegrationTest, ThrowingCallback_CostsOneBatch_NotTheDrain)
{
    std::atomic<int> calls{0};
    mtc::CallbackAuthProvider auth{[&calls]() -> mt::Expected<std::string, mt::Error>
                                   {
                                       if (++calls == 2)
                                       {
                                           throw std::runtime_error{"token endpoint refused"};
                                       }
                                       return std::string{"Bearer tok"};
                                   },
                                   std::chrono::milliseconds(0)};

    mtfk::FakeTransport transport;
    mtfk::FakeDiagnosticsSink sink;
    ExportThreeBatches(auth, transport, sink);

    EXPECT_EQ(transport.sent_specs.size(), 2U) << "the other batches in the drain still ship";
    EXPECT_EQ(sink.batches_sent, 2U);
    EXPECT_EQ(sink.batches_failed, 1U) << "one batch lost, not the whole drain";
    EXPECT_EQ(DropCount(sink, mt::DropReason::NonRetryableFailure), 1U);
    EXPECT_NE(sink.last_error_message.find("authorization"), std::string::npos)
        << sink.last_error_message;
    EXPECT_NE(sink.last_error_message.find("token endpoint refused"), std::string::npos)
        << "the callback's own message survives the InternalFailure conversion";
}
