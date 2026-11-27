// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "microtel/attribute.hpp"
#include "microtel/error.hpp"
#include "microtel/expected.hpp"
#include "microtel/export_transport.hpp"
#include "microtel/internal/resource_detector.hpp"
#include "microtel/leaf_receiver.hpp"
#include "microtel/log_sink.hpp"
#include "microtel/protocol.hpp"
#include "microtel/provider.hpp"
#include "microtel/resource.hpp"
#include "microtel/sampler.hpp"
#include "microtel/view.hpp"

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace microtel
{

/// @brief Aggregation temporality preference for the metrics pipeline.
///
/// Selects, per instrument kind, whether metric data points accumulate since
/// the series started (cumulative) or reset each export cycle (delta), as the
/// OTLP exporter spec defines the three presets:
///
/// | Instrument kind | `Cumulative` | `Delta` | `LowMemory` |
/// |---|---|---|---|
/// | Counter, Histogram, ExponentialHistogram | Cumulative | Delta | Delta |
/// | ObservableCounter | Cumulative | Delta | Cumulative |
/// | UpDownCounter, ObservableUpDownCounter | Cumulative | Cumulative | Cumulative |
///
/// Gauges have no temporality under any preset.
///
/// Maps to `OTEL_EXPORTER_OTLP_METRICS_TEMPORALITY_PREFERENCE`
/// ("cumulative" | "delta" | "lowmemory"). OTel default: cumulative.
enum class TemporalityPreference : std::uint8_t
{
    Cumulative = 0,  ///< All instruments report cumulative sums.
    /// Delta for Counter, Histogram, ExponentialHistogram and
    /// ObservableCounter; cumulative for UpDownCounter and
    /// ObservableUpDownCounter.
    Delta = 1,
    /// Delta for Counter, Histogram and ExponentialHistogram; cumulative for
    /// ObservableCounter, UpDownCounter and ObservableUpDownCounter.
    LowMemory = 2,
};

/// @brief Drop policy when the span queue reaches `max_queue_size`.
enum class DropPolicy : std::uint8_t
{
    /// @brief Drop the incoming record (caller's `End()`). FIFO preserved.
    DropNewest = 0,
    /// @brief Evict the oldest queued record to make room.
    DropOldest = 1,
};

/// @brief Batch span processor configuration.
///
/// Designated-initialiser construction is the expected idiom:
/// `WithBatch({.max_queue_size = 8192, .schedule_delay = 5s})`.
struct BatchOptions
{
    std::uint32_t max_queue_size = 8192;
    std::uint32_t max_export_batch_size = 512;
    std::chrono::milliseconds schedule_delay = std::chrono::seconds(5);
    DropPolicy drop_policy = DropPolicy::DropNewest;
};

/// @brief Six-axis timeout taxonomy (`docs/configuration.md` §3.4).
struct TimeoutOptions
{
    std::chrono::milliseconds connect = std::chrono::seconds(10);
    std::chrono::milliseconds tls_handshake = std::chrono::seconds(10);
    std::chrono::milliseconds per_export = std::chrono::seconds(10);
    std::chrono::milliseconds retry_budget = std::chrono::seconds(60);
    std::chrono::milliseconds flush = std::chrono::seconds(5);
    std::chrono::milliseconds shutdown = std::chrono::seconds(5);
};

/// @brief TLS material configuration.
struct TlsOptions
{
    bool insecure = false;
    std::filesystem::path ca_bundle;    ///< empty: use system trust
    std::filesystem::path client_cert;  ///< empty: no mTLS
    std::filesystem::path client_key;   ///< empty: no mTLS
    std::string sni_override;           ///< empty: derive from endpoint host
};

/// @brief Span structural-limit configuration (per `docs/memory-model.md` §7).
struct SpanLimitOptions
{
    std::uint32_t attribute_count_limit = 128;
    std::uint32_t event_count_limit = 128;
    std::uint32_t link_count_limit = 128;
    std::uint32_t attribute_value_length_limit = 4096;
    std::uint32_t event_attribute_count_limit = 128;
    std::uint32_t link_attribute_count_limit = 128;
};

/// @brief Per-instrument cardinality cap for the metrics pipeline.
///
/// When the number of distinct attribute-set combinations for one instrument
/// exceeds `max_cardinality`, further new attribute sets fold into the overflow
/// series (`otel.metric.overflow=true`).  The OTel-spec default is 2000.
///
/// Overridden at runtime by `MICROTEL_METRIC_CARDINALITY_LIMIT` (decimal integer).
struct MetricLimitOptions
{
    std::size_t max_cardinality = 2000;
};

/// @brief Memory-budget configuration (per `docs/memory-model.md` §6).
struct MemoryLimitOptions
{
    std::uint64_t max_total_queue_bytes = 16ULL * 1024ULL * 1024ULL;  // 16 MiB
    std::uint32_t max_record_bytes = 64 * 1024;                       // 64 KiB
    std::uint32_t max_response_bytes = 1 * 1024 * 1024;               // 1 MiB
    std::uint32_t max_trailer_bytes = 64 * 1024;                      // 64 KiB
    std::uint32_t max_decompressed_bytes = 4 * 1024 * 1024;           // 4 MiB
};

/// @brief Callback returning the current `Authorization` header value.
///
/// Called per-export-batch with results cached for a configurable TTL.
///
/// **On failure the batch is dropped, not sent unauthenticated** — an error
/// return and a throw cost the same one batch, counted as
/// `non_retryable_failure` with the callback's message in
/// `HealthSnapshot::last_error_message` (`docs/interfaces.md` §4.9). A throw
/// is caught at the provider boundary and converted to
/// `Error::Kind::InternalFailure`; returning the error is still preferable,
/// since the kind then survives to the health snapshot.
using AuthCallback = std::function<Expected<std::string, Error>()>;

/// @brief Fluent builder for configuring and constructing a `Provider`.
///
/// Single-shot: `Build()` consumes the builder. A second call returns
/// `ConfigError::Kind::BuildAlreadyConsumed`.
///
/// @threadsafety Externally synchronised — caller serialises chained `WithXxx`
///               calls. The completed `Build()` produces a `Provider` that is
///               itself thread-safe.
class SdkBuilder
{
public:
    SdkBuilder() noexcept;
    ~SdkBuilder() noexcept;

    SdkBuilder(const SdkBuilder&) = delete;
    SdkBuilder& operator=(const SdkBuilder&) = delete;
    SdkBuilder(SdkBuilder&&) noexcept = default;
    SdkBuilder& operator=(SdkBuilder&&) noexcept = default;

    /// @brief Load configuration from a `microtel.toml` file.
    ///
    /// Subsequent `WithXxx` calls override file-resolved values per the
    /// precedence rules in `docs/configuration.md` §1.
    SdkBuilder& FromFile(std::filesystem::path path);

    SdkBuilder& WithEndpoint(std::string endpoint);
    SdkBuilder& WithProtocol(Protocol p);
    SdkBuilder& WithCompressionGzip(bool on);
    /// @brief Set static request headers, merged per name over the env and
    ///        file sources (`docs/configuration.md` §3.3).
    ///
    /// `Build()` fails with `ConfigError::Kind::InvalidValue` on field
    /// `exporter.headers.<name>` for a name HTTP/2 forbids in a request or
    /// that microtel sets itself for the resolved protocol, and for a static
    /// `authorization` when `WithAuthProvider` is also set (ICP 0038).
    SdkBuilder& WithHeaders(std::vector<KeyValue> headers);

    SdkBuilder& WithServiceName(std::string name);
    SdkBuilder& WithServiceVersion(std::string version);
    SdkBuilder& WithResource(std::vector<KeyValue> attrs);

    /// @brief Name this provider's profile. Defaults to `kDefaultProfileName`.
    ///
    /// Names identify live providers within the process, and `microtel::
    /// GetProvider(name)` is how one is found again. Each named profile is fully
    /// independent — its own endpoint, protocol, TLS material, sampler,
    /// `Resource`, pipelines, worker threads and I/O thread — and nothing is
    /// shared between them (ICP 0027).
    ///
    /// Compared byte-for-byte; no normalisation, no case folding. `Build()`
    /// fails with `ConfigError::Kind::DuplicateProfileName` if another **live**
    /// provider already carries this name — shutting a provider down does not
    /// release its name, destroying it does — with
    /// `ConfigError::Kind::ProfileLimitExceeded` when the process already holds
    /// the maximum number of live providers, and with
    /// `ConfigError::Kind::InvalidValue` if @p name is empty: an unnamed profile
    /// is `"default"`, not `""`.
    SdkBuilder& WithProfileName(std::string name);

    /// @brief Register a resource detector.
    ///
    /// Call once per detector; registration order is significant. `Build()`
    /// runs each detector exactly once, on the calling thread, and merges their
    /// contributions per `docs/configuration.md` §3.2: detectors first (a later one
    /// overriding an earlier one), then the configuration — `microtel.toml`,
    /// environment and code, resolved key by key in that ascending order
    /// (`docs/configuration.md` §1). A key set in any configured source therefore always beats the
    /// same key from a detector.
    ///
    /// A detector that returns a `ConfigError` is logged at Warn and skipped.
    /// Setting `sdk.resource_detectors_strict` in `microtel.toml`, or
    /// `MICROTEL_RESOURCE_DETECTORS_STRICT` in the environment, makes the same
    /// failure fail `Build()` instead.
    ///
    /// `microtel::MakeProcessDetector()` and `microtel::MakeHostDetector()` in
    /// `microtel/resource_detectors.hpp` supply the built-in detectors.
    ///
    /// @param detector ownership is moved in; `nullptr` is ignored.
    SdkBuilder& WithResourceDetector(std::unique_ptr<internal::IResourceDetector> detector);

    SdkBuilder& WithSampler(SamplerHandle sampler);
    /// @brief Set the batch processor knobs (span and log pipelines).
    ///
    /// Validated by `Build()` with the same rules `Provider::SetBatchOptions`
    /// applies: a zero `max_queue_size`, a zero `max_export_batch_size`, a
    /// `max_export_batch_size` above `max_queue_size`, or a `schedule_delay`
    /// of zero or less fails `Build()` with `ConfigError::Kind::InvalidValue`.
    SdkBuilder& WithBatch(BatchOptions opts);
    SdkBuilder& WithSpanLimits(SpanLimitOptions opts);
    SdkBuilder& WithMemoryLimits(MemoryLimitOptions opts);

    SdkBuilder& WithTimeouts(TimeoutOptions opts);
    SdkBuilder& WithTls(TlsOptions opts);
    /// @brief Supply the `authorization` header value per export from @p cb,
    ///        cached for @p cache_ttl.
    ///
    /// A static `authorization` header from any source then fails `Build()`
    /// (`InvalidValue`, field `exporter.headers.<name>`), because both would
    /// be sent (ICP 0038).
    SdkBuilder& WithAuthProvider(AuthCallback cb,
                                 std::chrono::milliseconds cache_ttl = std::chrono::seconds(60));

    /// @brief Set the `PeriodicExportingMetricReader` export interval.
    ///
    /// Overrides `OTEL_METRIC_EXPORT_INTERVAL`. OTel default: 60 s.
    SdkBuilder& WithMetricInterval(std::chrono::milliseconds interval);

    /// @brief Set the aggregation temporality preference for the metrics pipeline.
    ///
    /// Overrides `OTEL_EXPORTER_OTLP_METRICS_TEMPORALITY_PREFERENCE`.
    /// OTel default: `Cumulative`.
    SdkBuilder& WithMetricTemporality(TemporalityPreference pref);

    /// @brief Set the per-instrument cardinality cap.
    ///
    /// Overrides `MICROTEL_METRIC_CARDINALITY_LIMIT`.
    SdkBuilder& WithMetricLimits(MetricLimitOptions opts);

    /// @brief Register one metric view.
    ///
    /// Views are evaluated in registration order. When no view matches an
    /// instrument, a default stream is created with the original instrument
    /// name. Multiple matching views create one stream each (fan-out).
    SdkBuilder& WithView(ViewConfig view);

    /// @brief Enable the concentrator's leaf receiver (ICP 0034,
    ///        `docs/leaf-concentrator-design.md` §4.3). Experimental.
    ///
    /// `Provider::GetLeafReceiver()` then returns a live receiver instead of
    /// the no-op one. The `[concentrator]` TOML table and the
    /// `MICROTEL_CONCENTRATOR_*` variables set the same options; @p opts is
    /// merged over them as the highest source (`docs/configuration.md` §3.14):
    /// every scalar and the resolver come from @p opts, while
    /// `leaf_defaults_resource` merges per key and `leaves` per leaf id.
    ///
    /// `Build()` validates the result and fails with
    /// `ConfigError::Kind::InvalidValue` on a bad limit or duration, a reserved
    /// or `leaf_id_attribute` key in a configured Resource, or a configured
    /// Resource over its budget: `max_node_resource_bytes` for a leaf whose
    /// effective time mode is `Unix`, else `max_leaf_resource_bytes`. In a
    /// library built without `MICROTEL_WITH_CONCENTRATOR` (the default),
    /// `Build()` fails with `InvalidValue` on field `concentrator.enabled` if
    /// any source enables the receiver.
    SdkBuilder& WithLeafReceiver(LeafReceiverOptions opts);

    /// @brief Export through an application-supplied transport instead of
    ///        microtel's HTTP/2 one (ICP 0036).
    ///
    /// The Provider owns @p transport and hands it every encoded OTLP request
    /// of the signals @p opts enables — traces only by default. No HTTP/2
    /// transport, reactor or I/O thread is built, and `Provider::Connect()`
    /// succeeds without doing anything. Retries, the retry budget, batching
    /// and every health counter work as they do for HTTP; the `SendResult`
    /// mapping is `docs/error-model.md` §7.3. Read `ExportTransport` before
    /// implementing one: a `Send` that ignores both its deadline and `Cancel`
    /// blocks the Provider's destructor forever.
    ///
    /// **Settings with no meaning here.** `WithEndpoint`, `WithProtocol`,
    /// `WithHeaders`, `WithTls`, `WithAuthProvider` and `WithCompressionGzip`
    /// together with this make `Build()` fail with
    /// `ConfigError::Kind::InvalidValue` on field `exporter.transport`. The
    /// same settings from the environment (`OTEL_EXPORTER_OTLP_*`) or the
    /// `[exporter]` table are ignored, with one `Warn` naming them. Of
    /// `TimeoutOptions`, `per_export` bounds each `Send` and `retry_budget`,
    /// `flush` and `shutdown` apply as usual; `connect` and `tls_handshake`
    /// are unused.
    ///
    /// **`HealthSnapshot::connection_state`** means "sends are succeeding",
    /// not "the peer is reachable": `Disconnected` until the first `Success`,
    /// `Connected` after one, `Reconnecting` after a failure that follows a
    /// success, `Closed` after `Shutdown`. On a fire-and-forget link such as
    /// UDP, `Connected` says only that requests are leaving this host.
    ///
    /// **Signals left off** get a no-op `Meter` or `Logger`, and one `Warn` the
    /// first time; no pipeline or worker is built for them. With `traces`
    /// off, the sampler is replaced by always-off.
    ///
    /// @param transport non-null; ownership moves into the Provider, which
    ///        destroys it after every exporter worker has been joined. A null
    ///        transport fails `Build()` with `InvalidValue`.
    /// @param opts which signals to export, and the request cap.
    SdkBuilder& WithExportTransport(std::unique_ptr<ExportTransport> transport,
                                    ExportTransportOptions opts = {});

    /// @brief Validate configuration and construct a `Provider`.
    ///
    /// Eager validation: TOML parsing, env-var parsing, endpoint URL
    /// validation, TLS material readability, batch-option coherence (see
    /// `WithBatch`). Network reachability is **not**
    /// validated — call `Provider::Connect()` if eager network preflight is
    /// needed.
    ///
    /// @return non-null `shared_ptr<Provider>` on success.
    /// @return `ConfigError` on any validation failure.
    [[nodiscard]] Expected<std::shared_ptr<Provider>, ConfigError> Build();

private:
    struct Impl;  ///< pimpl; defined in src/sdk/sdk_builder.cpp
    std::unique_ptr<Impl> m_impl;
};

}  // namespace microtel
