// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#include "transport/http1_response_parser.hpp"

#include "microtel/internal/transport.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace microtel::transport
{

namespace
{

constexpr std::string_view kHttp10 = "HTTP/1.0";
constexpr std::string_view kHttp11 = "HTTP/1.1";
constexpr std::string_view kOptionalWhitespace = " \t";
constexpr std::size_t kStatusCodeDigits = 3;
constexpr int kMinStatus = 100;
constexpr int kMaxStatus = 599;
constexpr int kInformationalCeiling = 200;
constexpr int kSwitchingProtocols = 101;
constexpr int kNoContent = 204;
constexpr int kNotModified = 304;
constexpr int kDecimalBase = 10;
constexpr unsigned kHexBase = 16U;
constexpr unsigned kHexLetterOffset = 10U;
/// Sixteen hex digits fill 64 bits; a longer chunk size is not a size.
constexpr std::size_t kMaxChunkSizeDigits = 16;
constexpr unsigned char kDel = 0x7F;
constexpr unsigned char kFirstVisible = 0x20;
/// Where the status code starts: after the version and one SP.
constexpr std::size_t kStatusCodeOffset = kHttp11.size() + 1;

[[nodiscard]] char ToLowerAscii(char c) noexcept
{
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

[[nodiscard]] bool EqualsIgnoreCase(std::string_view a, std::string_view b) noexcept
{
    return std::ranges::equal(
        a, b, [](char x, char y) { return ToLowerAscii(x) == ToLowerAscii(y); });
}

[[nodiscard]] std::string_view Trim(std::string_view s) noexcept
{
    const auto first = s.find_first_not_of(kOptionalWhitespace);
    if (first == std::string_view::npos)
    {
        return {};
    }
    const auto last = s.find_last_not_of(kOptionalWhitespace);
    return s.substr(first, last - first + 1);
}

[[nodiscard]] bool IsDigit(char c) noexcept
{
    return c >= '0' && c <= '9';
}

/// RFC 9110 §5.6.2 `tchar`.
[[nodiscard]] bool IsTokenChar(char c) noexcept
{
    constexpr std::string_view kTokenPunctuation = "!#$%&'*+-.^_`|~";
    const bool alpha = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
    return alpha || IsDigit(c) || kTokenPunctuation.find(c) != std::string_view::npos;
}

[[nodiscard]] bool IsToken(std::string_view s) noexcept
{
    return !s.empty() && std::ranges::all_of(s, IsTokenChar);
}

/// RFC 9110 §5.5 field-value: visible characters, SP, HTAB and obs-text.
[[nodiscard]] bool IsFieldValue(std::string_view s) noexcept
{
    return std::ranges::all_of(s,
                               [](char c)
                               {
                                   const auto u = static_cast<unsigned char>(c);
                                   const bool control = u < kFirstVisible || u == kDel;
                                   return !control || c == '\t';
                               });
}

/// A decimal `Content-Length`, or `nullopt` if it is not one or overflows.
[[nodiscard]] std::optional<std::size_t> ParseDecimal(std::string_view s) noexcept
{
    if (s.empty())
    {
        return std::nullopt;
    }
    constexpr std::size_t kMax = std::numeric_limits<std::size_t>::max();
    std::size_t value = 0;
    for (const char c : s)
    {
        if (!IsDigit(c))
        {
            return std::nullopt;
        }
        const auto digit = static_cast<std::size_t>(c - '0');
        if (value > (kMax - digit) / kDecimalBase)
        {
            return std::nullopt;
        }
        value = (value * kDecimalBase) + digit;
    }
    return value;
}

[[nodiscard]] std::optional<unsigned> HexDigit(char c) noexcept
{
    if (IsDigit(c))
    {
        return static_cast<unsigned>(c - '0');
    }
    const char lower = ToLowerAscii(c);
    if (lower >= 'a' && lower <= 'f')
    {
        return static_cast<unsigned>(lower - 'a') + kHexLetterOffset;
    }
    return std::nullopt;
}

/// The size at the head of a chunk-size line, extensions ignored.
[[nodiscard]] std::optional<std::uint64_t> ParseChunkSize(std::string_view line) noexcept
{
    const auto end = line.find_first_not_of("0123456789abcdefABCDEF");
    const auto digits = line.substr(0, end);
    if (digits.empty() || digits.size() > kMaxChunkSizeDigits)
    {
        return std::nullopt;
    }
    const auto rest = Trim(line.substr(digits.size()));
    if (!rest.empty() && rest.front() != ';')
    {
        return std::nullopt;
    }
    std::uint64_t value = 0;
    for (const char c : digits)
    {
        value = (value * kHexBase) + HexDigit(c).value_or(0U);
    }
    return value;
}

/// The status code of a status line, or `nullopt` if the line is not one.
/// Sets @p http10 from the version.
[[nodiscard]] std::optional<int> ParseStatusLine(std::string_view line, bool& http10) noexcept
{
    const auto version = line.substr(0, kHttp11.size());
    if (version != kHttp11 && version != kHttp10)
    {
        return std::nullopt;
    }
    http10 = (version == kHttp10);
    auto rest = line.substr(version.size());
    if (rest.empty() || rest.front() != ' ')
    {
        return std::nullopt;
    }
    rest.remove_prefix(1);
    const auto digits = rest.substr(0, kStatusCodeDigits);
    if (digits.size() != kStatusCodeDigits || !std::ranges::all_of(digits, IsDigit))
    {
        return std::nullopt;
    }
    rest.remove_prefix(kStatusCodeDigits);
    if (!rest.empty() && rest.front() != ' ')
    {
        return std::nullopt;
    }
    int code = 0;
    for (const char c : digits)
    {
        code = (code * kDecimalBase) + (c - '0');
    }
    if (code < kMinStatus || code > kMaxStatus)
    {
        return std::nullopt;
    }
    return code;
}

}  // namespace

Http1ResponseParser::Http1ResponseParser(Http1ResponseLimits limits) noexcept : m_limits(limits) {}

bool Http1ResponseParser::IsTooLarge() const noexcept
{
    return m_fault == Fault::HeadersTooLarge || m_fault == Fault::TrailersTooLarge ||
           m_fault == Fault::BodyTooLarge;
}

std::string_view Http1ResponseParser::Describe(Fault fault) noexcept
{
    switch (fault)
    {
        case Fault::None:
            return "no fault";
        case Fault::MalformedStatusLine:
            return "malformed HTTP/1.1 status line";
        case Fault::MalformedHeader:
            return "malformed HTTP/1.1 header field";
        case Fault::ObsFold:
            return "obsolete line folding in an HTTP/1.1 header field";
        case Fault::HeadersTooLarge:
            return "response headers exceed max_trailer_bytes";
        case Fault::ConflictingFraming:
            return "response carries both Content-Length and Transfer-Encoding";
        case Fault::UnsupportedTransferEncoding:
            return "response Transfer-Encoding is not chunked";
        case Fault::MalformedContentLength:
            return "malformed Content-Length";
        case Fault::MalformedChunk:
            return "malformed chunked body";
        case Fault::TrailersTooLarge:
            return "response trailers exceed max_trailer_bytes";
        case Fault::BodyTooLarge:
            return "response exceeds max_response_bytes";
        case Fault::UnexpectedUpgrade:
            return "peer switched protocols unasked (101)";
        case Fault::Truncated:
            return "connection closed before the response was complete";
    }
    return "unknown fault";
}

void Http1ResponseParser::Reset() noexcept
{
    m_state = State::NeedMore;
    m_fault = Fault::None;
    m_started = false;
    m_keep_alive = true;
    m_http10 = false;
    m_body.clear();
    StartResponse();
}

void Http1ResponseParser::StartResponse() noexcept
{
    m_phase = Phase::StatusLine;
    m_line.clear();
    m_section_bytes = 0;
    m_remaining = 0;
    m_content_length.reset();
    m_chunked = false;
    m_status = 0;
    m_headers.clear();
}

void Http1ResponseParser::Fail(Fault fault) noexcept
{
    m_state = State::Failed;
    m_fault = fault;
    m_keep_alive = false;
}

void Http1ResponseParser::Finish() noexcept
{
    m_phase = Phase::Done;
    m_state = State::Complete;
}

std::size_t Http1ResponseParser::Feed(std::span<const std::byte> data)
{
    std::size_t consumed = 0;
    while (consumed < data.size() && m_state == State::NeedMore)
    {
        m_started = true;
        const auto rest = data.subspan(consumed);
        const bool body = m_phase == Phase::FixedBody || m_phase == Phase::ChunkData ||
                          m_phase == Phase::CloseBody;
        consumed += body ? FeedBody(rest) : FeedLine(rest);
    }
    return consumed;
}

void Http1ResponseParser::FeedEof()
{
    if (m_state != State::NeedMore)
    {
        return;
    }
    if (m_phase == Phase::CloseBody)
    {
        Finish();
        return;
    }
    Fail(Fault::Truncated);
}

std::size_t Http1ResponseParser::LineBudget() const noexcept
{
    return m_limits.max_header_bytes;
}

void Http1ResponseParser::ChargeLineByte()
{
    ++m_section_bytes;
    if (m_section_bytes <= LineBudget())
    {
        return;
    }
    switch (m_phase)
    {
        case Phase::TrailerLine:
            Fail(Fault::TrailersTooLarge);
            break;
        case Phase::ChunkSize:
        case Phase::ChunkDataEnd:
            // A chunk-size line is bounded too, but it is not a header: an
            // endless chunk extension is a malformed body, not a large one.
            Fail(Fault::MalformedChunk);
            break;
        case Phase::StatusLine:
        case Phase::HeaderLine:
        case Phase::FixedBody:
        case Phase::ChunkData:
        case Phase::CloseBody:
        case Phase::Done:
            Fail(Fault::HeadersTooLarge);
            break;
    }
}

std::size_t Http1ResponseParser::FeedLine(std::span<const std::byte> data)
{
    std::size_t consumed = 0;
    while (consumed < data.size() && m_state == State::NeedMore)
    {
        const auto c = static_cast<char>(data[consumed]);
        ++consumed;
        ChargeLineByte();
        if (m_state != State::NeedMore)
        {
            break;
        }
        if (c != '\n')
        {
            m_line.push_back(c);
            continue;
        }
        if (!m_line.empty() && m_line.back() == '\r')
        {
            m_line.pop_back();
        }
        const std::string line = std::move(m_line);
        m_line.clear();
        OnLine(line);
        break;
    }
    return consumed;
}

std::size_t Http1ResponseParser::FeedBody(std::span<const std::byte> data)
{
    if (m_phase == Phase::CloseBody)
    {
        // Fill to the cap, then fail on the first byte past it, so where the
        // body stops does not depend on how the bytes were split.
        const std::size_t room = m_limits.max_body_bytes - m_body.size();
        const std::size_t take = std::min(data.size(), room);
        m_body.insert(m_body.end(), data.begin(), data.begin() + static_cast<std::ptrdiff_t>(take));
        if (data.size() > room)
        {
            Fail(Fault::BodyTooLarge);
        }
        return take;
    }
    const std::size_t take = std::min(data.size(), m_remaining);
    m_body.insert(m_body.end(), data.begin(), data.begin() + static_cast<std::ptrdiff_t>(take));
    m_remaining -= take;
    if (m_remaining == 0)
    {
        if (m_phase == Phase::FixedBody)
        {
            Finish();
        }
        else
        {
            m_phase = Phase::ChunkDataEnd;
        }
    }
    return take;
}

void Http1ResponseParser::OnLine(std::string_view line)
{
    if (line.find('\r') != std::string_view::npos)
    {
        Fail(m_phase == Phase::StatusLine ? Fault::MalformedStatusLine : Fault::MalformedHeader);
        return;
    }
    switch (m_phase)
    {
        case Phase::StatusLine:
            OnStatusLine(line);
            break;
        case Phase::HeaderLine:
            OnHeaderLine(line);
            break;
        case Phase::ChunkSize:
            OnChunkSizeLine(line);
            break;
        case Phase::ChunkDataEnd:
            OnChunkDataEnd(line);
            break;
        case Phase::TrailerLine:
            OnTrailerLine(line);
            break;
        case Phase::FixedBody:
        case Phase::ChunkData:
        case Phase::CloseBody:
        case Phase::Done:
            break;
    }
}

void Http1ResponseParser::OnStatusLine(std::string_view line)
{
    // RFC 9112 §2.2 lets a recipient skip blank lines before the start line;
    // they still count against the head's budget.
    if (line.empty())
    {
        return;
    }
    const auto code = ParseStatusLine(line, m_http10);
    if (!code)
    {
        Fail(Fault::MalformedStatusLine);
        return;
    }
    m_status = *code;
    m_headers.push_back(internal::HeaderField{
        .name = ":status",
        .value = std::string{line.substr(kStatusCodeOffset, kStatusCodeDigits)}});
    m_phase = Phase::HeaderLine;
}

void Http1ResponseParser::OnHeaderLine(std::string_view line)
{
    if (line.empty())
    {
        OnEndOfHead();
        return;
    }
    if (line.front() == ' ' || line.front() == '\t')
    {
        Fail(Fault::ObsFold);
        return;
    }
    const auto colon = line.find(':');
    if (colon == std::string_view::npos)
    {
        Fail(Fault::MalformedHeader);
        return;
    }
    const auto name = line.substr(0, colon);
    const auto value = Trim(line.substr(colon + 1));
    if (!IsToken(name) || !IsFieldValue(value))
    {
        Fail(Fault::MalformedHeader);
        return;
    }
    OnHeaderField(name, value);
}

void Http1ResponseParser::OnHeaderField(std::string_view name, std::string_view value)
{
    std::string lowered{name};
    std::ranges::transform(lowered, lowered.begin(), ToLowerAscii);
    if (lowered == "connection")
    {
        OnConnectionField(value);
    }
    else if (lowered == "content-length")
    {
        OnContentLengthField(value);
    }
    else if (lowered == "transfer-encoding")
    {
        OnTransferEncodingField(value);
    }
    m_headers.push_back(
        internal::HeaderField{.name = std::move(lowered), .value = std::string{value}});
}

void Http1ResponseParser::OnConnectionField(std::string_view value)
{
    while (!value.empty())
    {
        const auto comma = value.find(',');
        if (EqualsIgnoreCase(Trim(value.substr(0, comma)), "close"))
        {
            m_keep_alive = false;
        }
        value = (comma == std::string_view::npos) ? std::string_view{} : value.substr(comma + 1);
    }
}

void Http1ResponseParser::OnContentLengthField(std::string_view value)
{
    const auto length = ParseDecimal(value);
    if (!length || (m_content_length && *m_content_length != *length))
    {
        Fail(Fault::MalformedContentLength);
        return;
    }
    m_content_length = length;
}

void Http1ResponseParser::OnTransferEncodingField(std::string_view value)
{
    // Only a lone `chunked` is a coding this client can read. A second
    // Transfer-Encoding field, even another `chunked`, would mean the body
    // was chunked twice.
    if (m_chunked || !EqualsIgnoreCase(value, "chunked"))
    {
        Fail(Fault::UnsupportedTransferEncoding);
        return;
    }
    m_chunked = true;
}

void Http1ResponseParser::OnEndOfHead()
{
    if (m_status == kSwitchingProtocols)
    {
        Fail(Fault::UnexpectedUpgrade);
        return;
    }
    if (m_status < kInformationalCeiling)
    {
        // An interim response: its fields describe nothing about the final
        // one, and a `Connection: close` in it closes nothing yet.
        const bool keep_alive = m_keep_alive;
        StartResponse();
        m_keep_alive = keep_alive;
        return;
    }
    if (m_http10)
    {
        m_keep_alive = false;
    }
    if (m_content_length && m_chunked)
    {
        Fail(Fault::ConflictingFraming);
        return;
    }
    StartBody();
}

void Http1ResponseParser::StartBody()
{
    if (m_status == kNoContent || m_status == kNotModified)
    {
        Finish();
        return;
    }
    if (m_chunked)
    {
        m_phase = Phase::ChunkSize;
        m_section_bytes = 0;
        return;
    }
    if (!m_content_length)
    {
        // Delimited by the connection closing, which also ends keep-alive.
        m_keep_alive = false;
        m_phase = Phase::CloseBody;
        return;
    }
    if (*m_content_length > m_limits.max_body_bytes)
    {
        Fail(Fault::BodyTooLarge);
        return;
    }
    if (*m_content_length == 0)
    {
        Finish();
        return;
    }
    m_remaining = *m_content_length;
    m_body.reserve(m_remaining);
    m_phase = Phase::FixedBody;
}

void Http1ResponseParser::OnChunkSizeLine(std::string_view line)
{
    m_section_bytes = 0;
    const auto size = ParseChunkSize(line);
    if (!size)
    {
        Fail(Fault::MalformedChunk);
        return;
    }
    if (*size == 0)
    {
        m_phase = Phase::TrailerLine;
        return;
    }
    if (*size > m_limits.max_body_bytes - m_body.size())
    {
        Fail(Fault::BodyTooLarge);
        return;
    }
    m_remaining = static_cast<std::size_t>(*size);
    m_phase = Phase::ChunkData;
}

void Http1ResponseParser::OnChunkDataEnd(std::string_view line)
{
    if (!line.empty())
    {
        Fail(Fault::MalformedChunk);
        return;
    }
    m_section_bytes = 0;
    m_phase = Phase::ChunkSize;
}

void Http1ResponseParser::OnTrailerLine(std::string_view line)
{
    // Trailers are read only to find the end of the message: nothing in
    // OTLP/HTTP puts meaning in one.
    if (line.empty())
    {
        Finish();
    }
}

}  // namespace microtel::transport
