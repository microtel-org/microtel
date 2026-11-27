// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#include "common/config/env_resolver.hpp"

#include "microtel/attribute.hpp"
#include "microtel/error.hpp"
#include "microtel/leaf_receiver.hpp"
#include "microtel/protocol.hpp"
#include "microtel/sdk_builder.hpp"

#include "common/config/concentrator_config.hpp"
#include "common/config/table_merge.hpp"

#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace microtel::config
{

namespace
{

/// Return the value of an env var, or empty string if unset / empty.
[[nodiscard]] std::string GetEnv(const char* name) noexcept
{
    // NOLINTNEXTLINE(concurrency-mt-unsafe) — read-only at config load time
    const char* const val = ::getenv(name);
    return (val != nullptr) ? std::string{val} : std::string{};
}

/// Parse an integer millisecond value from a string.
[[nodiscard]] microtel::Expected<std::chrono::milliseconds, ConfigError> ParseMs(
    std::string_view sv, const char* var_name)
{
    std::int64_t ms = 0;
    const auto [ptr, ec] = std::from_chars(sv.data(), sv.data() + sv.size(), ms);
    if (ec != std::errc{} || ptr != sv.data() + sv.size())
    {
        return microtel::make_unexpected(
            ConfigError{.kind = ConfigError::Kind::EnvParseFailure,
                        .field = var_name,
                        .message = std::string{var_name} + ": expected integer milliseconds"});
    }
    return std::chrono::milliseconds{ms};
}

/// Optional whitespace around a W3C Baggage list member, key or value.
constexpr std::string_view kOws = " \t";
constexpr std::size_t kPercentEscapeLen = 3;  // '%' and two hex digits
constexpr int kHexBase = 16;

/// Strip leading and trailing spaces and tabs.
[[nodiscard]] std::string_view TrimOws(std::string_view sv) noexcept
{
    const auto first = sv.find_first_not_of(kOws);
    if (first == std::string_view::npos)
    {
        return {};
    }
    const auto last = sv.find_last_not_of(kOws);
    return sv.substr(first, last - first + 1);
}

/// Decode `%XX` escapes. `std::nullopt` on a `%` not followed by two hex digits.
[[nodiscard]] std::optional<std::string> PercentDecode(std::string_view sv)
{
    std::string out;
    out.reserve(sv.size());
    while (!sv.empty())
    {
        const auto pct = sv.find('%');
        out.append(sv.substr(0, pct));
        if (pct == std::string_view::npos)
        {
            break;
        }
        const std::string_view hex = sv.substr(pct + 1, kPercentEscapeLen - 1);
        unsigned int byte = 0;
        const auto [ptr, ec] = std::from_chars(hex.data(), hex.data() + hex.size(), byte, kHexBase);
        if (hex.size() != kPercentEscapeLen - 1 || ec != std::errc{} ||
            ptr != hex.data() + hex.size())
        {
            return std::nullopt;
        }
        out.push_back(static_cast<char>(byte));
        sv = sv.substr(pct + kPercentEscapeLen);
    }
    return out;
}

/// Parse a "key=value,key2=value2" list into KeyValue pairs.
///
/// The OTel key=value variables use the W3C Baggage list format without
/// metadata (issue #413): whitespace around each key and value is dropped, and
/// values are percent-decoded after trimming, so `%20` survives as a space.
/// Keys are not decoded. Each token must contain a '='; empty tokens are
/// skipped. Both errors are `EnvParseFailure` and never echo a value, which may
/// be a secret: a missing '=' names the entry's 1-based position in the list,
/// a malformed escape names the key.
[[nodiscard]] microtel::Expected<std::vector<KeyValue>, ConfigError> ParseKeyValueList(
    std::string_view sv, const char* var_name)
{
    std::vector<KeyValue> result;
    std::size_t entry = 0;
    while (!sv.empty())
    {
        ++entry;
        const auto comma = sv.find(',');
        const std::string_view token = TrimOws(sv.substr(0, comma));
        sv = (comma == std::string_view::npos) ? std::string_view{} : sv.substr(comma + 1);
        if (token.empty())
        {
            continue;
        }
        const auto eq = token.find('=');
        if (eq == std::string_view::npos)
        {
            return microtel::make_unexpected(
                ConfigError{.kind = ConfigError::Kind::EnvParseFailure,
                            .field = var_name,
                            .message = std::string{var_name} + ": entry " + std::to_string(entry) +
                                       " is not a key=value pair (missing '=')"});
        }
        const std::string_view key = TrimOws(token.substr(0, eq));
        auto value = PercentDecode(TrimOws(token.substr(eq + 1)));
        if (!value)
        {
            return microtel::make_unexpected(
                ConfigError{.kind = ConfigError::Kind::EnvParseFailure,
                            .field = var_name,
                            .message = std::string{var_name} +
                                       ": malformed percent-escape in "
                                       "the value of " +
                                       std::string{key} + " (expected %XX, two hex digits)"});
        }
        result.push_back({.key = std::string{key}, .value = std::move(*value)});
    }
    return result;
}

/// Parse OTEL_EXPORTER_OTLP_PROTOCOL → Protocol.
[[nodiscard]] microtel::Expected<Protocol, ConfigError> ParseProtocol(std::string_view sv)
{
    if (sv == "grpc")
    {
        return Protocol::Grpc;
    }
    if (sv == "http/protobuf" || sv == "http")
    {
        return Protocol::Http;
    }
    return microtel::make_unexpected(
        ConfigError{.kind = ConfigError::Kind::EnvParseFailure,
                    .field = "OTEL_EXPORTER_OTLP_PROTOCOL",
                    .message = R"(expected "grpc" or "http/protobuf")"});
}

/// Apply OTEL_EXPORTER_OTLP_TIMEOUT to cfg if the env var is set.
[[nodiscard]] microtel::Expected<void, ConfigError> OverlayTimeout(Config& cfg)
{
    const auto v = GetEnv("OTEL_EXPORTER_OTLP_TIMEOUT");
    if (v.empty())
    {
        return {};
    }
    auto ms = ParseMs(v, "OTEL_EXPORTER_OTLP_TIMEOUT");
    if (!ms)
    {
        return microtel::make_unexpected(ms.error());
    }
    cfg.timeouts.per_export = *ms;
    return {};
}

/// Apply OTEL_EXPORTER_OTLP_HEADERS to cfg if the env var is set.
[[nodiscard]] microtel::Expected<void, ConfigError> OverlayHeaders(Config& cfg)
{
    const auto v = GetEnv("OTEL_EXPORTER_OTLP_HEADERS");
    if (v.empty())
    {
        return {};
    }
    auto headers = ParseKeyValueList(v, "OTEL_EXPORTER_OTLP_HEADERS");
    if (!headers)
    {
        return microtel::make_unexpected(headers.error());
    }
    MergeHeaders(cfg.headers, *headers);
    return {};
}

/// Apply OTEL_EXPORTER_OTLP_PROTOCOL to cfg if the env var is set.
[[nodiscard]] microtel::Expected<void, ConfigError> OverlayProtocol(Config& cfg)
{
    const auto v = GetEnv("OTEL_EXPORTER_OTLP_PROTOCOL");
    if (v.empty())
    {
        return {};
    }
    auto proto = ParseProtocol(v);
    if (!proto)
    {
        return microtel::make_unexpected(proto.error());
    }
    cfg.protocol = *proto;
    cfg.protocol_explicit = true;
    return {};
}

/// Apply OTEL_METRIC_EXPORT_INTERVAL to cfg if the env var is set.
[[nodiscard]] microtel::Expected<void, ConfigError> OverlayMetricInterval(Config& cfg)
{
    const auto v = GetEnv("OTEL_METRIC_EXPORT_INTERVAL");
    if (v.empty())
    {
        return {};
    }
    auto ms = ParseMs(v, "OTEL_METRIC_EXPORT_INTERVAL");
    if (!ms)
    {
        return microtel::make_unexpected(ms.error());
    }
    cfg.metric_interval = *ms;
    return {};
}

/// Apply OTEL_EXPORTER_OTLP_METRICS_TEMPORALITY_PREFERENCE to cfg if the env var is set.
[[nodiscard]] microtel::Expected<void, ConfigError> OverlayMetricTemporality(Config& cfg)
{
    const auto v = GetEnv("OTEL_EXPORTER_OTLP_METRICS_TEMPORALITY_PREFERENCE");
    if (v.empty())
    {
        return {};
    }
    if (v == "cumulative")
    {
        cfg.metric_temporality = microtel::TemporalityPreference::Cumulative;
        return {};
    }
    if (v == "delta")
    {
        cfg.metric_temporality = microtel::TemporalityPreference::Delta;
        return {};
    }
    if (v == "lowmemory")
    {
        cfg.metric_temporality = microtel::TemporalityPreference::LowMemory;
        return {};
    }
    return microtel::make_unexpected(
        ConfigError{.kind = ConfigError::Kind::EnvParseFailure,
                    .field = "OTEL_EXPORTER_OTLP_METRICS_TEMPORALITY_PREFERENCE",
                    .message = R"(expected "cumulative", "delta", or "lowmemory")"});
}

/// Apply OTEL_RESOURCE_ATTRIBUTES to cfg if the env var is set.
[[nodiscard]] microtel::Expected<void, ConfigError> OverlayResourceAttrs(Config& cfg)
{
    const auto v = GetEnv("OTEL_RESOURCE_ATTRIBUTES");
    if (v.empty())
    {
        return {};
    }
    auto attrs = ParseKeyValueList(v, "OTEL_RESOURCE_ATTRIBUTES");
    if (!attrs)
    {
        return microtel::make_unexpected(attrs.error());
    }
    MergeResourceAttrs(cfg.resource_attrs, *attrs);
    return {};
}

/// Apply MICROTEL_RESOURCE_DETECTORS_STRICT to cfg if the env var is set.
///
/// Unlike `OTEL_EXPORTER_OTLP_COMPRESSION`, an unrecognised value is rejected
/// rather than read as "off": the whole point of the setting is to turn a
/// silently-skipped detector into a loud failure, and a typo that quietly left
/// it disabled would defeat that.
[[nodiscard]] microtel::Expected<void, ConfigError> OverlayResourceDetectorsStrict(Config& cfg)
{
    const auto v = GetEnv("MICROTEL_RESOURCE_DETECTORS_STRICT");
    if (v.empty())
    {
        return {};
    }
    if (v == "true" || v == "1")
    {
        cfg.resource_detectors_strict = true;
        return {};
    }
    if (v == "false" || v == "0")
    {
        cfg.resource_detectors_strict = false;
        return {};
    }
    return microtel::make_unexpected(
        ConfigError{.kind = ConfigError::Kind::EnvParseFailure,
                    .field = "MICROTEL_RESOURCE_DETECTORS_STRICT",
                    .message = R"(expected "true"/"1" or "false"/"0")"});
}

/// Apply MICROTEL_LOG_LEVEL to cfg if the env var is set (ICP 0026 §6).
///
/// Rejected rather than ignored on a typo, for the reason
/// `MICROTEL_RESOURCE_DETECTORS_STRICT` is: an operator who raises the level to
/// debug a problem and quietly gets the default back has been told nothing.
[[nodiscard]] microtel::Expected<void, ConfigError> OverlayLogLevel(Config& cfg)
{
    const auto v = GetEnv("MICROTEL_LOG_LEVEL");
    if (v.empty())
    {
        return {};
    }
    const auto parsed = ParseLogLevel(v);
    if (!parsed)
    {
        return microtel::make_unexpected(
            ConfigError{.kind = ConfigError::Kind::EnvParseFailure,
                        .field = "MICROTEL_LOG_LEVEL",
                        .message = R"(expected "trace", "debug", "info", "warn" or "error")"});
    }
    cfg.log_level = *parsed;
    return {};
}

/// Apply one variable whose text @p parse turns into a value, or refuses with
/// @p expected in the message.
template <typename T, typename Parse>
[[nodiscard]] microtel::Expected<void, ConfigError> OverlayParsed(const char* var_name,
                                                                  Parse parse,
                                                                  std::string_view expected,
                                                                  T& out)
{
    const auto v = GetEnv(var_name);
    if (v.empty())
    {
        return {};
    }
    auto parsed = parse(std::string_view{v});
    if (!parsed.has_value())
    {
        return microtel::make_unexpected(
            ConfigError{.kind = ConfigError::Kind::EnvParseFailure,
                        .field = var_name,
                        .message = std::string{var_name} + ": expected " + std::string{expected}});
    }
    out = *parsed;
    return {};
}

[[nodiscard]] std::optional<bool> ParseBool(std::string_view v) noexcept
{
    if (v == "true" || v == "1")
    {
        return true;
    }
    if (v == "false" || v == "0")
    {
        return false;
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<std::uint32_t> ParseCount(std::string_view v) noexcept
{
    std::uint32_t n = 0;
    const auto [ptr, ec] = std::from_chars(v.data(), v.data() + v.size(), n);
    if (ec != std::errc{} || ptr != v.data() + v.size())
    {
        return std::nullopt;
    }
    return n;
}

/// The `MICROTEL_CONCENTRATOR_*` variables (`docs/leaf-concentrator-design.md`
/// §4.3): scalars only, plus the fleet-wide Resource defaults. Per-leaf
/// settings have no environment form.
[[nodiscard]] microtel::Expected<void, ConfigError> OverlayConcentratorEnv(Config& cfg)
{
    LeafReceiverOptions& o = cfg.concentrator;
    auto r = OverlayParsed(
        "MICROTEL_CONCENTRATOR_ENABLED", ParseBool, R"("true"/"1" or "false"/"0")", o.enabled);
    if (r)
    {
        r = OverlayParsed("MICROTEL_CONCENTRATOR_MAX_PAYLOAD_BYTES",
                          ParseByteSize,
                          R"(a byte count, optionally with a B, KiB or MiB suffix)",
                          o.max_payload_bytes);
    }
    if (r)
    {
        r = OverlayParsed(
            "MICROTEL_CONCENTRATOR_MAX_LEAVES", ParseCount, "a decimal integer", o.max_leaves);
    }
    if (r)
    {
        r = OverlayParsed("MICROTEL_CONCENTRATOR_UNKNOWN_LEAF",
                          ParseUnknownLeafPolicy,
                          R"("accept" or "reject")",
                          o.unknown_leaf);
    }
    if (r)
    {
        r = OverlayParsed(
            "MICROTEL_CONCENTRATOR_DEFAULT_TIME_MODE",
            ParseDefaultTimeMode,
            R"("auto", "concentrator_stamped", "sync_relative", "boot_relative" or "unix")",
            o.default_time_mode);
    }
    if (!r)
    {
        return r;
    }
    const auto v = GetEnv("MICROTEL_CONCENTRATOR_RESOURCE_ATTRIBUTES");
    if (v.empty())
    {
        return {};
    }
    auto attrs = ParseKeyValueList(v, "MICROTEL_CONCENTRATOR_RESOURCE_ATTRIBUTES");
    if (!attrs)
    {
        return microtel::make_unexpected(attrs.error());
    }
    MergeResourceAttrs(o.leaf_defaults_resource, *attrs);
    return {};
}

/// The signal infixes of the per-signal variables, in `SignalIndex` order.
constexpr std::array<std::string_view, kSignalCount> kSignalInfixes{"TRACES", "METRICS", "LOGS"};

/// A per-signal exporter setting microtel does not implement, and the shared
/// setting that applies to every signal instead.
struct UnsupportedSignalSetting
{
    std::string_view suffix;  ///< e.g. `HEADERS` in `OTEL_EXPORTER_OTLP_TRACES_HEADERS`
    std::string_view shared;  ///< what the Warn points at instead
};

constexpr std::array<UnsupportedSignalSetting, 8> kUnsupportedSignalSettings{{
    {.suffix = "HEADERS", .shared = "OTEL_EXPORTER_OTLP_HEADERS"},
    {.suffix = "PROTOCOL", .shared = "OTEL_EXPORTER_OTLP_PROTOCOL"},
    {.suffix = "TIMEOUT", .shared = "OTEL_EXPORTER_OTLP_TIMEOUT"},
    {.suffix = "COMPRESSION", .shared = "OTEL_EXPORTER_OTLP_COMPRESSION"},
    {.suffix = "CERTIFICATE", .shared = "OTEL_EXPORTER_OTLP_CERTIFICATE"},
    {.suffix = "CLIENT_KEY", .shared = "tls.client_key (WithTls or [tls])"},
    {.suffix = "CLIENT_CERTIFICATE", .shared = "tls.client_cert (WithTls or [tls])"},
    {.suffix = "INSECURE", .shared = "tls.insecure (WithTls or [tls])"},
}};

/// Read OTEL_EXPORTER_OTLP_{TRACES,METRICS,LOGS}_ENDPOINT. `Validate` checks
/// them and resolves them against the base endpoint (docs/configuration.md §3.3).
void OverlaySignalEndpoints(Config& cfg)
{
    for (std::size_t i = 0; i < kSignalCount; ++i)
    {
        if (auto v = GetEnv(std::string{kSignalEndpointVars.at(i)}.c_str()); !v.empty())
        {
            cfg.signal_endpoint_urls.at(i) = std::move(v);
        }
    }
}

/// Record one warning if `OTEL_EXPORTER_OTLP_<signal>_<setting.suffix>` is
/// set. The value is never read, so it is never echoed either.
void WarnIfSignalVarSet(Config& cfg,
                        std::string_view signal,
                        const UnsupportedSignalSetting& setting)
{
    std::string name = "OTEL_EXPORTER_OTLP_";
    name.append(signal).append("_").append(setting.suffix);
    if (GetEnv(name.c_str()).empty())
    {
        return;
    }
    cfg.config_warnings.push_back(
        name +
        " is set but per-signal exporter settings are not supported, so it is ignored; microtel "
        "exports every signal over one connection, and " +
        std::string{setting.shared} + " applies to all of them");
}

/// Record one warning for each per-signal exporter variable that is set but
/// not implemented.
void WarnOnUnsupportedSignalVars(Config& cfg)
{
    for (const std::string_view signal : kSignalInfixes)
    {
        for (const auto& setting : kUnsupportedSignalSettings)
        {
            WarnIfSignalVarSet(cfg, signal, setting);
        }
    }
}

/// The exporter half of the overlay: endpoint, protocol, headers, timeout,
/// compression and CA bundle.
[[nodiscard]] microtel::Expected<void, ConfigError> OverlayExporterEnv(Config& cfg)
{
    // OTEL_EXPORTER_OTLP_ENDPOINT
    if (const auto v = GetEnv("OTEL_EXPORTER_OTLP_ENDPOINT"); !v.empty())
    {
        cfg.endpoint_url = v;
        cfg.endpoint_source = "OTEL_EXPORTER_OTLP_ENDPOINT";
    }

    // OTEL_EXPORTER_OTLP_{TRACES,METRICS,LOGS}_ENDPOINT, and the per-signal
    // variables that are not implemented
    OverlaySignalEndpoints(cfg);
    WarnOnUnsupportedSignalVars(cfg);

    // OTEL_EXPORTER_OTLP_PROTOCOL
    if (auto r = OverlayProtocol(cfg); !r)
    {
        return microtel::make_unexpected(r.error());
    }

    // OTEL_EXPORTER_OTLP_HEADERS
    if (auto r = OverlayHeaders(cfg); !r)
    {
        return microtel::make_unexpected(r.error());
    }

    // OTEL_EXPORTER_OTLP_TIMEOUT (integer milliseconds)
    if (auto r = OverlayTimeout(cfg); !r)
    {
        return microtel::make_unexpected(r.error());
    }

    // OTEL_EXPORTER_OTLP_COMPRESSION
    if (const auto v = GetEnv("OTEL_EXPORTER_OTLP_COMPRESSION"); !v.empty())
    {
        cfg.compression_gzip = (v == "gzip");
    }

    // OTEL_EXPORTER_OTLP_CERTIFICATE → ca_bundle path
    if (const auto v = GetEnv("OTEL_EXPORTER_OTLP_CERTIFICATE"); !v.empty())
    {
        cfg.tls.ca_bundle = v;
    }

    return {};
}

/// The resource and metrics half: service identity, resource attributes, the
/// detector policy, and the two metric-pipeline settings.
[[nodiscard]] microtel::Expected<void, ConfigError> OverlayResourceAndMetricEnv(Config& cfg)
{
    // OTEL_SERVICE_NAME
    if (const auto v = GetEnv("OTEL_SERVICE_NAME"); !v.empty())
    {
        cfg.service_name = v;
    }

    // OTEL_RESOURCE_ATTRIBUTES
    if (auto r = OverlayResourceAttrs(cfg); !r)
    {
        return microtel::make_unexpected(r.error());
    }

    // MICROTEL_RESOURCE_DETECTORS_STRICT
    if (auto r = OverlayResourceDetectorsStrict(cfg); !r)
    {
        return microtel::make_unexpected(r.error());
    }

    // OTEL_METRIC_EXPORT_INTERVAL (integer milliseconds)
    if (auto r = OverlayMetricInterval(cfg); !r)
    {
        return microtel::make_unexpected(r.error());
    }

    // OTEL_EXPORTER_OTLP_METRICS_TEMPORALITY_PREFERENCE
    if (auto r = OverlayMetricTemporality(cfg); !r)
    {
        return microtel::make_unexpected(r.error());
    }

    // MICROTEL_LOG_LEVEL
    if (auto r = OverlayLogLevel(cfg); !r)
    {
        return microtel::make_unexpected(r.error());
    }

    return {};
}

}  // namespace

microtel::Expected<void, ConfigError> OverlayEnv(Config& cfg)
{
    if (auto r = OverlayExporterEnv(cfg); !r)
    {
        return microtel::make_unexpected(r.error());
    }
    if (auto r = OverlayResourceAndMetricEnv(cfg); !r)
    {
        return microtel::make_unexpected(r.error());
    }
    return OverlayConcentratorEnv(cfg);
}

}  // namespace microtel::config
