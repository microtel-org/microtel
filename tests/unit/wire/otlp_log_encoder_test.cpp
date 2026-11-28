// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0
//
// TDD tests for OtlpEncoder::Encode(LogBatchHandle) — encodes a
// LogBatchHandle into an OTLP ExportLogsServiceRequest and round-trips
// via upb to verify every field landed in the right proto slot.
//
// This file is the only test TU that includes the logs upb headers directly.
// The production restriction ("only otlp_encoder.cpp touches upb") applies to
// src/, not to tests/.

#include "wire/encoder/otlp_encoder.hpp"

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#include "opentelemetry/proto/collector/logs/v1/logs_service.upb.h"
#include "opentelemetry/proto/common/v1/common.upb.h"
#include "opentelemetry/proto/logs/v1/logs.upb.h"
#include "opentelemetry/proto/resource/v1/resource.upb.h"
#include "upb/mem/arena.h"
#pragma GCC diagnostic pop

#include "microtel/internal/log_batch.hpp"
#include "microtel/log_record.hpp"
#include "microtel/resource.hpp"

#include "fakes/failing_upb_alloc.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace mt = microtel;
namespace mti = microtel::internal;
namespace mtw = microtel::wire;

namespace
{

std::chrono::system_clock::time_point NsEpoch(std::uint64_t ns)
{
    return std::chrono::system_clock::time_point{std::chrono::nanoseconds{ns}};
}

// Round-trip helper: parse bytes into a ScopeLogs + ResourceLogs pair.
struct ParsedLogs
{
    const opentelemetry_proto_logs_v1_ScopeLogs* sl = nullptr;
    const opentelemetry_proto_logs_v1_ResourceLogs* rl = nullptr;
    upb_Arena* arena = nullptr;
};

ParsedLogs ParseFirst(const mti::EncodedPayload& payload)
{
    ParsedLogs result;
    result.arena = upb_Arena_New();
    const auto* req = opentelemetry_proto_collector_logs_v1_ExportLogsServiceRequest_parse(
        reinterpret_cast<const char*>(payload.Bytes().data()), payload.Size(), result.arena);
    if (req == nullptr)
    {
        return result;
    }
    std::size_t rl_count = 0;
    const opentelemetry_proto_logs_v1_ResourceLogs* const* rls =
        opentelemetry_proto_collector_logs_v1_ExportLogsServiceRequest_resource_logs(req,
                                                                                     &rl_count);
    if (rl_count == 0 || rls == nullptr)
    {
        return result;
    }
    result.rl = rls[0];
    std::size_t sl_count = 0;
    const opentelemetry_proto_logs_v1_ScopeLogs* const* sls =
        opentelemetry_proto_logs_v1_ResourceLogs_scope_logs(result.rl, &sl_count);
    if (sl_count > 0 && sls != nullptr)
    {
        result.sl = sls[0];
    }
    return result;
}

// Retrieve the first LogRecord from a ParsedLogs (may be nullptr).
const opentelemetry_proto_logs_v1_LogRecord* FirstRecord(const ParsedLogs& parsed)
{
    if (parsed.sl == nullptr)
    {
        return nullptr;
    }
    std::size_t count = 0;
    const opentelemetry_proto_logs_v1_LogRecord* const* recs =
        opentelemetry_proto_logs_v1_ScopeLogs_log_records(parsed.sl, &count);
    return (count > 0 && recs != nullptr) ? recs[0] : nullptr;
}

mti::LogBatchHandle MakeBatch(std::vector<mt::LogRecord> records,
                              const std::string& scope_name = "test.scope",
                              const std::string& scope_version = "1.0")
{
    return mti::LogBatchHandle{
        std::move(records),
        std::make_shared<mt::Resource>(),
        mti::InstrumentationScope{.name = scope_name, .version = scope_version},
    };
}

}  // namespace

// ── Empty batch → empty payload ───────────────────────────────────────────────

TEST(OtlpLogEncoderTest, EmptyBatch_ReturnsEmptyPayload)
{
    mtw::OtlpEncoder enc;
    const mti::EncodedPayload payload = enc.Encode(MakeBatch({}));
    EXPECT_EQ(payload.Size(), 0u);
}

// ── Arena allocation failure → empty payload ─────────────────────────────────

TEST(OtlpLogEncoderTest, ArenaAllocationFailure_ReturnsEmptyPayload)
{
    const mti::LogBatchHandle batch = MakeBatch({mt::LogRecord{.severity_text = "WARN"}});
    mtw::OtlpEncoder enc;

    mti::EncodedPayload payload;
    {
        const microtel::testing::ScopedFailingUpbAlloc fail_upb;
        payload = enc.Encode(batch);
    }

    EXPECT_EQ(payload.Size(), 0u);
}

// ── Severity number and text ──────────────────────────────────────────────────

TEST(OtlpLogEncoderTest, SeverityNumber_RoundTrip)
{
    mt::LogRecord rec = {
        .severity_number = mt::SeverityNumber::Error,
        .severity_text = "ERROR",
    };
    mtw::OtlpEncoder enc;
    const mti::EncodedPayload payload = enc.Encode(MakeBatch({std::move(rec)}));
    ASSERT_GT(payload.Size(), 0u);

    const ParsedLogs parsed = ParseFirst(payload);
    const auto* lr = FirstRecord(parsed);
    ASSERT_NE(lr, nullptr);
    EXPECT_EQ(opentelemetry_proto_logs_v1_LogRecord_severity_number(lr),
              static_cast<int32_t>(mt::SeverityNumber::Error));
    EXPECT_EQ(std::string(opentelemetry_proto_logs_v1_LogRecord_severity_text(lr).data,
                          opentelemetry_proto_logs_v1_LogRecord_severity_text(lr).size),
              "ERROR");
    upb_Arena_Free(parsed.arena);
}

// ── String body ───────────────────────────────────────────────────────────────

TEST(OtlpLogEncoderTest, StringBody_RoundTrip)
{
    mt::LogRecord rec = {
        .severity_number = mt::SeverityNumber::Info,
        .body = std::string{"hello from microtel"},
    };
    mtw::OtlpEncoder enc;
    const mti::EncodedPayload payload = enc.Encode(MakeBatch({std::move(rec)}));
    ASSERT_GT(payload.Size(), 0u);

    const ParsedLogs parsed = ParseFirst(payload);
    const auto* lr = FirstRecord(parsed);
    ASSERT_NE(lr, nullptr);
    ASSERT_TRUE(opentelemetry_proto_logs_v1_LogRecord_has_body(lr));
    const auto* body = opentelemetry_proto_logs_v1_LogRecord_body(lr);
    ASSERT_NE(body, nullptr);
    const upb_StringView sv = opentelemetry_proto_common_v1_AnyValue_string_value(body);
    EXPECT_EQ(std::string(sv.data, sv.size), "hello from microtel");
    upb_Arena_Free(parsed.arena);
}

// ── Integer body ──────────────────────────────────────────────────────────────

TEST(OtlpLogEncoderTest, IntBody_RoundTrip)
{
    mt::LogRecord rec = {
        .body = std::int64_t{42},
    };
    mtw::OtlpEncoder enc;
    const mti::EncodedPayload payload = enc.Encode(MakeBatch({std::move(rec)}));
    ASSERT_GT(payload.Size(), 0u);

    const ParsedLogs parsed = ParseFirst(payload);
    const auto* lr = FirstRecord(parsed);
    ASSERT_NE(lr, nullptr);
    ASSERT_TRUE(opentelemetry_proto_logs_v1_LogRecord_has_body(lr));
    const auto* body = opentelemetry_proto_logs_v1_LogRecord_body(lr);
    ASSERT_NE(body, nullptr);
    EXPECT_EQ(opentelemetry_proto_common_v1_AnyValue_int_value(body), 42);
    upb_Arena_Free(parsed.arena);
}

// ── Attributes ────────────────────────────────────────────────────────────────

TEST(OtlpLogEncoderTest, Attributes_RoundTrip)
{
    mt::LogRecord rec = {
        .attributes = {mt::KeyValue{.key = "service.name", .value = std::string{"svc-a"}},
                       mt::KeyValue{.key = "http.status_code", .value = std::int64_t{200}}},
    };
    mtw::OtlpEncoder enc;
    const mti::EncodedPayload payload = enc.Encode(MakeBatch({std::move(rec)}));
    ASSERT_GT(payload.Size(), 0u);

    const ParsedLogs parsed = ParseFirst(payload);
    const auto* lr = FirstRecord(parsed);
    ASSERT_NE(lr, nullptr);
    std::size_t attr_count = 0;
    const opentelemetry_proto_common_v1_KeyValue* const* attrs =
        opentelemetry_proto_logs_v1_LogRecord_attributes(lr, &attr_count);
    ASSERT_EQ(attr_count, 2u);
    const upb_StringView k0 = opentelemetry_proto_common_v1_KeyValue_key(attrs[0]);
    EXPECT_EQ(std::string(k0.data, k0.size), "service.name");
    upb_Arena_Free(parsed.arena);
}

// ── Time fields ───────────────────────────────────────────────────────────────

TEST(OtlpLogEncoderTest, TimeFields_RoundTrip)
{
    mt::LogRecord rec = {
        .time = NsEpoch(1'000'000'000ULL),
        .observed_time = NsEpoch(2'000'000'000ULL),
        .severity_number = mt::SeverityNumber::Debug,
    };
    mtw::OtlpEncoder enc;
    const mti::EncodedPayload payload = enc.Encode(MakeBatch({std::move(rec)}));
    ASSERT_GT(payload.Size(), 0u);

    const ParsedLogs parsed = ParseFirst(payload);
    const auto* lr = FirstRecord(parsed);
    ASSERT_NE(lr, nullptr);
    EXPECT_EQ(opentelemetry_proto_logs_v1_LogRecord_time_unix_nano(lr), 1'000'000'000ULL);
    EXPECT_EQ(opentelemetry_proto_logs_v1_LogRecord_observed_time_unix_nano(lr), 2'000'000'000ULL);
    upb_Arena_Free(parsed.arena);
}

// ── Trace correlation (trace_id / span_id / flags) ───────────────────────────

TEST(OtlpLogEncoderTest, TraceCorrelation_RoundTrip)
{
    const mt::TraceId::Bytes tid_bytes = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
    const mt::SpanId::Bytes sid_bytes = {0xA, 0xB, 0xC, 0xD, 0xE, 0xF, 0x01, 0x02};

    mt::LogRecord rec = {
        .trace_id = mt::TraceId{tid_bytes},
        .span_id = mt::SpanId{sid_bytes},
        .trace_flags = mt::TraceFlags{mt::TraceFlags::kSampled},
    };
    mtw::OtlpEncoder enc;
    const mti::EncodedPayload payload = enc.Encode(MakeBatch({std::move(rec)}));
    ASSERT_GT(payload.Size(), 0u);

    const ParsedLogs parsed = ParseFirst(payload);
    const auto* lr = FirstRecord(parsed);
    ASSERT_NE(lr, nullptr);

    const upb_StringView tid = opentelemetry_proto_logs_v1_LogRecord_trace_id(lr);
    ASSERT_EQ(tid.size, mt::TraceId::kSizeBytes);
    EXPECT_EQ(0, std::memcmp(tid.data, tid_bytes.data(), mt::TraceId::kSizeBytes));

    const upb_StringView sid = opentelemetry_proto_logs_v1_LogRecord_span_id(lr);
    ASSERT_EQ(sid.size, mt::SpanId::kSizeBytes);
    EXPECT_EQ(0, std::memcmp(sid.data, sid_bytes.data(), mt::SpanId::kSizeBytes));

    EXPECT_EQ(opentelemetry_proto_logs_v1_LogRecord_flags(lr),
              static_cast<uint32_t>(mt::TraceFlags::kSampled));
    upb_Arena_Free(parsed.arena);
}

// ── event_name ────────────────────────────────────────────────────────────────

TEST(OtlpLogEncoderTest, EventName_RoundTrip)
{
    mt::LogRecord rec = {
        .event_name = "exception",
    };
    mtw::OtlpEncoder enc;
    const mti::EncodedPayload payload = enc.Encode(MakeBatch({std::move(rec)}));
    ASSERT_GT(payload.Size(), 0u);

    const ParsedLogs parsed = ParseFirst(payload);
    const auto* lr = FirstRecord(parsed);
    ASSERT_NE(lr, nullptr);
    const upb_StringView ev = opentelemetry_proto_logs_v1_LogRecord_event_name(lr);
    EXPECT_EQ(std::string(ev.data, ev.size), "exception");
    upb_Arena_Free(parsed.arena);
}

// ── Scope name/version wired into ScopeLogs ──────────────────────────────────

TEST(OtlpLogEncoderTest, ScopeWired_RoundTrip)
{
    mt::LogRecord rec = {.severity_number = mt::SeverityNumber::Info};
    mtw::OtlpEncoder enc;
    const mti::EncodedPayload payload = enc.Encode(MakeBatch({std::move(rec)}, "my.lib", "2.0.1"));
    ASSERT_GT(payload.Size(), 0u);

    const ParsedLogs parsed = ParseFirst(payload);
    ASSERT_NE(parsed.sl, nullptr);
    const auto* scope = opentelemetry_proto_logs_v1_ScopeLogs_scope(parsed.sl);
    ASSERT_NE(scope, nullptr);
    const upb_StringView name = opentelemetry_proto_common_v1_InstrumentationScope_name(scope);
    EXPECT_EQ(std::string(name.data, name.size), "my.lib");
    const upb_StringView version =
        opentelemetry_proto_common_v1_InstrumentationScope_version(scope);
    EXPECT_EQ(std::string(version.data, version.size), "2.0.1");
    upb_Arena_Free(parsed.arena);
}

// ── Multiple log records in one batch ────────────────────────────────────────

TEST(OtlpLogEncoderTest, MultipleLogs_CountPreserved)
{
    std::vector<mt::LogRecord> recs;
    recs.push_back(
        mt::LogRecord{.severity_number = mt::SeverityNumber::Info, .body = std::string{"first"}});
    recs.push_back(
        mt::LogRecord{.severity_number = mt::SeverityNumber::Warn, .body = std::string{"second"}});
    recs.push_back(
        mt::LogRecord{.severity_number = mt::SeverityNumber::Error, .body = std::string{"third"}});
    mtw::OtlpEncoder enc;
    const mti::EncodedPayload payload = enc.Encode(MakeBatch(std::move(recs)));
    ASSERT_GT(payload.Size(), 0u);

    const ParsedLogs parsed = ParseFirst(payload);
    ASSERT_NE(parsed.sl, nullptr);
    std::size_t count = 0;
    (void)opentelemetry_proto_logs_v1_ScopeLogs_log_records(parsed.sl, &count);
    EXPECT_EQ(count, 3u);
    upb_Arena_Free(parsed.arena);
}

// ── dropped_attributes_count wired ───────────────────────────────────────────

TEST(OtlpLogEncoderTest, DroppedAttributesCount_RoundTrip)
{
    mt::LogRecord rec;
    rec.dropped_attributes_count = 3;
    mtw::OtlpEncoder enc;
    const mti::EncodedPayload payload = enc.Encode(MakeBatch({std::move(rec)}));
    ASSERT_GT(payload.Size(), 0u);

    const ParsedLogs parsed = ParseFirst(payload);
    const auto* lr = FirstRecord(parsed);
    ASSERT_NE(lr, nullptr);
    EXPECT_EQ(opentelemetry_proto_logs_v1_LogRecord_dropped_attributes_count(lr), 3u);
    upb_Arena_Free(parsed.arena);
}
