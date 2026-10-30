// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "microtel/internal/transport.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace microtel::transport
{

/// @brief The bounds an `Http1ResponseParser` enforces.
///
/// Both come from `ConnectOptions`, so a response costs the same memory over
/// HTTP/1.1 as over HTTP/2 (`docs/memory-model.md` §6).
struct Http1ResponseLimits
{
    /// `max_response_bytes`: the body, after chunked decoding.
    std::size_t max_body_bytes = 0;
    /// `max_trailer_bytes`: the raw bytes of the head (status line and header
    /// fields), and separately those of a chunked body's trailer section.
    std::size_t max_header_bytes = 0;
};

/// @brief An incremental parser for one HTTP/1.1 response (ICP 0042 §3).
///
/// A push parser: `Feed` takes whatever bytes the socket produced, in pieces
/// of any size down to one byte, and the result does not depend on how the
/// response was split. It reads only what a client sending one `POST` at a
/// time needs:
///
/// - the status line, `HTTP/1.0` or `HTTP/1.1`;
/// - header fields, with obs-fold rejected and the head bounded by
///   `max_header_bytes`;
/// - `1xx` interim responses, which are skipped (`101` is a failure: no
///   upgrade is ever requested);
/// - a body framed by `Content-Length`, by `Transfer-Encoding: chunked`
///   (extensions ignored, trailers read and discarded under the same bound),
///   or by the connection closing; `204` and `304` have none.
///
/// It rejects a response carrying both `Content-Length` and
/// `Transfer-Encoding` (RFC 9112 §6.3), and any `Transfer-Encoding` other
/// than `chunked`. A line may end in CRLF or a bare LF (RFC 9112 §2.2); a CR
/// anywhere else is a failure.
///
/// Memory is bounded by the limits: the head and the trailer section by
/// `max_header_bytes` each, the body by `max_body_bytes`.
///
/// One parser serves one connection: `Reset` readies it for the next
/// response. Not thread-safe; the transport drives it from its I/O thread.
class Http1ResponseParser
{
public:
    /// @brief Where parsing stands.
    enum class State : std::uint8_t
    {
        NeedMore,  ///< The response is incomplete; feed more bytes, or EOF.
        Complete,  ///< The response is complete; read it, then `Reset`.
        Failed,    ///< The response is unusable; see `GetFault`.
    };

    /// @brief Why parsing failed.
    enum class Fault : std::uint8_t
    {
        None,
        MalformedStatusLine,
        MalformedHeader,
        ObsFold,
        HeadersTooLarge,
        /// Both `Content-Length` and `Transfer-Encoding` (RFC 9112 §6.3).
        ConflictingFraming,
        UnsupportedTransferEncoding,
        MalformedContentLength,
        MalformedChunk,
        TrailersTooLarge,
        BodyTooLarge,
        /// `101 Switching Protocols`, which nothing microtel sends can ask for.
        UnexpectedUpgrade,
        /// The connection closed before the response was complete.
        Truncated,
    };

    explicit Http1ResponseParser(Http1ResponseLimits limits) noexcept;

    /// @brief Parse the next bytes of the response.
    ///
    /// Stops at the end of the response: bytes after it are not consumed. A
    /// client that never pipelines has no use for them, and the caller treats
    /// them as a protocol error.
    ///
    /// @return How many bytes of @p data were consumed. Zero once the state is
    ///         `Complete` or `Failed`.
    std::size_t Feed(std::span<const std::byte> data);

    /// @brief The peer closed the connection.
    ///
    /// Completes a body delimited by the connection closing; anything else
    /// still incomplete becomes `Fault::Truncated`. No effect once the state
    /// is `Complete` or `Failed`.
    void FeedEof();

    [[nodiscard]] State GetState() const noexcept
    {
        return m_state;
    }

    [[nodiscard]] Fault GetFault() const noexcept
    {
        return m_fault;
    }

    /// @brief True when the fault is one of the size limits, which the
    ///        transport reports as `TransportResult::response_too_large`.
    [[nodiscard]] bool IsTooLarge() const noexcept;

    /// @brief True once any byte of the response has been consumed, so a
    ///        close with nothing received can be told from a truncation.
    [[nodiscard]] bool HasStarted() const noexcept
    {
        return m_started;
    }

    /// @brief The final status code; 0 until the status line is parsed.
    [[nodiscard]] int StatusCode() const noexcept
    {
        return m_status;
    }

    /// @brief Whether the connection may carry the next request.
    ///
    /// False after `Connection: close`, an `HTTP/1.0` status line, or a body
    /// delimited by the connection closing. Meaningful once `Complete`.
    [[nodiscard]] bool KeepAlive() const noexcept
    {
        return m_keep_alive;
    }

    /// @brief The final response's header fields.
    ///
    /// In the shape `HttpWireCodec` reads from HTTP/2: names lowercased, and a
    /// synthesised `:status` entry first. Interim responses' fields are not
    /// included.
    [[nodiscard]] std::vector<internal::HeaderField>& Headers() noexcept
    {
        return m_headers;
    }

    /// @brief The body, after chunked decoding.
    [[nodiscard]] std::vector<std::byte>& Body() noexcept
    {
        return m_body;
    }

    /// @brief A short description of @p fault, for an error message.
    [[nodiscard]] static std::string_view Describe(Fault fault) noexcept;

    /// @brief Ready the parser for the next response on the same connection.
    void Reset() noexcept;

private:
    enum class Phase : std::uint8_t
    {
        StatusLine,
        HeaderLine,
        FixedBody,
        ChunkSize,
        ChunkData,
        ChunkDataEnd,
        TrailerLine,
        CloseBody,
        Done,
    };

    std::size_t FeedLine(std::span<const std::byte> data);
    std::size_t FeedBody(std::span<const std::byte> data);
    void OnLine(std::string_view line);
    void OnStatusLine(std::string_view line);
    void OnHeaderLine(std::string_view line);
    void OnHeaderField(std::string_view name, std::string_view value);
    void OnConnectionField(std::string_view value);
    void OnContentLengthField(std::string_view value);
    void OnTransferEncodingField(std::string_view value);
    void OnEndOfHead();
    void StartBody();
    void OnChunkSizeLine(std::string_view line);
    void OnChunkDataEnd(std::string_view line);
    void OnTrailerLine(std::string_view line);
    [[nodiscard]] std::size_t LineBudget() const noexcept;
    void ChargeLineByte();
    void StartResponse() noexcept;
    void Fail(Fault fault) noexcept;
    void Finish() noexcept;

    Http1ResponseLimits m_limits;
    State m_state = State::NeedMore;
    Fault m_fault = Fault::None;
    Phase m_phase = Phase::StatusLine;
    /// The line being accumulated, without its terminator.
    std::string m_line;
    /// Bytes charged to the current head, or to the trailer section.
    std::size_t m_section_bytes = 0;
    /// Bytes still to read of a `Content-Length` body or of the current chunk.
    std::size_t m_remaining = 0;
    std::optional<std::size_t> m_content_length;
    bool m_chunked = false;
    bool m_started = false;
    bool m_keep_alive = true;
    bool m_http10 = false;
    int m_status = 0;
    std::vector<internal::HeaderField> m_headers;
    std::vector<std::byte> m_body;
};

}  // namespace microtel::transport
