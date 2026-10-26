// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0
//
// LOCKED: upb headers are confined to src/wire/encoder/. This file and its
// sibling upb_arena.hpp are the only two that #include them; no upb type
// appears in any header outside this directory, which is the invariant the
// rule protects (CLAUDE.md rule 13, docs/interfaces.md §4.2,
// docs/memory-model.md §3.1). The wrapper cannot live in src/common/raii/
// precisely because including it would mean including upb.
//
// The wording changed from "the only file" when upb_arena.hpp was added; the
// containment guarantee did not.

#include "otlp_encoder.hpp"

#include "upb_arena.hpp"

// upb C headers use flexible array members (C99/C11, not ISO C++).
// Suppress the pedantic warning for this include block only.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#include "opentelemetry/proto/collector/logs/v1/logs_service.upb.h"
#include "opentelemetry/proto/collector/metrics/v1/metrics_service.upb.h"
#include "opentelemetry/proto/collector/trace/v1/trace_service.upb.h"
#include "opentelemetry/proto/common/v1/common.upb.h"
#include "opentelemetry/proto/logs/v1/logs.upb.h"
#include "opentelemetry/proto/metrics/v1/metrics.upb.h"
#include "opentelemetry/proto/resource/v1/resource.upb.h"
#include "opentelemetry/proto/trace/v1/trace.upb.h"
#include "upb/mem/arena.h"
#pragma GCC diagnostic pop

// microtel public + internal headers
#include "microtel/attribute.hpp"
#include "microtel/internal/batch.hpp"
#include "microtel/internal/encoded_payload.hpp"
#include "microtel/internal/log_batch.hpp"
#include "microtel/internal/metric_batch.hpp"
#include "microtel/log_record.hpp"
#include "microtel/resource.hpp"
#include "microtel/trace.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <memory>
#include <string>
#include <variant>

namespace microtel::wire
{

namespace
{

// ---------------------------------------------------------------------------
// Type aliases — shorten the upb generated names inside this TU only.
// ---------------------------------------------------------------------------

using UpbKV = opentelemetry_proto_common_v1_KeyValue;
using UpbAnyValue = opentelemetry_proto_common_v1_AnyValue;
using UpbArrayVal = opentelemetry_proto_common_v1_ArrayValue;
using UpbScope = opentelemetry_proto_common_v1_InstrumentationScope;
using UpbResource = opentelemetry_proto_resource_v1_Resource;
using UpbResSp = opentelemetry_proto_trace_v1_ResourceSpans;
using UpbScopeSp = opentelemetry_proto_trace_v1_ScopeSpans;
using UpbSpan = opentelemetry_proto_trace_v1_Span;
using UpbEvent = opentelemetry_proto_trace_v1_Span_Event;
using UpbLink = opentelemetry_proto_trace_v1_Span_Link;
using UpbStatus = opentelemetry_proto_trace_v1_Status;
using UpbRequest = opentelemetry_proto_collector_trace_v1_ExportTraceServiceRequest;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

upb_StringView Sv(const std::string& s) noexcept
{
    return upb_StringView_FromDataAndSize(s.data(), s.size());
}

// Takes const void* so callers pass uint8_t* implicitly; static_cast to char*
// is valid from void* (avoids reinterpret_cast of unrelated pointer types).
upb_StringView SvBytes(const void* data, std::size_t size) noexcept
{
    return upb_StringView_FromDataAndSize(static_cast<const char*>(data), size);
}

// A view of a copy of @p s in @p arena, for strings that do not outlive the
// call, such as `TraceState::ToHeader()`. Empty if the arena is exhausted.
upb_StringView ArenaSv(const std::string& s, upb_Arena* arena) noexcept
{
    char* const buf = static_cast<char*>(upb_Arena_Malloc(arena, s.size()));
    if (buf == nullptr)
    {
        return upb_StringView_FromDataAndSize(nullptr, 0);
    }
    std::copy_n(s.data(), s.size(), buf);
    return upb_StringView_FromDataAndSize(buf, s.size());
}

// OTLP `Span.flags` / `Span.Link.flags`: the W3C trace flags in bits 0-7, and
// whether the parent (for a span) or the linked context (for a link) is remote,
// which microtel always knows.
std::uint32_t SpanFlags(microtel::TraceFlags flags, bool remote) noexcept
{
    std::uint32_t bits = static_cast<std::uint32_t>(flags.AsByte()) |
                         static_cast<std::uint32_t>(
                             opentelemetry_proto_trace_v1_SPAN_FLAGS_CONTEXT_HAS_IS_REMOTE_MASK);
    if (remote)
    {
        bits |= static_cast<std::uint32_t>(
            opentelemetry_proto_trace_v1_SPAN_FLAGS_CONTEXT_IS_REMOTE_MASK);
    }
    return bits;
}

std::uint64_t ToNanos(std::chrono::system_clock::time_point tp) noexcept
{
    return static_cast<std::uint64_t>(
        std::chrono::time_point_cast<std::chrono::nanoseconds>(tp).time_since_epoch().count());
}

// SpanKind: microtel values are Internal=0..Consumer=4; OTLP proto is
// UNSPECIFIED=0, INTERNAL=1 .. CONSUMER=5 — add 1 to remap.
int32_t MapKind(microtel::SpanKind kind) noexcept
{
    return static_cast<int32_t>(kind) + 1;
}

// StatusCode: both enums use 0=Unset, 1=Ok, 2=Error — direct cast is safe.
int32_t MapStatusCode(microtel::StatusCode code) noexcept
{
    return static_cast<int32_t>(code);
}

// ---------------------------------------------------------------------------
// Attribute encoding
// ---------------------------------------------------------------------------

void EncodeAnyValueStr(UpbAnyValue* av, const std::string& s)
{
    opentelemetry_proto_common_v1_AnyValue_set_string_value(av, Sv(s));
}

void EncodeAnyValueBool(UpbAnyValue* av, bool v)
{
    opentelemetry_proto_common_v1_AnyValue_set_bool_value(av, v);
}

void EncodeAnyValueInt(UpbAnyValue* av, std::int64_t v)
{
    opentelemetry_proto_common_v1_AnyValue_set_int_value(av, v);
}

void EncodeAnyValueDouble(UpbAnyValue* av, double v)
{
    opentelemetry_proto_common_v1_AnyValue_set_double_value(av, v);
}

void EncodeArrayBool(UpbAnyValue* av, const std::vector<bool>& vec, upb_Arena* arena)
{
    UpbArrayVal* arr = opentelemetry_proto_common_v1_AnyValue_mutable_array_value(av, arena);
    for (const bool elem : vec)
    {
        UpbAnyValue* item = opentelemetry_proto_common_v1_ArrayValue_add_values(arr, arena);
        if (item != nullptr)
        {
            opentelemetry_proto_common_v1_AnyValue_set_bool_value(item, elem);
        }
    }
}

void EncodeArrayInt(UpbAnyValue* av, const std::vector<std::int64_t>& vec, upb_Arena* arena)
{
    UpbArrayVal* arr = opentelemetry_proto_common_v1_AnyValue_mutable_array_value(av, arena);
    for (const std::int64_t elem : vec)
    {
        UpbAnyValue* item = opentelemetry_proto_common_v1_ArrayValue_add_values(arr, arena);
        if (item != nullptr)
        {
            opentelemetry_proto_common_v1_AnyValue_set_int_value(item, elem);
        }
    }
}

void EncodeArrayDouble(UpbAnyValue* av, const std::vector<double>& vec, upb_Arena* arena)
{
    UpbArrayVal* arr = opentelemetry_proto_common_v1_AnyValue_mutable_array_value(av, arena);
    for (const double elem : vec)
    {
        UpbAnyValue* item = opentelemetry_proto_common_v1_ArrayValue_add_values(arr, arena);
        if (item != nullptr)
        {
            opentelemetry_proto_common_v1_AnyValue_set_double_value(item, elem);
        }
    }
}

void EncodeArrayStr(UpbAnyValue* av, const std::vector<std::string>& vec, upb_Arena* arena)
{
    UpbArrayVal* arr = opentelemetry_proto_common_v1_AnyValue_mutable_array_value(av, arena);
    for (const std::string& elem : vec)
    {
        UpbAnyValue* item = opentelemetry_proto_common_v1_ArrayValue_add_values(arr, arena);
        if (item != nullptr)
        {
            opentelemetry_proto_common_v1_AnyValue_set_string_value(item, Sv(elem));
        }
    }
}

void EncodeAttrValue(UpbAnyValue* av, const microtel::AttributeValue& val, upb_Arena* arena)
{
    std::visit(
        [av, arena](const auto& v)
        {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, bool>)
            {
                EncodeAnyValueBool(av, v);
            }
            else if constexpr (std::is_same_v<T, std::int64_t>)
            {
                EncodeAnyValueInt(av, v);
            }
            else if constexpr (std::is_same_v<T, double>)
            {
                EncodeAnyValueDouble(av, v);
            }
            else if constexpr (std::is_same_v<T, std::string>)
            {
                EncodeAnyValueStr(av, v);
            }
            else if constexpr (std::is_same_v<T, std::vector<bool>>)
            {
                EncodeArrayBool(av, v, arena);
            }
            else if constexpr (std::is_same_v<T, std::vector<std::int64_t>>)
            {
                EncodeArrayInt(av, v, arena);
            }
            else if constexpr (std::is_same_v<T, std::vector<double>>)
            {
                EncodeArrayDouble(av, v, arena);
            }
            else if constexpr (std::is_same_v<T, std::vector<std::string>>)
            {
                EncodeArrayStr(av, v, arena);
            }
        },
        val);
}

// AddFn: callable as UpbKV*(upb_Arena*) — e.g. a capturing lambda that
// closes over the parent message pointer and forwards to the concrete upb
// add_attributes function. Avoids reinterpret_cast of function pointers.
template <typename AddFn>
void EncodeAttributes(const std::vector<microtel::KeyValue>& attrs, upb_Arena* arena, AddFn add_kv)
{
    for (const auto& kv : attrs)
    {
        UpbKV* ukv = add_kv(arena);
        if (ukv == nullptr)
        {
            continue;
        }
        opentelemetry_proto_common_v1_KeyValue_set_key(ukv, Sv(kv.key));
        UpbAnyValue* av = opentelemetry_proto_common_v1_KeyValue_mutable_value(ukv, arena);
        if (av != nullptr)
        {
            EncodeAttrValue(av, kv.value, arena);
        }
    }
}

// ---------------------------------------------------------------------------
// Event / Link encoding
// ---------------------------------------------------------------------------

void EncodeEvent(const internal::SpanEvent& ev, UpbSpan* span, upb_Arena* arena)
{
    UpbEvent* uev = opentelemetry_proto_trace_v1_Span_add_events(span, arena);
    if (uev == nullptr)
    {
        return;
    }
    opentelemetry_proto_trace_v1_Span_Event_set_name(uev, Sv(ev.name));
    opentelemetry_proto_trace_v1_Span_Event_set_time_unix_nano(uev, ToNanos(ev.timestamp));

    EncodeAttributes(ev.attributes,
                     arena,
                     [uev](upb_Arena* a)
                     { return opentelemetry_proto_trace_v1_Span_Event_add_attributes(uev, a); });
}

void EncodeLink(const internal::SpanLink& lnk, UpbSpan* span, upb_Arena* arena)
{
    UpbLink* ulnk = opentelemetry_proto_trace_v1_Span_add_links(span, arena);
    if (ulnk == nullptr)
    {
        return;
    }
    const auto& tc = lnk.linked_context;
    opentelemetry_proto_trace_v1_Span_Link_set_trace_id(
        ulnk, SvBytes(tc.trace_id.AsBytes().data(), tc.trace_id.AsBytes().size()));
    opentelemetry_proto_trace_v1_Span_Link_set_span_id(
        ulnk, SvBytes(tc.span_id.AsBytes().data(), tc.span_id.AsBytes().size()));
    if (!tc.trace_state.Empty())
    {
        opentelemetry_proto_trace_v1_Span_Link_set_trace_state(
            ulnk, ArenaSv(tc.trace_state.ToHeader(), arena));
    }
    opentelemetry_proto_trace_v1_Span_Link_set_flags(ulnk, SpanFlags(tc.trace_flags, tc.remote));

    EncodeAttributes(lnk.attributes,
                     arena,
                     [ulnk](upb_Arena* a)
                     { return opentelemetry_proto_trace_v1_Span_Link_add_attributes(ulnk, a); });
}

// ---------------------------------------------------------------------------
// Status encoding
// ---------------------------------------------------------------------------

void EncodeStatus(const internal::SpanRecord& rec, UpbSpan* span, upb_Arena* arena)
{
    if (rec.status_code == microtel::StatusCode::Unset)
    {
        return;
    }
    UpbStatus* st = opentelemetry_proto_trace_v1_Span_mutable_status(span, arena);
    if (st == nullptr)
    {
        return;
    }
    opentelemetry_proto_trace_v1_Status_set_code(st, MapStatusCode(rec.status_code));
    if (!rec.status_description.empty())
    {
        opentelemetry_proto_trace_v1_Status_set_message(st, Sv(rec.status_description));
    }
}

// ---------------------------------------------------------------------------
// Span encoding
// ---------------------------------------------------------------------------

void EncodeSpan(const internal::SpanRecord& rec, UpbScopeSp* ss, upb_Arena* arena)
{
    UpbSpan* span = opentelemetry_proto_trace_v1_ScopeSpans_add_spans(ss, arena);
    if (span == nullptr)
    {
        return;
    }

    const auto& ctx = rec.context;
    opentelemetry_proto_trace_v1_Span_set_trace_id(
        span, SvBytes(ctx.trace_id.AsBytes().data(), ctx.trace_id.AsBytes().size()));
    opentelemetry_proto_trace_v1_Span_set_span_id(
        span, SvBytes(ctx.span_id.AsBytes().data(), ctx.span_id.AsBytes().size()));
    opentelemetry_proto_trace_v1_Span_set_name(span, Sv(rec.name));
    opentelemetry_proto_trace_v1_Span_set_kind(span, MapKind(rec.kind));
    opentelemetry_proto_trace_v1_Span_set_start_time_unix_nano(span, ToNanos(rec.start_time));
    opentelemetry_proto_trace_v1_Span_set_end_time_unix_nano(span, ToNanos(rec.end_time));

    if (rec.parent_context.IsValid())
    {
        const auto& pc = rec.parent_context;
        opentelemetry_proto_trace_v1_Span_set_parent_span_id(
            span, SvBytes(pc.span_id.AsBytes().data(), pc.span_id.AsBytes().size()));
    }
    if (!ctx.trace_state.Empty())
    {
        opentelemetry_proto_trace_v1_Span_set_trace_state(
            span, ArenaSv(ctx.trace_state.ToHeader(), arena));
    }
    const bool remote_parent = rec.parent_context.IsValid() && rec.parent_context.remote;
    opentelemetry_proto_trace_v1_Span_set_flags(span, SpanFlags(ctx.trace_flags, remote_parent));

    EncodeAttributes(rec.attributes,
                     arena,
                     [span](upb_Arena* a)
                     { return opentelemetry_proto_trace_v1_Span_add_attributes(span, a); });

    for (const auto& ev : rec.events)
    {
        EncodeEvent(ev, span, arena);
    }
    for (const auto& lnk : rec.links)
    {
        EncodeLink(lnk, span, arena);
    }

    EncodeStatus(rec, span, arena);
}

// ---------------------------------------------------------------------------
// Resource encoding
// ---------------------------------------------------------------------------

void EncodeResource(const microtel::Resource& res, UpbResSp* rs, upb_Arena* arena)
{
    UpbResource* ures = opentelemetry_proto_trace_v1_ResourceSpans_mutable_resource(rs, arena);
    if (ures == nullptr)
    {
        return;
    }
    EncodeAttributes(res.Attributes(),
                     arena,
                     [ures](upb_Arena* a)
                     { return opentelemetry_proto_resource_v1_Resource_add_attributes(ures, a); });
}

// ---------------------------------------------------------------------------
// Scope encoding
// ---------------------------------------------------------------------------

void EncodeScope(const internal::InstrumentationScope& scope, UpbScopeSp* ss, upb_Arena* arena)
{
    UpbScope* usc = opentelemetry_proto_trace_v1_ScopeSpans_mutable_scope(ss, arena);
    if (usc == nullptr)
    {
        return;
    }
    opentelemetry_proto_common_v1_InstrumentationScope_set_name(usc, Sv(scope.name));
    opentelemetry_proto_common_v1_InstrumentationScope_set_version(usc, Sv(scope.version));
}

// ---------------------------------------------------------------------------
// Metrics type aliases
// ---------------------------------------------------------------------------

using UpbResMet = opentelemetry_proto_metrics_v1_ResourceMetrics;
using UpbScoMet = opentelemetry_proto_metrics_v1_ScopeMetrics;
using UpbMetric = opentelemetry_proto_metrics_v1_Metric;
using UpbNDP = opentelemetry_proto_metrics_v1_NumberDataPoint;
using UpbHDP = opentelemetry_proto_metrics_v1_HistogramDataPoint;
using UpbMetReq = opentelemetry_proto_collector_metrics_v1_ExportMetricsServiceRequest;

// ---------------------------------------------------------------------------
// Metrics temporality mapping
// ---------------------------------------------------------------------------

int32_t MapTemporality(internal::AggregationTemporality t) noexcept
{
    using T = internal::AggregationTemporality;
    return t == T::Delta ? opentelemetry_proto_metrics_v1_AGGREGATION_TEMPORALITY_DELTA
                         : opentelemetry_proto_metrics_v1_AGGREGATION_TEMPORALITY_CUMULATIVE;
}

// ---------------------------------------------------------------------------
// NumberDataPoint encoding
// ---------------------------------------------------------------------------

void EncodeNumberPoint(const internal::NumberPoint& pt, UpbNDP* dp, upb_Arena* arena)
{
    opentelemetry_proto_metrics_v1_NumberDataPoint_set_start_time_unix_nano(dp,
                                                                            ToNanos(pt.start_time));
    opentelemetry_proto_metrics_v1_NumberDataPoint_set_time_unix_nano(dp, ToNanos(pt.time));
    std::visit(
        [dp](const auto& v)
        {
            if constexpr (std::is_same_v<std::decay_t<decltype(v)>, std::int64_t>)
            {
                opentelemetry_proto_metrics_v1_NumberDataPoint_set_as_int(dp, v);
            }
            else
            {
                opentelemetry_proto_metrics_v1_NumberDataPoint_set_as_double(dp, v);
            }
        },
        pt.value);
    EncodeAttributes(
        pt.attributes,
        arena,
        [dp](upb_Arena* a)
        { return opentelemetry_proto_metrics_v1_NumberDataPoint_add_attributes(dp, a); });
}

// ---------------------------------------------------------------------------
// Gauge encoding
// ---------------------------------------------------------------------------

void EncodeGauge(const internal::GaugeData& data, UpbMetric* m, upb_Arena* arena)
{
    auto* g = opentelemetry_proto_metrics_v1_Metric_mutable_gauge(m, arena);
    if (g == nullptr)
    {
        return;
    }
    for (const auto& pt : data.points)
    {
        UpbNDP* dp = opentelemetry_proto_metrics_v1_Gauge_add_data_points(g, arena);
        if (dp != nullptr)
        {
            EncodeNumberPoint(pt, dp, arena);
        }
    }
}

// ---------------------------------------------------------------------------
// Sum encoding
// ---------------------------------------------------------------------------

void EncodeSum(const internal::SumData& data, UpbMetric* m, upb_Arena* arena)
{
    auto* s = opentelemetry_proto_metrics_v1_Metric_mutable_sum(m, arena);
    if (s == nullptr)
    {
        return;
    }
    opentelemetry_proto_metrics_v1_Sum_set_aggregation_temporality(
        s, MapTemporality(data.temporality));
    opentelemetry_proto_metrics_v1_Sum_set_is_monotonic(s, data.is_monotonic);
    for (const auto& pt : data.points)
    {
        UpbNDP* dp = opentelemetry_proto_metrics_v1_Sum_add_data_points(s, arena);
        if (dp != nullptr)
        {
            EncodeNumberPoint(pt, dp, arena);
        }
    }
}

// ---------------------------------------------------------------------------
// HistogramDataPoint encoding
// ---------------------------------------------------------------------------

void EncodeHistogramPoint(const internal::HistogramPoint& pt, UpbHDP* dp, upb_Arena* arena)
{
    opentelemetry_proto_metrics_v1_HistogramDataPoint_set_start_time_unix_nano(
        dp, ToNanos(pt.start_time));
    opentelemetry_proto_metrics_v1_HistogramDataPoint_set_time_unix_nano(dp, ToNanos(pt.time));
    opentelemetry_proto_metrics_v1_HistogramDataPoint_set_count(dp, pt.count);
    opentelemetry_proto_metrics_v1_HistogramDataPoint_set_sum(dp, pt.sum);
    if (pt.min.has_value())
    {
        opentelemetry_proto_metrics_v1_HistogramDataPoint_set_min(dp, *pt.min);
    }
    if (pt.max.has_value())
    {
        opentelemetry_proto_metrics_v1_HistogramDataPoint_set_max(dp, *pt.max);
    }
    for (const auto bc : pt.bucket_counts)
    {
        (void)opentelemetry_proto_metrics_v1_HistogramDataPoint_add_bucket_counts(dp, bc, arena);
    }
    for (const auto eb : pt.explicit_bounds)
    {
        (void)opentelemetry_proto_metrics_v1_HistogramDataPoint_add_explicit_bounds(dp, eb, arena);
    }
    EncodeAttributes(
        pt.attributes,
        arena,
        [dp](upb_Arena* a)
        { return opentelemetry_proto_metrics_v1_HistogramDataPoint_add_attributes(dp, a); });
}

// ---------------------------------------------------------------------------
// Histogram encoding
// ---------------------------------------------------------------------------

void EncodeHistogram(const internal::HistogramData& data, UpbMetric* m, upb_Arena* arena)
{
    auto* h = opentelemetry_proto_metrics_v1_Metric_mutable_histogram(m, arena);
    if (h == nullptr)
    {
        return;
    }
    opentelemetry_proto_metrics_v1_Histogram_set_aggregation_temporality(
        h, MapTemporality(data.temporality));
    for (const auto& pt : data.points)
    {
        UpbHDP* dp = opentelemetry_proto_metrics_v1_Histogram_add_data_points(h, arena);
        if (dp != nullptr)
        {
            EncodeHistogramPoint(pt, dp, arena);
        }
    }
}

// ---------------------------------------------------------------------------
// Metric record encoding (variant dispatch)
// ---------------------------------------------------------------------------

void EncodeMetricRecord(const internal::MetricRecord& rec, UpbScoMet* sm, upb_Arena* arena)
{
    UpbMetric* m = opentelemetry_proto_metrics_v1_ScopeMetrics_add_metrics(sm, arena);
    if (m == nullptr)
    {
        return;
    }
    opentelemetry_proto_metrics_v1_Metric_set_name(m, Sv(rec.name));
    opentelemetry_proto_metrics_v1_Metric_set_description(m, Sv(rec.description));
    opentelemetry_proto_metrics_v1_Metric_set_unit(m, Sv(rec.unit));
    std::visit(
        [m, arena](const auto& data)
        {
            using T = std::decay_t<decltype(data)>;
            if constexpr (std::is_same_v<T, internal::SumData>)
            {
                EncodeSum(data, m, arena);
            }
            else if constexpr (std::is_same_v<T, internal::GaugeData>)
            {
                EncodeGauge(data, m, arena);
            }
            else if constexpr (std::is_same_v<T, internal::HistogramData>)
            {
                EncodeHistogram(data, m, arena);
            }
            // ExponentialHistogramData deferred to v1.2, with the rest of the
            // metrics work: the v1.1 rescope (ICP 0024) kept it there, and
            // interop-matrix.md §6 dates metrics collector-conformance v1.2.
        },
        rec.data);
}

// ---------------------------------------------------------------------------
// Metrics resource encoding
// ---------------------------------------------------------------------------

void EncodeMetricResource(const microtel::Resource& res, UpbResMet* rm, upb_Arena* arena)
{
    UpbResource* ures = opentelemetry_proto_metrics_v1_ResourceMetrics_mutable_resource(rm, arena);
    if (ures == nullptr)
    {
        return;
    }
    EncodeAttributes(res.Attributes(),
                     arena,
                     [ures](upb_Arena* a)
                     { return opentelemetry_proto_resource_v1_Resource_add_attributes(ures, a); });
}

// ---------------------------------------------------------------------------
// Metrics scope encoding
// ---------------------------------------------------------------------------

void EncodeMetricScope(const internal::InstrumentationScope& scope, UpbScoMet* sm, upb_Arena* arena)
{
    UpbScope* usc = opentelemetry_proto_metrics_v1_ScopeMetrics_mutable_scope(sm, arena);
    if (usc == nullptr)
    {
        return;
    }
    opentelemetry_proto_common_v1_InstrumentationScope_set_name(usc, Sv(scope.name));
    opentelemetry_proto_common_v1_InstrumentationScope_set_version(usc, Sv(scope.version));
}

}  // anonymous namespace

// ---------------------------------------------------------------------------
// OtlpEncoder::Encode (traces)
// ---------------------------------------------------------------------------

internal::EncodedPayload OtlpEncoder::Encode(const internal::BatchHandle& batch)
{
    if (batch.Spans().empty())
    {
        return {};
    }

    const UpbArena arena_owner;
    upb_Arena* const arena = arena_owner.Get();

    UpbRequest* req = opentelemetry_proto_collector_trace_v1_ExportTraceServiceRequest_new(arena);
    UpbResSp* rs =
        opentelemetry_proto_collector_trace_v1_ExportTraceServiceRequest_add_resource_spans(req,
                                                                                            arena);

    EncodeResource(batch.ResourceRef(), rs, arena);

    UpbScopeSp* ss = opentelemetry_proto_trace_v1_ResourceSpans_add_scope_spans(rs, arena);
    EncodeScope(batch.Scope(), ss, arena);

    for (const auto& rec : batch.Spans())
    {
        EncodeSpan(rec, ss, arena);
    }

    std::size_t len = 0;
    const char* buf = opentelemetry_proto_collector_trace_v1_ExportTraceServiceRequest_serialize(
        req, arena, &len);

    internal::EncodedPayload payload;
    if (buf != nullptr && len > 0)
    {
        auto bytes = std::make_unique<std::byte[]>(len);
        std::memcpy(bytes.get(), buf, len);
        payload = internal::EncodedPayload{std::move(bytes), len};
    }

    return payload;
}

// ---------------------------------------------------------------------------
// OtlpEncoder::Encode (metrics)
// ---------------------------------------------------------------------------

internal::EncodedPayload OtlpEncoder::Encode(const internal::MetricBatchHandle& batch)
{
    if (batch.Metrics().empty())
    {
        return {};
    }

    const UpbArena arena_owner;
    upb_Arena* const arena = arena_owner.Get();

    UpbMetReq* req =
        opentelemetry_proto_collector_metrics_v1_ExportMetricsServiceRequest_new(arena);
    UpbResMet* rm =
        opentelemetry_proto_collector_metrics_v1_ExportMetricsServiceRequest_add_resource_metrics(
            req, arena);

    EncodeMetricResource(batch.ResourceRef(), rm, arena);

    UpbScoMet* sm = opentelemetry_proto_metrics_v1_ResourceMetrics_add_scope_metrics(rm, arena);
    EncodeMetricScope(batch.Scope(), sm, arena);

    for (const auto& rec : batch.Metrics())
    {
        EncodeMetricRecord(rec, sm, arena);
    }

    std::size_t len = 0;
    const char* buf =
        opentelemetry_proto_collector_metrics_v1_ExportMetricsServiceRequest_serialize(
            req, arena, &len);

    internal::EncodedPayload payload;
    if (buf != nullptr && len > 0)
    {
        auto bytes = std::make_unique<std::byte[]>(len);
        std::memcpy(bytes.get(), buf, len);
        payload = internal::EncodedPayload{std::move(bytes), len};
    }

    return payload;
}

// ---------------------------------------------------------------------------
// OtlpEncoder::Encode (logs)
// ---------------------------------------------------------------------------

namespace
{

// Logs type aliases — shorten upb generated names inside this TU only.
using UpbResLog = opentelemetry_proto_logs_v1_ResourceLogs;
using UpbScoLog = opentelemetry_proto_logs_v1_ScopeLogs;
using UpbLogRec = opentelemetry_proto_logs_v1_LogRecord;
using UpbLogReq = opentelemetry_proto_collector_logs_v1_ExportLogsServiceRequest;

void EncodeLogResource(const microtel::Resource& res, UpbResLog* rl, upb_Arena* arena)
{
    UpbResource* ures = opentelemetry_proto_logs_v1_ResourceLogs_mutable_resource(rl, arena);
    if (ures == nullptr)
    {
        return;
    }
    EncodeAttributes(res.Attributes(),
                     arena,
                     [ures](upb_Arena* a)
                     { return opentelemetry_proto_resource_v1_Resource_add_attributes(ures, a); });
}

void EncodeLogScope(const internal::InstrumentationScope& scope, UpbScoLog* sl, upb_Arena* arena)
{
    UpbScope* usc = opentelemetry_proto_logs_v1_ScopeLogs_mutable_scope(sl, arena);
    if (usc == nullptr)
    {
        return;
    }
    opentelemetry_proto_common_v1_InstrumentationScope_set_name(usc, Sv(scope.name));
    opentelemetry_proto_common_v1_InstrumentationScope_set_version(usc, Sv(scope.version));
}

void EncodeLogRecord(const microtel::LogRecord& rec, UpbScoLog* sl, upb_Arena* arena)
{
    UpbLogRec* lr = opentelemetry_proto_logs_v1_ScopeLogs_add_log_records(sl, arena);
    if (lr == nullptr)
    {
        return;
    }

    // time / observed_time — zero-valued time_point encodes as 0 nanoseconds,
    // which the OTLP spec treats as "not set".
    opentelemetry_proto_logs_v1_LogRecord_set_time_unix_nano(lr, ToNanos(rec.time));
    opentelemetry_proto_logs_v1_LogRecord_set_observed_time_unix_nano(lr,
                                                                      ToNanos(rec.observed_time));

    // severity
    opentelemetry_proto_logs_v1_LogRecord_set_severity_number(
        lr, static_cast<int32_t>(rec.severity_number));
    if (!rec.severity_text.empty())
    {
        opentelemetry_proto_logs_v1_LogRecord_set_severity_text(lr, Sv(rec.severity_text));
    }

    // body — skip if the variant holds the default bool{false} sentinel,
    // which means "body not provided". Every other alternative (including
    // bool{true}) is a meaningful body value and is encoded.
    if (!std::holds_alternative<bool>(rec.body) || std::get<bool>(rec.body))
    {
        UpbAnyValue* av = opentelemetry_proto_logs_v1_LogRecord_mutable_body(lr, arena);
        if (av != nullptr)
        {
            EncodeAttrValue(av, rec.body, arena);
        }
    }

    // attributes
    EncodeAttributes(rec.attributes,
                     arena,
                     [lr](upb_Arena* a)
                     { return opentelemetry_proto_logs_v1_LogRecord_add_attributes(lr, a); });

    if (rec.dropped_attributes_count > 0)
    {
        opentelemetry_proto_logs_v1_LogRecord_set_dropped_attributes_count(
            lr, rec.dropped_attributes_count);
    }

    // trace correlation — only write if trace_id is valid; flags always written
    // alongside a valid trace_id
    if (rec.trace_id.IsValid())
    {
        opentelemetry_proto_logs_v1_LogRecord_set_trace_id(
            lr, SvBytes(rec.trace_id.AsBytes().data(), rec.trace_id.AsBytes().size()));
        opentelemetry_proto_logs_v1_LogRecord_set_span_id(
            lr, SvBytes(rec.span_id.AsBytes().data(), rec.span_id.AsBytes().size()));
        opentelemetry_proto_logs_v1_LogRecord_set_flags(
            lr, static_cast<uint32_t>(rec.trace_flags.AsByte()));
    }

    if (!rec.event_name.empty())
    {
        opentelemetry_proto_logs_v1_LogRecord_set_event_name(lr, Sv(rec.event_name));
    }
}

}  // namespace

internal::EncodedPayload OtlpEncoder::Encode(const internal::LogBatchHandle& batch)
{
    if (batch.Records().empty())
    {
        return {};
    }

    const UpbArena arena_owner;
    upb_Arena* const arena = arena_owner.Get();

    UpbLogReq* req = opentelemetry_proto_collector_logs_v1_ExportLogsServiceRequest_new(arena);
    UpbResLog* rl =
        opentelemetry_proto_collector_logs_v1_ExportLogsServiceRequest_add_resource_logs(req,
                                                                                         arena);

    EncodeLogResource(batch.ResourceRef(), rl, arena);

    UpbScoLog* sl = opentelemetry_proto_logs_v1_ResourceLogs_add_scope_logs(rl, arena);
    EncodeLogScope(batch.Scope(), sl, arena);

    for (const auto& rec : batch.Records())
    {
        EncodeLogRecord(rec, sl, arena);
    }

    std::size_t len = 0;
    const char* buf =
        opentelemetry_proto_collector_logs_v1_ExportLogsServiceRequest_serialize(req, arena, &len);

    internal::EncodedPayload payload;
    if (buf != nullptr && len > 0)
    {
        auto bytes = std::make_unique<std::byte[]>(len);
        std::memcpy(bytes.get(), buf, len);
        payload = internal::EncodedPayload{std::move(bytes), len};
    }

    return payload;
}

}  // namespace microtel::wire
