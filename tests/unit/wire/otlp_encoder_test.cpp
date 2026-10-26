// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0
//
// TDD tests for OtlpEncoder — encode a BatchHandle → round-trip parse with
// upb to assert every field landed in the right proto slot.
//
// This test is the only test file that includes the generated upb headers
// directly. The production restriction ("only otlp_encoder.cpp touches upb")
// applies to src/, not to tests/.

#include "wire/encoder/otlp_encoder.hpp"

// upb C headers use flexible array members — suppress pedantic warning.
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
#include "microtel/resource.hpp"
#include "microtel/trace.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace mt = microtel;
namespace mti = microtel::internal;
namespace mtw = microtel::wire;

// Helpers ------------------------------------------------------------------

static mt::TraceId MakeTraceId(std::uint8_t fill)
{
    mt::TraceId::Bytes b{};
    b.fill(fill);
    return mt::TraceId{b};
}

static mt::SpanId MakeSpanId(std::uint8_t fill)
{
    mt::SpanId::Bytes b{};
    b.fill(fill);
    return mt::SpanId{b};
}

static mt::SpanContext MakeCtx(std::uint8_t trace_fill, std::uint8_t span_fill)
{
    return mt::SpanContext{
        .trace_id = MakeTraceId(trace_fill),
        .span_id = MakeSpanId(span_fill),
        .trace_flags = mt::TraceFlags{mt::TraceFlags::kSampled},
    };
}

static std::chrono::system_clock::time_point NsEpoch(std::uint64_t ns)
{
    return std::chrono::system_clock::time_point{std::chrono::nanoseconds{ns}};
}

// Round-trip helper: parse encoded bytes back and return the first Span from
// the first ResourceSpans/ScopeSpans. Caller owns the arena.
struct ParsedSpan
{
    const opentelemetry_proto_trace_v1_Span* span = nullptr;
    const opentelemetry_proto_trace_v1_ResourceSpans* rs = nullptr;
    const opentelemetry_proto_trace_v1_ScopeSpans* ss = nullptr;
    upb_Arena* arena = nullptr;
};

static ParsedSpan ParseFirst(const mti::EncodedPayload& payload)
{
    ParsedSpan out{};
    out.arena = upb_Arena_New();

    // NOSONAR cpp:S3630 — std::byte* → char* is a well-defined character-type alias
    const char* const buf = reinterpret_cast<const char*>(payload.Bytes().data());
    const std::size_t size = payload.Size();

    auto* req = opentelemetry_proto_collector_trace_v1_ExportTraceServiceRequest_parse(
        buf, size, out.arena);
    if (req == nullptr)
    {
        return out;
    }

    std::size_t rs_count = 0;
    const opentelemetry_proto_trace_v1_ResourceSpans* const* rs_arr =
        opentelemetry_proto_collector_trace_v1_ExportTraceServiceRequest_resource_spans(req,
                                                                                        &rs_count);
    if (rs_count == 0 || rs_arr == nullptr)
    {
        return out;
    }
    out.rs = rs_arr[0];

    std::size_t ss_count = 0;
    const opentelemetry_proto_trace_v1_ScopeSpans* const* ss_arr =
        opentelemetry_proto_trace_v1_ResourceSpans_scope_spans(rs_arr[0], &ss_count);
    if (ss_count == 0 || ss_arr == nullptr)
    {
        return out;
    }
    out.ss = ss_arr[0];

    std::size_t span_count = 0;
    const opentelemetry_proto_trace_v1_Span* const* spans =
        opentelemetry_proto_trace_v1_ScopeSpans_spans(ss_arr[0], &span_count);
    if (span_count == 0 || spans == nullptr)
    {
        return out;
    }
    out.span = spans[0];
    return out;
}

static std::string SvStr(upb_StringView sv)
{
    return {sv.data, sv.size};
}

// --------------------------------------------------------------------------
// Tests
// --------------------------------------------------------------------------

class OtlpEncoderTest : public ::testing::Test
{
protected:
    void TearDown() override
    {
        if (m_arena != nullptr)
        {
            upb_Arena_Free(m_arena);
            m_arena = nullptr;
        }
    }

    mti::EncodedPayload Encode(mti::BatchHandle batch)
    {
        return m_encoder.Encode(batch);
    }

    // Build a minimal batch with one span having a given context and name.
    static mti::BatchHandle MinimalBatch(mt::SpanContext ctx,
                                         const std::string& name = "op",
                                         mt::SpanContext parent = {})
    {
        mti::SpanRecord rec{
            .context = std::move(ctx),
            .parent_context = std::move(parent),
            .name = name,
            .kind = mt::SpanKind::Internal,
            .status_code = mt::StatusCode::Unset,
            .start_time = NsEpoch(1'000'000'000ULL),
            .end_time = NsEpoch(2'000'000'000ULL),
        };
        auto resource = std::make_shared<const mt::Resource>();
        mti::InstrumentationScope scope{.name = "test_lib", .version = "1.0"};
        std::vector<mti::SpanRecord> records;
        records.push_back(std::move(rec));
        return mti::BatchHandle{std::move(records), std::move(resource), std::move(scope)};
    }

    mtw::OtlpEncoder m_encoder;
    upb_Arena* m_arena = nullptr;
};

// --- Basic smoke ---

TEST_F(OtlpEncoderTest, MinimalBatch_ProducesNonEmptyPayload)
{
    const auto payload = Encode(MinimalBatch(MakeCtx(0xAA, 0xBB)));
    EXPECT_GT(payload.Size(), 0U);
}

TEST_F(OtlpEncoderTest, EmptySpanList_ProducesEmptyPayload)
{
    auto resource = std::make_shared<const mt::Resource>();
    mti::InstrumentationScope scope{.name = "lib", .version = "0.1"};
    mti::BatchHandle empty{std::vector<mti::SpanRecord>{}, std::move(resource), std::move(scope)};

    const auto payload = Encode(std::move(empty));
    EXPECT_EQ(payload.Size(), 0U);
}

// --- Span identity fields ---

TEST_F(OtlpEncoderTest, TraceId_RoundTrips)
{
    const auto ctx = MakeCtx(0x11, 0x22);
    const auto payload = Encode(MinimalBatch(ctx));
    auto parsed = ParseFirst(payload);
    m_arena = parsed.arena;
    ASSERT_NE(parsed.span, nullptr);

    const auto tid = opentelemetry_proto_trace_v1_Span_trace_id(parsed.span);
    ASSERT_EQ(tid.size, mt::TraceId::kSizeBytes);
    const auto& expected = ctx.trace_id.AsBytes();
    EXPECT_EQ(std::memcmp(tid.data, expected.data(), expected.size()), 0);
}

TEST_F(OtlpEncoderTest, SpanId_RoundTrips)
{
    const auto ctx = MakeCtx(0x33, 0x44);
    const auto payload = Encode(MinimalBatch(ctx));
    auto parsed = ParseFirst(payload);
    m_arena = parsed.arena;
    ASSERT_NE(parsed.span, nullptr);

    const auto sid = opentelemetry_proto_trace_v1_Span_span_id(parsed.span);
    ASSERT_EQ(sid.size, mt::SpanId::kSizeBytes);
    const auto& expected = ctx.span_id.AsBytes();
    EXPECT_EQ(std::memcmp(sid.data, expected.data(), expected.size()), 0);
}

TEST_F(OtlpEncoderTest, ParentSpanId_EncodedWhenValid)
{
    const auto ctx = MakeCtx(0x01, 0x02);
    const auto parent = MakeCtx(0x01, 0xAB);
    const auto payload = Encode(MinimalBatch(ctx, "child", parent));
    auto parsed = ParseFirst(payload);
    m_arena = parsed.arena;
    ASSERT_NE(parsed.span, nullptr);

    const auto psid = opentelemetry_proto_trace_v1_Span_parent_span_id(parsed.span);
    ASSERT_EQ(psid.size, mt::SpanId::kSizeBytes);
    const auto& expected = parent.span_id.AsBytes();
    EXPECT_EQ(std::memcmp(psid.data, expected.data(), expected.size()), 0);
}

TEST_F(OtlpEncoderTest, ParentSpanId_ZeroWhenRootSpan)
{
    const auto ctx = MakeCtx(0x55, 0x66);
    const auto payload = Encode(MinimalBatch(ctx));
    auto parsed = ParseFirst(payload);
    m_arena = parsed.arena;
    ASSERT_NE(parsed.span, nullptr);

    // Root span: encoder does not set parent_span_id → upb returns empty StringView.
    const auto psid = opentelemetry_proto_trace_v1_Span_parent_span_id(parsed.span);
    EXPECT_EQ(psid.size, 0U);
}

TEST_F(OtlpEncoderTest, SpanName_RoundTrips)
{
    const auto ctx = MakeCtx(0x77, 0x88);
    const auto payload = Encode(MinimalBatch(ctx, "my_operation"));
    auto parsed = ParseFirst(payload);
    m_arena = parsed.arena;
    ASSERT_NE(parsed.span, nullptr);

    EXPECT_EQ(SvStr(opentelemetry_proto_trace_v1_Span_name(parsed.span)), "my_operation");
}

TEST_F(OtlpEncoderTest, SpanKind_ClientMapsToOtlpClient)
{
    const mt::SpanContext ctx = MakeCtx(0x01, 0x02);
    mti::SpanRecord rec{.context = ctx,
                        .name = "rpc",
                        .kind = mt::SpanKind::Client,
                        .start_time = NsEpoch(1),
                        .end_time = NsEpoch(2)};
    auto resource = std::make_shared<const mt::Resource>();
    mti::InstrumentationScope scope{.name = "lib", .version = "1"};
    std::vector<mti::SpanRecord> records;
    records.push_back(std::move(rec));
    mti::BatchHandle batch{std::move(records), std::move(resource), std::move(scope)};

    const auto payload = Encode(std::move(batch));
    auto parsed = ParseFirst(payload);
    m_arena = parsed.arena;
    ASSERT_NE(parsed.span, nullptr);

    EXPECT_EQ(opentelemetry_proto_trace_v1_Span_kind(parsed.span),
              opentelemetry_proto_trace_v1_Span_SPAN_KIND_CLIENT);
}

TEST_F(OtlpEncoderTest, Timestamps_RoundTrip)
{
    constexpr std::uint64_t kStart = 1'000'000'000ULL;
    constexpr std::uint64_t kEnd = 2'000'000'000ULL;

    const mt::SpanContext ctx = MakeCtx(0x01, 0x02);
    mti::SpanRecord rec{
        .context = ctx, .name = "ts", .start_time = NsEpoch(kStart), .end_time = NsEpoch(kEnd)};
    auto resource = std::make_shared<const mt::Resource>();
    mti::InstrumentationScope scope{.name = "lib", .version = "1"};
    std::vector<mti::SpanRecord> records;
    records.push_back(std::move(rec));
    mti::BatchHandle batch{std::move(records), std::move(resource), std::move(scope)};

    const auto payload = Encode(std::move(batch));
    auto parsed = ParseFirst(payload);
    m_arena = parsed.arena;
    ASSERT_NE(parsed.span, nullptr);

    EXPECT_EQ(opentelemetry_proto_trace_v1_Span_start_time_unix_nano(parsed.span), kStart);
    EXPECT_EQ(opentelemetry_proto_trace_v1_Span_end_time_unix_nano(parsed.span), kEnd);
}

// --- Status ---

TEST_F(OtlpEncoderTest, StatusError_RoundTrips)
{
    const mt::SpanContext ctx = MakeCtx(0x01, 0x02);
    mti::SpanRecord rec{.context = ctx,
                        .name = "fail",
                        .status_code = mt::StatusCode::Error,
                        .status_description = "something went wrong",
                        .start_time = NsEpoch(1),
                        .end_time = NsEpoch(2)};
    auto resource = std::make_shared<const mt::Resource>();
    mti::InstrumentationScope scope{.name = "lib", .version = "1"};
    std::vector<mti::SpanRecord> records;
    records.push_back(std::move(rec));
    mti::BatchHandle batch{std::move(records), std::move(resource), std::move(scope)};

    const auto payload = Encode(std::move(batch));
    auto parsed = ParseFirst(payload);
    m_arena = parsed.arena;
    ASSERT_NE(parsed.span, nullptr);

    const auto* status = opentelemetry_proto_trace_v1_Span_status(parsed.span);
    ASSERT_NE(status, nullptr);
    EXPECT_EQ(opentelemetry_proto_trace_v1_Status_code(status),
              opentelemetry_proto_trace_v1_Status_STATUS_CODE_ERROR);
    EXPECT_EQ(SvStr(opentelemetry_proto_trace_v1_Status_message(status)), "something went wrong");
}

// --- Attributes ---

TEST_F(OtlpEncoderTest, StringAttribute_RoundTrips)
{
    const mt::SpanContext ctx = MakeCtx(0x01, 0x02);
    mti::SpanRecord rec{.context = ctx,
                        .name = "attr_test",
                        .start_time = NsEpoch(1),
                        .end_time = NsEpoch(2),
                        .attributes = {{"http.method", std::string{"GET"}}}};
    auto resource = std::make_shared<const mt::Resource>();
    mti::InstrumentationScope scope{.name = "lib", .version = "1"};
    std::vector<mti::SpanRecord> records;
    records.push_back(std::move(rec));
    mti::BatchHandle batch{std::move(records), std::move(resource), std::move(scope)};

    const auto payload = Encode(std::move(batch));
    auto parsed = ParseFirst(payload);
    m_arena = parsed.arena;
    ASSERT_NE(parsed.span, nullptr);

    std::size_t count = 0;
    const auto* const* attrs = opentelemetry_proto_trace_v1_Span_attributes(parsed.span, &count);
    ASSERT_EQ(count, 1U);
    EXPECT_EQ(SvStr(opentelemetry_proto_common_v1_KeyValue_key(attrs[0])), "http.method");
    const auto* val = opentelemetry_proto_common_v1_KeyValue_value(attrs[0]);
    ASSERT_NE(val, nullptr);
    EXPECT_EQ(SvStr(opentelemetry_proto_common_v1_AnyValue_string_value(val)), "GET");
}

TEST_F(OtlpEncoderTest, BoolAttribute_RoundTrips)
{
    const mt::SpanContext ctx = MakeCtx(0x01, 0x02);
    mti::SpanRecord rec{.context = ctx,
                        .name = "bool_attr",
                        .start_time = NsEpoch(1),
                        .end_time = NsEpoch(2),
                        .attributes = {{"sampled", true}}};
    auto resource = std::make_shared<const mt::Resource>();
    mti::InstrumentationScope scope{.name = "lib", .version = "1"};
    std::vector<mti::SpanRecord> records;
    records.push_back(std::move(rec));
    mti::BatchHandle batch{std::move(records), std::move(resource), std::move(scope)};

    const auto payload = Encode(std::move(batch));
    auto parsed = ParseFirst(payload);
    m_arena = parsed.arena;
    ASSERT_NE(parsed.span, nullptr);

    std::size_t count = 0;
    const auto* const* attrs = opentelemetry_proto_trace_v1_Span_attributes(parsed.span, &count);
    ASSERT_EQ(count, 1U);
    const auto* val = opentelemetry_proto_common_v1_KeyValue_value(attrs[0]);
    ASSERT_NE(val, nullptr);
    EXPECT_TRUE(opentelemetry_proto_common_v1_AnyValue_bool_value(val));
}

TEST_F(OtlpEncoderTest, Int64Attribute_RoundTrips)
{
    const mt::SpanContext ctx = MakeCtx(0x01, 0x02);
    mti::SpanRecord rec{.context = ctx,
                        .name = "int_attr",
                        .start_time = NsEpoch(1),
                        .end_time = NsEpoch(2),
                        .attributes = {{"http.status_code", std::int64_t{200}}}};
    auto resource = std::make_shared<const mt::Resource>();
    mti::InstrumentationScope scope{.name = "lib", .version = "1"};
    std::vector<mti::SpanRecord> records;
    records.push_back(std::move(rec));
    mti::BatchHandle batch{std::move(records), std::move(resource), std::move(scope)};

    const auto payload = Encode(std::move(batch));
    auto parsed = ParseFirst(payload);
    m_arena = parsed.arena;
    ASSERT_NE(parsed.span, nullptr);

    std::size_t count = 0;
    const auto* const* attrs = opentelemetry_proto_trace_v1_Span_attributes(parsed.span, &count);
    ASSERT_EQ(count, 1U);
    const auto* val = opentelemetry_proto_common_v1_KeyValue_value(attrs[0]);
    ASSERT_NE(val, nullptr);
    EXPECT_EQ(opentelemetry_proto_common_v1_AnyValue_int_value(val), 200);
}

TEST_F(OtlpEncoderTest, DoubleAttribute_RoundTrips)
{
    const mt::SpanContext ctx = MakeCtx(0x01, 0x02);
    mti::SpanRecord rec{.context = ctx,
                        .name = "dbl_attr",
                        .start_time = NsEpoch(1),
                        .end_time = NsEpoch(2),
                        .attributes = {{"latency_ms", 3.14}}};
    auto resource = std::make_shared<const mt::Resource>();
    mti::InstrumentationScope scope{.name = "lib", .version = "1"};
    std::vector<mti::SpanRecord> records;
    records.push_back(std::move(rec));
    mti::BatchHandle batch{std::move(records), std::move(resource), std::move(scope)};

    const auto payload = Encode(std::move(batch));
    auto parsed = ParseFirst(payload);
    m_arena = parsed.arena;
    ASSERT_NE(parsed.span, nullptr);

    std::size_t count = 0;
    const auto* const* attrs = opentelemetry_proto_trace_v1_Span_attributes(parsed.span, &count);
    ASSERT_EQ(count, 1U);
    const auto* val = opentelemetry_proto_common_v1_KeyValue_value(attrs[0]);
    ASSERT_NE(val, nullptr);
    EXPECT_DOUBLE_EQ(opentelemetry_proto_common_v1_AnyValue_double_value(val), 3.14);
}

// --- Resource attributes ---

TEST_F(OtlpEncoderTest, ResourceAttributes_Encoded)
{
    const mt::SpanContext ctx = MakeCtx(0x01, 0x02);
    mti::SpanRecord rec{
        .context = ctx, .name = "op", .start_time = NsEpoch(1), .end_time = NsEpoch(2)};
    auto resource = std::make_shared<const mt::Resource>(
        std::vector<mt::KeyValue>{{"service.name", std::string{"my-service"}}});
    mti::InstrumentationScope scope{.name = "lib", .version = "1"};
    std::vector<mti::SpanRecord> records;
    records.push_back(std::move(rec));
    mti::BatchHandle batch{std::move(records), std::move(resource), std::move(scope)};

    const auto payload = Encode(std::move(batch));
    auto parsed = ParseFirst(payload);
    m_arena = parsed.arena;
    ASSERT_NE(parsed.rs, nullptr);

    const auto* res = opentelemetry_proto_trace_v1_ResourceSpans_resource(parsed.rs);
    ASSERT_NE(res, nullptr);

    std::size_t count = 0;
    const auto* const* attrs = opentelemetry_proto_resource_v1_Resource_attributes(res, &count);
    ASSERT_EQ(count, 1U);
    EXPECT_EQ(SvStr(opentelemetry_proto_common_v1_KeyValue_key(attrs[0])), "service.name");
    const auto* val = opentelemetry_proto_common_v1_KeyValue_value(attrs[0]);
    ASSERT_NE(val, nullptr);
    EXPECT_EQ(SvStr(opentelemetry_proto_common_v1_AnyValue_string_value(val)), "my-service");
}

// --- Instrumentation scope ---

TEST_F(OtlpEncoderTest, ScopeNameAndVersion_Encoded)
{
    const mt::SpanContext ctx = MakeCtx(0x01, 0x02);
    mti::SpanRecord rec{
        .context = ctx, .name = "op", .start_time = NsEpoch(1), .end_time = NsEpoch(2)};
    auto resource = std::make_shared<const mt::Resource>();
    mti::InstrumentationScope scope{.name = "opentelemetry_sdk", .version = "0.9.0"};
    std::vector<mti::SpanRecord> records;
    records.push_back(std::move(rec));
    mti::BatchHandle batch{std::move(records), std::move(resource), std::move(scope)};

    const auto payload = Encode(std::move(batch));
    auto parsed = ParseFirst(payload);
    m_arena = parsed.arena;
    ASSERT_NE(parsed.ss, nullptr);

    const auto* sc = opentelemetry_proto_trace_v1_ScopeSpans_scope(parsed.ss);
    ASSERT_NE(sc, nullptr);
    EXPECT_EQ(SvStr(opentelemetry_proto_common_v1_InstrumentationScope_name(sc)),
              "opentelemetry_sdk");
    EXPECT_EQ(SvStr(opentelemetry_proto_common_v1_InstrumentationScope_version(sc)), "0.9.0");
}

// --- Events ---

TEST_F(OtlpEncoderTest, SpanEvent_RoundTrips)
{
    const mt::SpanContext ctx = MakeCtx(0x01, 0x02);
    mti::SpanEvent ev;
    ev.name = "exception";
    ev.timestamp = NsEpoch(1'500'000'000ULL);
    ev.attributes.push_back({"exception.type", std::string{"RuntimeError"}});

    mti::SpanRecord rec{.context = ctx,
                        .name = "op",
                        .start_time = NsEpoch(1'000'000'000ULL),
                        .end_time = NsEpoch(2'000'000'000ULL)};
    rec.events.push_back(std::move(ev));

    auto resource = std::make_shared<const mt::Resource>();
    mti::InstrumentationScope scope{.name = "lib", .version = "1"};
    std::vector<mti::SpanRecord> records;
    records.push_back(std::move(rec));
    mti::BatchHandle batch{std::move(records), std::move(resource), std::move(scope)};

    const auto payload = Encode(std::move(batch));
    auto parsed = ParseFirst(payload);
    m_arena = parsed.arena;
    ASSERT_NE(parsed.span, nullptr);

    std::size_t ev_count = 0;
    const auto* const* events = opentelemetry_proto_trace_v1_Span_events(parsed.span, &ev_count);
    ASSERT_EQ(ev_count, 1U);
    EXPECT_EQ(SvStr(opentelemetry_proto_trace_v1_Span_Event_name(events[0])), "exception");
    EXPECT_EQ(opentelemetry_proto_trace_v1_Span_Event_time_unix_nano(events[0]), 1'500'000'000ULL);

    std::size_t attr_count = 0;
    const auto* const* ev_attrs =
        opentelemetry_proto_trace_v1_Span_Event_attributes(events[0], &attr_count);
    ASSERT_EQ(attr_count, 1U);
    EXPECT_EQ(SvStr(opentelemetry_proto_common_v1_KeyValue_key(ev_attrs[0])), "exception.type");
}

// --- Links ---

TEST_F(OtlpEncoderTest, SpanLink_RoundTrips)
{
    const mt::SpanContext ctx = MakeCtx(0x01, 0x02);
    const mt::SpanContext linked_ctx = MakeCtx(0xAB, 0xCD);

    mti::SpanLink lnk;
    lnk.linked_context = linked_ctx;
    lnk.attributes.push_back({"link.type", std::string{"follows_from"}});

    mti::SpanRecord rec{
        .context = ctx, .name = "op", .start_time = NsEpoch(1), .end_time = NsEpoch(2)};
    rec.links.push_back(std::move(lnk));

    auto resource = std::make_shared<const mt::Resource>();
    mti::InstrumentationScope scope{.name = "lib", .version = "1"};
    std::vector<mti::SpanRecord> records;
    records.push_back(std::move(rec));
    mti::BatchHandle batch{std::move(records), std::move(resource), std::move(scope)};

    const auto payload = Encode(std::move(batch));
    auto parsed = ParseFirst(payload);
    m_arena = parsed.arena;
    ASSERT_NE(parsed.span, nullptr);

    std::size_t lnk_count = 0;
    const auto* const* links = opentelemetry_proto_trace_v1_Span_links(parsed.span, &lnk_count);
    ASSERT_EQ(lnk_count, 1U);

    const auto tid = opentelemetry_proto_trace_v1_Span_Link_trace_id(links[0]);
    ASSERT_EQ(tid.size, mt::TraceId::kSizeBytes);
    const auto& expected_tid = linked_ctx.trace_id.AsBytes();
    EXPECT_EQ(std::memcmp(tid.data, expected_tid.data(), expected_tid.size()), 0);

    const auto sid = opentelemetry_proto_trace_v1_Span_Link_span_id(links[0]);
    ASSERT_EQ(sid.size, mt::SpanId::kSizeBytes);
    const auto& expected_sid = linked_ctx.span_id.AsBytes();
    EXPECT_EQ(std::memcmp(sid.data, expected_sid.data(), expected_sid.size()), 0);
}

// ── trace_state and flags (issue #451) ───────────────────────────────────────

namespace
{

constexpr std::uint32_t kHasIsRemote = 0x100U;  // SPAN_FLAGS_CONTEXT_HAS_IS_REMOTE_MASK
constexpr std::uint32_t kIsRemote = 0x200U;     // SPAN_FLAGS_CONTEXT_IS_REMOTE_MASK
constexpr std::uint32_t kSampledBit = 0x01U;

}  // namespace

TEST_F(OtlpEncoderTest, SpanTraceStateAndFlags_RemoteParent)
{
    mt::SpanContext ctx = MakeCtx(0x01, 0x02);
    ctx.trace_state = mt::TraceState::FromHeader("vendor=abc,other=1");
    mt::SpanContext parent = MakeCtx(0x01, 0x03);
    parent.remote = true;

    const auto payload = Encode(MinimalBatch(ctx, "op", parent));
    auto parsed = ParseFirst(payload);
    m_arena = parsed.arena;
    ASSERT_NE(parsed.span, nullptr);

    EXPECT_EQ(SvStr(opentelemetry_proto_trace_v1_Span_trace_state(parsed.span)),
              "vendor=abc,other=1");
    EXPECT_EQ(opentelemetry_proto_trace_v1_Span_flags(parsed.span),
              kSampledBit | kHasIsRemote | kIsRemote);
}

TEST_F(OtlpEncoderTest, SpanFlags_RootSpanParentIsKnownNotRemote)
{
    const auto payload = Encode(MinimalBatch(MakeCtx(0x01, 0x02)));
    auto parsed = ParseFirst(payload);
    m_arena = parsed.arena;
    ASSERT_NE(parsed.span, nullptr);

    EXPECT_EQ(opentelemetry_proto_trace_v1_Span_trace_state(parsed.span).size, 0U);
    EXPECT_EQ(opentelemetry_proto_trace_v1_Span_flags(parsed.span), kSampledBit | kHasIsRemote);
}

TEST_F(OtlpEncoderTest, SpanFlags_UnsampledWithLocalParent)
{
    mt::SpanContext ctx = MakeCtx(0x01, 0x02);
    ctx.trace_flags = mt::TraceFlags{};
    mt::SpanContext parent = MakeCtx(0x01, 0x03);
    parent.trace_flags = mt::TraceFlags{};

    const auto payload = Encode(MinimalBatch(ctx, "op", parent));
    auto parsed = ParseFirst(payload);
    m_arena = parsed.arena;
    ASSERT_NE(parsed.span, nullptr);

    EXPECT_EQ(opentelemetry_proto_trace_v1_Span_flags(parsed.span), kHasIsRemote);
}

TEST_F(OtlpEncoderTest, SpanLinkTraceStateAndFlags_RemoteLinkedContext)
{
    mt::SpanContext linked_ctx = MakeCtx(0xAB, 0xCD);
    linked_ctx.trace_state = mt::TraceState::FromHeader("linked=1");
    linked_ctx.remote = true;

    mti::SpanLink lnk;
    lnk.linked_context = linked_ctx;
    mti::SpanRecord rec{.context = MakeCtx(0x01, 0x02),
                        .name = "op",
                        .start_time = NsEpoch(1),
                        .end_time = NsEpoch(2)};
    rec.links.push_back(std::move(lnk));
    std::vector<mti::SpanRecord> records;
    records.push_back(std::move(rec));
    mti::BatchHandle batch{std::move(records),
                           std::make_shared<const mt::Resource>(),
                           mti::InstrumentationScope{.name = "lib", .version = "1"}};

    const auto payload = Encode(std::move(batch));
    auto parsed = ParseFirst(payload);
    m_arena = parsed.arena;
    ASSERT_NE(parsed.span, nullptr);

    std::size_t lnk_count = 0;
    const auto* const* links = opentelemetry_proto_trace_v1_Span_links(parsed.span, &lnk_count);
    ASSERT_EQ(lnk_count, 1U);
    EXPECT_EQ(SvStr(opentelemetry_proto_trace_v1_Span_Link_trace_state(links[0])), "linked=1");
    EXPECT_EQ(opentelemetry_proto_trace_v1_Span_Link_flags(links[0]),
              kSampledBit | kHasIsRemote | kIsRemote);
}
