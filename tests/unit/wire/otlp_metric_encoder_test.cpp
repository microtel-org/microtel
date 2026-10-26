// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0
//
// TDD tests for OtlpEncoder::Encode(MetricBatchHandle) — encodes a
// MetricBatchHandle into an OTLP ExportMetricsServiceRequest and round-trips
// with upb to verify every field landed in the right proto slot.
//
// This file is the only test TU that includes the metrics upb headers directly.
// The production restriction ("only otlp_encoder.cpp touches upb") applies to
// src/, not to tests/.

#include "wire/encoder/otlp_encoder.hpp"

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#include "opentelemetry/proto/collector/metrics/v1/metrics_service.upb.h"
#include "opentelemetry/proto/common/v1/common.upb.h"
#include "opentelemetry/proto/metrics/v1/metrics.upb.h"
#include "opentelemetry/proto/resource/v1/resource.upb.h"
#include "upb/mem/arena.h"
#pragma GCC diagnostic pop

#include "microtel/internal/metric_batch.hpp"
#include "microtel/resource.hpp"

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

// Round-trip helper: parse bytes and return the first ScopeMetrics from the
// first ResourceMetrics. Caller owns and must free parsed.arena.
struct ParsedMetrics
{
    const opentelemetry_proto_metrics_v1_ScopeMetrics* sm = nullptr;
    const opentelemetry_proto_metrics_v1_ResourceMetrics* rm = nullptr;
    upb_Arena* arena = nullptr;
};

ParsedMetrics ParseFirst(const mti::EncodedPayload& payload)
{
    ParsedMetrics result;
    result.arena = upb_Arena_New();
    const auto* req = opentelemetry_proto_collector_metrics_v1_ExportMetricsServiceRequest_parse(
        reinterpret_cast<const char*>(payload.Bytes().data()), payload.Size(), result.arena);
    if (req == nullptr)
    {
        return result;
    }
    std::size_t rm_count = 0;
    const opentelemetry_proto_metrics_v1_ResourceMetrics* const* rms =
        opentelemetry_proto_collector_metrics_v1_ExportMetricsServiceRequest_resource_metrics(
            req, &rm_count);
    if (rm_count == 0 || rms == nullptr)
    {
        return result;
    }
    result.rm = rms[0];
    std::size_t sm_count = 0;
    const opentelemetry_proto_metrics_v1_ScopeMetrics* const* sms =
        opentelemetry_proto_metrics_v1_ResourceMetrics_scope_metrics(result.rm, &sm_count);
    if (sm_count > 0 && sms != nullptr)
    {
        result.sm = sms[0];
    }
    return result;
}

mti::MetricBatchHandle MakeBatch(std::vector<mti::MetricRecord> records,
                                 const std::string& scope_name = "test.scope",
                                 const std::string& scope_version = "1.0")
{
    return mti::MetricBatchHandle{
        std::move(records),
        std::make_shared<mt::Resource>(),
        mti::InstrumentationScope{.name = scope_name, .version = scope_version},
    };
}

}  // namespace

// ── Empty batch → empty payload ───────────────────────────────────────────────

TEST(OtlpMetricEncoderTest, EmptyBatch_ReturnsEmptyPayload)
{
    mtw::OtlpEncoder enc;
    const mti::EncodedPayload payload = enc.Encode(MakeBatch({}));
    EXPECT_EQ(payload.Size(), 0u);
}

// ── Sum (int64) round-trip ────────────────────────────────────────────────────

TEST(OtlpMetricEncoderTest, SumInt_RoundTrip)
{
    const mti::NumberPoint pt{
        .start_time = NsEpoch(1000),
        .time = NsEpoch(2000),
        .value = std::int64_t{42},
    };
    const mti::MetricRecord rec{
        .name = "requests.total",
        .description = "Total requests",
        .unit = "{request}",
        .data =
            mti::SumData{
                .temporality = mti::AggregationTemporality::Cumulative,
                .is_monotonic = true,
                .points = {pt},
            },
    };
    mtw::OtlpEncoder enc;
    const mti::EncodedPayload payload = enc.Encode(MakeBatch({rec}));
    ASSERT_GT(payload.Size(), 0u);

    const ParsedMetrics parsed = ParseFirst(payload);
    ASSERT_NE(parsed.sm, nullptr);

    std::size_t n = 0;
    const opentelemetry_proto_metrics_v1_Metric* const* metrics =
        opentelemetry_proto_metrics_v1_ScopeMetrics_metrics(parsed.sm, &n);
    ASSERT_EQ(n, 1u);

    const opentelemetry_proto_metrics_v1_Metric* m = metrics[0];
    const upb_StringView name_sv = opentelemetry_proto_metrics_v1_Metric_name(m);
    EXPECT_EQ(std::string(name_sv.data, name_sv.size), "requests.total");
    const upb_StringView unit_sv = opentelemetry_proto_metrics_v1_Metric_unit(m);
    EXPECT_EQ(std::string(unit_sv.data, unit_sv.size), "{request}");

    ASSERT_TRUE(opentelemetry_proto_metrics_v1_Metric_has_sum(m));
    const opentelemetry_proto_metrics_v1_Sum* sum = opentelemetry_proto_metrics_v1_Metric_sum(m);
    EXPECT_EQ(opentelemetry_proto_metrics_v1_Sum_aggregation_temporality(sum),
              opentelemetry_proto_metrics_v1_AGGREGATION_TEMPORALITY_CUMULATIVE);
    EXPECT_TRUE(opentelemetry_proto_metrics_v1_Sum_is_monotonic(sum));

    std::size_t dp_count = 0;
    const opentelemetry_proto_metrics_v1_NumberDataPoint* const* dps =
        opentelemetry_proto_metrics_v1_Sum_data_points(sum, &dp_count);
    ASSERT_EQ(dp_count, 1u);
    EXPECT_EQ(opentelemetry_proto_metrics_v1_NumberDataPoint_start_time_unix_nano(dps[0]), 1000u);
    EXPECT_EQ(opentelemetry_proto_metrics_v1_NumberDataPoint_time_unix_nano(dps[0]), 2000u);
    EXPECT_EQ(opentelemetry_proto_metrics_v1_NumberDataPoint_as_int(dps[0]), 42);

    upb_Arena_Free(parsed.arena);
}

// ── Sum (double) round-trip ───────────────────────────────────────────────────

TEST(OtlpMetricEncoderTest, SumDouble_RoundTrip)
{
    const mti::NumberPoint pt{.time = NsEpoch(5000), .value = 3.14};
    const mti::MetricRecord rec{
        .name = "latency.p99",
        .data = mti::SumData{.is_monotonic = false, .points = {pt}},
    };
    mtw::OtlpEncoder enc;
    const mti::EncodedPayload payload = enc.Encode(MakeBatch({rec}));
    ASSERT_GT(payload.Size(), 0u);

    const ParsedMetrics parsed = ParseFirst(payload);
    ASSERT_NE(parsed.sm, nullptr);

    std::size_t n = 0;
    const opentelemetry_proto_metrics_v1_Metric* const* metrics =
        opentelemetry_proto_metrics_v1_ScopeMetrics_metrics(parsed.sm, &n);
    ASSERT_EQ(n, 1u);
    ASSERT_TRUE(opentelemetry_proto_metrics_v1_Metric_has_sum(metrics[0]));

    const opentelemetry_proto_metrics_v1_Sum* sum =
        opentelemetry_proto_metrics_v1_Metric_sum(metrics[0]);
    std::size_t dp_count = 0;
    const opentelemetry_proto_metrics_v1_NumberDataPoint* const* dps =
        opentelemetry_proto_metrics_v1_Sum_data_points(sum, &dp_count);
    ASSERT_EQ(dp_count, 1u);
    EXPECT_DOUBLE_EQ(opentelemetry_proto_metrics_v1_NumberDataPoint_as_double(dps[0]), 3.14);

    upb_Arena_Free(parsed.arena);
}

// ── Gauge round-trip ──────────────────────────────────────────────────────────

TEST(OtlpMetricEncoderTest, Gauge_RoundTrip)
{
    const mti::NumberPoint pt{.time = NsEpoch(3000), .value = std::int64_t{7}};
    const mti::MetricRecord rec{
        .name = "system.cpu",
        .data = mti::GaugeData{.points = {pt}},
    };
    mtw::OtlpEncoder enc;
    const mti::EncodedPayload payload = enc.Encode(MakeBatch({rec}));
    ASSERT_GT(payload.Size(), 0u);

    const ParsedMetrics parsed = ParseFirst(payload);
    ASSERT_NE(parsed.sm, nullptr);

    std::size_t n = 0;
    const opentelemetry_proto_metrics_v1_Metric* const* metrics =
        opentelemetry_proto_metrics_v1_ScopeMetrics_metrics(parsed.sm, &n);
    ASSERT_EQ(n, 1u);
    ASSERT_TRUE(opentelemetry_proto_metrics_v1_Metric_has_gauge(metrics[0]));

    const opentelemetry_proto_metrics_v1_Gauge* g =
        opentelemetry_proto_metrics_v1_Metric_gauge(metrics[0]);
    std::size_t dp_count = 0;
    const opentelemetry_proto_metrics_v1_NumberDataPoint* const* dps =
        opentelemetry_proto_metrics_v1_Gauge_data_points(g, &dp_count);
    ASSERT_EQ(dp_count, 1u);
    EXPECT_EQ(opentelemetry_proto_metrics_v1_NumberDataPoint_as_int(dps[0]), 7);

    upb_Arena_Free(parsed.arena);
}

// ── Histogram round-trip ──────────────────────────────────────────────────────

TEST(OtlpMetricEncoderTest, Histogram_RoundTrip)
{
    const mti::HistogramPoint pt{
        .start_time = NsEpoch(1000),
        .time = NsEpoch(9000),
        .count = 5,
        .sum = 42.0,
        .min = 1.0,
        .max = 20.0,
        .bucket_counts = {1, 2, 2},
        .explicit_bounds = {5.0, 10.0},
    };
    const mti::MetricRecord rec{
        .name = "request.duration",
        .unit = "ms",
        .data =
            mti::HistogramData{
                .temporality = mti::AggregationTemporality::Delta,
                .points = {pt},
            },
    };
    mtw::OtlpEncoder enc;
    const mti::EncodedPayload payload = enc.Encode(MakeBatch({rec}));
    ASSERT_GT(payload.Size(), 0u);

    const ParsedMetrics parsed = ParseFirst(payload);
    ASSERT_NE(parsed.sm, nullptr);

    std::size_t n = 0;
    const opentelemetry_proto_metrics_v1_Metric* const* metrics =
        opentelemetry_proto_metrics_v1_ScopeMetrics_metrics(parsed.sm, &n);
    ASSERT_EQ(n, 1u);
    ASSERT_TRUE(opentelemetry_proto_metrics_v1_Metric_has_histogram(metrics[0]));

    const opentelemetry_proto_metrics_v1_Histogram* hist =
        opentelemetry_proto_metrics_v1_Metric_histogram(metrics[0]);
    EXPECT_EQ(opentelemetry_proto_metrics_v1_Histogram_aggregation_temporality(hist),
              opentelemetry_proto_metrics_v1_AGGREGATION_TEMPORALITY_DELTA);

    std::size_t dp_count = 0;
    const opentelemetry_proto_metrics_v1_HistogramDataPoint* const* dps =
        opentelemetry_proto_metrics_v1_Histogram_data_points(hist, &dp_count);
    ASSERT_EQ(dp_count, 1u);
    const opentelemetry_proto_metrics_v1_HistogramDataPoint* dp = dps[0];

    EXPECT_EQ(opentelemetry_proto_metrics_v1_HistogramDataPoint_count(dp), 5u);
    EXPECT_DOUBLE_EQ(opentelemetry_proto_metrics_v1_HistogramDataPoint_sum(dp), 42.0);
    EXPECT_TRUE(opentelemetry_proto_metrics_v1_HistogramDataPoint_has_min(dp));
    EXPECT_DOUBLE_EQ(opentelemetry_proto_metrics_v1_HistogramDataPoint_min(dp), 1.0);
    EXPECT_TRUE(opentelemetry_proto_metrics_v1_HistogramDataPoint_has_max(dp));
    EXPECT_DOUBLE_EQ(opentelemetry_proto_metrics_v1_HistogramDataPoint_max(dp), 20.0);
    EXPECT_EQ(opentelemetry_proto_metrics_v1_HistogramDataPoint_start_time_unix_nano(dp), 1000u);
    EXPECT_EQ(opentelemetry_proto_metrics_v1_HistogramDataPoint_time_unix_nano(dp), 9000u);

    std::size_t bc_count = 0;
    const std::uint64_t* bc =
        opentelemetry_proto_metrics_v1_HistogramDataPoint_bucket_counts(dp, &bc_count);
    ASSERT_EQ(bc_count, 3u);
    EXPECT_EQ(bc[0], 1u);
    EXPECT_EQ(bc[1], 2u);
    EXPECT_EQ(bc[2], 2u);

    std::size_t eb_count = 0;
    const double* eb =
        opentelemetry_proto_metrics_v1_HistogramDataPoint_explicit_bounds(dp, &eb_count);
    ASSERT_EQ(eb_count, 2u);
    EXPECT_DOUBLE_EQ(eb[0], 5.0);
    EXPECT_DOUBLE_EQ(eb[1], 10.0);

    upb_Arena_Free(parsed.arena);
}

// ── Scope metadata round-trip ─────────────────────────────────────────────────

TEST(OtlpMetricEncoderTest, ScopeMetadata_RoundTrip)
{
    const mti::MetricRecord rec{
        .name = "dummy",
        .data = mti::GaugeData{.points = {{.value = std::int64_t{1}}}},
    };
    mtw::OtlpEncoder enc;
    const mti::EncodedPayload payload = enc.Encode(MakeBatch({rec}, "my.lib", "2.3"));
    ASSERT_GT(payload.Size(), 0u);

    const ParsedMetrics parsed = ParseFirst(payload);
    ASSERT_NE(parsed.sm, nullptr);

    const opentelemetry_proto_common_v1_InstrumentationScope* sc =
        opentelemetry_proto_metrics_v1_ScopeMetrics_scope(parsed.sm);
    ASSERT_NE(sc, nullptr);

    const upb_StringView name_sv = opentelemetry_proto_common_v1_InstrumentationScope_name(sc);
    EXPECT_EQ(std::string(name_sv.data, name_sv.size), "my.lib");
    const upb_StringView ver_sv = opentelemetry_proto_common_v1_InstrumentationScope_version(sc);
    EXPECT_EQ(std::string(ver_sv.data, ver_sv.size), "2.3");

    upb_Arena_Free(parsed.arena);
}

// ── Multiple metrics in a batch ───────────────────────────────────────────────

TEST(OtlpMetricEncoderTest, MultipleMetrics_AllPresent)
{
    const std::vector<mti::MetricRecord> recs{
        {.name = "a", .data = mti::GaugeData{.points = {{.value = std::int64_t{1}}}}},
        {.name = "b", .data = mti::GaugeData{.points = {{.value = std::int64_t{2}}}}},
        {.name = "c", .data = mti::GaugeData{.points = {{.value = std::int64_t{3}}}}},
    };
    mtw::OtlpEncoder enc;
    const mti::EncodedPayload payload = enc.Encode(MakeBatch(recs));
    ASSERT_GT(payload.Size(), 0u);

    const ParsedMetrics parsed = ParseFirst(payload);
    ASSERT_NE(parsed.sm, nullptr);

    std::size_t n = 0;
    opentelemetry_proto_metrics_v1_ScopeMetrics_metrics(parsed.sm, &n);
    EXPECT_EQ(n, 3u);

    upb_Arena_Free(parsed.arena);
}

// ── Cardinality overflow attribute encodes as bool_value=true ────────────────

TEST(OtlpMetricEncoderTest, OverflowAttribute_EncodesAsBoolValue)
{
    const mti::NumberPoint pt{
        .attributes = {mt::KeyValue{.key = "otel.metric.overflow", .value = true}},
        .time = NsEpoch(1000),
        .value = std::int64_t{1},
    };
    const mti::MetricRecord rec{
        .name = "requests.total",
        .data = mti::SumData{.is_monotonic = true, .points = {pt}},
    };
    mtw::OtlpEncoder enc;
    const mti::EncodedPayload payload = enc.Encode(MakeBatch({rec}));
    ASSERT_GT(payload.Size(), 0u);

    const ParsedMetrics parsed = ParseFirst(payload);
    ASSERT_NE(parsed.sm, nullptr);

    std::size_t n = 0;
    const opentelemetry_proto_metrics_v1_Metric* const* metrics =
        opentelemetry_proto_metrics_v1_ScopeMetrics_metrics(parsed.sm, &n);
    ASSERT_EQ(n, 1u);
    ASSERT_TRUE(opentelemetry_proto_metrics_v1_Metric_has_sum(metrics[0]));

    const opentelemetry_proto_metrics_v1_Sum* sum =
        opentelemetry_proto_metrics_v1_Metric_sum(metrics[0]);
    std::size_t dp_count = 0;
    const opentelemetry_proto_metrics_v1_NumberDataPoint* const* dps =
        opentelemetry_proto_metrics_v1_Sum_data_points(sum, &dp_count);
    ASSERT_EQ(dp_count, 1u);

    std::size_t attr_count = 0;
    const opentelemetry_proto_common_v1_KeyValue* const* attrs =
        opentelemetry_proto_metrics_v1_NumberDataPoint_attributes(dps[0], &attr_count);
    ASSERT_EQ(attr_count, 1u);

    const upb_StringView key_sv = opentelemetry_proto_common_v1_KeyValue_key(attrs[0]);
    EXPECT_EQ(std::string(key_sv.data, key_sv.size), "otel.metric.overflow");

    const opentelemetry_proto_common_v1_AnyValue* av =
        opentelemetry_proto_common_v1_KeyValue_value(attrs[0]);
    ASSERT_NE(av, nullptr);
    ASSERT_TRUE(opentelemetry_proto_common_v1_AnyValue_has_bool_value(av));
    EXPECT_TRUE(opentelemetry_proto_common_v1_AnyValue_bool_value(av));

    upb_Arena_Free(parsed.arena);
}

// ── Sum (Delta temporality) round-trip ───────────────────────────────────────

TEST(OtlpMetricEncoderTest, SumDeltaTemporality_RoundTrip)
{
    const mti::NumberPoint pt{.time = NsEpoch(1000), .value = std::int64_t{5}};
    const mti::MetricRecord rec{
        .name = "bytes.sent",
        .data =
            mti::SumData{
                .temporality = mti::AggregationTemporality::Delta,
                .is_monotonic = true,
                .points = {pt},
            },
    };
    mtw::OtlpEncoder enc;
    const mti::EncodedPayload payload = enc.Encode(MakeBatch({rec}));
    ASSERT_GT(payload.Size(), 0u);

    const ParsedMetrics parsed = ParseFirst(payload);
    ASSERT_NE(parsed.sm, nullptr);

    std::size_t n = 0;
    const opentelemetry_proto_metrics_v1_Metric* const* metrics =
        opentelemetry_proto_metrics_v1_ScopeMetrics_metrics(parsed.sm, &n);
    ASSERT_EQ(n, 1u);
    ASSERT_TRUE(opentelemetry_proto_metrics_v1_Metric_has_sum(metrics[0]));

    const opentelemetry_proto_metrics_v1_Sum* sum =
        opentelemetry_proto_metrics_v1_Metric_sum(metrics[0]);
    EXPECT_EQ(opentelemetry_proto_metrics_v1_Sum_aggregation_temporality(sum),
              opentelemetry_proto_metrics_v1_AGGREGATION_TEMPORALITY_DELTA);

    upb_Arena_Free(parsed.arena);
}

// ── Exponential histogram round-trip ─────────────────────────────────────────

TEST(OtlpMetricEncoderTest, ExponentialHistogram_RoundTrip)
{
    const mti::ExponentialHistogramPoint pt{
        .attributes = {mt::KeyValue{.key = "route", .value = std::string{"/cart"}}},
        .start_time = NsEpoch(1000),
        .time = NsEpoch(9000),
        .count = 6,
        .sum = 12.5,
        .min = -2.0,
        .max = 8.0,
        .scale = 3,
        .zero_count = 1,
        .positive = {.offset = 5, .bucket_counts = {1, 0, 2}},
        .negative = {.offset = -2, .bucket_counts = {2}},
    };
    const mti::MetricRecord rec{
        .name = "request.size",
        .unit = "By",
        .data =
            mti::ExponentialHistogramData{
                .temporality = mti::AggregationTemporality::Cumulative,
                .points = {pt},
            },
    };
    mtw::OtlpEncoder enc;
    const mti::EncodedPayload payload = enc.Encode(MakeBatch({rec}));
    ASSERT_GT(payload.Size(), 0u);

    const ParsedMetrics parsed = ParseFirst(payload);
    ASSERT_NE(parsed.sm, nullptr);

    std::size_t n = 0;
    const opentelemetry_proto_metrics_v1_Metric* const* metrics =
        opentelemetry_proto_metrics_v1_ScopeMetrics_metrics(parsed.sm, &n);
    ASSERT_EQ(n, 1u);
    ASSERT_TRUE(opentelemetry_proto_metrics_v1_Metric_has_exponential_histogram(metrics[0]));

    const opentelemetry_proto_metrics_v1_ExponentialHistogram* hist =
        opentelemetry_proto_metrics_v1_Metric_exponential_histogram(metrics[0]);
    EXPECT_EQ(opentelemetry_proto_metrics_v1_ExponentialHistogram_aggregation_temporality(hist),
              opentelemetry_proto_metrics_v1_AGGREGATION_TEMPORALITY_CUMULATIVE);

    std::size_t dp_count = 0;
    const opentelemetry_proto_metrics_v1_ExponentialHistogramDataPoint* const* dps =
        opentelemetry_proto_metrics_v1_ExponentialHistogram_data_points(hist, &dp_count);
    ASSERT_EQ(dp_count, 1u);
    const opentelemetry_proto_metrics_v1_ExponentialHistogramDataPoint* dp = dps[0];

    std::size_t attr_count = 0;
    const opentelemetry_proto_common_v1_KeyValue* const* attrs =
        opentelemetry_proto_metrics_v1_ExponentialHistogramDataPoint_attributes(dp, &attr_count);
    ASSERT_EQ(attr_count, 1u);
    const upb_StringView key = opentelemetry_proto_common_v1_KeyValue_key(attrs[0]);
    EXPECT_EQ(std::string(key.data, key.size), "route");

    EXPECT_EQ(opentelemetry_proto_metrics_v1_ExponentialHistogramDataPoint_start_time_unix_nano(dp),
              1000u);
    EXPECT_EQ(opentelemetry_proto_metrics_v1_ExponentialHistogramDataPoint_time_unix_nano(dp),
              9000u);
    EXPECT_EQ(opentelemetry_proto_metrics_v1_ExponentialHistogramDataPoint_count(dp), 6u);
    EXPECT_TRUE(opentelemetry_proto_metrics_v1_ExponentialHistogramDataPoint_has_sum(dp));
    EXPECT_DOUBLE_EQ(opentelemetry_proto_metrics_v1_ExponentialHistogramDataPoint_sum(dp), 12.5);
    EXPECT_TRUE(opentelemetry_proto_metrics_v1_ExponentialHistogramDataPoint_has_min(dp));
    EXPECT_DOUBLE_EQ(opentelemetry_proto_metrics_v1_ExponentialHistogramDataPoint_min(dp), -2.0);
    EXPECT_TRUE(opentelemetry_proto_metrics_v1_ExponentialHistogramDataPoint_has_max(dp));
    EXPECT_DOUBLE_EQ(opentelemetry_proto_metrics_v1_ExponentialHistogramDataPoint_max(dp), 8.0);
    EXPECT_EQ(opentelemetry_proto_metrics_v1_ExponentialHistogramDataPoint_scale(dp), 3);
    EXPECT_EQ(opentelemetry_proto_metrics_v1_ExponentialHistogramDataPoint_zero_count(dp), 1u);

    ASSERT_TRUE(opentelemetry_proto_metrics_v1_ExponentialHistogramDataPoint_has_positive(dp));
    const opentelemetry_proto_metrics_v1_ExponentialHistogramDataPoint_Buckets* pos =
        opentelemetry_proto_metrics_v1_ExponentialHistogramDataPoint_positive(dp);
    EXPECT_EQ(opentelemetry_proto_metrics_v1_ExponentialHistogramDataPoint_Buckets_offset(pos), 5);
    std::size_t pos_count = 0;
    const std::uint64_t* pos_counts =
        opentelemetry_proto_metrics_v1_ExponentialHistogramDataPoint_Buckets_bucket_counts(
            pos, &pos_count);
    ASSERT_EQ(pos_count, 3u);
    EXPECT_EQ(pos_counts[0], 1u);
    EXPECT_EQ(pos_counts[1], 0u);
    EXPECT_EQ(pos_counts[2], 2u);

    ASSERT_TRUE(opentelemetry_proto_metrics_v1_ExponentialHistogramDataPoint_has_negative(dp));
    const opentelemetry_proto_metrics_v1_ExponentialHistogramDataPoint_Buckets* neg =
        opentelemetry_proto_metrics_v1_ExponentialHistogramDataPoint_negative(dp);
    EXPECT_EQ(opentelemetry_proto_metrics_v1_ExponentialHistogramDataPoint_Buckets_offset(neg), -2);
    std::size_t neg_count = 0;
    const std::uint64_t* neg_counts =
        opentelemetry_proto_metrics_v1_ExponentialHistogramDataPoint_Buckets_bucket_counts(
            neg, &neg_count);
    ASSERT_EQ(neg_count, 1u);
    EXPECT_EQ(neg_counts[0], 2u);

    upb_Arena_Free(parsed.arena);
}

TEST(OtlpMetricEncoderTest, ExponentialHistogramDelta_OmitsAbsentMinMax)
{
    const mti::ExponentialHistogramPoint pt{
        .time = NsEpoch(4000),
        .count = 2,
        .sum = 6.0,
        .scale = 20,
        .positive = {.offset = 7, .bucket_counts = {2}},
    };
    const mti::MetricRecord rec{
        .name = "queue.wait",
        .data =
            mti::ExponentialHistogramData{
                .temporality = mti::AggregationTemporality::Delta,
                .points = {pt},
            },
    };
    mtw::OtlpEncoder enc;
    const mti::EncodedPayload payload = enc.Encode(MakeBatch({rec}));
    ASSERT_GT(payload.Size(), 0u);

    const ParsedMetrics parsed = ParseFirst(payload);
    ASSERT_NE(parsed.sm, nullptr);

    std::size_t n = 0;
    const opentelemetry_proto_metrics_v1_Metric* const* metrics =
        opentelemetry_proto_metrics_v1_ScopeMetrics_metrics(parsed.sm, &n);
    ASSERT_EQ(n, 1u);
    ASSERT_TRUE(opentelemetry_proto_metrics_v1_Metric_has_exponential_histogram(metrics[0]));

    const opentelemetry_proto_metrics_v1_ExponentialHistogram* hist =
        opentelemetry_proto_metrics_v1_Metric_exponential_histogram(metrics[0]);
    EXPECT_EQ(opentelemetry_proto_metrics_v1_ExponentialHistogram_aggregation_temporality(hist),
              opentelemetry_proto_metrics_v1_AGGREGATION_TEMPORALITY_DELTA);

    std::size_t dp_count = 0;
    const opentelemetry_proto_metrics_v1_ExponentialHistogramDataPoint* const* dps =
        opentelemetry_proto_metrics_v1_ExponentialHistogram_data_points(hist, &dp_count);
    ASSERT_EQ(dp_count, 1u);
    const opentelemetry_proto_metrics_v1_ExponentialHistogramDataPoint* dp = dps[0];

    EXPECT_EQ(opentelemetry_proto_metrics_v1_ExponentialHistogramDataPoint_count(dp), 2u);
    EXPECT_EQ(opentelemetry_proto_metrics_v1_ExponentialHistogramDataPoint_scale(dp), 20);
    EXPECT_FALSE(opentelemetry_proto_metrics_v1_ExponentialHistogramDataPoint_has_min(dp));
    EXPECT_FALSE(opentelemetry_proto_metrics_v1_ExponentialHistogramDataPoint_has_max(dp));

    std::size_t neg_count = 0;
    if (opentelemetry_proto_metrics_v1_ExponentialHistogramDataPoint_has_negative(dp))
    {
        static_cast<void>(
            opentelemetry_proto_metrics_v1_ExponentialHistogramDataPoint_Buckets_bucket_counts(
                opentelemetry_proto_metrics_v1_ExponentialHistogramDataPoint_negative(dp),
                &neg_count));
    }
    EXPECT_EQ(neg_count, 0u);

    upb_Arena_Free(parsed.arena);
}
