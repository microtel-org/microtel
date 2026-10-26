// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0
//
// The metrics conformance scenarios, shared by tests/conformance/http/metrics_test.cpp
// and tests/conformance/grpc/metrics_test.cpp.
//
// Each scenario records through a caller-built Provider, calls ForceFlush
// (which collects a metrics snapshot and exports it), and asserts on what the
// collector wrote to /out/metrics.jsonl. As with the logs suite, the payload
// assertions are shared by both protocols: the file exporter serialises the
// decoded ResourceMetrics, by which point the receiving protocol is gone.
//
// Every instrument is named `<marker>.<kind>` with a per-run marker, and each
// assertion is narrowed to that one metric's JSON object with
// `EnclosingObject`, because the collector's batch processor may merge
// several exports into one line.
//
// The expected fragments are the pinned collector's protojson rendering, read
// off a real run: camelCase keys, int64 values and counts as quoted strings,
// doubles and `aggregationTemporality` as bare numbers, zero-valued fields
// omitted.

#pragma once

#include "microtel/attribute.hpp"
#include "microtel/meter.hpp"
#include "microtel/provider.hpp"
#include "microtel/status.hpp"

#include "conformance/support/collector_output.hpp"
#include "conformance/support/conformance_env.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace microtel::testing::metrics
{

/// @brief Environment variable naming the collector's metrics output file.
inline constexpr const char* kMetricsOutputFileEnv = "MICROTEL_CONFORMANCE_METRICS_OUTPUT_FILE";

inline constexpr const char* kServiceName = "microtel-conformance";
inline constexpr const char* kScopeVersion = "1.0";

inline constexpr auto kFlushTimeout = std::chrono::seconds(30);
inline constexpr auto kCollectorPollTimeout = std::chrono::seconds(15);
inline constexpr auto kPollInterval = std::chrono::milliseconds(100);

/// Nesting from a metric's `name` outward: the metric itself, then its
/// `ScopeMetrics`, then its `ResourceMetrics`.
inline constexpr std::size_t kMetricLevel = 1;
inline constexpr std::size_t kScopeMetricsLevel = 2;
inline constexpr std::size_t kResourceMetricsLevel = 3;

inline constexpr std::int64_t kCounterAdd = 5;
inline constexpr std::int64_t kUpDownAdd = 7;
inline constexpr std::int64_t kUpDownSubtract = -3;
inline constexpr double kGaugeFirst = 1.5;
inline constexpr double kGaugeLast = 2.5;
inline constexpr double kHistogramLow = 0.5;
inline constexpr double kHistogramMid = 5.0;
inline constexpr double kHistogramHigh = 50.0;
inline constexpr double kHistogramBoundLow = 1.0;
inline constexpr double kHistogramBoundHigh = 10.0;
inline constexpr double kExpHistogramValue = 4.0;
inline constexpr std::int64_t kObservedCounter = 11;
inline constexpr std::int64_t kObservedUpDown = -2;
inline constexpr double kObservedGauge = 3.25;
inline constexpr std::int64_t kFirstAdd = 3;
inline constexpr std::int64_t kSecondAdd = 4;
inline constexpr std::size_t kGzipCounterCount = 20;

// Fragments, confirmed against the pinned collector.
inline constexpr const char* kServiceNameJson =
    R"({"key":"service.name","value":{"stringValue":"microtel-conformance"}})";
inline constexpr const char* kAttrJson =
    R"({"key":"conformance.attr","value":{"stringValue":"attr-value"}})";
inline constexpr const char* kCumulativeJson = R"("aggregationTemporality":2)";
inline constexpr const char* kDeltaJson = R"("aggregationTemporality":1)";
inline constexpr const char* kMonotonicJson = R"("isMonotonic":true)";
inline constexpr const char* kCounterValueJson = R"("asInt":"5")";
inline constexpr const char* kUpDownValueJson = R"("asInt":"4")";
inline constexpr const char* kGaugeValueJson = R"("asDouble":2.5)";
inline constexpr const char* kHistogramCountJson = R"("count":"3")";
inline constexpr const char* kHistogramSumJson = R"("sum":55.5)";
inline constexpr const char* kHistogramBucketsJson = R"("bucketCounts":["1","1","1"])";
inline constexpr const char* kHistogramBoundsJson = R"("explicitBounds":[1,10])";
inline constexpr const char* kHistogramMinJson = R"("min":0.5)";
inline constexpr const char* kHistogramMaxJson = R"("max":50)";
inline constexpr const char* kExpHistogramCountJson = R"("count":"1")";
inline constexpr const char* kExpHistogramSumJson = R"("sum":4)";
inline constexpr const char* kExpHistogramScaleJson = R"("scale":20)";
inline constexpr const char* kExpHistogramPositiveJson =
    R"("positive":{"offset":2097151,"bucketCounts":["1"]})";
inline constexpr const char* kExpHistogramMinJson = R"("min":4)";
inline constexpr const char* kExpHistogramMaxJson = R"("max":4)";
inline constexpr const char* kObservedCounterJson = R"("asInt":"11")";
inline constexpr const char* kObservedUpDownJson = R"("asInt":"-2")";
inline constexpr const char* kObservedGaugeJson = R"("asDouble":3.25)";

/// @brief Asserts @p haystack contains @p fragment, reporting @p haystack if not.
inline void ExpectContains(const std::string& haystack, const std::string& fragment)
{
    EXPECT_NE(haystack.find(fragment), std::string::npos)
        << "collector output is missing " << fragment << "\n  in: " << haystack;
}

/// @brief Asserts @p haystack does not contain @p fragment.
inline void ExpectLacks(const std::string& haystack, const std::string& fragment)
{
    EXPECT_EQ(haystack.find(fragment), std::string::npos)
        << "collector output unexpectedly has " << fragment << "\n  in: " << haystack;
}

/// @brief The value of the quoted-uint64 member @p key (e.g. `timeUnixNano`)
///        in @p object, or 0 if absent (protojson omits zero).
inline std::uint64_t NanosField(const std::string& object, const std::string& key)
{
    const std::string prefix = R"(")" + key + R"(":")";
    const std::size_t pos = object.find(prefix);
    if (pos == std::string::npos)
    {
        return 0;
    }
    return std::stoull(object.substr(pos + prefix.size()));
}

/// @brief Asserts a point's `0 < startTimeUnixNano <= timeUnixNano`, the
///        rules of ICP 0039; a gauge (@p has_start false) has a time and no
///        start.
inline void ExpectTimestamps(const std::string& object, const bool has_start = true)
{
    const std::uint64_t time = NanosField(object, "timeUnixNano");
    const std::uint64_t start = NanosField(object, "startTimeUnixNano");
    EXPECT_GT(time, 0U) << "no timeUnixNano in: " << object;
    if (!has_start)
    {
        EXPECT_EQ(start, 0U) << "unexpected startTimeUnixNano in: " << object;
        return;
    }
    EXPECT_GT(start, 0U) << "no startTimeUnixNano in: " << object;
    EXPECT_LE(start, time) << "start after time in: " << object;
}

/// @brief The exact `name` member for the metric @p name — quoted on both
///        sides, so `<m>.counter` cannot match inside `<m>.counter.2`.
inline std::string MetricNameJson(const std::string& name)
{
    return R"("name":")" + name + R"(")";
}

/// @brief One attribute, attached to every synchronous measurement.
inline std::vector<KeyValue> MeasurementAttributes()
{
    return {KeyValue{.key = "conformance.attr", .value = std::string{"attr-value"}}};
}

/// @brief Flushes @p provider: collects a metrics snapshot and exports it.
inline void Flush(Provider& provider)
{
    ASSERT_EQ(provider.ForceFlush(kFlushTimeout), Status::Completed);
}

/// @brief Waits for the metric named @p name and returns the object @p levels
///        out from its name (see `EnclosingObject`).
///
/// @return the object, or empty if it never arrived; a matched object is never
///         empty, so the caller asserts non-empty rather than dereferencing an
///         optional (gtest's early return is invisible to clang-tidy).
inline std::string AwaitMetricObject(const std::string& output_file,
                                     const std::string& name,
                                     const std::size_t levels = kMetricLevel)
{
    const std::string needle = MetricNameJson(name);
    const auto line = PollForLineContaining(output_file, needle, kCollectorPollTimeout);
    if (!line.has_value())
    {
        return {};
    }
    return EnclosingObject(*line, needle, levels).value_or(std::string{});
}

/// @brief Polls until @p needle occurs at least @p expected times.
/// @return the last observed count, so over-delivery stays visible.
inline std::size_t PollForOccurrences(const std::string& path,
                                      const std::string& needle,
                                      const std::size_t expected)
{
    const auto deadline = std::chrono::steady_clock::now() + kCollectorPollTimeout;
    std::size_t count = CountOccurrences(path, needle);
    while (count < expected && std::chrono::steady_clock::now() < deadline)
    {
        std::this_thread::sleep_for(kPollInterval);
        count = CountOccurrences(path, needle);
    }
    return count;
}

/// @brief The metric object around the occurrence of @p needle at @p pos in
///        @p line. `EnclosingObject` always narrows around the *first*
///        occurrence, and the batch processor can put two exports of one
///        metric on the same line.
inline std::string MetricObjectAt(const std::string& line, const std::size_t pos)
{
    const std::vector<std::size_t> open = detail::OpenObjectsAt(line, pos);
    if (open.empty())
    {
        return {};
    }
    const std::size_t end = detail::ObjectEnd(line, open.back());
    return end == std::string::npos ? std::string{} : line.substr(open.back(), end - open.back());
}

/// @brief The objects of every export of the metric named @p name, in file
///        order, once at least @p expected of them have arrived.
inline std::vector<std::string> AwaitMetricObjects(const std::string& output_file,
                                                   const std::string& name,
                                                   const std::size_t expected)
{
    const std::string needle = MetricNameJson(name);
    PollForOccurrences(output_file, needle, expected);
    std::istringstream content(detail::ReadWholeFile(output_file));
    std::vector<std::string> objects;
    std::string line;
    while (std::getline(content, line))
    {
        for (std::size_t pos = line.find(needle); pos != std::string::npos;
             pos = line.find(needle, pos + needle.size()))
        {
            objects.push_back(MetricObjectAt(line, pos));
        }
    }
    return objects;
}

/// @brief Asserts the cumulative Counter and UpDownCounter of
///        `RunSyncInstruments`.
inline void ExpectSyncSums(const std::string& output_file, const std::string& marker)
{
    const std::string counter = AwaitMetricObject(output_file, marker + ".counter");
    ASSERT_FALSE(counter.empty()) << "counter never reached " << output_file;
    ExpectContains(counter, R"("description":"a counter")");
    ExpectContains(counter, R"("unit":"1")");
    ExpectContains(counter, R"("sum":{)");
    ExpectContains(counter, kCounterValueJson);
    ExpectContains(counter, kAttrJson);
    ExpectContains(counter, kCumulativeJson);
    ExpectContains(counter, kMonotonicJson);
    ExpectTimestamps(counter);

    const std::string updown = AwaitMetricObject(output_file, marker + ".updown");
    ASSERT_FALSE(updown.empty()) << "up-down counter never reached " << output_file;
    ExpectContains(updown, R"("sum":{)");
    ExpectContains(updown, kUpDownValueJson);
    ExpectContains(updown, kCumulativeJson);
    ExpectLacks(updown, kMonotonicJson);
    ExpectTimestamps(updown);
}

/// @brief Asserts the Gauge and both Histograms of `RunSyncInstruments`.
inline void ExpectSyncGaugeAndHistograms(const std::string& output_file, const std::string& marker)
{
    const std::string gauge = AwaitMetricObject(output_file, marker + ".gauge");
    ASSERT_FALSE(gauge.empty()) << "gauge never reached " << output_file;
    ExpectContains(gauge, R"("gauge":{)");
    ExpectContains(gauge, kGaugeValueJson);
    ExpectTimestamps(gauge, /*has_start=*/false);

    const std::string histogram = AwaitMetricObject(output_file, marker + ".histogram");
    ASSERT_FALSE(histogram.empty()) << "histogram never reached " << output_file;
    ExpectContains(histogram, R"("unit":"ms")");
    ExpectContains(histogram, R"("histogram":{)");
    ExpectContains(histogram, kHistogramCountJson);
    ExpectContains(histogram, kHistogramSumJson);
    ExpectContains(histogram, kHistogramBucketsJson);
    ExpectContains(histogram, kHistogramBoundsJson);
    ExpectContains(histogram, kHistogramMinJson);
    ExpectContains(histogram, kHistogramMaxJson);
    ExpectContains(histogram, kCumulativeJson);
    ExpectTimestamps(histogram);

    const std::string exp_histogram = AwaitMetricObject(output_file, marker + ".exphistogram");
    ASSERT_FALSE(exp_histogram.empty()) << "exponential histogram never reached " << output_file;
    ExpectContains(exp_histogram, R"("exponentialHistogram":{)");
    ExpectContains(exp_histogram, kExpHistogramCountJson);
    ExpectContains(exp_histogram, kExpHistogramSumJson);
    ExpectContains(exp_histogram, kExpHistogramScaleJson);
    ExpectContains(exp_histogram, kExpHistogramPositiveJson);
    ExpectContains(exp_histogram, kExpHistogramMinJson);
    ExpectContains(exp_histogram, kExpHistogramMaxJson);
    ExpectContains(exp_histogram, kAttrJson);
    ExpectContains(exp_histogram, kCumulativeJson);
    ExpectTimestamps(exp_histogram);
}

/// @brief One of each synchronous instrument kind, cumulative: Counter,
///        UpDownCounter, Gauge, explicit-bucket Histogram and exponential
///        Histogram. Asserts each one's data point, its temporality and
///        monotonicity, and the resource and scope it arrived under.
inline void RunSyncInstruments(Provider& provider,
                               const std::string& output_file,
                               const std::string& scope_name)
{
    const std::string marker = UniqueMarker();
    const auto attrs = MeasurementAttributes();
    {
        const auto meter = provider.GetMeter(scope_name, kScopeVersion);
        const auto counter =
            meter->CreateCounter<std::int64_t>(marker + ".counter", "a counter", "1");
        const auto updown = meter->CreateUpDownCounter<std::int64_t>(marker + ".updown");
        const auto gauge = meter->CreateGauge<double>(marker + ".gauge");
        const auto histogram = meter->CreateHistogram<double>(
            marker + ".histogram", "", "ms", {kHistogramBoundLow, kHistogramBoundHigh});
        const auto exp_histogram =
            meter->CreateExponentialHistogram<double>(marker + ".exphistogram");

        counter->Add(kCounterAdd, attrs);
        updown->Add(kUpDownAdd, attrs);
        updown->Add(kUpDownSubtract, attrs);
        gauge->Record(kGaugeFirst, attrs);
        gauge->Record(kGaugeLast, attrs);
        histogram->Record(kHistogramLow, attrs);
        histogram->Record(kHistogramMid, attrs);
        histogram->Record(kHistogramHigh, attrs);
        exp_histogram->Record(kExpHistogramValue, attrs);
        Flush(provider);
    }

    ExpectSyncSums(output_file, marker);
    ExpectSyncGaugeAndHistograms(output_file, marker);

    const std::string scope =
        AwaitMetricObject(output_file, marker + ".counter", kScopeMetricsLevel);
    ExpectContains(scope, R"("scope":{"name":")" + scope_name + R"(","version":"1.0"})");
    const std::string resource =
        AwaitMetricObject(output_file, marker + ".counter", kResourceMetricsLevel);
    ExpectContains(resource, kServiceNameJson);
}

/// @brief One of each observable instrument kind. Their callbacks run during
///        the ForceFlush collection; asserts each reported value and kind.
inline void RunObservableInstruments(Provider& provider,
                                     const std::string& output_file,
                                     const std::string& scope_name)
{
    const std::string marker = UniqueMarker();
    {
        const auto meter = provider.GetMeter(scope_name, kScopeVersion);
        [[maybe_unused]] const auto observed_counter = meter->CreateObservableCounter<std::int64_t>(
            marker + ".observable.counter",
            "",
            "",
            [](ObservableResult<std::int64_t>& result) { result.Observe(kObservedCounter, {}); });
        [[maybe_unused]] const auto observed_updown =
            meter->CreateObservableUpDownCounter<std::int64_t>(
                marker + ".observable.updown",
                "",
                "",
                [](ObservableResult<std::int64_t>& result)
                { result.Observe(kObservedUpDown, {}); });
        [[maybe_unused]] const auto observed_gauge = meter->CreateObservableGauge<double>(
            marker + ".observable.gauge",
            "",
            "",
            [](ObservableResult<double>& result) { result.Observe(kObservedGauge, {}); });
        Flush(provider);
    }

    const std::string counter = AwaitMetricObject(output_file, marker + ".observable.counter");
    ASSERT_FALSE(counter.empty()) << "observable counter never reached " << output_file;
    ExpectContains(counter, R"("sum":{)");
    ExpectContains(counter, kObservedCounterJson);
    ExpectContains(counter, kMonotonicJson);
    ExpectTimestamps(counter);

    const std::string updown = AwaitMetricObject(output_file, marker + ".observable.updown");
    ASSERT_FALSE(updown.empty()) << "observable up-down counter never reached " << output_file;
    ExpectContains(updown, R"("sum":{)");
    ExpectContains(updown, kObservedUpDownJson);
    ExpectLacks(updown, kMonotonicJson);
    ExpectTimestamps(updown);

    const std::string gauge = AwaitMetricObject(output_file, marker + ".observable.gauge");
    ASSERT_FALSE(gauge.empty()) << "observable gauge never reached " << output_file;
    ExpectContains(gauge, R"("gauge":{)");
    ExpectContains(gauge, kObservedGaugeJson);
    ExpectTimestamps(gauge, /*has_start=*/false);
}

/// @brief A counter exported twice, 3 then 4 more. A cumulative provider
///        reports 3 then 7; a delta provider reports 3 then 4. @p delta says
///        which temporality @p provider was built with.
inline void RunTemporality(Provider& provider,
                           const std::string& output_file,
                           const std::string& scope_name,
                           const bool delta)
{
    const std::string marker = UniqueMarker();
    const std::string name = marker + ".temporality";
    const auto attrs = MeasurementAttributes();
    {
        const auto meter = provider.GetMeter(scope_name, kScopeVersion);
        const auto counter = meter->CreateCounter<std::int64_t>(name);
        counter->Add(kFirstAdd, attrs);
        Flush(provider);
        counter->Add(kSecondAdd, attrs);
        Flush(provider);
    }

    // Two ForceFlush exports, and for a cumulative provider a third from its
    // shutdown flush, which repeats the last value. Only the first two are
    // asserted.
    constexpr std::size_t kExports = 2;
    const std::vector<std::string> exports = AwaitMetricObjects(output_file, name, kExports);
    ASSERT_GE(exports.size(), kExports) << "expected one metric per export in " << output_file;
    const std::string second_value = delta ? R"("asInt":"4")" : R"("asInt":"7")";
    ExpectContains(exports[0], R"("asInt":"3")");
    ExpectContains(exports[1], second_value);
    for (std::size_t i = 0; i < kExports; ++i)
    {
        const std::string& exported = exports[i];
        ExpectContains(exported, delta ? kDeltaJson : kCumulativeJson);
        ExpectContains(exported, kMonotonicJson);
        ExpectTimestamps(exported);
    }
    // Delta windows chain: the second starts where the first ended. A
    // cumulative series keeps one start (ICP 0039).
    const std::uint64_t second_start = NanosField(exports[1], "startTimeUnixNano");
    const std::uint64_t expected_start = delta ? NanosField(exports[0], "timeUnixNano")
                                               : NanosField(exports[0], "startTimeUnixNano");
    EXPECT_EQ(second_start, expected_start) << exports[0] << "\n" << exports[1];
}

/// @brief Twenty counters through a gzip-compressed provider, each exported
///        exactly once.
inline void RunGzipDelivery(Provider& provider,
                            const std::string& output_file,
                            const std::string& scope_name)
{
    const std::string marker = UniqueMarker();
    const auto attrs = MeasurementAttributes();
    {
        const auto meter = provider.GetMeter(scope_name, kScopeVersion);
        for (std::size_t i = 0; i < kGzipCounterCount; ++i)
        {
            const auto counter =
                meter->CreateCounter<std::int64_t>(marker + ".gzip." + std::to_string(i));
            counter->Add(kCounterAdd, attrs);
        }
        Flush(provider);
    }
    for (std::size_t i = 0; i < kGzipCounterCount; ++i)
    {
        const std::string needle = MetricNameJson(marker + ".gzip." + std::to_string(i));
        EXPECT_EQ(PollForOccurrences(output_file, needle, 1), 1U) << needle;
    }
}

}  // namespace microtel::testing::metrics
