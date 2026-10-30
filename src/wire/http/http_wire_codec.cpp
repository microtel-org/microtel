// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#include "wire/http/http_wire_codec.hpp"

#include "microtel/error.hpp"
#include "microtel/internal/encoded_payload.hpp"
#include "microtel/internal/transport.hpp"
#include "microtel/internal/wire_result.hpp"

#include "wire/auth_failure.hpp"
#include "wire/gzip.hpp"
#include "wire/header_validation.hpp"
#include "wire/otlp_response.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace microtel::wire
{

namespace
{

constexpr std::string_view kV1TracesPath = "/v1/traces";

// HTTP status code ranges and retryable codes (error-model.md §7.1)
constexpr int kHttpSuccessMin = 200;
constexpr int kHttpSuccessMax = 300;
constexpr int kHttpTooManyRequests = 429;
constexpr int kHttpBadGateway = 502;
constexpr int kHttpServiceUnavailable = 503;
constexpr int kHttpGatewayTimeout = 504;

// Number of built-in request headers added before any extra headers
constexpr std::size_t kBuiltInHeaderCount = 7U;

/// Why a response body yielded nothing usable. Both are terminal; they differ
/// only in which counter and which operator message they produce.
enum class BodyError : std::uint8_t
{
    Malformed = 0,
    TooLarge = 1,
};

[[nodiscard]] std::optional<std::string_view> FindHeaderValue(
    const std::vector<internal::HeaderField>& headers, std::string_view name)
{
    for (const auto& h : headers)
    {
        if (h.name == name)
        {
            return std::string_view{h.value};
        }
    }
    return std::nullopt;
}

/// @brief Parse an integer-form `Retry-After` value (seconds).
///
/// Handles only the integer form. HTTP-date form is not parsed in v1
/// (treated as absent). Returns `nullopt` on parse failure.
[[nodiscard]] std::optional<std::chrono::milliseconds> ParseRetryAfter(
    const std::vector<internal::HeaderField>& headers) noexcept
{
    for (const auto& h : headers)
    {
        if (h.name != "retry-after")
        {
            continue;
        }
        std::uint32_t secs = 0;
        const auto* begin = h.value.data();
        const auto* end = begin + h.value.size();
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
        const auto [ptr, ec] = std::from_chars(begin, end, secs);
        if (ec == std::errc{} && ptr == end)
        {
            return std::chrono::milliseconds(static_cast<std::int64_t>(secs) * 1000);
        }
        return std::nullopt;
    }
    return std::nullopt;
}

/// @brief Extract the numeric HTTP status code from response headers.
///
/// Returns 0 if the `:status` header is absent or not a valid integer.
[[nodiscard]] int ParseStatusCode(const std::vector<internal::HeaderField>& headers) noexcept
{
    for (const auto& h : headers)
    {
        if (h.name != ":status")
        {
            continue;
        }
        int code = 0;
        const auto* begin = h.value.data();
        const auto* end = begin + h.value.size();
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
        const auto [ptr, ec] = std::from_chars(begin, end, code);
        if (ec == std::errc{})
        {
            return code;
        }
        return 0;
    }
    return 0;
}

/// @brief Classify an HTTP status code per error-model.md §7.1.
[[nodiscard]] internal::WireResult ClassifyStatus(
    int code, const std::vector<internal::HeaderField>& headers) noexcept
{
    if (code >= kHttpSuccessMin && code < kHttpSuccessMax)
    {
        return {.success = true, .retry_after = {}, .error = {}, .response_excerpt = {}};
    }
    if (code == kHttpTooManyRequests || code == kHttpBadGateway ||
        code == kHttpServiceUnavailable || code == kHttpGatewayTimeout)
    {
        return {
            .success = false,
            .retryable = true,
            .retry_after = ParseRetryAfter(headers),
            .error = {},
            .response_excerpt = {},
        };
    }
    // All other 4xx and 5xx are non-retryable.
    return {
        .success = false,
        .retryable = false,
        .retry_after = {},
        .error = Error{.kind = Error::Kind::Protocol, .message = "HTTP " + std::to_string(code)},
        .response_excerpt = {},
    };
}

/// @brief Undo `content-encoding` on the response body.
///
/// Runs before anything reads the body, so neither `ParseRejectedSpans` nor
/// the diagnostics excerpt ever sees compressed bytes — the former would call
/// them unparseable and fail a delivered batch, and the latter would put
/// binary noise in front of an operator.
///
/// @param result the completed transport response.
/// @param max_decompressed ceiling on the inflated size.
/// @param owned receives the inflated bytes when the body was gzipped. The
///        returned span borrows it, so it must outlive that span.
[[nodiscard]] microtel::Expected<std::span<const std::byte>, BodyError> DecodeBody(
    const internal::TransportResult& result,
    std::size_t max_decompressed,
    std::vector<std::byte>& owned)
{
    const std::span<const std::byte> raw{result.response_body};
    const auto encoding = FindHeaderValue(result.response_headers, "content-encoding");
    // An empty body has nothing to decode and nothing to misread, so a header
    // on one is a server quirk rather than a protocol violation.
    if (!encoding.has_value() || encoding->empty() || *encoding == "identity" || raw.empty())
    {
        return raw;
    }
    if (*encoding != "gzip")
    {
        // gzip is the only encoding this client asks for. Anything else is
        // bytes it cannot read, and the retry brings back the same ones.
        return microtel::make_unexpected(BodyError::Malformed);
    }
    auto inflated = GzipDecompress(raw, max_decompressed);
    if (!inflated)
    {
        const bool too_large = (inflated.error() == GzipDecompressError::TooLarge);
        return microtel::make_unexpected(too_large ? BodyError::TooLarge : BodyError::Malformed);
    }
    owned = std::move(*inflated);
    return std::span<const std::byte>{owned};
}

[[nodiscard]] internal::WireResult BodyFailure(BodyError err, internal::IDiagnosticsSink* diag)
{
    const bool too_large = (err == BodyError::TooLarge);
    const auto reason =
        too_large ? DropReason::DecompressionTooLarge : DropReason::MalformedResponse;
    if (diag != nullptr)
    {
        diag->RecordDrop(reason);
    }
    const std::string_view message = too_large ? "response exceeds max_decompressed_bytes"
                                               : "undecodable response content-encoding";
    return {
        .success = false,
        // Terminal per error-model.md §7.1: a peer that answers in an encoding
        // we cannot read, or with a decompression bomb, does it again on the
        // retry. `Kind` is `Malformed` for both — the counter and the message
        // carry the distinction, and `Error::Kind` is public surface not worth
        // churning for it.
        .retryable = false,
        .retry_after = {},
        .error = Error{.kind = Error::Kind::Malformed, .message = std::string{message}},
        .response_excerpt = {},
    };
}

/// @brief A 2xx whose body is not a well-formed Export response.
///
/// `docs/error-model.md` §7.1, last row: terminal and counted as
/// `malformed_response`. Before issue #223 the parser reported 0 for such a
/// body exactly as for an absent `partial_success`, so a rejection the peer
/// tried to report was accounted as a clean success.
[[nodiscard]] internal::WireResult UnparseableBody(internal::IDiagnosticsSink* diag)
{
    if (diag != nullptr)
    {
        diag->RecordDrop(DropReason::MalformedResponse);
    }
    return {
        .success = false,
        // The peer answers the retry with the same bytes.
        .retryable = false,
        .retry_after = {},
        .error =
            Error{.kind = Error::Kind::Malformed, .message = "unparseable export response body"},
        .response_excerpt = {},
    };
}

/// @brief Turn a failed `TransportResult` into a `WireResult`.
///
/// A transport failure is retryable by default — connection reset, refused
/// peer, read timeout are exactly what the retry engine exists for. The one
/// exception is a response the transport refused to buffer: the peer answers
/// the retry with the same oversized response, so it is terminal and counted
/// (`docs/error-model.md` §3 and §7.1). A request the transport refused to
/// *queue* stays retryable — a full request queue drains — but is counted too,
/// because the batch behind it was not delivered on this attempt.
[[nodiscard]] internal::WireResult TransportFailure(const internal::TransportResult& result,
                                                    internal::IDiagnosticsSink* diag)
{
    if (result.transport_busy && diag != nullptr)
    {
        diag->RecordDrop(DropReason::TransportBusy);
    }
    if (!result.response_too_large)
    {
        return {
            .success = false,
            .retryable = true,
            .retry_after = {},
            .error = result.error,
            .response_excerpt = {},
        };
    }
    if (diag != nullptr)
    {
        diag->RecordDrop(DropReason::ResponseTooLarge);
    }
    return {
        .success = false,
        .retryable = false,
        .retry_after = {},
        .error = result.error,
        .response_excerpt = {},
    };
}

}  // namespace

HttpWireCodec::HttpWireCodec(internal::ITransport* transport,
                             HttpWireCodecConfig config,
                             internal::IAuthProvider* auth,
                             internal::IDiagnosticsSink* diag,
                             internal::ISteadyClock* clock,
                             internal::ConnectOptions connect_opts) noexcept
    : m_transport(transport),
      m_config(std::move(config)),
      m_auth(auth),
      m_diag(diag),
      m_clock(clock),
      m_connect_opts(std::move(connect_opts))
{
}

std::string HttpWireCodec::ResolvePath() const noexcept
{
    if (!m_config.signal_path.empty())
    {
        return m_config.signal_path;
    }
    if (m_config.path.empty() || m_config.path == "/")
    {
        return std::string{kV1TracesPath};
    }
    return m_config.path + std::string{kV1TracesPath};
}

microtel::Expected<std::vector<internal::HeaderField>, internal::WireResult>
HttpWireCodec::BuildHeaders(std::size_t content_length, bool compressed) const
{
    std::vector<internal::HeaderField> headers;
    headers.reserve(kBuiltInHeaderCount + m_config.extra_headers.size());

    headers.push_back({.name = ":method", .value = "POST"});
    headers.push_back({.name = ":scheme", .value = m_config.scheme});
    headers.push_back({.name = ":path", .value = ResolvePath()});
    headers.push_back({.name = ":authority", .value = m_config.host});
    headers.push_back({.name = "content-type", .value = "application/x-protobuf"});
    headers.push_back({.name = "content-length", .value = std::to_string(content_length)});
    // Unconditional, and independent of `compression_gzip`: this states what
    // the client can decode, not what it chose to encode. The response path
    // now handles `content-encoding: gzip`, so there is nothing left to gate
    // it on — and staying silent costs response bandwidth for no benefit.
    headers.push_back({.name = "accept-encoding", .value = "gzip"});
    if (compressed)
    {
        headers.push_back({.name = "content-encoding", .value = "gzip"});
    }

    for (const auto& h : m_config.extra_headers)
    {
        headers.push_back(h);
    }

    if (auto err = AppendAuthHeader(headers))
    {
        return microtel::make_unexpected(AuthFailure(*err));
    }
    if (auto err = ValidateRequestHeaders(headers))
    {
        return microtel::make_unexpected(InvalidHeaderFailure(std::move(*err)));
    }

    return headers;
}

std::optional<Error> HttpWireCodec::AppendAuthHeader(
    std::vector<internal::HeaderField>& headers) const
{
    if (m_auth == nullptr)
    {
        return std::nullopt;
    }
    const auto now = (m_clock != nullptr)
                         ? m_clock->Now()
                         : internal::TimePointSteady{std::chrono::steady_clock::now()};
    auto auth_result = m_auth->GetAuthorization(now);
    if (!auth_result.has_value())
    {
        return std::move(auth_result.error());
    }
    const auto& token_opt = auth_result.value();
    if (!token_opt.has_value())
    {
        return std::nullopt;
    }
    headers.push_back({.name = "authorization", .value = token_opt.value()});
    return std::nullopt;
}

std::string HttpWireCodec::BuildExcerpt(std::span<const std::byte> body)
{
    if (body.empty())
    {
        return {};
    }
    constexpr std::size_t kMaxExcerpt = 256;
    const std::size_t n = std::min(body.size(), kMaxExcerpt);
    std::string excerpt(n, '\0');
    for (std::size_t i = 0; i < n; ++i)
    {
        excerpt[i] = static_cast<char>(body[i]);
    }
    return excerpt;
}

std::optional<internal::WireResult> HttpWireCodec::EnsureConnected()
{
    if (m_transport->GetState() == ConnectionState::Connected)
    {
        return std::nullopt;
    }
    auto connected = m_transport->Connect(m_connect_opts);
    if (!connected)
    {
        // The transport owns no diagnostics sink; the codec is where a failed
        // connect is observed, so it is where the counter moves. One attempt,
        // one increment — a fan-out behind a single prologue connect is one
        // connect failure, not one per payload. Recorded for every kind,
        // retryable or not: the loss is the same to an operator.
        if (m_diag != nullptr)
        {
            m_diag->RecordDrop(DropReason::ConnectFailure);
        }
        // A `Protocol` connect failure is a permanent mismatch — an
        // HTTP/1.1-only receiver, or a TLS endpoint that would not negotiate
        // h2 (issue #166). No backoff outlasts a misconfiguration, so retrying
        // one spends the whole budget and buries the message that names the
        // fix. Everything else the transport reports (Network, Cancelled) is
        // worth another attempt.
        return internal::WireResult{
            .success = false,
            .retryable = connected.error().kind != Error::Kind::Protocol,
            .retry_after = {},
            .error = connected.error(),
            .response_excerpt = {},
        };
    }
    return std::nullopt;
}

HttpWireCodec::Body HttpWireCodec::PrepareBody(std::span<const std::byte> raw,
                                               std::vector<std::byte>& storage) const noexcept
{
    if (m_config.compression_gzip)
    {
        auto result = GzipCompress(raw);
        if (result)
        {
            storage = std::move(*result);
            return Body{.bytes = std::span<const std::byte>{storage}, .compressed = true};
        }
        // Compression failed — fall through and send the payload as-is.
        // `deflate` fails only on allocation failure or a programming error,
        // and neither is a reason to throw away spans the caller already
        // produced.
    }
    return Body{.bytes = raw, .compressed = false};
}

internal::WireResult HttpWireCodec::Send(internal::EncodedPayload&& payload,
                                         std::chrono::milliseconds deadline)
{
    if (auto failure = EnsureConnected())
    {
        return std::move(*failure);
    }

    const internal::EncodedPayload owned = std::move(payload);

    // `compressed` must outlive the request: RequestSpec::payload borrows it.
    std::vector<std::byte> compressed;
    const Body body = PrepareBody(owned.Bytes(), compressed);

    auto headers = BuildHeaders(body.bytes.size(), body.compressed);
    if (!headers)
    {
        // The auth provider failed, or a field cannot be carried: drop the
        // batch rather than send it without the header (`docs/interfaces.md`
        // §4.9, issue #250) or with an injected one (ICP 0042 §3).
        return std::move(headers.error());
    }

    internal::RequestSpec spec{
        .headers = std::move(*headers),
        .payload = body.bytes,
        .deadline = deadline,
    };

    auto handle = m_transport->Send(std::move(spec));
    auto& fut = handle.Future();

    const auto status = fut.wait_for(deadline);
    if (status != std::future_status::ready)
    {
        m_transport->Cancel(handle);
        return {
            .success = false,
            .retryable = true,
            .retry_after = {},
            .error = Error{.kind = Error::Kind::Cancelled, .message = "request deadline exceeded"},
            .response_excerpt = {},
        };
    }

    auto result = fut.get();
    if (!result.success)
    {
        return TransportFailure(result, m_diag);
    }

    return ClassifyResponse(result);
}

internal::WireResult HttpWireCodec::ClassifyResponse(const internal::TransportResult& result) const
{
    // Owns the inflated bytes for as long as `body` borrows them.
    std::vector<std::byte> inflated;
    const auto body = DecodeBody(result, m_config.max_decompressed_bytes, inflated);
    if (!body)
    {
        return BodyFailure(body.error(), m_diag);
    }

    const int code = ParseStatusCode(result.response_headers);
    auto wire = ClassifyStatus(code, result.response_headers);
    if (wire.success && !body->empty())
    {
        const auto parsed = ParseRejectedSpans(*body);
        if (parsed.outcome == PartialSuccessOutcome::Unparseable)
        {
            wire = UnparseableBody(m_diag);
        }
        wire.partial_success_rejected = parsed.rejected;
    }
    wire.response_excerpt = BuildExcerpt(*body);
    return wire;
}

internal::WireResult HttpWireCodec::CollectOneResult(
    InFlight& item, std::chrono::steady_clock::time_point deadline_point)
{
    const auto remaining = deadline_point - std::chrono::steady_clock::now();
    const auto wait_ms = (remaining > std::chrono::nanoseconds{0})
                             ? std::chrono::duration_cast<std::chrono::milliseconds>(remaining)
                             : std::chrono::milliseconds{0};

    const auto status = item.handle.Future().wait_for(wait_ms);
    if (status != std::future_status::ready)
    {
        m_transport->Cancel(item.handle);
        return {
            .success = false,
            .retryable = true,
            .retry_after = {},
            .error = Error{.kind = Error::Kind::Cancelled, .message = "request deadline exceeded"},
            .response_excerpt = {},
        };
    }

    auto result = item.handle.Future().get();
    if (!result.success)
    {
        return TransportFailure(result, m_diag);
    }

    return ClassifyResponse(result);
}

std::vector<internal::WireResult> HttpWireCodec::SendAll(
    std::vector<internal::EncodedPayload> payloads, std::chrono::milliseconds deadline)
{
    if (payloads.empty())
    {
        return {};
    }

    if (auto failure = EnsureConnected())
    {
        // One copy per payload: `results[i]` must line up with the caller's
        // `payloads[i]` (OtlpExporter::FanOutAndProcess indexes both by i).
        return std::vector<internal::WireResult>(payloads.size(), *failure);
    }

    // Keeping each EncodedPayload alive alongside its handle is required:
    // RequestSpec::payload is a span borrowing from it; the I/O thread reads
    // those bytes until the stream closes. With compression on the span
    // borrows `InFlight::compressed` instead — same rule, second buffer.
    //
    // Taking the span before moving the owner into `InFlight` depends on both
    // owners keeping their bytes in a heap block that a move transfers rather
    // than copies. That holds for `std::vector` and for `EncodedPayload`'s
    // `unique_ptr` buffer. It would stop holding if either became a
    // small-buffer-optimised or inline-array type, and the break would be
    // silent — so a change to how these store their bytes has to revisit this
    // loop.
    std::vector<InFlight> in_flight;
    in_flight.reserve(payloads.size());
    const auto deadline_point = std::chrono::steady_clock::now() + deadline;

    // Indexed rather than appended: a payload that fails to compress never
    // reaches `in_flight`, so `results[i]` would otherwise drift out of step
    // with `payloads[i]`.
    std::vector<internal::WireResult> results(payloads.size());

    // Submit all requests without waiting — each opens a separate HTTP/2 stream.
    for (std::size_t i = 0; i < payloads.size(); ++i)
    {
        std::vector<std::byte> compressed;
        const Body body = PrepareBody(payloads[i].Bytes(), compressed);

        auto headers = BuildHeaders(body.bytes.size(), body.compressed);
        if (!headers)
        {
            // This batch alone: the rest of the fan-out is unaffected, and
            // `results[i]` already lines up with `payloads[i]`.
            results[i] = std::move(headers.error());
            continue;
        }
        internal::RequestSpec spec{
            .headers = std::move(*headers),
            .payload = body.bytes,
            .deadline = deadline,
        };
        auto handle = m_transport->Send(std::move(spec));
        in_flight.push_back(InFlight{.payload = std::move(payloads[i]),
                                     .handle = std::move(handle),
                                     .compressed = std::move(compressed),
                                     .index = i});
    }

    for (auto& item : in_flight)
    {
        results[item.index] = CollectOneResult(item, deadline_point);
    }
    return results;
}

}  // namespace microtel::wire
