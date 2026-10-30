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
#include <string>
#include <vector>

namespace microtel::wire
{

/// @brief Configuration for `GrpcWireCodec`.
struct GrpcWireCodecConfig
{
    std::string host;    ///< value for the `:authority` header (e.g. "host:4317")
    std::string scheme;  ///< "https" or "http"
    std::vector<internal::HeaderField> extra_headers;  ///< forwarded verbatim
    /// @brief gRPC method path override. When non-empty, used as-is for the
    /// `:path` pseudo-header instead of the default trace service path.
    /// Use the metrics service path to point this codec at the metrics endpoint.
    std::string service_path;
    /// @brief When true, the request message is gzip-compressed, the frame's
    /// compression flag is `0x01`, and `grpc-encoding: gzip` is set. Response
    /// decompression is independent of this flag: `grpc-accept-encoding: gzip`
    /// is advertised unconditionally (`docs/grpc-wire-protocol.md` §5.2).
    bool compression_gzip{false};
    /// @brief Ceiling on the decompressed size of a `CF = 0x01` response
    /// message. Past it the response is failed and `decompression_too_large`
    /// counted, rather than the bomb being materialised. Plumbed from
    /// `MemoryLimitOptions::max_decompressed_bytes`.
    std::uint32_t max_decompressed_bytes{kDefaultMaxDecompressedBytes};
};

/// @brief OTLP/gRPC implementation of `IWireCodec` — no gRPC library.
///
/// Wraps the encoded payload in a 5-byte gRPC length-prefix frame, submits
/// the request via `ITransport`, parses `grpc-status` from the trailing
/// HEADERS frame (or the initial HEADERS frame for trailer-only responses),
/// and classifies the result per `docs/error-model.md §7.2`.
///
/// **RESOURCE_EXHAUSTED (8):** retryable only when `grpc-status-details-bin`
/// contains a `google.rpc.RetryInfo` entry. The codec decodes the header
/// using a minimal hand-written proto wire-format reader; no upb dependency
/// for this path in M4.
///
/// **Response framing:** the single response DATA-frame message is validated
/// before it is parsed — the compression flag and the declared length are both
/// checked, and a `CF = 0x01` message is inflated under
/// `max_decompressed_bytes` (`docs/grpc-wire-protocol.md` §2.3 and §5.2).
///
/// **Not implemented:** `grpc-timeout` — microtel manages its own deadlines
/// and uses `RST_STREAM` on a local timeout (§2.5).
///
/// **Dependencies (all non-owning):**
/// - `ITransport` — required; connected lazily on the first `Send` call if
///   not already connected (ICP 0017). A failed connect attempt is reported
///   as an ordinary retryable `WireResult`, not a distinct shape.
/// - `IAuthProvider` — optional; `Authorization` header populated if set.
/// - `IDiagnosticsSink` — optional; records `connect_failure`,
///   `malformed_response` and `decompression_too_large`.
/// - `ISteadyClock` — optional; passed to `IAuthProvider` for TTL arithmetic.
///
/// @threadsafety Not thread-safe — single caller (exporter worker).
/// @see docs/grpc-wire-protocol.md
/// @see docs/error-model.md §7.2
/// @see docs/icps/0017-lazy-transport-connect.md
class GrpcWireCodec final : public internal::IWireCodec
{
public:
    explicit GrpcWireCodec(internal::ITransport* transport,
                           GrpcWireCodecConfig config,
                           internal::IAuthProvider* auth = nullptr,
                           internal::IDiagnosticsSink* diag = nullptr,
                           internal::ISteadyClock* clock = nullptr,
                           internal::ConnectOptions connect_opts = {}) noexcept;

    ~GrpcWireCodec() noexcept override = default;

    GrpcWireCodec(const GrpcWireCodec&) = delete;
    GrpcWireCodec& operator=(const GrpcWireCodec&) = delete;
    GrpcWireCodec(GrpcWireCodec&&) = delete;
    GrpcWireCodec& operator=(GrpcWireCodec&&) = delete;

    [[nodiscard]] internal::WireResult Send(internal::EncodedPayload&& payload,
                                            std::chrono::milliseconds deadline) override;

private:
    /// @brief Builds the complete request header block, `authorization`
    ///        included, and validates every field (`ValidateRequestHeaders`).
    /// @return the headers, or the terminal `WireResult` to return instead: a
    ///         batch whose header block cannot be completed is dropped rather
    ///         than sent without the header (`docs/interfaces.md` §4.9, issue
    ///         #250), and one carrying a CR, LF or NUL is dropped before the
    ///         transport sees it (ICP 0042 §3).
    [[nodiscard]] microtel::Expected<std::vector<internal::HeaderField>, internal::WireResult>
    BuildHeaders(bool compressed) const;
    /// @brief Appends `authorization` from the auth provider, if there is one.
    /// @return `nullopt` when the header was appended, or when there is no
    ///         provider or it has no value to give; otherwise the provider's
    ///         `Error`.
    [[nodiscard]] std::optional<microtel::Error> AppendAuthHeader(
        std::vector<internal::HeaderField>& headers) const;
    /// @brief Connects `m_transport` if it isn't already (ICP 0017).
    /// @return `nullopt` when the transport is connected (already, or newly);
    ///         otherwise the retryable `WireResult` to return immediately.
    [[nodiscard]] std::optional<internal::WireResult> EnsureConnected();

    internal::ITransport* m_transport;
    GrpcWireCodecConfig m_config;
    internal::IAuthProvider* m_auth;
    internal::IDiagnosticsSink* m_diag;
    internal::ISteadyClock* m_clock;
    internal::ConnectOptions m_connect_opts;
    /// @brief Whether the §4.2 "no grpc-status trailer" warn has fired on the
    /// current connection. Reset whenever `EnsureConnected` establishes a new
    /// one, which is the unit §4.2 names — a codec that warned once for its
    /// whole lifetime would go quiet across the reconnect an operator is
    /// watching for.
    bool m_warned_missing_grpc_status{false};
};

}  // namespace microtel::wire
