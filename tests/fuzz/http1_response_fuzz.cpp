// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

// Fuzz harness for Http1ResponseParser (ICP 0042 §3).
//
// Input layout:
//   byte 0  max_body_bytes   = 1 + (b0 & 0x3F) * 64  (1 .. 4033)
//   byte 1  max_header_bytes = 8 + b1 * 8            (8 .. 2048)
//   byte 2  seed for the random split below
//   rest    the bytes the peer sent, then EOF
//
// The same bytes are parsed three ways: in one Feed, one byte per Feed, and
// in pieces of pseudo-random size. Responses are parsed back to back, with
// Reset after each complete one, as the transport does on a keep-alive
// connection. Beyond the sanitizers, the harness asserts:
//   - the three ways agree exactly: state, fault, status, keep-alive, header
//     fields and body of every response, and how many bytes each consumed;
//   - Feed always makes progress while the parser needs more;
//   - the body never exceeds max_body_bytes, and the header fields never
//     exceed max_header_bytes (the synthesised :status included);
//   - a complete response has a final status, 200..599.

#include "microtel/internal/transport.hpp"

#include "transport/http1_response_parser.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <vector>

namespace
{

using microtel::transport::Http1ResponseLimits;
using microtel::transport::Http1ResponseParser;

constexpr std::size_t kHeaderBytes = 3;
constexpr std::size_t kBodyStep = 64;
constexpr std::size_t kHeaderStep = 8;
constexpr std::uint8_t kBodySelectorMask = 0x3F;
constexpr int kMinFinalStatus = 200;
constexpr int kMaxStatus = 599;

struct Outcome
{
    Http1ResponseParser::State state = Http1ResponseParser::State::NeedMore;
    Http1ResponseParser::Fault fault = Http1ResponseParser::Fault::None;
    int status = 0;
    bool keep_alive = false;
    std::vector<microtel::internal::HeaderField> headers;
    std::vector<std::byte> body;
    std::size_t end = 0;  ///< Input offset where this response ended.

    bool operator==(const Outcome& o) const
    {
        const bool same_headers = headers.size() == o.headers.size() &&
                                  std::equal(headers.begin(),
                                             headers.end(),
                                             o.headers.begin(),
                                             [](const auto& a, const auto& b)
                                             { return a.name == b.name && a.value == b.value; });
        return state == o.state && fault == o.fault && status == o.status &&
               keep_alive == o.keep_alive && same_headers && body == o.body && end == o.end;
    }
};

void Check(bool ok)
{
    if (!ok)
    {
        __builtin_trap();
    }
}

void CheckBounds(Http1ResponseParser& parser, const Http1ResponseLimits& limits)
{
    Check(parser.Body().size() <= limits.max_body_bytes);
    std::size_t header_bytes = 0;
    for (const auto& h : parser.Headers())
    {
        header_bytes += h.name.size() + h.value.size();
    }
    Check(header_bytes <= limits.max_header_bytes);
}

Outcome Record(Http1ResponseParser& parser, std::size_t end)
{
    Outcome out;
    out.state = parser.GetState();
    out.fault = parser.GetFault();
    out.status = parser.StatusCode();
    out.keep_alive = parser.KeepAlive();
    out.headers = parser.Headers();
    out.body = parser.Body();
    out.end = end;
    if (out.state == Http1ResponseParser::State::Complete)
    {
        Check(out.status >= kMinFinalStatus && out.status <= kMaxStatus);
    }
    return out;
}

/// Parse @p data as back-to-back responses, in pieces of the size @p next_piece
/// picks, then EOF.
std::vector<Outcome> Run(std::span<const std::byte> data,
                         const Http1ResponseLimits& limits,
                         const std::function<std::size_t()>& next_piece)
{
    std::vector<Outcome> outcomes;
    Http1ResponseParser parser{limits};
    std::size_t pos = 0;
    while (pos < data.size())
    {
        const std::size_t piece = std::min(next_piece(), data.size() - pos);
        const std::size_t consumed = parser.Feed(data.subspan(pos, piece));
        Check(consumed <= piece);
        pos += consumed;
        CheckBounds(parser, limits);
        const auto state = parser.GetState();
        if (state == Http1ResponseParser::State::NeedMore)
        {
            Check(consumed == piece);  // progress: a parser that needs more takes it all
            continue;
        }
        outcomes.push_back(Record(parser, pos));
        if (state == Http1ResponseParser::State::Failed)
        {
            return outcomes;
        }
        parser.Reset();
    }
    parser.FeedEof();
    CheckBounds(parser, limits);
    outcomes.push_back(Record(parser, pos));
    return outcomes;
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    if (size < kHeaderBytes)
    {
        return 0;
    }
    const Http1ResponseLimits limits{
        .max_body_bytes = 1 + (static_cast<std::size_t>(data[0] & kBodySelectorMask) * kBodyStep),
        .max_header_bytes = kHeaderStep + (static_cast<std::size_t>(data[1]) * kHeaderStep),
    };
    std::uint32_t seed = data[2];
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    const std::span<const std::byte> input{reinterpret_cast<const std::byte*>(data + kHeaderBytes),
                                           size - kHeaderBytes};

    const auto whole = Run(input, limits, [&] { return input.size(); });
    const auto bytewise = Run(input, limits, [] { return std::size_t{1}; });
    const auto random = Run(input,
                            limits,
                            [&]
                            {
                                // A small LCG: the split must be reproducible from the input.
                                constexpr std::uint32_t kMul = 1103515245U;
                                constexpr std::uint32_t kInc = 12345U;
                                constexpr std::uint32_t kPieceMask = 0x1F;
                                seed = (seed * kMul) + kInc;
                                return static_cast<std::size_t>((seed >> 16U) & kPieceMask) + 1;
                            });
    Check(whole == bytewise);
    Check(whole == random);
    return 0;
}
