// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "microtel/error.hpp"
#include "microtel/expected.hpp"
#include "microtel/internal/auth_provider.hpp"
#include "microtel/internal/clock.hpp"
#include "microtel/internal/diagnostics_sink.hpp"
#include "microtel/internal/transport.hpp"
#include "microtel/internal/wire_codec.hpp"
#include "microtel/internal/wire_result.hpp"

#include "wire/gzip.hpp"

#include <chrono>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace microtel::wire
{

/// @brief Configuration for `HttpWireCodec`.
struct HttpWireCodecConfig
{
    std::string host;    ///< value for the `:authority` header (e.g. "host:4318")
    std::string scheme;  ///< "http" or "https"
    /// @brief URL base path. Empty string or "/" resolves to `/v1/traces`.
    /// Any other value is used as a prefix: `<path>/v1/traces`.
    std::string path;
    /// @brief Full signal path override. When non-empty, used as-is for the
    /// `:path` pseudo-header, ignoring `path` and the default `/v1/traces`
    /// suffix. Use `/v1/metrics` to point this codec at the metrics endpoint.
    std::string signal_path;
    std::vector<internal::HeaderField> extra_headers;  ///< forwarded verbatim
    /// @brief When true, request bodies are gzip-compressed and
    /// `content-encoding: gzip` is set. Response decompression is independent
    /// of this flag: `accept-encoding: gzip` is advertised unconditionally.
    bool compression_gzip{false};
    /// @brief Ceiling on the decompressed size of a `content-encoding: gzip`
    /// response body. Past it the response is failed and
    /// `decompression_too_large` counted, rather than the bomb being
    /// materialised. Plumbed from `MemoryLimitOptions::max_decompressed_bytes`.
    std::uint32_t max_decompressed_bytes{kDefaultMaxDecompressedBytes};
};

/// @brief OTLP/HTTP-protobuf implementation of `IWireCodec`.
///
/// Builds an HTTP/2 POST request from the encoded payload, submits it via the
/// injected `ITransport`, waits for the response, and classifies the HTTP
/// status code per the matrix in `docs/error-model.md` §7.1.
///
/// **Dependencies (all non-owning):**
/// - `ITransport` — required; connected lazily on the first `Send`/`SendAll`
///   call if not already connected (ICP 0017). A failed connect attempt is
///   reported as an ordinary retryable `WireResult`, not a distinct shape.
/// - `IAuthProvider` — optional; if non-null, the `Authorization` header is
///   populated on every request.
/// - `IDiagnosticsSink` — optional; records `connect_failure`,
///   `malformed_response` and `decompression_too_large`.
/// - `ISteadyClock` — optional; if non-null, passed to `IAuthProvider::Get-
///   Authorization` for TTL arithmetic. Falls back to `steady_clock::now()`.
///
/// @threadsafety Not thread-safe — single caller (exporter worker).
/// @see docs/error-model.md §7.1
/// @see docs/icps/0017-lazy-transport-connect.md
class HttpWireCodec final : public internal::IWireCodec
{
public:
    explicit HttpWireCodec(internal::ITransport* transport,
                           HttpWireCodecConfig config,
                           internal::IAuthProvider* auth = nullptr,
                           internal::IDiagnosticsSink* diag = nullptr,
                           internal::ISteadyClock* clock = nullptr,
                           internal::ConnectOptions connect_opts = {}) noexcept;

    ~HttpWireCodec() noexcept override = default;

    HttpWireCodec(const HttpWireCodec&) = delete;
    HttpWireCodec& operator=(const HttpWireCodec&) = delete;
    HttpWireCodec(HttpWireCodec&&) = delete;
    HttpWireCodec& operator=(HttpWireCodec&&) = delete;

    [[nodiscard]] internal::WireResult Send(internal::EncodedPayload&& payload,
                                            std::chrono::milliseconds deadline) override;

    [[nodiscard]] std::vector<internal::WireResult> SendAll(
        std::vector<internal::EncodedPayload> payloads,
        std::chrono::milliseconds deadline) override;

private:
    struct InFlight
    {
        internal::EncodedPayload payload;
        internal::RequestHandle handle;
        /// @brief Compressed body, when `compression_gzip` is on. Empty
        /// otherwise. Held here for the same reason as `payload`: the spec's
        /// span borrows it until the stream closes.
        std::vector<std::byte> compressed;
        /// @brief Index into the caller's `payloads`, so `SendAll` can return
        /// results in the caller's order even when an entry never went out.
        std::size_t index{0};
    };

    /// @brief What actually goes on the wire, and whether it ended up gzipped.
    struct Body
    {
        std::span<const std::byte> bytes;
        /// @brief True only if `bytes` really is a gzip stream. Drives the
        /// `content-encoding` header, so it must never be inferred from
        /// config: the two disagree whenever compression was attempted and
        /// failed.
        bool compressed;
    };

    /// @brief Gzip-compresses @p raw into @p storage when `compression_gzip`
    ///        is set; otherwise a no-op.
    /// @param raw the uncompressed body.
    /// @param storage buffer that owns the compressed bytes on return. Must
    ///        outlive the request: the returned span borrows from it.
    /// @return the bytes to send. Cannot fail: if `deflate` fails — which in
    ///         practice means allocation failure — the payload is sent
    ///         uncompressed rather than dropped. Losing telemetry is a worse
    ///         outcome than ignoring a compression preference, and every OTLP
    ///         server accepts an identity-encoded body.
    [[nodiscard]] Body PrepareBody(std::span<const std::byte> raw,
                                   std::vector<std::byte>& storage) const noexcept;

    [[nodiscard]] std::string ResolvePath() const noexcept;
    /// @brief Builds the complete request header block, `authorization`
    ///        included, and validates every field (`ValidateRequestHeaders`).
    /// @return the headers, or the terminal `WireResult` to return instead: a
    ///         batch whose header block cannot be completed is dropped rather
    ///         than sent without the header (`docs/interfaces.md` §4.9, issue
    ///         #250), and one carrying a CR, LF or NUL is dropped before the
    ///         transport sees it (ICP 0042 §3).
    [[nodiscard]] microtel::Expected<std::vector<internal::HeaderField>, internal::WireResult>
    BuildHeaders(std::size_t content_length, bool compressed) const;
    /// @brief Appends `authorization` from the auth provider, if there is one.
    /// @return `nullopt` when the header was appended, or when there is no
    ///         provider or it has no value to give; otherwise the provider's
    ///         `Error`.
    [[nodiscard]] std::optional<Error> AppendAuthHeader(
        std::vector<internal::HeaderField>& headers) const;
    [[nodiscard]] static std::string BuildExcerpt(std::span<const std::byte> body);
    /// @brief Turn a completed transport response into a `WireResult`.
    ///
    /// Decodes `content-encoding` first — an encoding this codec cannot read
    /// makes the response unusable whatever its status — then classifies the
    /// status and parses the partial-success body. Shared by `Send` and
    /// `SendAll` so the two cannot drift apart.
    [[nodiscard]] internal::WireResult ClassifyResponse(
        const internal::TransportResult& result) const;
    [[nodiscard]] internal::WireResult CollectOneResult(
        InFlight& item, std::chrono::steady_clock::time_point deadline_point);
    /// @brief Connects `m_transport` if it isn't already (ICP 0017).
    /// @return `nullopt` when the transport is connected (already, or newly);
    ///         otherwise the retryable `WireResult` to return immediately.
    [[nodiscard]] std::optional<internal::WireResult> EnsureConnected();

    internal::ITransport* m_transport;
    HttpWireCodecConfig m_config;
    internal::IAuthProvider* m_auth;
    internal::IDiagnosticsSink* m_diag;
    internal::ISteadyClock* m_clock;
    internal::ConnectOptions m_connect_opts;
};

}  // namespace microtel::wire
