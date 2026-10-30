// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

// GCC 15 false-positive: variant copy-assign inlined into <variant> internals
// triggers -Wfree-nonheap-object. Not present on g++-13 (CI) or clang.
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic ignored "-Wfree-nonheap-object"
#endif

#include "microtel/sdk_builder.hpp"

#include "microtel/attribute.hpp"
#include "microtel/error.hpp"
#include "microtel/expected.hpp"
#include "microtel/export_transport.hpp"
#include "microtel/internal/batch_group_exporter.hpp"
#include "microtel/internal/otlp_trace_decoder.hpp"
#include "microtel/internal/sampler.hpp"
#include "microtel/internal/transport.hpp"
#include "microtel/leaf_receiver.hpp"
#include "microtel/log_sink.hpp"
#include "microtel/protocol.hpp"
#include "microtel/resource.hpp"
#include "microtel/sampler.hpp"

#include "common/config/auth_providers.hpp"
#include "common/config/concentrator_config.hpp"
#include "common/config/config.hpp"
#include "common/config/config_validator.hpp"
#include "common/config/env_resolver.hpp"
#include "common/config/table_merge.hpp"
#include "common/config/toml_loader.hpp"
#include "common/internal_log.hpp"
#include "exporter/otlp_exporter.hpp"
#include "exporter/otlp_log_exporter.hpp"
#include "exporter/otlp_metric_exporter.hpp"
#include "sdk/batch_span_processor.hpp"
#ifdef MICROTEL_WITH_CONCENTRATOR
#include "sdk/leaf_options.hpp"
#endif
#include "sdk/metric_attribute_set.hpp"
#include "sdk/provider_registry.hpp"
#include "sdk/resource_builder.hpp"
#include "sdk/sdk_provider.hpp"
#include "sdk/view_registry.hpp"
#include "transport/epoll_reactor.hpp"
#include "transport/fallback_transport.hpp"
#include "transport/http2_transport.hpp"
#include "wire/custom/export_transport_codec.hpp"
#include "wire/encoder/otlp_encoder.hpp"
#ifdef MICROTEL_WITH_CONCENTRATOR
#include "wire/encoder/otlp_trace_decoder.hpp"
#endif
#include "wire/grpc/grpc_wire_codec.hpp"
#include "wire/http/http_wire_codec.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace microtel
{

// ---------------------------------------------------------------------------
// Impl
// ---------------------------------------------------------------------------

struct SdkBuilder::Impl
{
    std::optional<std::filesystem::path> file_path;

    std::optional<std::string> endpoint;
    std::optional<Protocol> protocol;
    std::optional<bool> compression_gzip;
    std::optional<std::vector<KeyValue>> headers;
    std::optional<std::string> service_name;
    std::optional<std::string> service_version;
    std::optional<std::vector<KeyValue>> resource_attrs;
    /// Registration-ordered; `Build` runs each exactly once (interfaces.md §4.10).
    std::vector<std::unique_ptr<internal::IResourceDetector>> resource_detectors;
    SamplerHandle sampler;
    std::optional<BatchOptions> batch;
    std::optional<SpanLimitOptions> span_limits;
    std::optional<MemoryLimitOptions> memory_limits;
    std::optional<TimeoutOptions> timeouts;
    std::optional<TlsOptions> tls;
    std::optional<AuthCallback> auth_cb;
    std::chrono::milliseconds auth_cache_ttl{std::chrono::seconds(60)};
    std::optional<std::chrono::milliseconds> metric_interval;
    std::optional<TemporalityPreference> metric_temporality;
    std::optional<MetricLimitOptions> metric_limits;
    std::vector<ViewConfig> views;
    /// Unset means the default profile; empty is a validation error, because an
    /// unnamed profile is "default", not "" (ICP 0027 §5).
    std::optional<std::string> profile_name;
    /// Unset: the receiver is whatever the `[concentrator]` table and the
    /// `MICROTEL_CONCENTRATOR_*` variables say, disabled by default. Set: merged
    /// over them, code highest (design §4.3).
    std::optional<LeafReceiverOptions> leaf_receiver;
    /// The application's transport (ICP 0036). Moves into the Provider.
    std::unique_ptr<ExportTransport> export_transport;
    /// Set by `WithExportTransport`, even with a null transport: it is what
    /// marks the build as custom-transport, so a null one is refused rather
    /// than silently falling back to HTTP.
    std::optional<ExportTransportOptions> export_transport_opts;

    bool consumed = false;

    /// @brief File, environment, code, then validation.
    /// @param ignored with a custom transport, receives the names of the
    ///        exporter settings the file or environment set, which were
    ///        dropped; the caller warns once `logging.level` is applied.
    [[nodiscard]] Expected<config::Config, ConfigError> LoadConfig(
        std::vector<std::string_view>& ignored) const;
    /// @brief Refuse a custom transport that is null or set together with a
    ///        code-set exporter setting it would silently override (ICP 0036).
    [[nodiscard]] Expected<void, ConfigError> CheckExportTransport() const;
    /// @brief Emit the custom-transport warnings and, with traces off, swap
    ///        in the always-off sampler. Only called with a custom transport.
    /// @param ignored the settings `LoadConfig` dropped.
    void PrepareCustomTransport(const std::vector<std::string_view>& ignored);
    void ApplyExporterOverrides(config::Config& cfg) const;
    void ApplyResourceOverrides(config::Config& cfg) const;

    /// @brief `Build`'s steps 7–12: encoder, codecs, exporters, processor,
    ///        provider, and the registration that claims the profile.
    ///
    /// Split out so that neither half carries the whole of `Build`: this one
    /// assembles the pipeline out of the four things `Build` resolved, and
    /// `Build` itself stays a readable sequence of validations.
    ///
    /// @param cfg          the resolved configuration.
    /// @param resource     the merged `Resource`; ownership moves in.
    /// @param auth         the auth provider, or nullptr; ownership moves in.
    /// @param transport    the transport, already constructed, or nullptr with an
    ///                     application `ExportTransport`; ownership moves in.
    /// @param profile_name the profile to register the provider under.
    /// @return the registered provider, or the `ConfigError` that refused it.
    [[nodiscard]] Expected<std::shared_ptr<Provider>, ConfigError> Assemble(
        const config::Config& cfg,
        std::shared_ptr<const Resource> resource,
        std::unique_ptr<internal::IAuthProvider> auth,
        std::unique_ptr<internal::ITransport> transport,
        const std::string& profile_name);
};

// ---------------------------------------------------------------------------
// SdkBuilder — constructor / destructor
// ---------------------------------------------------------------------------

SdkBuilder::SdkBuilder() noexcept : m_impl(std::make_unique<Impl>())
{
    m_impl->sampler = MakeAlwaysOnSampler();
}

SdkBuilder::~SdkBuilder() noexcept = default;

// ---------------------------------------------------------------------------
// SdkBuilder — WithXxx methods
// ---------------------------------------------------------------------------

SdkBuilder& SdkBuilder::FromFile(std::filesystem::path path)
{
    m_impl->file_path = std::move(path);
    return *this;
}

SdkBuilder& SdkBuilder::WithEndpoint(std::string endpoint)
{
    m_impl->endpoint = std::move(endpoint);
    return *this;
}

SdkBuilder& SdkBuilder::WithProtocol(Protocol p)
{
    m_impl->protocol = p;
    return *this;
}

SdkBuilder& SdkBuilder::WithCompressionGzip(bool on)
{
    m_impl->compression_gzip = on;
    return *this;
}

SdkBuilder& SdkBuilder::WithHeaders(std::vector<KeyValue> headers)
{
    m_impl->headers = std::move(headers);
    return *this;
}

SdkBuilder& SdkBuilder::WithServiceName(std::string name)
{
    m_impl->service_name = std::move(name);
    return *this;
}

SdkBuilder& SdkBuilder::WithServiceVersion(std::string version)
{
    m_impl->service_version = std::move(version);
    return *this;
}

SdkBuilder& SdkBuilder::WithResource(std::vector<KeyValue> attrs)
{
    m_impl->resource_attrs = std::move(attrs);
    return *this;
}

SdkBuilder& SdkBuilder::WithProfileName(std::string name)
{
    m_impl->profile_name = std::move(name);
    return *this;
}

SdkBuilder& SdkBuilder::WithResourceDetector(std::unique_ptr<internal::IResourceDetector> detector)
{
    // A null detector is dropped rather than stored: `BuildResource` documents
    // its span as non-null, and a moved-from unique_ptr reaching Build() as a
    // crash would be a poor trade for the check this costs.
    if (detector != nullptr)
    {
        m_impl->resource_detectors.push_back(std::move(detector));
    }
    return *this;
}

SdkBuilder& SdkBuilder::WithSampler(SamplerHandle sampler)
{
    m_impl->sampler = std::move(sampler);
    return *this;
}

SdkBuilder& SdkBuilder::WithBatch(BatchOptions opts)
{
    m_impl->batch = opts;
    return *this;
}

SdkBuilder& SdkBuilder::WithSpanLimits(SpanLimitOptions opts)
{
    m_impl->span_limits = opts;
    return *this;
}

SdkBuilder& SdkBuilder::WithMemoryLimits(MemoryLimitOptions opts)
{
    m_impl->memory_limits = opts;
    return *this;
}

SdkBuilder& SdkBuilder::WithTimeouts(TimeoutOptions opts)
{
    m_impl->timeouts = opts;
    return *this;
}

SdkBuilder& SdkBuilder::WithTls(TlsOptions opts)
{
    m_impl->tls = std::move(opts);
    return *this;
}

SdkBuilder& SdkBuilder::WithAuthProvider(AuthCallback cb, std::chrono::milliseconds cache_ttl)
{
    m_impl->auth_cb = std::move(cb);
    m_impl->auth_cache_ttl = cache_ttl;
    return *this;
}

SdkBuilder& SdkBuilder::WithMetricInterval(std::chrono::milliseconds interval)
{
    m_impl->metric_interval = interval;
    return *this;
}

SdkBuilder& SdkBuilder::WithMetricTemporality(TemporalityPreference pref)
{
    m_impl->metric_temporality = pref;
    return *this;
}

SdkBuilder& SdkBuilder::WithMetricLimits(MetricLimitOptions opts)
{
    m_impl->metric_limits = opts;
    return *this;
}

SdkBuilder& SdkBuilder::WithView(ViewConfig view)
{
    m_impl->views.push_back(std::move(view));
    return *this;
}

SdkBuilder& SdkBuilder::WithLeafReceiver(LeafReceiverOptions opts)
{
    m_impl->leaf_receiver = std::move(opts);
    return *this;
}

SdkBuilder& SdkBuilder::WithExportTransport(std::unique_ptr<ExportTransport> transport,
                                            ExportTransportOptions opts)
{
    m_impl->export_transport = std::move(transport);
    m_impl->export_transport_opts = opts;
    return *this;
}

// ---------------------------------------------------------------------------
// Build helpers
// ---------------------------------------------------------------------------

namespace
{

/// @brief Seed the process-global internal-log filter from the resolved config.
///
/// Called before anything in `Build()` logs, so `logging.level` and
/// `MICROTEL_LOG_LEVEL` govern the validation warnings below as well as
/// everything after. `Provider::SetLogLevel` retunes the same knob at runtime
/// (ICP 0026 §6, `docs/configuration.md` §3.11).
///
/// The return is discarded rather than checked: `config::ParseLogLevel` is the
/// only producer of this field and it only ever yields a declared enumerator,
/// so the rejection path is unreachable from here.
///
/// @param cfg borrowed; read only.
void ApplyLogLevel(const config::Config& cfg) noexcept
{
    (void)internal::SetMinLogLevel(cfg.log_level);
}

/// @brief Log one Warn line per unknown `microtel.toml` key the loader skipped
///        under `[config] unknown_keys = "warn"`.
///
/// The loader only collects them (`Config::unknown_keys_warned`): it runs
/// before `ApplyLogLevel`, so logging from there would ignore `logging.level`.
///
/// @param cfg Borrowed; read only.
void WarnOnUnknownKeys(const config::Config& cfg)
{
    for (const std::string& key : cfg.unknown_keys_warned)
    {
        internal::LogImpl(LogLevel::Warn,
                          R"(unknown configuration key ")" + key +
                              R"msg(" ignored ([config] unknown_keys = "warn"))msg");
    }
}

/// @brief Emit the warnings for configurations that are legal but very likely
///        wrong.
///
/// Neither case is rejected here. Plaintext OTLP/HTTP is legitimate in front of
/// an h2c-capable proxy (and the bench harness's own sink), and a default build
/// permits `insecure = true` outright (docs/configuration.md §3.5) — the hard ban belongs to
/// `MICROTEL_FORBID_INSECURE_TLS=ON`, which `config::Validate` enforces before
/// this ever runs. Both are, however, overwhelmingly likely to be a mistake,
/// and `config::Validate` returns `Expected<void, ConfigError>`: it can reject
/// a configuration but it cannot warn about one.
///
/// @param cfg Borrowed; read only. Both fields are post-resolution:
///            `cfg.endpoint.scheme` has collapsed `grpc://` to `http`, and
///            `cfg.protocol` has already taken `Grpc` from that same scheme,
///            so the plaintext warning below sees a gRPC endpoint as gRPC.
void WarnOnRiskyConfig(const config::Config& cfg) noexcept
{
    // h2c with prior knowledge: microtel has no HTTP/1.1 mode, and a stock
    // OpenTelemetry Collector's plaintext OTLP/HTTP receiver has no h2c. See
    // issue #166. OTLP/gRPC over the same scheme is unaffected — gRPC is h2c
    // by definition — so the protocol is part of the condition.
    if (cfg.protocol == Protocol::Http && cfg.endpoint.scheme == "http")
    {
        internal::LogImpl(
            LogLevel::Warn,
            "plaintext OTLP/HTTP (http:// with protocol=http) is HTTP/2 with prior knowledge "
            "and cannot reach an HTTP/1.1-only OTLP receiver such as a stock OpenTelemetry "
            "Collector - use https:// or OTLP/gRPC; see docs/compatibility-matrix.md");
    }

    if (cfg.tls.insecure)
    {
        internal::LogImpl(LogLevel::Warn,
                          "tls.insecure = true - TLS certificate verification is "
                          "disabled and any certificate will be accepted, including an "
                          "attacker's. Not for production; see docs/compatibility-matrix.md");
    }
}

/// @brief Parse `MICROTEL_METRIC_CARDINALITY_LIMIT` from the environment.
///
/// Returns the parsed value if the variable is set and contains a positive
/// decimal integer; returns `std::nullopt` for an unset, empty, zero, or
/// malformed value.
[[nodiscard]] std::optional<std::size_t> ParseEnvCardinality() noexcept
{
    const char* const raw = std::getenv("MICROTEL_METRIC_CARDINALITY_LIMIT");
    if (raw == nullptr)
    {
        return std::nullopt;
    }
    try
    {
        const std::string str{raw};
        std::size_t pos{};
        const auto val = std::stoull(str, &pos);
        if (pos != str.size() || val == 0)
        {
            return std::nullopt;
        }
        return static_cast<std::size_t>(val);
    }
    catch (const std::exception&)
    {
        return std::nullopt;
    }
}

[[nodiscard]] internal::ConnectOptions BuildConnectOptions(const config::Config& cfg)
{
    return internal::ConnectOptions{
        .endpoint = cfg.endpoint.scheme + "://" + cfg.endpoint.host + ":" +
                    std::to_string(cfg.endpoint.port),
        .protocol = cfg.protocol,
        .connect_timeout = cfg.timeouts.connect,
        .tls_handshake_timeout = cfg.timeouts.tls_handshake,
        .insecure = cfg.tls.insecure,
        .ca_bundle = cfg.tls.ca_bundle,
        .client_cert = cfg.tls.client_cert,
        .client_key = cfg.tls.client_key,
        .sni_override = cfg.tls.sni_override,
        .max_response_bytes = cfg.memory_limits.max_response_bytes,
        .max_trailer_bytes = cfg.memory_limits.max_trailer_bytes,
    };
}

[[nodiscard]] std::vector<internal::HeaderField> ToHeaderFields(const std::vector<KeyValue>& kvs)
{
    std::vector<internal::HeaderField> out;
    out.reserve(kvs.size());
    for (const auto& kv : kvs)
    {
        const auto* sv = std::get_if<std::string>(&kv.value);
        if (sv != nullptr)
        {
            out.push_back(internal::HeaderField{.name = kv.key, .value = *sv});
        }
    }
    return out;
}

[[nodiscard]] ConfigError TransportInitError(std::string_view what, const Error& error)
{
    return ConfigError{.kind = ConfigError::Kind::Unspecified,
                       .field = {},
                       .message = std::string{what} + " init failed: " + error.message};
}

[[nodiscard]] Expected<std::unique_ptr<internal::IReactor>, Error> CreateReactor()
{
    auto reactor = transport::EpollReactor::Create();
    if (!reactor)
    {
        return make_unexpected(reactor.error());
    }
    return std::unique_ptr<internal::IReactor>{std::move(*reactor)};
}

/// @brief The transport for @p protocol: OTLP/HTTP gets the HTTP/1.1
///        fallback, gRPC a bare `Http2Transport`, so it cannot fall back by
///        construction (ICP 0042 §2).
[[nodiscard]] Expected<std::unique_ptr<internal::ITransport>, ConfigError> CreateTransport(
    Protocol protocol)
{
    if (protocol == Protocol::Http)
    {
        auto fallback = transport::FallbackTransport::Create(CreateReactor);
        if (!fallback)
        {
            return make_unexpected(TransportInitError("transport", fallback.error()));
        }
        return std::move(*fallback);
    }
    auto reactor = CreateReactor();
    if (!reactor)
    {
        return make_unexpected(TransportInitError("reactor", reactor.error()));
    }
    auto http2 = transport::Http2Transport::Create(std::move(*reactor));
    if (!http2)
    {
        return make_unexpected(TransportInitError("transport", http2.error()));
    }
    return std::move(*http2);
}

/// @brief The HTTP/2 transport, or none with an application `ExportTransport`:
///        no Http2Transport, no reactor, no I/O thread (ICP 0036 Decision 1).
[[nodiscard]] Expected<std::unique_ptr<internal::ITransport>, ConfigError> CreateTransportUnless(
    bool custom_transport, Protocol protocol)
{
    if (custom_transport)
    {
        return std::unique_ptr<internal::ITransport>{};
    }
    return CreateTransport(protocol);
}

constexpr std::string_view kHttpMetricsPath = "/v1/metrics";
constexpr std::string_view kGrpcMetricsPath =
    "/opentelemetry.proto.collector.metrics.v1.MetricsService/Export";
constexpr std::string_view kHttpLogsPath = "/v1/logs";
constexpr std::string_view kGrpcLogsPath =
    "/opentelemetry.proto.collector.logs.v1.LogsService/Export";

/// @brief Construct the wire codec for the configured protocol.
///
/// @param diag the diagnostics sink, which the codecs need rather than merely
///        accept: connect_failure, malformed_response and
///        decompression_too_large are all recorded inside the codec, and
///        passing nullptr here made every one of them invisible to
///        `GetExporterHealth()`.
[[nodiscard]] std::unique_ptr<internal::IWireCodec> BuildWireCodec(
    internal::ITransport* transport,
    const config::Config& cfg,
    std::vector<internal::HeaderField> extra_headers,
    internal::IAuthProvider* auth,
    internal::IDiagnosticsSink* diag,
    std::string_view signal_path = {})
{
    const auto host_port = cfg.endpoint.host + ":" + std::to_string(cfg.endpoint.port);
    if (cfg.protocol == Protocol::Grpc)
    {
        return std::make_unique<wire::GrpcWireCodec>(
            transport,
            wire::GrpcWireCodecConfig{
                .host = host_port,
                .scheme = cfg.endpoint.scheme,
                .extra_headers = std::move(extra_headers),
                .service_path = std::string{signal_path},
                .compression_gzip = cfg.compression_gzip,
                .max_decompressed_bytes = cfg.memory_limits.max_decompressed_bytes,
            },
            auth,
            diag,
            /*clock=*/nullptr,
            BuildConnectOptions(cfg));
    }
    return std::make_unique<wire::HttpWireCodec>(
        transport,
        wire::HttpWireCodecConfig{
            .host = host_port,
            .scheme = cfg.endpoint.scheme,
            .path = cfg.endpoint.path,
            .signal_path = std::string{signal_path},
            .extra_headers = std::move(extra_headers),
            .compression_gzip = cfg.compression_gzip,
            .max_decompressed_bytes = cfg.memory_limits.max_decompressed_bytes,
        },
        auth,
        diag,
        /*clock=*/nullptr,
        BuildConnectOptions(cfg));
}

[[nodiscard]] sdk::ViewRegistry BuildViewRegistry(std::vector<ViewConfig>& views)
{
    sdk::ViewRegistry reg;
    for (auto& v : views)
    {
        reg.Add(std::move(v));
    }
    return reg;
}

/// @brief Resolve the effective per-instrument cardinality cap.
///
/// Precedence (lowest to highest): SDK default → env var
/// `MICROTEL_METRIC_CARDINALITY_LIMIT` → `WithMetricLimits()`.
[[nodiscard]] std::size_t ResolveMaxCardinality(
    const std::optional<MetricLimitOptions>& explicit_limits) noexcept
{
    std::size_t cap = sdk::kDefaultMaxCardinality;
    const auto env_card = ParseEnvCardinality();
    if (env_card.has_value())
    {
        cap = *env_card;
    }
    if (explicit_limits.has_value())
    {
        cap = explicit_limits->max_cardinality;
    }
    return cap;
}

struct ExporterPack
{
    std::unique_ptr<internal::IWireCodec> codec;
    std::unique_ptr<internal::IWireCodec> metric_codec;
    std::unique_ptr<internal::IWireCodec> log_codec;
    /// Borrowed alias of `exporter` seen as a group exporter, for the span
    /// processor (design §3.6.1). Before `exporter` so one designated
    /// initializer list can take the alias and then move the owner.
    internal::IBatchGroupExporter* group_exporter = nullptr;
    std::unique_ptr<internal::IExporter> exporter;
    std::unique_ptr<internal::IMetricExporter> metric_exporter;
    std::unique_ptr<internal::ILogExporter> log_exporter;
};

/// @brief Build the auth provider, or nullptr when no callback was supplied.
[[nodiscard]] std::unique_ptr<internal::IAuthProvider> BuildAuthProvider(
    std::optional<AuthCallback>& cb, std::chrono::milliseconds cache_ttl)
{
    if (!cb)
    {
        return nullptr;
    }
    // NOLINTBEGIN(bugprone-unchecked-optional-access)
    return std::make_unique<config::CallbackAuthProvider>(std::move(*cb), cache_ttl);
    // NOLINTEND(bugprone-unchecked-optional-access)
}

/// @brief The retry policy all three signals share (issue #222).
///
/// `retry_budget` is the only retry axis TimeoutOptions exposes; the rest of
/// RetryPolicyConfig (attempts, backoff shape, jitter) has no config surface
/// and keeps its OTLP-recommended in-class defaults.
[[nodiscard]] exporter::RetryPolicyConfig SharedRetryPolicy(const config::Config& cfg)
{
    return exporter::RetryPolicyConfig{.retry_budget = cfg.timeouts.retry_budget};
}

/// @brief The trace exporter's configuration for either transport.
[[nodiscard]] exporter::OtlpExporterConfig TraceExporterConfig(const config::Config& cfg)
{
    // A request carries at most one processor batch's worth of spans, so
    // joining drained batches never makes a request larger than a batch the
    // processor could have cut (design §3.6.1). Fixed at build time: a later
    // SetBatchOptions retunes the processor, not this. The queue is budgeted
    // in spans, a fixed number of full batches' worth, so a drain split over
    // many leaves' Resources costs no more of it than one leaf's (issue #345).
    const std::size_t queued_spans = exporter::QueuedSpanBudget(cfg.batch.max_export_batch_size);
    return exporter::OtlpExporterConfig{
        .max_queue_size = queued_spans,
        .max_queued_spans = queued_spans,
        .export_deadline = cfg.timeouts.per_export,
        .retry_policy = SharedRetryPolicy(cfg),
        .max_spans_per_request = cfg.batch.max_export_batch_size,
        .max_request_bytes = 0,
        .on_shutdown_timeout = {},
    };
}

[[nodiscard]] ExporterPack BuildExporters(wire::OtlpEncoder* encoder,
                                          internal::ITransport* transport,
                                          internal::IAuthProvider* auth,
                                          const config::Config& cfg,
                                          internal::IDiagnosticsSink* diag)
{
    auto codec = BuildWireCodec(transport, cfg, ToHeaderFields(cfg.headers), auth, diag);
    const std::string_view metric_path =
        cfg.protocol == Protocol::Grpc ? kGrpcMetricsPath : kHttpMetricsPath;
    auto metric_codec =
        BuildWireCodec(transport, cfg, ToHeaderFields(cfg.headers), auth, diag, metric_path);
    const std::string_view log_path =
        cfg.protocol == Protocol::Grpc ? kGrpcLogsPath : kHttpLogsPath;
    auto log_codec =
        BuildWireCodec(transport, cfg, ToHeaderFields(cfg.headers), auth, diag, log_path);

    const exporter::OtlpExporterConfig ex_cfg = TraceExporterConfig(cfg);
    auto trace_exp = std::make_unique<exporter::OtlpExporter>(encoder, codec.get(), ex_cfg, diag);
    // One sink across all three signals: batches_sent / batches_failed are
    // therefore cross-signal aggregates (see docs/error-model.md §3).
    auto metric_exp = std::make_unique<exporter::OtlpMetricExporter>(
        encoder,
        metric_codec.get(),
        exporter::OtlpMetricExporterConfig{.export_deadline = cfg.timeouts.per_export,
                                           .retry_policy = SharedRetryPolicy(cfg),
                                           .on_shutdown_timeout = {}},
        diag);
    auto log_exp = std::make_unique<exporter::OtlpLogExporter>(
        encoder,
        log_codec.get(),
        exporter::OtlpLogExporterConfig{.export_deadline = cfg.timeouts.per_export,
                                        .retry_policy = SharedRetryPolicy(cfg),
                                        .on_shutdown_timeout = {}},
        diag);

    return ExporterPack{
        .codec = std::move(codec),
        .metric_codec = std::move(metric_codec),
        .log_codec = std::move(log_codec),
        .group_exporter = trace_exp.get(),
        .exporter = std::move(trace_exp),
        .metric_exporter = std::move(metric_exp),
        .log_exporter = std::move(log_exp),
    };
}

/// @brief The exporters for an application `ExportTransport` (ICP 0036):
///        one `ExportTransportCodec` per enabled signal over one channel, and
///        no metric or log exporter, so no worker, for a signal left off.
///
/// Every exporter's shutdown hook cancels the channel's in-flight `Send`, so
/// whichever exporter's wait expires first wakes it.
///
/// @param channel borrowed; must outlive every codec and exporter returned.
[[nodiscard]] ExporterPack BuildCustomExporters(wire::OtlpEncoder* encoder,
                                                wire::ExportTransportChannel* channel,
                                                const ExportTransportOptions& opts,
                                                const config::Config& cfg,
                                                internal::IDiagnosticsSink* diag)
{
    const auto cancel = [channel] { channel->CancelInFlight(); };
    ExporterPack pack;

    pack.codec = std::make_unique<wire::ExportTransportCodec>(
        channel, ExportSignal::Traces, opts.max_request_bytes);
    exporter::OtlpExporterConfig ex_cfg = TraceExporterConfig(cfg);
    ex_cfg.max_request_bytes = opts.max_request_bytes;
    ex_cfg.on_shutdown_timeout = cancel;
    auto trace_exp =
        std::make_unique<exporter::OtlpExporter>(encoder, pack.codec.get(), ex_cfg, diag);
    pack.group_exporter = trace_exp.get();
    pack.exporter = std::move(trace_exp);

    if (opts.metrics)
    {
        pack.metric_codec =
            std::make_unique<wire::ExportTransportCodec>(channel, ExportSignal::Metrics);
        pack.metric_exporter = std::make_unique<exporter::OtlpMetricExporter>(
            encoder,
            pack.metric_codec.get(),
            exporter::OtlpMetricExporterConfig{.export_deadline = cfg.timeouts.per_export,
                                               .retry_policy = SharedRetryPolicy(cfg),
                                               .on_shutdown_timeout = cancel},
            diag);
    }
    if (opts.logs)
    {
        pack.log_codec = std::make_unique<wire::ExportTransportCodec>(channel, ExportSignal::Logs);
        pack.log_exporter = std::make_unique<exporter::OtlpLogExporter>(
            encoder,
            pack.log_codec.get(),
            exporter::OtlpLogExporterConfig{.export_deadline = cfg.timeouts.per_export,
                                            .retry_policy = SharedRetryPolicy(cfg),
                                            .on_shutdown_timeout = cancel},
            diag);
    }
    return pack;
}

/// @brief Build the span processor from the resolved configuration.
///
/// No scope is passed: the processor stamps each batch with the scope that
/// arrived with the span, i.e. the one `GetTracer` was called with (ICP 0023).
/// The service identity reaches the wire through the Resource.
///
/// @param exporter non-owning; must outlive the processor.
/// @param resource shared with every batch the processor emits.
/// @param cfg borrowed; read for the batch options and the two memory limits
///        the processor enforces (`max_record_bytes`, `max_total_queue_bytes`).
/// @param diag non-owning diagnostics sink.
/// @param group_exporter @p exporter seen as a group exporter, so each drain
///        is handed over in one call (design §3.6.1). Non-owning.
[[nodiscard]] std::unique_ptr<sdk::BatchSpanProcessor> BuildSpanProcessor(
    internal::IExporter* exporter,
    std::shared_ptr<const Resource> resource,
    const config::Config& cfg,
    internal::IDiagnosticsSink* diag,
    internal::IBatchGroupExporter* group_exporter)
{
    return std::make_unique<sdk::BatchSpanProcessor>(exporter,
                                                     std::move(resource),
                                                     cfg.batch,
                                                     cfg.memory_limits.max_record_bytes,
                                                     cfg.memory_limits.max_total_queue_bytes,
                                                     diag,
                                                     group_exporter);
}

/// @brief The decoder the leaf receiver owns: the upb one when the
///        concentrator is compiled in and enabled, else none.
[[nodiscard]] std::unique_ptr<internal::IOtlpTraceDecoder> MakeLeafDecoder(bool enabled)
{
#ifdef MICROTEL_WITH_CONCENTRATOR
    if (enabled)
    {
        return std::make_unique<wire::OtlpTraceDecoder>();
    }
#else
    (void)enabled;
#endif
    return nullptr;
}

/// @brief Check the resolved concentrator options — file, environment and
///        `WithLeafReceiver` — or refuse them in a build without the
///        concentrator (design §6.2).
///
/// `ConfigError::Kind::FeatureNotCompiled` is ICP 0030's, still a draft, so a
/// build without the option refuses with `InvalidValue` and names the option in
/// the message. That changes to `FeatureNotCompiled` when ICP 0030 lands.
[[nodiscard]] Expected<void, ConfigError> CheckLeafReceiver(const LeafReceiverOptions& opts)
{
    if (!opts.enabled)
    {
        return {};
    }
#ifdef MICROTEL_WITH_CONCENTRATOR
    return sdk::ValidateLeafReceiverOptions(opts);
#else
    return make_unexpected(ConfigError{
        .kind = ConfigError::Kind::InvalidValue,
        .field = "concentrator.enabled",
        .message = "the leaf receiver is not compiled into this build of microtel; rebuild with "
                   "-DMICROTEL_WITH_CONCENTRATOR=ON"});
#endif
}

}  // namespace

// ---------------------------------------------------------------------------
// Impl — config loading and code overrides
// ---------------------------------------------------------------------------

namespace
{

/// `ConfigError::field` for every custom-transport refusal (ICP 0036).
constexpr const char* kExportTransportField = "exporter.transport";

/// What `Validate` parses in place of an endpoint with a custom transport. It
/// is never connected to: no HTTP/2 transport exists to connect with. https,
/// so that nothing downstream mistakes it for plaintext.
constexpr std::string_view kCustomTransportPlaceholderEndpoint = "https://localhost:4318";

[[nodiscard]] bool TlsConfigured(const TlsOptions& tls) noexcept
{
    return tls.insecure || !tls.ca_bundle.empty() || !tls.client_cert.empty() ||
           !tls.client_key.empty() || !tls.sni_override.empty();
}

/// @brief Drop the exporter settings a custom transport gives no meaning to,
///        as the file and environment left them (ICP 0036 Decision 1).
/// @return the names of those that were set, for one Warn.
[[nodiscard]] std::vector<std::string_view> DropExporterSettings(config::Config& cfg)
{
    std::vector<std::string_view> ignored;
    const std::array<std::pair<bool, std::string_view>, 5> checks{{
        {!cfg.endpoint_url.empty(), "endpoint"},
        {cfg.protocol_explicit, "protocol"},
        {!cfg.headers.empty(), "headers"},
        {cfg.compression_gzip, "compression"},
        {TlsConfigured(cfg.tls), "tls"},
    }};
    for (const auto& [set, name] : checks)
    {
        if (set)
        {
            ignored.push_back(name);
        }
    }
    const config::Config defaults;
    cfg.endpoint_url = std::string{kCustomTransportPlaceholderEndpoint};
    cfg.protocol = defaults.protocol;
    cfg.protocol_explicit = false;
    cfg.headers.clear();
    cfg.compression_gzip = false;
    cfg.tls = TlsOptions{};
    return ignored;
}

/// @brief The warnings a custom-transport build owes, emitted once the log
///        level is applied: the settings dropped, and a trace signal left off.
void WarnOnCustomTransport(const std::vector<std::string_view>& ignored,
                           const ExportTransportOptions& opts)
{
    if (!ignored.empty())
    {
        std::string names;
        for (const auto name : ignored)
        {
            names += names.empty() ? "" : ", ";
            names += name;
        }
        internal::LogImpl(LogLevel::Warn,
                          "export transport: exporter settings from the environment or file "
                          "are ignored with a custom transport: " +
                              names);
    }
    if (!opts.traces)
    {
        internal::LogImpl(LogLevel::Warn,
                          "export transport: traces are off in ExportTransportOptions; the "
                          "sampler is always-off and no span is recorded");
    }
}

}  // namespace

void SdkBuilder::Impl::PrepareCustomTransport(const std::vector<std::string_view>& ignored)
{
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access) — only called when set
    const ExportTransportOptions& opts = *export_transport_opts;
    WarnOnCustomTransport(ignored, opts);
    if (!opts.traces)
    {
        sampler = MakeAlwaysOffSampler();
    }
}

Expected<void, ConfigError> SdkBuilder::Impl::CheckExportTransport() const
{
    if (!export_transport_opts)
    {
        return {};
    }
    if (export_transport == nullptr)
    {
        return make_unexpected(ConfigError{.kind = ConfigError::Kind::InvalidValue,
                                           .field = kExportTransportField,
                                           .message = "WithExportTransport needs a non-null "
                                                      "ExportTransport"});
    }
    const std::array<std::pair<bool, std::string_view>, 6> conflicts{{
        {endpoint.has_value(), "WithEndpoint"},
        {protocol.has_value(), "WithProtocol"},
        {headers.has_value(), "WithHeaders"},
        {tls.has_value(), "WithTls"},
        {auth_cb.has_value(), "WithAuthProvider"},
        {compression_gzip.has_value(), "WithCompressionGzip"},
    }};
    for (const auto& [set, name] : conflicts)
    {
        if (set)
        {
            return make_unexpected(ConfigError{
                .kind = ConfigError::Kind::InvalidValue,
                .field = kExportTransportField,
                .message = std::string{name} +
                           " has no meaning with WithExportTransport, which replaces the "
                           "HTTP/2 exporter; set one or the other"});
        }
    }
    return {};
}

Expected<config::Config, ConfigError> SdkBuilder::Impl::LoadConfig(
    std::vector<std::string_view>& ignored) const
{
    config::Config cfg;
    if (file_path)
    {
        auto file_cfg = config::LoadToml(*file_path);
        if (!file_cfg)
        {
            return make_unexpected(file_cfg.error());
        }
        cfg = std::move(*file_cfg);
    }
    if (auto r = config::OverlayEnv(cfg); !r)
    {
        return make_unexpected(r.error());
    }
    if (export_transport_opts)
    {
        // Before the code layer, which CheckExportTransport has already
        // guaranteed sets none of these, and before Validate, which would
        // otherwise reject a malformed endpoint nothing will ever use.
        ignored = DropExporterSettings(cfg);
    }
    ApplyExporterOverrides(cfg);
    ApplyResourceOverrides(cfg);
    if (leaf_receiver)
    {
        config::MergeLeafReceiverOptions(cfg.concentrator, *leaf_receiver);
    }
    if (auto r = config::Validate(cfg); !r)
    {
        return make_unexpected(r.error());
    }
    if (auth_cb)
    {
        if (auto r = config::CheckNoStaticAuthorization(cfg.headers); !r)
        {
            return make_unexpected(r.error());
        }
    }
    return cfg;
}

void SdkBuilder::Impl::ApplyExporterOverrides(config::Config& cfg) const
{
    if (endpoint)
    {
        cfg.endpoint_url = *endpoint;
    }
    if (protocol)
    {
        cfg.protocol = *protocol;
        cfg.protocol_explicit = true;
    }
    if (compression_gzip)
    {
        cfg.compression_gzip = *compression_gzip;
    }
    if (headers)
    {
        config::MergeHeaders(cfg.headers, *headers);
    }
    if (tls)
    {
        cfg.tls = *tls;
    }
    if (metric_interval)
    {
        cfg.metric_interval = *metric_interval;
    }
    if (metric_temporality)
    {
        cfg.metric_temporality = *metric_temporality;
    }
}

void SdkBuilder::Impl::ApplyResourceOverrides(config::Config& cfg) const
{
    if (service_name)
    {
        cfg.service_name = *service_name;
    }
    if (service_version)
    {
        cfg.service_version = *service_version;
    }
    if (resource_attrs)
    {
        config::MergeResourceAttrs(cfg.resource_attrs, *resource_attrs);
    }
    if (batch)
    {
        cfg.batch = *batch;
    }
    if (span_limits)
    {
        cfg.span_limits = *span_limits;
    }
    if (memory_limits)
    {
        cfg.memory_limits = *memory_limits;
    }
    if (timeouts)
    {
        cfg.timeouts = *timeouts;
    }
}

// ---------------------------------------------------------------------------
// Profile registration (ICP 0027)
// ---------------------------------------------------------------------------

namespace
{

/// Undotted on purpose. `ConfigError::field` is a dotted path *when the setting
/// has one*, and the profile name has no `microtel.toml` key and no environment
/// variable: it is chosen at the call site, by `WithProfileName`. Calling it
/// `sdk.profile_name` would send a reader to `docs/configuration.md` looking for
/// a key that is not there.
constexpr const char* kProfileNameField = "profile_name";

[[nodiscard]] ConfigError DuplicateProfileNameError(std::string_view name)
{
    return ConfigError{
        .kind = ConfigError::Kind::DuplicateProfileName,
        .field = kProfileNameField,
        .message = "a live provider is already registered under profile name '" +
                   std::string{name} +
                   "'; shutting a provider down does not release its name, destroying it does"};
}

/// The name this build registers under, or the error that stops it.
///
/// Run before anything is constructed, so the ordinary sequential duplicate
/// costs nothing. It cannot settle the concurrent case — two `Build()`s of one
/// name both pass here — which is what the claim after construction is for.
[[nodiscard]] Expected<std::string, ConfigError> ResolveProfileName(
    const std::optional<std::string>& configured)
{
    std::string name = configured.value_or(std::string{kDefaultProfileName});
    if (name.empty())
    {
        return make_unexpected(
            ConfigError{.kind = ConfigError::Kind::InvalidValue,
                        .field = kProfileNameField,
                        .message = "profile name must not be empty; omit WithProfileName "
                                   "to build the default profile"});
    }
    if (sdk::FindProvider(name) != nullptr)
    {
        return make_unexpected(DuplicateProfileNameError(name));
    }
    return name;
}

/// The error a refused registration becomes. `kMaxProfiles` is internal, so
/// this message is the only place a consumer can learn the number.
[[nodiscard]] ConfigError RegistrationError(sdk::RegistrationResult result, std::string_view name)
{
    if (result == sdk::RegistrationResult::CapacityExhausted)
    {
        return ConfigError{.kind = ConfigError::Kind::ProfileLimitExceeded,
                           .field = kProfileNameField,
                           .message = "the process already holds the maximum of " +
                                      std::to_string(sdk::kMaxProfiles) + " live providers"};
    }
    return DuplicateProfileNameError(name);
}

}  // namespace

// ---------------------------------------------------------------------------
// Build
// ---------------------------------------------------------------------------

Expected<std::shared_ptr<Provider>, ConfigError> SdkBuilder::Build()
{
    if (m_impl->consumed)
    {
        return make_unexpected(ConfigError{.kind = ConfigError::Kind::BuildAlreadyConsumed,
                                           .field = {},
                                           .message = "SdkBuilder::Build() called more than once"});
    }
    m_impl->consumed = true;

    // --- Step 0: the profile this provider will be registered under ---------
    const auto profile = ResolveProfileName(m_impl->profile_name);
    if (!profile)
    {
        return make_unexpected(profile.error());
    }

    // --- Step 0b: an application transport (ICP 0036) ----------------------
    if (auto r = m_impl->CheckExportTransport(); !r)
    {
        return make_unexpected(r.error());
    }
    const bool custom_transport = m_impl->export_transport_opts.has_value();

    // --- Steps 1–2: assemble and validate Config (file → env → code) -------
    std::vector<std::string_view> ignored_settings;
    auto cfg_result = m_impl->LoadConfig(ignored_settings);
    if (!cfg_result)
    {
        return make_unexpected(cfg_result.error());
    }
    const config::Config cfg = std::move(*cfg_result);
    ApplyLogLevel(cfg);
    WarnOnUnknownKeys(cfg);
    if (custom_transport)
    {
        m_impl->PrepareCustomTransport(ignored_settings);
    }
    else
    {
        WarnOnRiskyConfig(cfg);
    }

    // --- Step 2b: the leaf receiver (ICP 0034, design §4.3, §6.2) ----------
    if (auto r = CheckLeafReceiver(cfg.concentrator); !r)
    {
        return make_unexpected(r.error());
    }

    // --- Step 3: resource (configuration.md §3.2 — defaults, detectors, env, user)
    auto resource_result = sdk::BuildResource(cfg, m_impl->resource_detectors, *profile);
    if (!resource_result)
    {
        return make_unexpected(resource_result.error());
    }
    auto resource = std::make_shared<const Resource>(std::move(*resource_result));

    // --- Step 4: auth provider ----------------------------------------------
    auto auth = BuildAuthProvider(m_impl->auth_cb, m_impl->auth_cache_ttl);

    // --- Steps 5–6: transport -----------------------------------------------
    auto transport_result = CreateTransportUnless(custom_transport, cfg.protocol);
    if (!transport_result)
    {
        return make_unexpected(transport_result.error());
    }

    // --- Steps 7–12: the pipeline, and the profile it is registered under ---
    return m_impl->Assemble(
        cfg, std::move(resource), std::move(auth), std::move(*transport_result), *profile);
}

Expected<std::shared_ptr<Provider>, ConfigError> SdkBuilder::Impl::Assemble(
    const config::Config& cfg,
    std::shared_ptr<const Resource> resource,
    std::unique_ptr<internal::IAuthProvider> auth,
    std::unique_ptr<internal::ITransport> transport,
    const std::string& profile_name)
{
    // --- Steps 7–9: encoder + codecs + exporters ----------------------------
    const bool leaf_receiver_enabled = cfg.concentrator.enabled;
    auto encoder = std::make_unique<wire::OtlpEncoder>();
    // Created before the exporters because they borrow it; ownership moves
    // into the Provider below, which declares it first and so destroys it last.
    auto diagnostics = std::make_unique<sdk::DiagnosticsCounters>();
    std::unique_ptr<wire::ExportTransportChannel> export_channel;
    ExporterPack exporters;
    if (export_transport_opts)
    {
        export_channel =
            std::make_unique<wire::ExportTransportChannel>(std::move(export_transport));
        exporters = BuildCustomExporters(
            encoder.get(), export_channel.get(), *export_transport_opts, cfg, diagnostics.get());
    }
    else
    {
        exporters =
            BuildExporters(encoder.get(), transport.get(), auth.get(), cfg, diagnostics.get());
    }

    // --- Step 10: processor -------------------------------------------------
    auto processor = BuildSpanProcessor(
        exporters.exporter.get(), resource, cfg, diagnostics.get(), exporters.group_exporter);

    // --- Step 11: resolve cardinality cap and build view registry ------------
    const std::size_t max_cardinality = ResolveMaxCardinality(metric_limits);
    auto provider = std::make_shared<sdk::SdkProvider>(sdk::SdkProviderArgs{
        .diagnostics = std::move(diagnostics),
        .encoder = std::move(encoder),
        .auth = std::move(auth),
        .transport = std::move(transport),
        .export_channel = std::move(export_channel),
        .codec = std::move(exporters.codec),
        .exporter = std::move(exporters.exporter),
        .batch_span_processor = processor.get(),
        .processor = std::move(processor),
        .resource = std::move(resource),
        .sampler = std::move(sampler),
        .span_limits = cfg.span_limits,
        .connect_opts = BuildConnectOptions(cfg),
        .metric_codec = std::move(exporters.metric_codec),
        .metric_exporter = std::move(exporters.metric_exporter),
        .metric_interval = cfg.metric_interval,
        .metric_temporality = cfg.metric_temporality,
        .metric_max_cardinality = max_cardinality,
        .view_registry = BuildViewRegistry(views),
        .log_codec = std::move(exporters.log_codec),
        .log_exporter = std::move(exporters.log_exporter),
        .log_batch_opts = cfg.batch,
        .profile_name = profile_name,
        .leaf_receiver = leaf_receiver_enabled ? std::optional{cfg.concentrator} : std::nullopt,
        .leaf_decoder = MakeLeafDecoder(leaf_receiver_enabled),
    });

    // --- Step 12: claim the profile -----------------------------------------
    // After construction, because the name is compared against providers that
    // are live, and this one is not live until it exists. A refusal here costs
    // one construction and an immediate teardown — a startup-path cost, paid
    // once, by a program with a bug (ICP 0027 §2).
    if (const sdk::RegistrationResult registered = sdk::RegisterProvider(provider.get());
        registered != sdk::RegistrationResult::Registered)
    {
        return make_unexpected(RegistrationError(registered, profile_name));
    }
    return provider;
}

}  // namespace microtel
