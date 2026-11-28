// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0
//
// ConcatenateTraceRequests (docs/leaf-concentrator-design.md §3.6.1): the
// concatenation of two ExportTraceServiceRequest encodings is a valid request
// whose resource_spans are the first's followed by the second's. Checked by
// decoding the output with upb.

#include "wire/encoder/trace_request_concat.hpp"

#include "wire/encoder/otlp_encoder.hpp"

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#include "opentelemetry/proto/collector/trace/v1/trace_service.upb.h"
#include "opentelemetry/proto/common/v1/common.upb.h"
#include "opentelemetry/proto/resource/v1/resource.upb.h"
#include "opentelemetry/proto/trace/v1/trace.upb.h"
#include "upb/mem/arena.h"
#pragma GCC diagnostic pop

#include "microtel/internal/batch.hpp"
#include "microtel/internal/encoded_payload.hpp"
#include "microtel/resource.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace mt = microtel;
namespace mti = microtel::internal;
namespace mtw = microtel::wire;

namespace
{

mti::EncodedPayload EncodeFor(const std::string& device, const std::string& span_name)
{
    std::vector<mti::SpanRecord> spans(1);
    spans[0].name = span_name;
    const mti::BatchHandle batch{std::move(spans),
                                 std::make_shared<const mt::Resource>(std::vector<mt::KeyValue>{
                                     {.key = "device.id", .value = device}}),
                                 mti::InstrumentationScope{.name = "scope", .version = "1"}};
    mtw::OtlpEncoder encoder;
    return encoder.Encode(batch);
}

std::string View(upb_StringView v)
{
    return std::string{v.data, v.size};
}

/// The device.id of one ResourceSpans and the name of its first span.
std::pair<std::string, std::string> Summarise(const opentelemetry_proto_trace_v1_ResourceSpans* rs)
{
    const auto* const res = opentelemetry_proto_trace_v1_ResourceSpans_resource(rs);
    std::size_t n_attrs = 0;
    const auto* const* attrs = opentelemetry_proto_resource_v1_Resource_attributes(res, &n_attrs);
    const auto* const value = opentelemetry_proto_common_v1_KeyValue_value(attrs[0]);
    std::size_t n_ss = 0;
    const auto* const* ss = opentelemetry_proto_trace_v1_ResourceSpans_scope_spans(rs, &n_ss);
    std::size_t n_spans = 0;
    const auto* const* spans = opentelemetry_proto_trace_v1_ScopeSpans_spans(ss[0], &n_spans);
    return {View(opentelemetry_proto_common_v1_AnyValue_string_value(value)),
            View(opentelemetry_proto_trace_v1_Span_name(spans[0]))};
}

/// Summarise every ResourceSpans of the request in @p payload.
std::vector<std::pair<std::string, std::string>> Decode(const mti::EncodedPayload& payload)
{
    upb_Arena* const arena = upb_Arena_New();
    std::vector<std::pair<std::string, std::string>> out;
    const auto bytes = payload.Bytes();
    const auto* const req = opentelemetry_proto_collector_trace_v1_ExportTraceServiceRequest_parse(
        reinterpret_cast<const char*>(bytes.data()), bytes.size(), arena);
    EXPECT_NE(req, nullptr);
    std::size_t n = 0;
    const auto* const* rss =
        req == nullptr
            ? nullptr
            : opentelemetry_proto_collector_trace_v1_ExportTraceServiceRequest_resource_spans(req,
                                                                                              &n);
    out.reserve(n);
    for (std::size_t i = 0; i < n; ++i)
    {
        out.push_back(Summarise(rss[i]));
    }
    upb_Arena_Free(arena);
    return out;
}

}  // namespace

TEST(TraceRequestConcatTest, ConcatenationDecodesToTheUnionOfResourceSpansInOrder)
{
    std::vector<mti::EncodedPayload> parts;
    parts.push_back(EncodeFor("leaf-a", "span-a"));
    parts.push_back(EncodeFor("leaf-b", "span-b"));
    parts.push_back(EncodeFor("leaf-c", "span-c"));

    const auto merged = mtw::ConcatenateTraceRequests(parts);

    const auto decoded = Decode(merged);
    ASSERT_EQ(decoded.size(), 3U);
    EXPECT_EQ(decoded[0], (std::pair<std::string, std::string>{"leaf-a", "span-a"}));
    EXPECT_EQ(decoded[1], (std::pair<std::string, std::string>{"leaf-b", "span-b"}));
    EXPECT_EQ(decoded[2], (std::pair<std::string, std::string>{"leaf-c", "span-c"}));
}

TEST(TraceRequestConcatTest, SizeIsTheSumOfThePartsAndBytesAreTheirConcatenation)
{
    auto a = EncodeFor("a", "x");
    auto b = EncodeFor("bb", "yy");
    std::vector<std::byte> expected(a.Bytes().begin(), a.Bytes().end());
    expected.insert(expected.end(), b.Bytes().begin(), b.Bytes().end());
    std::vector<mti::EncodedPayload> parts;
    parts.push_back(std::move(a));
    parts.push_back(std::move(b));

    const auto merged = mtw::ConcatenateTraceRequests(parts);

    EXPECT_EQ(std::vector<std::byte>(merged.Bytes().begin(), merged.Bytes().end()), expected);
}

TEST(TraceRequestConcatTest, ASinglePartIsReturnedUnchanged)
{
    auto only = EncodeFor("solo", "s");
    const auto* const original = only.Bytes().data();
    std::vector<mti::EncodedPayload> parts;
    parts.push_back(std::move(only));

    const auto merged = mtw::ConcatenateTraceRequests(parts);

    EXPECT_EQ(merged.Bytes().data(), original) << "one part needs no copy";
}

TEST(TraceRequestConcatTest, NoPartsGivesAnEmptyPayload)
{
    const auto merged = mtw::ConcatenateTraceRequests({});
    EXPECT_EQ(merged.Size(), 0U);
}

TEST(TraceRequestConcatTest, EmptyPartsContributeNothing)
{
    std::vector<mti::EncodedPayload> parts;
    parts.emplace_back();
    parts.push_back(EncodeFor("leaf-a", "span-a"));
    parts.emplace_back();

    const auto merged = mtw::ConcatenateTraceRequests(parts);

    ASSERT_EQ(Decode(merged).size(), 1U);
}

TEST(TraceRequestConcatTest, ASubspanJoinsOnlyItsRunOfTheBuffer)
{
    std::vector<mti::EncodedPayload> parts;
    parts.push_back(EncodeFor("leaf-a", "span-a"));
    parts.push_back(EncodeFor("leaf-b", "span-b"));
    parts.push_back(EncodeFor("leaf-c", "span-c"));
    parts.push_back(EncodeFor("leaf-d", "span-d"));

    const auto merged = mtw::ConcatenateTraceRequests(std::span{parts}.subspan(1, 2));

    const auto decoded = Decode(merged);
    ASSERT_EQ(decoded.size(), 2U);
    EXPECT_EQ(decoded[0], (std::pair<std::string, std::string>{"leaf-b", "span-b"}));
    EXPECT_EQ(decoded[1], (std::pair<std::string, std::string>{"leaf-c", "span-c"}));
}
