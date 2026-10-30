// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#include "wire/grpc/grpc_wire_codec.hpp"

#include "microtel/error.hpp"
#include "microtel/internal/encoded_payload.hpp"
#include "microtel/internal/transport.hpp"
#include "microtel/internal/wire_result.hpp"
#include "microtel/version.hpp"

#include "common/internal_log.hpp"
#include "wire/auth_failure.hpp"
#include "wire/grpc/grpc_status.hpp"
#include "wire/gzip.hpp"
#include "wire/header_validation.hpp"
#include "wire/otlp_response.hpp"

#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <future>
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

constexpr std::string_view kGrpcTracesPath =
    "/opentelemetry.proto.collector.trace.v1.TraceService/Export";
constexpr std::string_view kRetryInfoTypeUrl = "type.googleapis.com/google.rpc.RetryInfo";

// `microtel-cpp/<version>`, required by grpc-wire-protocol.md §2.1. Spelled as a literal rather
// than assembled at runtime — this is on the per-export header path — with the
// version half checked against `kVersionString` at compile time. It had read
// "microtel-cpp/0.1.0" since M4 with nothing to catch the drift.
constexpr std::string_view kUserAgentPrefix = "microtel-cpp/";
constexpr std::string_view kUserAgent = "microtel-cpp/1.2.5";
static_assert(kUserAgent.substr(kUserAgentPrefix.size()) == kVersionString,
              "gRPC user-agent must carry microtel::kVersionString "
              "(grpc-wire-protocol.md §2.1)");

// ---------------------------------------------------------------------------
// Base64 alphabet position constants (RFC 4648 §5)
// ---------------------------------------------------------------------------

constexpr int kB64AlphaLowerOffset = 26;   // 'a' maps to index 26
constexpr int kB64DigitOffset = 52;        // '0' maps to index 52
constexpr int kB64PlusOrMinus = 62;        // '+' (standard) or '-' (URL-safe)
constexpr int kB64SlashOrUnderscore = 63;  // '/' (standard) or '_' (URL-safe)
constexpr int kB64InvalidChar = -1;

// Base64 decode output reservation: ceil(n * 3 / 4)
constexpr std::size_t kB64OutputBytesPerGroup = 3U;
constexpr std::size_t kB64InputCharsPerGroup = 4U;

// Bit masks for Base64 carry bits between output bytes
constexpr unsigned kB64Mask4Bit = 0xFU;  // 4-bit carry (2nd output byte)
constexpr unsigned kB64Mask2Bit = 0x3U;  // 2-bit carry (3rd output byte)

// ---------------------------------------------------------------------------
// Proto varint decoding constants
// ---------------------------------------------------------------------------

constexpr std::uint8_t kVarintContinueBit = 0x80U;
constexpr std::uint64_t kVarintDataMask = 0x7FU;
constexpr unsigned kVarintShiftStep = 7U;
constexpr unsigned kVarintMaxShift = 64U;

// ---------------------------------------------------------------------------
// Proto wire-type fixed-width field sizes
// ---------------------------------------------------------------------------

constexpr std::size_t kWireWidth64Bit = 8U;
constexpr std::size_t kWireWidth32Bit = 4U;

// ---------------------------------------------------------------------------
// Proto field numbers
// ---------------------------------------------------------------------------

// google.protobuf.Duration
constexpr std::uint32_t kFieldDurationSeconds = 1U;
constexpr std::uint32_t kFieldDurationNanos = 2U;

// google.rpc.RetryInfo
constexpr std::uint32_t kFieldRetryDelay = 1U;

// google.protobuf.Any
constexpr std::uint32_t kFieldAnyTypeUrl = 1U;
constexpr std::uint32_t kFieldAnyValue = 2U;

// google.rpc.Status
constexpr std::uint32_t kFieldStatusDetails = 3U;

// ---------------------------------------------------------------------------
// google.protobuf.Duration conversion factors
// ---------------------------------------------------------------------------

constexpr std::int64_t kMillisPerSecond = 1000;
constexpr std::int32_t kNanosPerMilli = 1'000'000;

// ---------------------------------------------------------------------------
// gRPC DATA frame constants
// ---------------------------------------------------------------------------

constexpr std::size_t kGrpcFrameHeaderSize = 5U;
constexpr std::uint8_t kGrpcUncompressedFlag = 0x00U;
constexpr std::uint8_t kGrpcCompressedFlag = 0x01U;
constexpr std::uint8_t kByteMask = 0xFFU;
constexpr unsigned kByteShift24 = 24U;
constexpr unsigned kByteShift16 = 16U;
constexpr unsigned kByteShift8 = 8U;

// Sentinel for a gRPC status that could not be parsed from headers
constexpr int kGrpcStatusUnparsed = -1;

// ---------------------------------------------------------------------------
// Base64 URL-safe decode (RFC 4648 §5, no padding required)
// ---------------------------------------------------------------------------

[[nodiscard]] int B64CharValue(char c) noexcept
{
    if (c >= 'A' && c <= 'Z')
    {
        return c - 'A';
    }
    if (c >= 'a' && c <= 'z')
    {
        return c - 'a' + kB64AlphaLowerOffset;
    }
    if (c >= '0' && c <= '9')
    {
        return c - '0' + kB64DigitOffset;
    }
    if (c == '-' || c == '+')
    {
        return kB64PlusOrMinus;
    }
    if (c == '_' || c == '/')
    {
        return kB64SlashOrUnderscore;
    }
    return kB64InvalidChar;
}

[[nodiscard]] std::optional<std::vector<std::uint8_t>> Base64UrlDecode(std::string_view sv)
{
    while (!sv.empty() && sv.back() == '=')
    {
        sv.remove_suffix(1);
    }
    std::vector<std::uint8_t> out;
    out.reserve(((sv.size() * kB64OutputBytesPerGroup) + (kB64InputCharsPerGroup - 1U)) /
                kB64InputCharsPerGroup);
    std::size_t i = 0;
    while (i + 1U < sv.size())
    {
        const int c0 = B64CharValue(sv[i]);
        const int c1 = B64CharValue(sv[i + 1U]);
        if (c0 < 0 || c1 < 0)
        {
            return std::nullopt;
        }
        out.push_back(static_cast<std::uint8_t>((static_cast<unsigned>(c0) << 2U) |
                                                (static_cast<unsigned>(c1) >> 4U)));
        i += 2U;
        if (i >= sv.size())
        {
            continue;
        }
        const int c2 = B64CharValue(sv[i]);
        if (c2 < 0)
        {
            return std::nullopt;
        }
        out.push_back(static_cast<std::uint8_t>(((static_cast<unsigned>(c1) & kB64Mask4Bit) << 4U) |
                                                (static_cast<unsigned>(c2) >> 2U)));
        ++i;
        if (i >= sv.size())
        {
            continue;
        }
        const int c3 = B64CharValue(sv[i]);
        if (c3 < 0)
        {
            return std::nullopt;
        }
        out.push_back(static_cast<std::uint8_t>(((static_cast<unsigned>(c2) & kB64Mask2Bit) << 6U) |
                                                static_cast<unsigned>(c3)));
        ++i;
    }
    return out;
}

// ---------------------------------------------------------------------------
// Minimal proto wire-format reader
// ---------------------------------------------------------------------------

using ByteSpan = std::span<const std::uint8_t>;

[[nodiscard]] std::optional<std::uint64_t> ReadVarint(ByteSpan& buf)
{
    std::uint64_t result = 0;
    unsigned shift = 0U;
    while (!buf.empty())
    {
        const std::uint8_t b = buf.front();
        buf = buf.subspan(1);
        result |= static_cast<std::uint64_t>(b & kVarintDataMask) << shift;
        if ((b & kVarintContinueBit) == 0U)
        {
            return result;
        }
        shift += kVarintShiftStep;
        if (shift >= kVarintMaxShift)
        {
            return std::nullopt;
        }
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<ByteSpan> ReadLenDelim(ByteSpan& buf)
{
    const auto len = ReadVarint(buf);
    if (!len.has_value() || *len > buf.size())
    {
        return std::nullopt;
    }
    const ByteSpan result = buf.subspan(0, static_cast<std::size_t>(*len));
    buf = buf.subspan(static_cast<std::size_t>(*len));
    return result;
}

[[nodiscard]] bool SkipField(ByteSpan& buf, std::uint32_t wire_type)
{
    constexpr std::uint32_t kWtVarint = 0;
    constexpr std::uint32_t kWtLenDelim = 2;
    constexpr std::uint32_t kWt64Bit = 1;
    constexpr std::uint32_t kWt32Bit = 5;
    if (wire_type == kWtVarint)
    {
        return ReadVarint(buf).has_value();
    }
    if (wire_type == kWtLenDelim)
    {
        return ReadLenDelim(buf).has_value();
    }
    if (wire_type == kWt64Bit)
    {
        if (buf.size() < kWireWidth64Bit)
        {
            return false;
        }
        buf = buf.subspan(kWireWidth64Bit);
        return true;
    }
    if (wire_type == kWt32Bit)
    {
        if (buf.size() < kWireWidth32Bit)
        {
            return false;
        }
        buf = buf.subspan(kWireWidth32Bit);
        return true;
    }
    return false;
}

// Handles one field in a google.protobuf.Duration message.
[[nodiscard]] bool ParseDurationField(
    ByteSpan& data, std::uint32_t fn, std::uint32_t wt, std::int64_t& seconds, std::int32_t& nanos)
{
    if (fn == kFieldDurationSeconds && wt == 0U)
    {
        const auto v = ReadVarint(data);
        if (!v.has_value())
        {
            return false;
        }
        seconds = static_cast<std::int64_t>(*v);
        return true;
    }
    if (fn == kFieldDurationNanos && wt == 0U)
    {
        const auto v = ReadVarint(data);
        if (!v.has_value())
        {
            return false;
        }
        nanos = static_cast<std::int32_t>(*v);
        return true;
    }
    return SkipField(data, wt);
}

// Parses google.protobuf.Duration → chrono::milliseconds.
[[nodiscard]] std::optional<std::chrono::milliseconds> ParseDurationMs(ByteSpan data)
{
    std::int64_t seconds = 0;
    std::int32_t nanos = 0;
    while (!data.empty())
    {
        const auto tag = ReadVarint(data);
        if (!tag.has_value())
        {
            return std::nullopt;
        }
        const auto fn = static_cast<std::uint32_t>(*tag >> 3U);
        const auto wt = static_cast<std::uint32_t>(*tag & 0x7U);
        if (!ParseDurationField(data, fn, wt, seconds, nanos))
        {
            return std::nullopt;
        }
    }
    return std::chrono::milliseconds{(seconds * kMillisPerSecond) + (nanos / kNanosPerMilli)};
}

// Reads the retry_delay field (field 1, wire type 2) from a RetryInfo message.
[[nodiscard]] std::optional<std::chrono::milliseconds> ReadRetryDelayField(ByteSpan& data)
{
    const auto delay_bytes = ReadLenDelim(data);
    if (!delay_bytes.has_value())
    {
        return std::nullopt;
    }
    return ParseDurationMs(*delay_bytes);
}

// Parses google.rpc.RetryInfo → retry_delay as chrono::milliseconds.
[[nodiscard]] std::optional<std::chrono::milliseconds> ParseRetryInfoMs(ByteSpan data)
{
    while (!data.empty())
    {
        const auto tag = ReadVarint(data);
        if (!tag.has_value())
        {
            return std::nullopt;
        }
        const auto fn = static_cast<std::uint32_t>(*tag >> 3U);
        const auto wt = static_cast<std::uint32_t>(*tag & 0x7U);
        if (fn == kFieldRetryDelay && wt == 2U)
        {
            return ReadRetryDelayField(data);
        }
        if (!SkipField(data, wt))
        {
            return std::nullopt;
        }
    }
    return std::nullopt;
}

// Handles one field in a google.protobuf.Any message.
[[nodiscard]] bool ParseAnyField(ByteSpan& data,
                                 std::uint32_t fn,
                                 std::uint32_t wt,
                                 std::string& type_url,
                                 std::optional<ByteSpan>& value)
{
    if (fn == kFieldAnyTypeUrl && wt == 2U)
    {
        const auto bytes = ReadLenDelim(data);
        if (!bytes.has_value())
        {
            return false;
        }
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
        type_url.assign(reinterpret_cast<const char*>(bytes->data()), bytes->size());
        return true;
    }
    if (fn == kFieldAnyValue && wt == 2U)
    {
        value = ReadLenDelim(data);
        return value.has_value();
    }
    return SkipField(data, wt);
}

// Parses one google.protobuf.Any and returns {type_url, value bytes}.
[[nodiscard]] std::optional<std::pair<std::string, ByteSpan>> ParseAny(ByteSpan data)
{
    std::string type_url;
    std::optional<ByteSpan> value;
    while (!data.empty())
    {
        const auto tag = ReadVarint(data);
        if (!tag.has_value())
        {
            return std::nullopt;
        }
        const auto fn = static_cast<std::uint32_t>(*tag >> 3U);
        const auto wt = static_cast<std::uint32_t>(*tag & 0x7U);
        if (!ParseAnyField(data, fn, wt, type_url, value))
        {
            return std::nullopt;
        }
    }
    if (!value.has_value())
    {
        return std::nullopt;
    }
    return std::pair{std::move(type_url), *value};
}

// Tries to extract RetryInfo from one google.protobuf.Any field.
[[nodiscard]] std::optional<std::chrono::milliseconds> TryParseRetryInfoFromAny(ByteSpan any_bytes)
{
    auto any = ParseAny(any_bytes);
    if (!any.has_value() || any->first != kRetryInfoTypeUrl)
    {
        return std::nullopt;
    }
    return ParseRetryInfoMs(any->second);
}

struct RetrySearchSignal
{
    bool stop = false;
    std::optional<std::chrono::milliseconds> delay;
};

// Processes one field from google.rpc.Status: returns {stop=true} when the search
// should end (RetryInfo found, or unrecoverable parse error), {stop=false} to continue.
[[nodiscard]] RetrySearchSignal SearchStatusField(ByteSpan& data,
                                                  std::uint32_t fn,
                                                  std::uint32_t wt)
{
    if (fn == kFieldStatusDetails && wt == 2U)
    {
        const auto any_bytes = ReadLenDelim(data);
        if (!any_bytes.has_value())
        {
            return RetrySearchSignal{.stop = true, .delay = {}};
        }
        const auto result = TryParseRetryInfoFromAny(*any_bytes);
        return RetrySearchSignal{.stop = result.has_value(), .delay = result};
    }
    return RetrySearchSignal{.stop = !SkipField(data, wt), .delay = {}};
}

// Walks google.rpc.Status.details[] for a RetryInfo entry.
[[nodiscard]] std::optional<std::chrono::milliseconds> ParseStatusForRetryInfo(ByteSpan data)
{
    while (!data.empty())
    {
        const auto tag = ReadVarint(data);
        if (!tag.has_value())
        {
            return std::nullopt;
        }
        const auto fn = static_cast<std::uint32_t>(*tag >> 3U);
        const auto wt = static_cast<std::uint32_t>(*tag & 0x7U);
        const auto signal = SearchStatusField(data, fn, wt);
        if (signal.stop)
        {
            return signal.delay;
        }
    }
    return std::nullopt;
}

// Decodes grpc-status-details-bin (base64) and extracts RetryInfo delay.
[[nodiscard]] std::optional<std::chrono::milliseconds> TryDecodeRetryDelay(
    std::string_view details_bin)
{
    const auto bytes = Base64UrlDecode(details_bin);
    if (!bytes.has_value() || bytes->empty())
    {
        return std::nullopt;
    }
    const ByteSpan span{bytes->data(), bytes->size()};
    return ParseStatusForRetryInfo(span);
}

// ---------------------------------------------------------------------------
// Header utilities
// ---------------------------------------------------------------------------

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

[[nodiscard]] int ParseHttpStatus(const std::vector<internal::HeaderField>& headers) noexcept
{
    for (const auto& h : headers)
    {
        if (h.name != ":status")
        {
            continue;
        }
        int code = 0;
        const auto* const p = h.value.data();
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
        if (std::from_chars(p, p + h.value.size(), code).ec == std::errc{})
        {
            return code;
        }
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Response DATA frame decoding — §2.3 (framing) and §5.2 (decompression)
// ---------------------------------------------------------------------------

/// Why a response frame yielded no message. All are terminal; they differ
/// only in which counter and which operator message they produce.
enum class FrameError : std::uint8_t
{
    Malformed = 0,
    TooLarge = 1,
    /// The frame was sound but its message is not a well-formed Export
    /// response (issue #223).
    UnparseableMessage = 2,
};

[[nodiscard]] std::uint32_t ReadFrameLength(std::span<const std::byte> body) noexcept
{
    return (std::to_integer<std::uint32_t>(body[1]) << kByteShift24) |
           (std::to_integer<std::uint32_t>(body[2]) << kByteShift16) |
           (std::to_integer<std::uint32_t>(body[3]) << kByteShift8) |
           std::to_integer<std::uint32_t>(body[4]);
}

/// @brief Validate one response DATA frame and hand back its message bytes.
///
/// Replaces an unconditional 5-byte skip that read neither the compression
/// flag nor the declared length: a compressed or truncated response reached
/// the protobuf parser as noise, and `ParseRejectedSpans` then reported 0 for
/// noise exactly as for an absent body — so a rejected batch looked clean.
///
/// @param body the whole response body; empty is a legal empty success.
/// @param max_decompressed ceiling applied to a `CF = 0x01` message.
/// @param owned receives the inflated bytes when the message was compressed.
///        The returned span borrows it, so it must outlive that span.
/// @return the message bytes ready for `ParseRejectedSpans`.
[[nodiscard]] microtel::Expected<std::span<const std::byte>, FrameError> DecodeResponseFrame(
    std::span<const std::byte> body, std::size_t max_decompressed, std::vector<std::byte>& owned)
{
    if (body.empty())
    {
        // No DATA at all: the ordinary empty success, and the trailer-only
        // shape from §2.5. Not a truncated frame.
        return std::span<const std::byte>{};
    }
    if (body.size() < kGrpcFrameHeaderSize)
    {
        return microtel::make_unexpected(FrameError::Malformed);
    }
    const auto flag = std::to_integer<std::uint8_t>(body[0]);
    const auto message = body.subspan(kGrpcFrameHeaderSize);
    // Equality, not "at least": a short count leaves trailing bytes, which for
    // OTLP unary means a second message, and §2.3 rejects that.
    if (ReadFrameLength(body) != message.size())
    {
        return microtel::make_unexpected(FrameError::Malformed);
    }
    if (flag == kGrpcUncompressedFlag)
    {
        return message;
    }
    if (flag != kGrpcCompressedFlag)
    {
        return microtel::make_unexpected(FrameError::Malformed);
    }
    auto inflated = GzipDecompress(message, max_decompressed);
    if (!inflated)
    {
        const bool too_large = (inflated.error() == GzipDecompressError::TooLarge);
        return microtel::make_unexpected(too_large ? FrameError::TooLarge : FrameError::Malformed);
    }
    owned = std::move(*inflated);
    return std::span<const std::byte>{owned};
}

[[nodiscard]] internal::WireResult FrameFailure(FrameError err, internal::IDiagnosticsSink* diag)
{
    const bool too_large = (err == FrameError::TooLarge);
    const auto reason =
        too_large ? DropReason::DecompressionTooLarge : DropReason::MalformedResponse;
    if (diag != nullptr)
    {
        diag->RecordDrop(reason);
    }
    std::string_view message = "malformed gRPC response frame";
    if (too_large)
    {
        message = "response exceeds max_decompressed_bytes";
    }
    else if (err == FrameError::UnparseableMessage)
    {
        message = "unparseable export response message";
    }
    return internal::WireResult{
        .success = false,
        // None is transient: a peer that mis-frames, answers with an
        // unparseable message, or with a decompression bomb, does the same on
        // the retry. error-model.md §7.2 makes all of them terminal. `Kind` is
        // `Malformed` for each — the counter and the message carry the
        // distinction, and `Error::Kind` is public surface not worth churning
        // for it.
        .retryable = false,
        .retry_after = {},
        .partial_success_rejected = 0,
        .error = microtel::Error{.kind = microtel::Error::Kind::Malformed,
                                 .message = std::string{message}},
        .response_excerpt = {},
    };
}

// ---------------------------------------------------------------------------
// grpc-message (§4.4)
// ---------------------------------------------------------------------------

/// @brief Find and percent-decode `grpc-message`.
///
/// Trailers first, then the initial HEADERS — the same fallback the
/// `grpc-status` lookup uses, because a trailer-only response (§2.5) carries
/// both in the first frame.
///
/// @return the decoded text, or empty when the trailer is absent or empty.
[[nodiscard]] std::string DecodeGrpcMessage(const internal::TransportResult& tr)
{
    auto raw = FindHeaderValue(tr.response_trailers, "grpc-message");
    if (!raw.has_value())
    {
        raw = FindHeaderValue(tr.response_headers, "grpc-message");
    }
    if (!raw.has_value() || raw->empty())
    {
        return {};
    }
    return PercentDecode(*raw);
}

/// @brief Truncate @p message for `WireResult::response_excerpt`.
///
/// Mirrors `HttpWireCodec::BuildExcerpt`: a diagnostics field, bounded, never
/// the authoritative copy of anything.
[[nodiscard]] std::string BuildExcerpt(const std::string& message)
{
    if (message.size() <= kMaxGrpcMessageChars)
    {
        return message;
    }
    return message.substr(0, kMaxGrpcMessageChars);
}

// ---------------------------------------------------------------------------
// gRPC status classification
// ---------------------------------------------------------------------------

/// @brief §4.2 warn, emitted once per connection.
///
/// A proxy that terminates gRPC streams without trailers does it to every
/// export, so warning per batch would bury the very signal it raises. The flag
/// lives on the codec and resets when a new connection is established, which
/// is the unit §4.2 names.
void WarnMissingGrpcStatusOnce(int http_status, bool& already_warned)
{
    if (already_warned)
    {
        return;
    }
    already_warned = true;
    internal::LogImpl(LogLevel::Warn,
                      "gRPC response carried no grpc-status trailer (HTTP " +
                          std::to_string(http_status) +
                          ") - an intermediary is terminating gRPC streams without trailers; "
                          "see docs/grpc-wire-protocol.md 4.2");
}

[[nodiscard]] internal::WireResult ClassifyMissingGrpcStatus(
    const std::vector<internal::HeaderField>& headers,
    internal::IDiagnosticsSink* diag,
    bool& already_warned)
{
    const int http_status = ParseHttpStatus(headers);
    const bool retryable =
        (http_status == 429 || http_status == 502 || http_status == 503 || http_status == 504);
    // Only the non-retryable half is malformed: a 429/502/503/504 without
    // grpc-status is an intermediary talking, and error-model.md §7.2 names
    // no counter for it.
    if (!retryable && diag != nullptr)
    {
        diag->RecordDrop(DropReason::MalformedResponse);
    }
    // Warned either way: retryable or not, the peer is not speaking gRPC and
    // that is what the operator needs to know.
    WarnMissingGrpcStatusOnce(http_status, already_warned);
    return internal::WireResult{
        .success = false,
        .retryable = retryable,
        .retry_after = {},
        .partial_success_rejected = 0,
        // Naming the HTTP status is the whole point: "missing grpc-status"
        // alone cannot tell a 404 endpoint from a 502 proxy.
        .error = microtel::Error{.kind = microtel::Error::Kind::Protocol,
                                 .message = "missing grpc-status (HTTP " +
                                            std::to_string(http_status) + ")"},
        .response_excerpt = {},
    };
}

/// @brief The terminal half of RESOURCE_EXHAUSTED: overloaded, no RetryInfo.
///
/// Reached both when `grpc-status-details-bin` is absent and when it is present
/// but carries no decodable `RetryInfo` — indistinguishable to the caller, and
/// error-model.md §7.2 treats them as one row.
[[nodiscard]] internal::WireResult ResourceExhaustedWithoutRetryInfo(
    const internal::TransportResult& tr)
{
    constexpr int kResourceExhausted = 8;
    const std::string decoded = DecodeGrpcMessage(tr);
    std::string message = FormatGrpcError(kResourceExhausted, decoded);
    // The absent RetryInfo is why this is terminal rather than backed off, so
    // it belongs in the message even when the server said nothing else.
    message.append(" - no RetryInfo, not retried");
    return internal::WireResult{
        .success = false,
        .retryable = false,
        .retry_after = {},
        .partial_success_rejected = 0,
        .error =
            microtel::Error{.kind = microtel::Error::Kind::Protocol, .message = std::move(message)},
        .response_excerpt = BuildExcerpt(decoded),
    };
}

[[nodiscard]] internal::WireResult ClassifyResourceExhausted(const internal::TransportResult& tr)
{
    auto details = FindHeaderValue(tr.response_trailers, "grpc-status-details-bin");
    if (!details.has_value())
    {
        details = FindHeaderValue(tr.response_headers, "grpc-status-details-bin");
    }
    if (!details.has_value())
    {
        return ResourceExhaustedWithoutRetryInfo(tr);
    }
    const auto delay = TryDecodeRetryDelay(*details);
    if (!delay.has_value())
    {
        return ResourceExhaustedWithoutRetryInfo(tr);
    }
    return internal::WireResult{
        .success = false,
        .retryable = true,
        .retry_after = delay,
        .partial_success_rejected = 0,
        .error = {},
        .response_excerpt = {},
    };
}

/// @brief `grpc-status: 0` — the body is the partial-success message.
[[nodiscard]] internal::WireResult ClassifyGrpcSuccess(const internal::TransportResult& tr,
                                                       std::size_t max_decompressed,
                                                       internal::IDiagnosticsSink* diag)
{
    // Owns the inflated bytes for as long as `message` borrows them.
    std::vector<std::byte> inflated;
    const auto message = DecodeResponseFrame(tr.response_body, max_decompressed, inflated);
    if (!message)
    {
        return FrameFailure(message.error(), diag);
    }
    const auto parsed = ParseRejectedSpans(*message);
    if (parsed.outcome == PartialSuccessOutcome::Unparseable)
    {
        // error-model.md §7.2 "truncated message": terminal, malformed_response.
        return FrameFailure(FrameError::UnparseableMessage, diag);
    }
    return internal::WireResult{
        .success = true,
        .retryable = false,
        .retry_after = {},
        .partial_success_rejected = parsed.rejected,
        .error = {},
        .response_excerpt = {},
    };
}

[[nodiscard]] internal::WireResult ClassifyGrpcCode(int code,
                                                    const internal::TransportResult& tr,
                                                    std::size_t max_decompressed,
                                                    internal::IDiagnosticsSink* diag)
{
    if (code == 0)
    {
        return ClassifyGrpcSuccess(tr, max_decompressed, diag);
    }
    constexpr int kResourceExhausted = 8;
    if (code == kResourceExhausted)
    {
        // The one status whose retryability the table cannot answer on its own.
        return ClassifyResourceExhausted(tr);
    }
    // Retryability comes from the §7.2 table rather than a row of loose
    // comparisons, so the matrix and the code cannot drift. A code outside the
    // canonical range has no entry and is terminal: we cannot classify what we
    // do not recognise.
    const auto info = LookupGrpcStatus(code);
    const bool retryable = info.has_value() && info->retryable;
    const std::string decoded = DecodeGrpcMessage(tr);
    return internal::WireResult{
        .success = false,
        .retryable = retryable,
        .retry_after = {},
        .partial_success_rejected = 0,
        .error = microtel::Error{.kind = microtel::Error::Kind::Protocol,
                                 .message = FormatGrpcError(code, decoded)},
        .response_excerpt = BuildExcerpt(decoded),
    };
}

/// @brief Classify a request the transport failed rather than answered.
///
/// Transport-level failures — connection reset, refused, read timeout — are
/// retryable, matching `HttpWireCodec` and this codec's own `EnsureConnected`
/// path (ICP 0017): they are the transient conditions the retry engine exists
/// for, and returning false here once meant a gRPC deployment dropped a batch
/// on the first collector restart that HTTP would have delivered.
///
/// A response the transport refused to buffer is the exception. The peer sends
/// the same oversized response on the retry, so it is terminal and counted
/// (`docs/error-model.md` §3 and §7.1). A request the transport refused to
/// *queue* stays retryable — a full request queue drains — but is counted too,
/// because the batch behind it was not delivered on this attempt.
[[nodiscard]] internal::WireResult ClassifyTransportFailure(const internal::TransportResult& tr,
                                                            internal::IDiagnosticsSink* diag)
{
    if (tr.response_too_large && diag != nullptr)
    {
        diag->RecordDrop(DropReason::ResponseTooLarge);
    }
    if (tr.transport_busy && diag != nullptr)
    {
        diag->RecordDrop(DropReason::TransportBusy);
    }
    return internal::WireResult{
        .success = false,
        .retryable = !tr.response_too_large,
        .retry_after = {},
        .partial_success_rejected = 0,
        .error = tr.error,
        .response_excerpt = {},
    };
}

[[nodiscard]] internal::WireResult ClassifyResponse(const internal::TransportResult& tr,
                                                    std::size_t max_decompressed,
                                                    internal::IDiagnosticsSink* diag,
                                                    bool& already_warned)
{
    if (tr.error.has_value())
    {
        return ClassifyTransportFailure(tr, diag);
    }
    auto status_sv = FindHeaderValue(tr.response_trailers, "grpc-status");
    if (!status_sv.has_value())
    {
        status_sv = FindHeaderValue(tr.response_headers, "grpc-status");
    }
    if (!status_sv.has_value())
    {
        return ClassifyMissingGrpcStatus(tr.response_headers, diag, already_warned);
    }
    int code = kGrpcStatusUnparsed;
    const auto* const p = status_sv->data();
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    (void)std::from_chars(p, p + status_sv->size(), code);
    return ClassifyGrpcCode(code, tr, max_decompressed, diag);
}

// ---------------------------------------------------------------------------
// gRPC frame builder — 5-byte length prefix (compression flag + BE uint32)
// ---------------------------------------------------------------------------

[[nodiscard]] std::vector<std::byte> FramePayload(std::span<const std::byte> body, bool compressed)
{
    const std::size_t n = body.size();
    // GCC 15 false-positive -Wfree-nonheap-object fires on reserve+push_back
    // when inlined; size-constructor + index assignment avoids that analysis path.
    std::vector<std::byte> framed(kGrpcFrameHeaderSize + n);
    framed[0] = std::byte{compressed ? kGrpcCompressedFlag : kGrpcUncompressedFlag};
    framed[1] = std::byte{static_cast<std::uint8_t>((n >> kByteShift24) & kByteMask)};
    framed[2] = std::byte{static_cast<std::uint8_t>((n >> kByteShift16) & kByteMask)};
    framed[3] = std::byte{static_cast<std::uint8_t>((n >> kByteShift8) & kByteMask)};
    framed[4] = std::byte{static_cast<std::uint8_t>(n & kByteMask)};
    std::ranges::copy(body, framed.begin() + kGrpcFrameHeaderSize);
    return framed;
}

}  // namespace

// ---------------------------------------------------------------------------
// GrpcWireCodec
// ---------------------------------------------------------------------------

GrpcWireCodec::GrpcWireCodec(internal::ITransport* transport,
                             GrpcWireCodecConfig config,
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

microtel::Expected<std::vector<internal::HeaderField>, internal::WireResult>
GrpcWireCodec::BuildHeaders(bool compressed) const
{
    std::vector<internal::HeaderField> headers;
    headers.push_back({.name = ":method", .value = "POST"});
    headers.push_back({.name = ":scheme", .value = m_config.scheme});
    headers.push_back({.name = ":authority", .value = m_config.host});
    const std::string path =
        m_config.service_path.empty() ? std::string{kGrpcTracesPath} : m_config.service_path;
    headers.push_back({.name = ":path", .value = path});
    headers.push_back({.name = "te", .value = "trailers"});
    headers.push_back({.name = "content-type", .value = "application/grpc+proto"});
    headers.push_back({.name = "user-agent", .value = std::string{kUserAgent}});
    // Unconditional, and independent of `compression_gzip` (§5.2): this says
    // what the client can decode, not what it chose to encode. The codec now
    // handles `CF = 0x01` on the way back, so there is nothing left to gate it
    // on — and staying silent costs response bandwidth for no benefit.
    headers.push_back({.name = "grpc-accept-encoding", .value = "gzip"});
    if (compressed)
    {
        headers.push_back({.name = "grpc-encoding", .value = "gzip"});
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

std::optional<microtel::Error> GrpcWireCodec::AppendAuthHeader(
    std::vector<internal::HeaderField>& headers) const
{
    if (m_auth == nullptr)
    {
        return std::nullopt;
    }
    const auto now = (m_clock != nullptr) ? m_clock->Now() : std::chrono::steady_clock::now();
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

std::optional<internal::WireResult> GrpcWireCodec::EnsureConnected()
{
    if (m_transport->GetState() == ConnectionState::Connected)
    {
        return std::nullopt;
    }
    auto connected = m_transport->Connect(m_connect_opts);
    if (!connected)
    {
        // Same reasoning as HttpWireCodec::EnsureConnected: the transport owns
        // no sink, so the codec records what it observed. Recorded for every
        // kind, retryable or not — the loss is the same to an operator.
        if (m_diag != nullptr)
        {
            m_diag->RecordDrop(DropReason::ConnectFailure);
        }
        // A `Protocol` connect failure is a permanent mismatch — a TLS
        // endpoint that would not negotiate h2 (issue #166). No backoff
        // outlasts a misconfiguration, so retrying one spends the whole budget
        // and buries the message that names the fix. Everything else the
        // transport reports (Network, Cancelled) is worth another attempt.
        return internal::WireResult{
            .success = false,
            .retryable = connected.error().kind != microtel::Error::Kind::Protocol,
            .retry_after = {},
            .partial_success_rejected = 0,
            .error = connected.error(),
            .response_excerpt = {},
        };
    }
    // A new connection: the §4.2 warn is per-connection, so the next malformed
    // response on this one is a first occurrence again.
    m_warned_missing_grpc_status = false;
    return std::nullopt;
}

internal::WireResult GrpcWireCodec::Send(internal::EncodedPayload&& payload,
                                         std::chrono::milliseconds deadline)
{
    if (auto failure = EnsureConnected())
    {
        return std::move(*failure);
    }

    const internal::EncodedPayload owned = std::move(payload);
    std::vector<std::byte> compressed;
    std::span<const std::byte> body = owned.Bytes();
    bool did_compress = false;
    if (m_config.compression_gzip)
    {
        // On failure fall back to the uncompressed body rather than dropping
        // the batch. gRPC's compression flag is per-message, so an
        // uncompressed message is legal even with `grpc-encoding` negotiated
        // — but only if the flag and the header agree with what was actually
        // done, which is why `did_compress` drives both below.
        auto result = GzipCompress(body);
        if (result)
        {
            compressed = std::move(*result);
            body = compressed;
            did_compress = true;
        }
    }
    const auto framed = FramePayload(body, did_compress);
    auto headers = BuildHeaders(did_compress);
    if (!headers)
    {
        // The auth provider failed, or a field cannot be carried: drop the
        // batch rather than send it without the header (`docs/interfaces.md`
        // §4.9, issue #250) or with an injected one (ICP 0042 §3).
        return std::move(headers.error());
    }
    internal::RequestSpec spec{
        .headers = std::move(*headers),
        .payload = std::span<const std::byte>{framed.data(), framed.size()},
        .deadline = deadline,
    };
    auto handle = m_transport->Send(std::move(spec));
    auto& fut = handle.Future();

    // Bounded, matching HttpWireCodec::Send. This was an unbounded get(),
    // which blocked forever whenever a promise went unfulfilled — as the
    // mid-connection drop path did until ICP 0018. That path now fulfils, so
    // this is defence in depth: a future bug that abandons a promise costs one
    // deadline, not a permanently wedged exporter thread.
    if (fut.wait_for(deadline) != std::future_status::ready)
    {
        m_transport->Cancel(handle);
        return internal::WireResult{
            .success = false,
            .retryable = true,
            .retry_after = {},
            .partial_success_rejected = 0,
            .error = microtel::Error{.kind = microtel::Error::Kind::Cancelled,
                                     .message = "request deadline exceeded"},
            .response_excerpt = {},
        };
    }

    const auto tr = fut.get();
    return ClassifyResponse(
        tr, m_config.max_decompressed_bytes, m_diag, m_warned_missing_grpc_status);
}

}  // namespace microtel::wire
