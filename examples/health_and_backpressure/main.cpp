// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0
//
// health_and_backpressure — what `Provider::GetExporterHealth()` tells an
// operator, demonstrated by breaking the pipeline in two different ways.
//
// Three phases, each a provider of its own:
//
//   1. queue overflow   a deliberately tiny queue, filled faster than the
//                       exporter can drain it. `queue_depth_now` climbs and
//                       `drop_counters[QueueFull]` starts counting.
//   2. stalled collector a provider pointed at a port nothing is listening on.
//                       `connection_state` stays Disconnected, `batches_failed`
//                       climbs, and `last_error_message` says why.
//   3. recovery         the real endpoint again: batches land, the drop
//                       counters stay where they were, and a trace ID is
//                       printed for checking against Tempo.
//
// Usage:
//   health_and_backpressure [endpoint] [dead-endpoint]
//
// [endpoint]      defaults to http://localhost:4317, the shared examples stack
//                 (examples/stack/up.sh).
// [dead-endpoint] defaults to http://127.0.0.1:14317 — a port nothing serves.
//                 Any closed port will do; the phase asserts nothing about it
//                 beyond "the connection does not complete".
//
// The endpoint is **not** one of the four hot-reload knobs (ICP 0026), so
// "recovering" onto a live collector means building a new provider, not
// retuning the stalled one. That is the honest shape and README.md says so.

#include "microtel/provider.hpp"
#include "microtel/sdk_builder.hpp"
#include "microtel/span.hpp"
#include "microtel/tracer.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <utility>

namespace
{

constexpr const char* kDefaultEndpoint{"http://localhost:4317"};
constexpr const char* kDefaultDeadEndpoint{"http://127.0.0.1:14317"};
constexpr const char* kServiceName{"microtel-health-example"};
constexpr const char* kScopeVersion{"1.0.0"};

constexpr std::chrono::seconds kFlushTimeout{10};
constexpr std::chrono::seconds kShortFlushTimeout{3};
constexpr std::chrono::seconds kShutdownTimeout{5};

// Phase 1: a queue two orders of magnitude smaller than the 8192 default, fed
// from a tight loop. The five-second schedule delay is the default and is left
// alone on purpose — the queue is what is being stressed, not the timer.
constexpr std::uint32_t kTinyQueueSize{64};
constexpr std::uint32_t kTinyBatchSize{32};
// Enough to overflow a 64-slot queue many times over — drops start within the
// first few hundred — and few enough that the demo Tempo behind the stack is
// not asked to ingest tens of thousands of one-span traces, which it will do
// but which crowds out everything else a reader wanted to look at.
constexpr std::uint64_t kBurstSpans{3000};
constexpr std::uint64_t kPollEvery{500};

// Phase 2: a burst small enough that the failure is about the endpoint rather
// than the queue.
constexpr std::uint64_t kStalledSpans{200};

// Phase 3: enough spans to be obvious in Grafana, few enough to land in one
// batch.
constexpr std::uint64_t kRecoverySpans{50};

const char* StatusToString(microtel::Status status) noexcept
{
    switch (status)
    {
        case microtel::Status::Completed:
            return "Completed";
        case microtel::Status::TimedOut:
            return "TimedOut";
        case microtel::Status::AlreadyShutDown:
            return "AlreadyShutDown";
        case microtel::Status::Failed:
            return "Failed";
        case microtel::Status::InvalidArgument:
            return "InvalidArgument";
        case microtel::Status::Unsupported:
            return "Unsupported";
    }
    return "Unknown";
}

const char* ConnectionStateToString(microtel::ConnectionState state) noexcept
{
    switch (state)
    {
        case microtel::ConnectionState::Disconnected:
            return "Disconnected";
        case microtel::ConnectionState::Connecting:
            return "Connecting";
        case microtel::ConnectionState::Connected:
            return "Connected";
        case microtel::ConnectionState::Reconnecting:
            return "Reconnecting";
        case microtel::ConnectionState::Closed:
            return "Closed";
    }
    return "Unknown";
}

// `HealthSnapshot::drop_counters` is indexed by `DropReason`, so a reader that
// wants names needs this table. Exhaustive without a `default`, so -Wswitch
// fails the build if the enum grows — which is exactly what the array
// indexing contract needs (see `microtel/provider.hpp`).
const char* DropReasonToString(microtel::DropReason reason) noexcept
{
    switch (reason)
    {
        case microtel::DropReason::QueueFull:
            return "QueueFull";
        case microtel::DropReason::RecordTooLarge:
            return "RecordTooLarge";
        case microtel::DropReason::SpanAttributeLimit:
            return "SpanAttributeLimit";
        case microtel::DropReason::SpanEventLimit:
            return "SpanEventLimit";
        case microtel::DropReason::SpanLinkLimit:
            return "SpanLinkLimit";
        case microtel::DropReason::EventAttributeLimit:
            return "EventAttributeLimit";
        case microtel::DropReason::LinkAttributeLimit:
            return "LinkAttributeLimit";
        case microtel::DropReason::AttributeValueTruncated:
            return "AttributeValueTruncated";
        case microtel::DropReason::PostShutdown:
            return "PostShutdown";
        case microtel::DropReason::ResponseTooLarge:
            return "ResponseTooLarge";
        case microtel::DropReason::DecompressionTooLarge:
            return "DecompressionTooLarge";
        case microtel::DropReason::MalformedResponse:
            return "MalformedResponse";
        case microtel::DropReason::PartialSuccessRejection:
            return "PartialSuccessRejection";
        case microtel::DropReason::NonRetryableFailure:
            return "NonRetryableFailure";
        case microtel::DropReason::RetryableFailureRecovered:
            return "RetryableFailureRecovered";
        case microtel::DropReason::RetryBudgetExhausted:
            return "RetryBudgetExhausted";
        case microtel::DropReason::TransportBusy:
            return "TransportBusy";
        case microtel::DropReason::ConnectFailure:
            return "ConnectFailure";
        case microtel::DropReason::ForceFlushTimeout:
            return "ForceFlushTimeout";
        case microtel::DropReason::ShutdownTimeout:
            return "ShutdownTimeout";
        case microtel::DropReason::CardinalityOverflow:
            return "CardinalityOverflow";
        case microtel::DropReason::MetricCallbackTimeout:
            return "MetricCallbackTimeout";
        case microtel::DropReason::NonFiniteValue:
            return "NonFiniteValue";
        case microtel::DropReason::LogAttributeLimit:
            return "LogAttributeLimit";
        case microtel::DropReason::LeafPayloadMalformed:
            return "LeafPayloadMalformed";
        case microtel::DropReason::LeafPayloadTooLarge:
            return "LeafPayloadTooLarge";
        case microtel::DropReason::LeafUnknown:
            return "LeafUnknown";
        case microtel::DropReason::NegativeHistogramValue:
            return "NegativeHistogramValue";
    }
    return "Unknown";
}

void PrintDropCounters(const microtel::HealthSnapshot& health)
{
    std::cout << "    drops:";
    bool any = false;
    for (std::size_t i = 0; i < microtel::kDropReasonCount; ++i)
    {
        if (health.drop_counters[i] != 0)
        {
            std::cout << ' ' << DropReasonToString(static_cast<microtel::DropReason>(i)) << '='
                      << health.drop_counters[i];
            any = true;
        }
    }
    std::cout << (any ? "" : " none") << '\n';
}

// The whole snapshot, one field per operator question. README.md maps each
// line to what an operator does about it.
void PrintHealth(const char* label, const microtel::HealthSnapshot& health)
{
    std::cout << "  [" << label << "]\n"
              << "    connection_state=" << ConnectionStateToString(health.connection_state)
              << " batches_sent=" << health.batches_sent
              << " batches_failed=" << health.batches_failed
              << " queue_depth_now=" << health.queue_depth_now << '\n';
    PrintDropCounters(health);
    if (!health.last_error_message.empty())
    {
        std::cout << "    last_error: " << health.last_error_message << '\n';
    }
    if (health.last_error_time.has_value())
    {
        const auto age = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now() - *health.last_error_time);
        std::cout << "    last_error_age_ms: " << age.count() << '\n';
    }
}

microtel::Expected<std::shared_ptr<microtel::Provider>, microtel::ConfigError> BuildProvider(
    const std::string& endpoint,
    std::string profile,
    microtel::BatchOptions batch,
    microtel::TimeoutOptions timeouts)
{
    return microtel::SdkBuilder{}
        .WithEndpoint(endpoint)
        .WithProtocol(microtel::Protocol::Grpc)
        .WithServiceName(kServiceName)
        .WithServiceVersion(kScopeVersion)
        .WithProfileName(std::move(profile))
        .WithBatch(batch)
        .WithTimeouts(timeouts)
        .Build();
}

/// @return the trace ID of the last span emitted, for pasting into Tempo.
std::string EmitSpans(microtel::Tracer& tracer, std::uint64_t count, const char* phase)
{
    std::string last_trace_id;
    for (std::uint64_t i = 0; i < count; ++i)
    {
        const auto span = tracer.StartSpan(
            "health.tick",
            {.kind = microtel::SpanKind::Server, .parent = {}, .start_time = {}, .attributes = {}});
        span->SetAttribute("phase", std::string{phase});
        span->SetAttribute("index", static_cast<std::int64_t>(i));
        last_trace_id = span->GetContext().trace_id.ToHex();
        span->End();
    }
    return last_trace_id;
}

// Phase 1 — the producer outruns the exporter.
//
// `End()` never blocks (docs/threading-model.md §3.1): when the queue is full
// the record is dropped instead, so an instrumented application keeps its
// latency and loses telemetry. `QueueFull` is how it finds out.
void RunQueueOverflowPhase(const std::string& endpoint)
{
    std::cout << "\n=== phase 1: a tiny queue, filled faster than it drains ===\n"
              << "max_queue_size=" << kTinyQueueSize << " max_export_batch_size=" << kTinyBatchSize
              << " spans=" << kBurstSpans << '\n';

    auto built = BuildProvider(endpoint,
                               "health-backpressure",
                               {.max_queue_size = kTinyQueueSize,
                                .max_export_batch_size = kTinyBatchSize,
                                .schedule_delay = std::chrono::seconds{5},
                                .drop_policy = microtel::DropPolicy::DropNewest},
                               microtel::TimeoutOptions{});
    if (!built)
    {
        std::cerr << "Build() failed: " << built.error().message << '\n';
        return;
    }
    const std::shared_ptr<microtel::Provider> provider = std::move(*built);
    const std::shared_ptr<microtel::Tracer> tracer =
        provider->GetTracer("microtel-health-example", kScopeVersion);

    for (std::uint64_t sent = 0; sent < kBurstSpans; sent += kPollEvery)
    {
        EmitSpans(*tracer, kPollEvery, "overflow");
        const std::string label = "after " + std::to_string(sent + kPollEvery) + " spans";
        PrintHealth(label.c_str(), provider->GetExporterHealth());
    }

    std::cout << "  ForceFlush: " << StatusToString(provider->ForceFlush(kFlushTimeout)) << '\n';
    PrintHealth("after flush", provider->GetExporterHealth());
    std::cout << "  Shutdown: " << StatusToString(provider->Shutdown(kShutdownTimeout)) << '\n';
}

// Phase 2 — nothing is listening.
//
// Short timeouts throughout: a failure path that sits out a default sixty-second
// retry budget teaches nothing and takes a minute doing it.
void RunStalledCollectorPhase(const std::string& dead_endpoint)
{
    std::cout << "\n=== phase 2: the collector is not there (" << dead_endpoint << ") ===\n";

    auto built = BuildProvider(dead_endpoint,
                               "health-stalled",
                               microtel::BatchOptions{},
                               {.connect = std::chrono::seconds{1},
                                .tls_handshake = std::chrono::seconds{1},
                                .per_export = std::chrono::seconds{2},
                                .retry_budget = std::chrono::seconds{2},
                                .flush = std::chrono::seconds{3},
                                .shutdown = std::chrono::seconds{3}});
    if (!built)
    {
        std::cerr << "Build() failed: " << built.error().message << '\n';
        return;
    }
    const std::shared_ptr<microtel::Provider> provider = std::move(*built);

    if (auto connected = provider->Connect(); !connected)
    {
        std::cout << "  Connect() failed as expected: " << connected.error().message << '\n';
    }
    PrintHealth("after the failed connect", provider->GetExporterHealth());

    const std::shared_ptr<microtel::Tracer> tracer =
        provider->GetTracer("microtel-health-example", kScopeVersion);
    EmitSpans(*tracer, kStalledSpans, "stalled");

    std::cout << "  ForceFlush: " << StatusToString(provider->ForceFlush(kShortFlushTimeout))
              << '\n';
    PrintHealth("after the failed flush", provider->GetExporterHealth());
    std::cout << "  Shutdown: " << StatusToString(provider->Shutdown(kShutdownTimeout)) << '\n';
}

// Phase 3 — a healthy pipeline, for contrast and for a trace to look at.
void RunRecoveryPhase(const std::string& endpoint)
{
    std::cout << "\n=== phase 3: back on a live collector (" << endpoint << ") ===\n";

    auto built = BuildProvider(
        endpoint, "health-recovered", microtel::BatchOptions{}, microtel::TimeoutOptions{});
    if (!built)
    {
        std::cerr << "Build() failed: " << built.error().message << '\n';
        return;
    }
    const std::shared_ptr<microtel::Provider> provider = std::move(*built);
    const std::shared_ptr<microtel::Tracer> tracer =
        provider->GetTracer("microtel-health-example", kScopeVersion);

    const std::string trace_id = EmitSpans(*tracer, kRecoverySpans, "recovered");
    std::cout << "  ForceFlush: " << StatusToString(provider->ForceFlush(kFlushTimeout)) << '\n';
    PrintHealth("after flush", provider->GetExporterHealth());
    std::cout << "  trace_id: " << trace_id
              << "\n  Grafana: http://localhost:3000 (Explore -> Tempo, or the "
                 "\"microtel - recent traces\" dashboard)\n";
    std::cout << "  Shutdown: " << StatusToString(provider->Shutdown(kShutdownTimeout)) << '\n';
}

}  // namespace

int main(int argc, char** argv)
{
    const std::string endpoint{(argc > 1) ? argv[1] : kDefaultEndpoint};
    const std::string dead_endpoint{(argc > 2) ? argv[2] : kDefaultDeadEndpoint};

    RunQueueOverflowPhase(endpoint);
    RunStalledCollectorPhase(dead_endpoint);
    RunRecoveryPhase(endpoint);

    return 0;
}
