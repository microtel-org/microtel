// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#include "common/config/config_validator.hpp"

#include "microtel/error.hpp"
#include "microtel/protocol.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <variant>
#include <vector>

namespace microtel::config
{

namespace
{

// ---------------------------------------------------------------------------
// Named constants
// ---------------------------------------------------------------------------

constexpr std::string_view kSchemeHttps = "https";
constexpr std::string_view kSchemeHttp = "http";
constexpr std::string_view kSchemeGrpc = "grpc";
constexpr std::string_view kSchemeGrpcs = "grpcs";
constexpr std::string_view kSchemeSep = "://";

constexpr std::uint16_t kDefaultPortGrpc = 4317;
constexpr std::uint16_t kDefaultPortHttp = 4318;
constexpr std::uint16_t kPortMax = 65535;

/// OTel resource semantic conventions: `service.name` is required, and this is
/// the placeholder a producer uses when it has not been told one.
constexpr std::string_view kUnknownService = "unknown_service";

// ---------------------------------------------------------------------------
// URL parsing helpers
// ---------------------------------------------------------------------------

/// Extract `scheme` from "scheme://rest". Returns empty on failure.
[[nodiscard]] std::string_view ExtractScheme(std::string_view url)
{
    const auto sep = url.find(kSchemeSep);
    if (sep == std::string_view::npos)
    {
        return {};
    }
    return url.substr(0, sep);
}

/// Remove "scheme://" prefix. `url` must already have the prefix.
[[nodiscard]] std::string_view StripScheme(std::string_view url)
{
    const auto sep = url.find(kSchemeSep);
    return url.substr(sep + kSchemeSep.size());
}

/// Split "host:port/path" → host, port string, path. Port is empty if absent.
struct AuthorityPath
{
    std::string_view host;
    std::string_view port_str;
    std::string_view path;
};

[[nodiscard]] AuthorityPath SplitAuthorityPath(std::string_view authority_and_path)
{
    AuthorityPath result;

    // Separate path from authority.
    const auto slash = authority_and_path.find('/');
    const std::string_view authority = (slash == std::string_view::npos)
                                           ? authority_and_path
                                           : authority_and_path.substr(0, slash);
    result.path =
        (slash == std::string_view::npos) ? std::string_view{} : authority_and_path.substr(slash);

    // Separate host from port.
    const auto colon = authority.rfind(':');
    if (colon == std::string_view::npos)
    {
        result.host = authority;
    }
    else
    {
        result.host = authority.substr(0, colon);
        result.port_str = authority.substr(colon + 1);
    }
    return result;
}

/// Parse the port component of an endpoint URL.
[[nodiscard]] microtel::Expected<std::uint16_t, ConfigError> ParsePort(std::string_view port_str,
                                                                       Protocol protocol,
                                                                       std::string_view field)
{
    if (port_str.empty())
    {
        return (protocol == Protocol::Grpc) ? kDefaultPortGrpc : kDefaultPortHttp;
    }
    std::uint32_t parsed = 0;
    const auto [ptr, ec] =
        std::from_chars(port_str.data(), port_str.data() + port_str.size(), parsed);
    if (ec != std::errc{} || ptr != port_str.data() + port_str.size() || parsed > kPortMax ||
        parsed == 0)
    {
        return microtel::make_unexpected(ConfigError{.kind = ConfigError::Kind::EndpointMalformed,
                                                     .field = std::string{field},
                                                     .message = "invalid port in endpoint URL"});
    }
    return static_cast<std::uint16_t>(parsed);
}

/// Parse an endpoint URL into ParsedEndpoint components.
/// Does not validate file-system resources.
///
/// @param field what a failure names: `exporter.endpoint` for the base
///        endpoint, the variable's name for a per-signal one.
[[nodiscard]] microtel::Expected<ParsedEndpoint, ConfigError> ParseEndpointUrl(
    const std::string& url, Protocol protocol, std::string_view field)
{
    if (url.empty())
    {
        return microtel::make_unexpected(ConfigError{.kind = ConfigError::Kind::EndpointMalformed,
                                                     .field = std::string{field},
                                                     .message = "endpoint URL is empty"});
    }

    const std::string_view scheme = ExtractScheme(url);
    if (scheme.empty())
    {
        return microtel::make_unexpected(
            ConfigError{.kind = ConfigError::Kind::EndpointMalformed,
                        .field = std::string{field},
                        .message = "endpoint URL missing scheme (expected https:// or http://)"});
    }

    // Normalise scheme: grpc → https, grpcs → https, http → http, https → https.
    std::string resolved_scheme;
    if (scheme == kSchemeHttps || scheme == kSchemeGrpcs)
    {
        resolved_scheme = std::string{kSchemeHttps};
    }
    else if (scheme == kSchemeHttp || scheme == kSchemeGrpc)
    {
        resolved_scheme = std::string{kSchemeHttp};
    }
    else
    {
        return microtel::make_unexpected(
            ConfigError{.kind = ConfigError::Kind::EndpointMalformed,
                        .field = std::string{field},
                        .message = "unsupported scheme: " + std::string{scheme}});
    }

    const auto ap = SplitAuthorityPath(StripScheme(url));

    if (ap.host.empty())
    {
        return microtel::make_unexpected(ConfigError{.kind = ConfigError::Kind::EndpointMalformed,
                                                     .field = std::string{field},
                                                     .message = "endpoint URL has empty host"});
    }

    // Parse optional port.
    auto port_result = ParsePort(ap.port_str, protocol, field);
    if (!port_result)
    {
        return microtel::make_unexpected(port_result.error());
    }
    const std::uint16_t port = *port_result;

    // Strip trailing slash from path so it's a clean base.
    std::string path{ap.path};
    if (path == "/")
    {
        path.clear();
    }

    return ParsedEndpoint{
        .scheme = resolved_scheme,
        .host = std::string{ap.host},
        .port = port,
        .path = path,
    };
}

/// @brief Resolve the effective protocol against the endpoint scheme.
///
/// `grpc://` and `grpcs://` are microtel shorthand for OTLP/gRPC (`docs/configuration.md` §3.3).
/// The shorthand selects the protocol when the user has not named one; when the
/// user has named `http`, the two disagree and the configuration is rejected
/// rather than silently resolved in either direction — the same rule, and the
/// same `ProtocolMismatch` kind, that the gRPC-path check below applies.
///
/// `https://` and `http://` say nothing about the protocol: `https://` plus an
/// explicit `protocol` is the canonical spelling of a gRPC endpoint, and
/// plaintext h2c gRPC over `http://` is legitimate. Only the two gRPC-named
/// schemes carry an opinion, so only they are consulted here.
///
/// A URL with no recognisable scheme falls through unchanged; `ParseEndpointUrl`
/// is what reports it, and it reports it as `EndpointMalformed`.
///
/// Every endpoint in effect is consulted - the base endpoint when a signal
/// uses it, and each per-signal endpoint - because they share one connection
/// and so one protocol. The first gRPC-named scheme decides.
[[nodiscard]] microtel::Expected<Protocol, ConfigError> ResolveProtocol(const Config& cfg,
                                                                        bool base_in_effect)
{
    const auto names_grpc = [](std::string_view url)
    {
        const std::string_view scheme = ExtractScheme(url);
        return scheme == kSchemeGrpc || scheme == kSchemeGrpcs;
    };
    std::string_view grpc_url;
    if (base_in_effect && names_grpc(cfg.endpoint_url))
    {
        grpc_url = cfg.endpoint_url;
    }
    for (const std::string& url : cfg.signal_endpoint_urls)
    {
        if (grpc_url.empty() && names_grpc(url))
        {
            grpc_url = url;
        }
    }
    if (grpc_url.empty())
    {
        return cfg.protocol;
    }
    const std::string_view scheme = ExtractScheme(grpc_url);
    if (cfg.protocol_explicit && cfg.protocol != Protocol::Grpc)
    {
        return microtel::make_unexpected(ConfigError{
            .kind = ConfigError::Kind::ProtocolMismatch,
            .field = "exporter.protocol",
            .message = "endpoint scheme \"" + std::string{scheme} +
                       "://\" selects OTLP/gRPC but protocol is set to \"http\"; use an "
                       "http:// or https:// endpoint, or drop the explicit protocol"});
    }
    return Protocol::Grpc;
}

// ---------------------------------------------------------------------------
// Per-signal endpoints (docs/configuration.md §3.3)
// ---------------------------------------------------------------------------

/// The OTLP/HTTP path of each signal relative to the base endpoint, in
/// `SignalIndex` order (OTel spec, "Endpoint URLs for OTLP/HTTP").
constexpr std::array<std::string_view, kSignalCount> kDefaultSignalPaths{
    "/v1/traces", "/v1/metrics", "/v1/logs"};

/// `scheme://host:port`, with the default port already filled in.
[[nodiscard]] std::string OriginOf(const ParsedEndpoint& e)
{
    return e.scheme + "://" + e.host + ":" + std::to_string(e.port);
}

/// The endpoint whose origin every other endpoint in effect must match.
struct OriginAnchor
{
    std::string_view name;  ///< the variable or key that set it
    std::string origin;     ///< `OriginOf` its parsed URL
};

/// @brief Whether the base endpoint is in effect.
///
/// It is when at least one signal has no per-signal endpoint of its own,
/// unless it is unset and a per-signal endpoint can supply the origin instead.
/// Unset with no per-signal endpoint either, it is still in effect, so that
/// parsing it reports the missing endpoint exactly as before.
[[nodiscard]] bool BaseEndpointInEffect(const Config& cfg) noexcept
{
    const auto unset = [](const std::string& url) { return url.empty(); };
    const bool base_used = std::ranges::any_of(cfg.signal_endpoint_urls, unset);
    const bool any_signal_endpoint = !std::ranges::all_of(cfg.signal_endpoint_urls, unset);
    return base_used && (!cfg.endpoint_url.empty() || !any_signal_endpoint);
}

/// Parse the base endpoint, which refuses a path under gRPC as it always has.
[[nodiscard]] microtel::Expected<ParsedEndpoint, ConfigError> ParseBaseEndpoint(const Config& cfg)
{
    auto endpoint = ParseEndpointUrl(cfg.endpoint_url, cfg.protocol, "exporter.endpoint");
    if (!endpoint)
    {
        return microtel::make_unexpected(endpoint.error());
    }
    if (cfg.protocol == Protocol::Grpc && !endpoint->path.empty())
    {
        return microtel::make_unexpected(
            ConfigError{.kind = ConfigError::Kind::ProtocolMismatch,
                        .field = "exporter.endpoint",
                        .message = "gRPC endpoint URLs must not include a path"});
    }
    return endpoint;
}

/// The refusal for two endpoints in effect whose origins differ.
[[nodiscard]] ConfigError OriginMismatch(std::string_view name,
                                         const std::string& origin,
                                         const OriginAnchor& anchor)
{
    return ConfigError{
        .kind = ConfigError::Kind::InvalidValue,
        .field = std::string{name},
        .message = std::string{name} + " (" + origin + ") and " + std::string{anchor.name} + " (" +
                   anchor.origin +
                   ") resolve to different origins; microtel exports every signal over one "
                   "connection, so every endpoint in effect must share one scheme, host and port"};
}

/// @brief Resolve one signal's per-signal endpoint.
///
/// Checks its origin against @p anchor, or makes it the anchor (and @p shared
/// the connection's origin) if there is none yet. Under OTLP/HTTP its path is
/// the signal's path, as-is; under OTLP/gRPC a path is ignored with a warning.
[[nodiscard]] microtel::Expected<void, ConfigError> ResolveSignalEndpoint(
    Config& cfg, std::size_t signal, std::optional<OriginAnchor>& anchor, ParsedEndpoint& shared)
{
    const std::string_view name = kSignalEndpointVars.at(signal);
    auto endpoint = ParseEndpointUrl(cfg.signal_endpoint_urls.at(signal), cfg.protocol, name);
    if (!endpoint)
    {
        return microtel::make_unexpected(endpoint.error());
    }
    std::string origin = OriginOf(*endpoint);
    if (anchor && origin != anchor->origin)
    {
        return microtel::make_unexpected(OriginMismatch(name, origin, *anchor));
    }
    if (!anchor)
    {
        anchor = OriginAnchor{.name = name, .origin = std::move(origin)};
        shared = *endpoint;
    }
    if (cfg.protocol == Protocol::Http)
    {
        cfg.signal_paths.at(signal) = endpoint->path.empty() ? "/" : endpoint->path;
    }
    else if (!endpoint->path.empty())
    {
        cfg.config_warnings.push_back(
            std::string{name} + ": path \"" + endpoint->path +
            "\" ignored; OTLP/gRPC request paths are fixed by the service, so a per-signal "
            "endpoint contributes only its scheme, host and port");
    }
    return {};
}

/// @brief Give each signal on the base endpoint its OTLP/HTTP path: the base
///        path, then `/v1/<signal>`.
///
/// The spec joins the two with one slash, so a base path that ends in one
/// (`/mycollector/`) does not gain a second. Nothing to do under OTLP/gRPC.
void AssignBasePaths(Config& cfg, std::string_view base_path)
{
    if (cfg.protocol != Protocol::Http)
    {
        return;
    }
    if (base_path.ends_with('/'))
    {
        base_path.remove_suffix(1);
    }
    for (std::size_t i = 0; i < kSignalCount; ++i)
    {
        if (cfg.signal_endpoint_urls.at(i).empty())
        {
            cfg.signal_paths.at(i) =
                std::string{base_path} + std::string{kDefaultSignalPaths.at(i)};
        }
    }
}

/// @brief Resolve the endpoints in effect to one connection origin and a
///        request path per signal (docs/configuration.md §3.3).
///
/// On success `cfg.endpoint` holds the shared origin and the base endpoint's
/// path (empty when no signal uses the base), and, under OTLP/HTTP,
/// `cfg.signal_paths` the path of each signal.
[[nodiscard]] microtel::Expected<void, ConfigError> ResolveEndpoints(Config& cfg)
{
    std::optional<OriginAnchor> anchor;
    ParsedEndpoint shared;
    if (BaseEndpointInEffect(cfg))
    {
        auto base = ParseBaseEndpoint(cfg);
        if (!base)
        {
            return microtel::make_unexpected(base.error());
        }
        anchor = OriginAnchor{.name = cfg.endpoint_source, .origin = OriginOf(*base)};
        shared = std::move(*base);
    }
    std::string base_path = shared.path;
    AssignBasePaths(cfg, base_path);
    for (std::size_t i = 0; i < kSignalCount; ++i)
    {
        if (cfg.signal_endpoint_urls.at(i).empty())
        {
            continue;
        }
        if (auto r = ResolveSignalEndpoint(cfg, i, anchor, shared); !r)
        {
            return r;
        }
    }
    shared.path = std::move(base_path);
    cfg.endpoint = std::move(shared);
    return {};
}

/// Check that a path-string refers to a readable file.
[[nodiscard]] bool IsReadable(const std::filesystem::path& p)
{
    if (p.empty())
    {
        return true;  // absent means "not configured", which is fine
    }
    std::error_code ec;
    return std::filesystem::is_regular_file(p, ec) && !ec;
}

/// @brief Reject `insecure = true` when the build forbids it.
///
/// `MICROTEL_FORBID_INSECURE_TLS=ON` compiles the macro into this translation
/// unit (see src/common/config/CMakeLists.txt). `docs/configuration.md` §3.5 makes the refusal an
/// initialisation failure, not a warning: a default build only warns, and
/// `SdkBuilder`'s `WarnOnRiskyConfig` owns that half.
///
/// This is the only place in the tree that tests the macro. Everything else
/// calls this function unconditionally, so the OFF build differs from the ON
/// build by the contents of one function body and nothing else.
[[nodiscard]] microtel::Expected<void, ConfigError> CheckInsecureAllowed(
    [[maybe_unused]] const Config& cfg)
{
#ifdef MICROTEL_FORBID_INSECURE_TLS
    if (cfg.tls.insecure)
    {
        return microtel::make_unexpected(
            ConfigError{.kind = ConfigError::Kind::InsecureDisallowed,
                        .field = "tls.insecure",
                        .message = "tls.insecure = true is refused: this build was compiled "
                                   "with MICROTEL_FORBID_INSECURE_TLS=ON"});
    }
#endif
    return {};
}

/// @brief Check the configured TLS material is coherent and readable.
///
/// Split out of `Validate` to keep both function bodies inside the cognitive
/// complexity budget; it carries no state and is called exactly once.
[[nodiscard]] microtel::Expected<void, ConfigError> ValidateTlsMaterial(const Config& cfg)
{
    if (!IsReadable(cfg.tls.ca_bundle))
    {
        return microtel::make_unexpected(
            ConfigError{.kind = ConfigError::Kind::TlsMaterialUnreadable,
                        .field = "tls.ca_bundle",
                        .message = "CA bundle not readable: " + cfg.tls.ca_bundle.string()});
    }

    const bool has_cert = !cfg.tls.client_cert.empty();
    const bool has_key = !cfg.tls.client_key.empty();

    if (has_cert && !has_key)
    {
        return microtel::make_unexpected(
            ConfigError{.kind = ConfigError::Kind::InvalidValue,
                        .field = "tls.client_key",
                        .message = "client_cert is set but client_key is missing"});
    }
    if (has_key && !has_cert)
    {
        return microtel::make_unexpected(
            ConfigError{.kind = ConfigError::Kind::InvalidValue,
                        .field = "tls.client_cert",
                        .message = "client_key is set but client_cert is missing"});
    }
    if (has_cert && !IsReadable(cfg.tls.client_cert))
    {
        return microtel::make_unexpected(
            ConfigError{.kind = ConfigError::Kind::TlsMaterialUnreadable,
                        .field = "tls.client_cert",
                        .message = "client cert not readable: " + cfg.tls.client_cert.string()});
    }
    if (has_key && !IsReadable(cfg.tls.client_key))
    {
        return microtel::make_unexpected(
            ConfigError{.kind = ConfigError::Kind::TlsMaterialUnreadable,
                        .field = "tls.client_key",
                        .message = "client key not readable: " + cfg.tls.client_key.string()});
    }
    return {};
}

// ---------------------------------------------------------------------------
// Static request header names (ICP 0038)
// ---------------------------------------------------------------------------

/// Forbidden in any HTTP/2 request (RFC 9113 §8.2.2). `te: trailers` is the
/// one legal `te`; the gRPC codec sends it and it means nothing on HTTP.
constexpr std::array<std::string_view, 6> kConnectionSpecificHeaders{
    "connection", "keep-alive", "proxy-connection", "transfer-encoding", "upgrade", "te"};

/// Set by the gRPC codec (`docs/grpc-wire-protocol.md` §2.1).
constexpr std::array<std::string_view, 4> kGrpcCodecHeaders{
    "content-type", "user-agent", "grpc-encoding", "grpc-accept-encoding"};

/// Set by the HTTP codec.
constexpr std::array<std::string_view, 4> kHttpCodecHeaders{
    "content-type", "content-length", "content-encoding", "accept-encoding"};

/// Not sent on gRPC, whose body length varies per request; a fixed value
/// would contradict every body but one (ICP 0038 amendment).
constexpr std::string_view kContentLengthHeader = "content-length";
constexpr std::string_view kHostHeader = "host";
/// RFC 9113 §8.2.1: a field value never contains these, and never starts or
/// ends with SP or HTAB.
constexpr std::string_view kForbiddenValueChars{"\0\r\n", 3};
constexpr std::string_view kValueEdgeWhitespace = " \t";
constexpr std::string_view kAuthorizationHeader = "authorization";
constexpr std::string_view kHeadersFieldPrefix = "exporter.headers.";
/// RFC 9110 §5.6.2 `tchar`, less ALPHA and DIGIT.
constexpr std::string_view kTokenPunctuation = "!#$%&'*+-.^_`|~";

[[nodiscard]] bool IsTokenChar(char c) noexcept
{
    return std::isalnum(static_cast<unsigned char>(c)) != 0 ||
           kTokenPunctuation.find(c) != std::string_view::npos;
}

/// RFC 9110 §5.1 field names are tokens. A pseudo-header's leading `:` is not
/// a token character, so this also rejects every pseudo-header.
[[nodiscard]] bool IsToken(std::string_view name) noexcept
{
    return !name.empty() && std::ranges::all_of(name, IsTokenChar);
}

[[nodiscard]] bool EqualsIgnoreCase(std::string_view a, std::string_view b) noexcept
{
    return std::ranges::equal(a,
                              b,
                              [](char x, char y)
                              {
                                  return std::tolower(static_cast<unsigned char>(x)) ==
                                         std::tolower(static_cast<unsigned char>(y));
                              });
}

template <std::size_t N>
[[nodiscard]] bool IsOneOf(std::string_view name,
                           const std::array<std::string_view, N>& set) noexcept
{
    return std::ranges::any_of(set,
                               [name](std::string_view s) { return EqualsIgnoreCase(name, s); });
}

/// The protocol-specific half of `HeaderNameFault`.
[[nodiscard]] std::optional<std::string_view> ProtocolHeaderFault(std::string_view name,
                                                                  Protocol protocol) noexcept
{
    if (protocol == Protocol::Http)
    {
        if (IsOneOf(name, kHttpCodecHeaders))
        {
            return "is set by microtel for this protocol";
        }
        return std::nullopt;
    }
    if (IsOneOf(name, kGrpcCodecHeaders))
    {
        return "is set by microtel for this protocol";
    }
    if (EqualsIgnoreCase(name, kContentLengthHeader))
    {
        return "cannot be fixed on gRPC, where every request body has its own length";
    }
    return std::nullopt;
}

/// @return Why @p name cannot be a static header under @p protocol, or
///         `std::nullopt` if it can.
[[nodiscard]] std::optional<std::string_view> HeaderNameFault(std::string_view name,
                                                              Protocol protocol) noexcept
{
    if (!IsToken(name))
    {
        return "is not a valid header name (RFC 9110 token); pseudo-headers are set by microtel";
    }
    if (IsOneOf(name, kConnectionSpecificHeaders))
    {
        return "is connection-specific, which HTTP/2 forbids in a request (RFC 9113 §8.2.2)";
    }
    if (EqualsIgnoreCase(name, kHostHeader))
    {
        return "is set by microtel from the endpoint, as :authority";
    }
    return ProtocolHeaderFault(name, protocol);
}

/// The error for header @p name; the value is never included (it may be a
/// secret).
[[nodiscard]] ConfigError HeaderError(const std::string& name, std::string_view reason)
{
    return ConfigError{.kind = ConfigError::Kind::InvalidValue,
                       .field = std::string{kHeadersFieldPrefix} + name,
                       .message = "header \"" + name + "\" " + std::string{reason}};
}

/// The value half of the header check (issue #412). A header whose value is
/// not a string is dropped before the codec, so only strings are checked.
[[nodiscard]] std::optional<std::string_view> HeaderValueFault(const AttributeValue& value) noexcept
{
    const auto* const str = std::get_if<std::string>(&value);
    return (str == nullptr) ? std::nullopt : FieldValueFault(*str);
}

[[nodiscard]] microtel::Expected<void, ConfigError> ValidateHeaders(const Config& cfg)
{
    for (const auto& header : cfg.headers)
    {
        if (const auto fault = HeaderNameFault(header.key, cfg.protocol))
        {
            return microtel::make_unexpected(HeaderError(header.key, *fault));
        }
        if (const auto fault = HeaderValueFault(header.value))
        {
            return microtel::make_unexpected(HeaderError(header.key, *fault));
        }
    }
    return {};
}

}  // namespace

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

std::optional<std::string_view> FieldValueFault(std::string_view value) noexcept
{
    if (value.find_first_of(kForbiddenValueChars) != std::string_view::npos)
    {
        return "value contains CR, LF or NUL, which HTTP/2 forbids (RFC 9113 §8.2.1)";
    }
    if (!value.empty() && (kValueEdgeWhitespace.find(value.front()) != std::string_view::npos ||
                           kValueEdgeWhitespace.find(value.back()) != std::string_view::npos))
    {
        return "value starts or ends with a space or tab, which HTTP/2 forbids "
               "(RFC 9113 §8.2.1)";
    }
    return std::nullopt;
}

std::optional<BatchOptionsFault> CheckBatchOptions(const BatchOptions& opts) noexcept
{
    if (opts.max_queue_size == 0U)
    {
        return BatchOptionsFault{.field = "sdk.max_queue_size",
                                 .message = "max_queue_size must be greater than zero"};
    }
    if (opts.max_export_batch_size == 0U)
    {
        return BatchOptionsFault{.field = "sdk.max_export_batch_size",
                                 .message = "max_export_batch_size must be greater than zero"};
    }
    if (opts.max_export_batch_size > opts.max_queue_size)
    {
        return BatchOptionsFault{.field = "sdk.max_export_batch_size",
                                 .message = "max_export_batch_size must not exceed max_queue_size"};
    }
    if (opts.schedule_delay.count() <= 0)
    {
        return BatchOptionsFault{.field = "sdk.schedule_delay_ms",
                                 .message = "schedule_delay must be greater than zero"};
    }
    return std::nullopt;
}

microtel::Expected<void, ConfigError> CheckNoStaticAuthorization(
    const std::vector<KeyValue>& headers)
{
    for (const auto& header : headers)
    {
        if (EqualsIgnoreCase(header.key, kAuthorizationHeader))
        {
            return microtel::make_unexpected(HeaderError(
                header.key, "is set by the WithAuthProvider callback; set one or the other"));
        }
    }
    return {};
}

microtel::Expected<void, ConfigError> Validate(Config& cfg)
{
    // --- Protocol (docs/configuration.md §3.3) ---
    // Before the endpoint is parsed: the resolved protocol is what picks the
    // default port, so `grpc://collector` means 4317 and not 4318.
    auto protocol = ResolveProtocol(cfg, BaseEndpointInEffect(cfg));
    if (!protocol)
    {
        return microtel::make_unexpected(protocol.error());
    }
    cfg.protocol = *protocol;

    // --- Endpoint URLs: the base and the per-signal ones, on one origin; a
    // path on a gRPC base endpoint is refused (docs/configuration.md §3.3) ---
    if (auto endpoints_ok = ResolveEndpoints(cfg); !endpoints_ok)
    {
        return microtel::make_unexpected(endpoints_ok.error());
    }

    // --- TLS ---
    if (auto insecure_ok = CheckInsecureAllowed(cfg); !insecure_ok)
    {
        return microtel::make_unexpected(insecure_ok.error());
    }
    if (auto tls_ok = ValidateTlsMaterial(cfg); !tls_ok)
    {
        return microtel::make_unexpected(tls_ok.error());
    }

    // --- Static headers (ICP 0038, issue #412): after the protocol is
    // resolved, because the names the codec sets depend on it ---
    if (auto headers_ok = ValidateHeaders(cfg); !headers_ok)
    {
        return microtel::make_unexpected(headers_ok.error());
    }

    // --- Batch coherence (the same rules SetBatchOptions applies, #267) ---
    if (const auto fault = CheckBatchOptions(cfg.batch))
    {
        return microtel::make_unexpected(ConfigError{.kind = ConfigError::Kind::InvalidValue,
                                                     .field = std::string{fault->field},
                                                     .message = std::string{fault->message}});
    }

    // --- Service identity ---
    // `service.name` is required by the OTel resource semantic conventions;
    // resolving the placeholder here rather than at resource-assembly time
    // keeps one owner for the default and leaves `Config` a complete record of
    // what the pipeline will actually report.
    // `service_name_defaulted` records which of the two it is, because the
    // resource merge order (docs/configuration.md §3.2) puts a built-in default below a detector's
    // contribution and a configured name above it.
    cfg.service_name_defaulted = cfg.service_name.empty();
    if (cfg.service_name_defaulted)
    {
        cfg.service_name = std::string{kUnknownService};
    }

    return {};
}

}  // namespace microtel::config
