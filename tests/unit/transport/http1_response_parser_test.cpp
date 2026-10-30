// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0
//
// Unit tests for Http1ResponseParser (ICP 0042 §3): status line, header
// fields, Content-Length and chunked bodies, keep-alive versus close, and the
// size limits. Every case is also fed one byte at a time, because the parser
// is a push parser and the split must not change the result.

#include "transport/http1_response_parser.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace mtt = microtel::transport;

using Parser = mtt::Http1ResponseParser;
using State = Parser::State;
using Fault = Parser::Fault;

namespace
{

constexpr mtt::Http1ResponseLimits kLimits{.max_body_bytes = 1024, .max_header_bytes = 512};

std::span<const std::byte> AsBytes(std::string_view text)
{
    return std::as_bytes(std::span{text.data(), text.size()});
}

/// Feed @p text whole, or one byte at a time, and return how much was consumed.
std::size_t FeedAll(Parser& parser, std::string_view text, bool bytewise)
{
    const auto bytes = AsBytes(text);
    if (!bytewise)
    {
        return parser.Feed(bytes);
    }
    std::size_t consumed = 0;
    for (std::size_t i = 0; i < bytes.size(); ++i)
    {
        consumed += parser.Feed(bytes.subspan(i, 1));
    }
    return consumed;
}

std::string BodyText(Parser& parser)
{
    std::string out;
    for (const auto b : parser.Body())
    {
        out.push_back(static_cast<char>(b));
    }
    return out;
}

std::string FindHeader(Parser& parser, std::string_view name)
{
    for (const auto& h : parser.Headers())
    {
        if (h.name == name)
        {
            return h.value;
        }
    }
    return "<absent>";
}

/// Each test runs twice: the response fed whole, then one byte at a time.
class Http1ResponseParserTest : public ::testing::TestWithParam<bool>
{
protected:
    Parser m_parser{kLimits};

    std::size_t Feed(std::string_view text)
    {
        return FeedAll(m_parser, text, GetParam());
    }

    /// Feed @p text and expect a failure with @p fault.
    void ExpectFault(std::string_view text, Fault fault)
    {
        Feed(text);
        m_parser.FeedEof();
        EXPECT_EQ(m_parser.GetState(), State::Failed) << text;
        EXPECT_EQ(m_parser.GetFault(), fault) << text;
    }
};

INSTANTIATE_TEST_SUITE_P(WholeAndBytewise,
                         Http1ResponseParserTest,
                         ::testing::Bool(),
                         [](const ::testing::TestParamInfo<bool>& info)
                         { return info.param ? "Bytewise" : "Whole"; });

}  // namespace

// ---------------------------------------------------------------------------
// Status line and header fields
// ---------------------------------------------------------------------------

TEST_P(Http1ResponseParserTest, ContentLength_CompleteWithBodyAndHeaders)
{
    const std::string_view text = "HTTP/1.1 200 OK\r\nContent-Type: application/x-protobuf\r\n"
                                  "Content-Length: 5\r\n\r\nhello";
    EXPECT_EQ(Feed(text), text.size());
    ASSERT_EQ(m_parser.GetState(), State::Complete);
    EXPECT_EQ(m_parser.StatusCode(), 200);
    EXPECT_TRUE(m_parser.KeepAlive());
    EXPECT_EQ(BodyText(m_parser), "hello");
    ASSERT_FALSE(m_parser.Headers().empty());
    EXPECT_EQ(m_parser.Headers().front().name, ":status") << "synthesised first, as HTTP/2 has it";
    EXPECT_EQ(m_parser.Headers().front().value, "200");
    EXPECT_EQ(FindHeader(m_parser, "content-type"), "application/x-protobuf")
        << "names are lowercased";
}

TEST_P(Http1ResponseParserTest, HeaderValue_OptionalWhitespaceTrimmed)
{
    Feed("HTTP/1.1 200 OK\r\nRetry-After: \t 7 \t\r\nContent-Length: 0\r\n\r\n");
    ASSERT_EQ(m_parser.GetState(), State::Complete);
    EXPECT_EQ(FindHeader(m_parser, "retry-after"), "7");
}

TEST_P(Http1ResponseParserTest, BareLfLineEndings_Accepted)
{
    Feed("HTTP/1.1 503 Service Unavailable\nContent-Length: 2\n\nno");
    ASSERT_EQ(m_parser.GetState(), State::Complete);
    EXPECT_EQ(m_parser.StatusCode(), 503);
    EXPECT_EQ(BodyText(m_parser), "no");
}

TEST_P(Http1ResponseParserTest, StatusLine_WithoutReasonPhrase_Accepted)
{
    Feed("HTTP/1.1 204\r\n\r\n");
    ASSERT_EQ(m_parser.GetState(), State::Complete);
    EXPECT_EQ(m_parser.StatusCode(), 204);
}

TEST_P(Http1ResponseParserTest, StatusLine_Malformed_Fails)
{
    for (const std::string_view line : {"HTTP/2 200 OK\r\n\r\n",
                                        "HTTP/1.1 20 OK\r\n\r\n",
                                        "HTTP/1.1 2000 OK\r\n\r\n",
                                        "HTTP/1.1 abc OK\r\n\r\n",
                                        "ICY 200 OK\r\n\r\n",
                                        "HTTP/1.1 099 Low\r\n\r\n",
                                        "HTTP/1.1 200OK\r\n\r\n"})
    {
        Parser parser{kLimits};
        FeedAll(parser, line, GetParam());
        EXPECT_EQ(parser.GetFault(), Fault::MalformedStatusLine) << line;
    }
}

TEST_P(Http1ResponseParserTest, ObsFold_Fails)
{
    ExpectFault("HTTP/1.1 200 OK\r\nX-A: one\r\n two\r\nContent-Length: 0\r\n\r\n", Fault::ObsFold);
}

TEST_P(Http1ResponseParserTest, HeaderField_Malformed_Fails)
{
    for (const std::string_view text : {
             "HTTP/1.1 200 OK\r\nNoColon\r\n\r\n",
             "HTTP/1.1 200 OK\r\n: empty-name\r\n\r\n",
             "HTTP/1.1 200 OK\r\nX-A : space-before-colon\r\n\r\n",
             "HTTP/1.1 200 OK\r\nX(A): bad-token\r\n\r\n",
             "HTTP/1.1 200 OK\r\nX-A: bare\rcr\r\n\r\n",
         })
    {
        Parser parser{kLimits};
        FeedAll(parser, text, GetParam());
        EXPECT_EQ(parser.GetFault(), Fault::MalformedHeader) << text;
    }
    Parser parser{kLimits};
    using std::string_view_literals::operator""sv;
    const auto nul = "HTTP/1.1 200 OK\r\nX-A: a\0b\r\n\r\n"sv;
    FeedAll(parser, nul, GetParam());
    EXPECT_EQ(parser.GetFault(), Fault::MalformedHeader) << "NUL in a value";
}

TEST_P(Http1ResponseParserTest, Head_OverMaxHeaderBytes_FailsTooLarge)
{
    std::string text = "HTTP/1.1 200 OK\r\nX-Big: ";
    text.append(kLimits.max_header_bytes, 'a');
    text += "\r\n\r\n";
    ExpectFault(text, Fault::HeadersTooLarge);
    EXPECT_TRUE(m_parser.IsTooLarge());
}

TEST_P(Http1ResponseParserTest, Head_UnterminatedLine_IsBoundedToo)
{
    std::string text = "HTTP/1.1 200 OK\r\nX-Big: ";
    text.append(kLimits.max_header_bytes * 4, 'a');
    const auto consumed = Feed(text);
    EXPECT_EQ(m_parser.GetFault(), Fault::HeadersTooLarge);
    EXPECT_LE(consumed, kLimits.max_header_bytes + 1) << "stops buffering at the bound";
}

// ---------------------------------------------------------------------------
// Interim responses
// ---------------------------------------------------------------------------

TEST_P(Http1ResponseParserTest, InterimResponses_Skipped)
{
    Feed("HTTP/1.1 100 Continue\r\nX-Interim: 1\r\n\r\n"
         "HTTP/1.1 103 Early Hints\r\nLink: </a>\r\n\r\n"
         "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok");
    ASSERT_EQ(m_parser.GetState(), State::Complete);
    EXPECT_EQ(m_parser.StatusCode(), 200);
    EXPECT_EQ(FindHeader(m_parser, "x-interim"), "<absent>") << "interim fields are dropped";
    EXPECT_EQ(FindHeader(m_parser, "link"), "<absent>");
    EXPECT_EQ(BodyText(m_parser), "ok");
}

TEST_P(Http1ResponseParserTest, SwitchingProtocols_Fails)
{
    ExpectFault("HTTP/1.1 101 Switching Protocols\r\nUpgrade: h2c\r\n\r\n",
                Fault::UnexpectedUpgrade);
}

// ---------------------------------------------------------------------------
// Body framing
// ---------------------------------------------------------------------------

TEST_P(Http1ResponseParserTest, Chunked_DecodedWithExtensionsAndTrailers)
{
    const std::string_view text = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
                                  "5;name=value\r\nhello\r\n"
                                  "6\r\n world\r\n"
                                  "0\r\nX-Trailer: t\r\n\r\n";
    EXPECT_EQ(Feed(text), text.size());
    ASSERT_EQ(m_parser.GetState(), State::Complete);
    EXPECT_EQ(BodyText(m_parser), "hello world");
    EXPECT_TRUE(m_parser.KeepAlive());
    EXPECT_EQ(FindHeader(m_parser, "x-trailer"), "<absent>") << "trailers are discarded";
}

TEST_P(Http1ResponseParserTest, Chunked_UppercaseHexAndCaseInsensitiveCoding)
{
    Feed("HTTP/1.1 200 OK\r\nTransfer-Encoding: Chunked\r\n\r\nA\r\n0123456789\r\n0\r\n\r\n");
    ASSERT_EQ(m_parser.GetState(), State::Complete);
    EXPECT_EQ(BodyText(m_parser), "0123456789");
}

TEST_P(Http1ResponseParserTest, Chunked_MalformedSize_Fails)
{
    for (const std::string_view text : {
             "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\nzz\r\n",
             "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n\r\n",
             "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n-1\r\n",
             "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n2\r\nokX\r\n",
             "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n11111111111111111\r\n",
         })
    {
        Parser parser{kLimits};
        FeedAll(parser, text, GetParam());
        EXPECT_EQ(parser.GetFault(), Fault::MalformedChunk) << text;
    }
}

TEST_P(Http1ResponseParserTest, Chunked_EndlessExtension_IsBoundedAndMalformed)
{
    std::string text = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n5;ext=";
    text.append(kLimits.max_header_bytes * 4, 'e');
    const auto consumed = Feed(text);
    EXPECT_EQ(m_parser.GetFault(), Fault::MalformedChunk) << "an endless extension is not a size";
    EXPECT_FALSE(m_parser.IsTooLarge());
    EXPECT_LT(consumed, text.size()) << "stops buffering at the line bound";
}

TEST_P(Http1ResponseParserTest, Chunked_TrailersOverBound_FailsTooLarge)
{
    std::string text = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n0\r\nX-T: ";
    text.append(kLimits.max_header_bytes, 't');
    text += "\r\n\r\n";
    ExpectFault(text, Fault::TrailersTooLarge);
    EXPECT_TRUE(m_parser.IsTooLarge());
}

TEST_P(Http1ResponseParserTest, Chunked_BodyOverMax_FailsTooLarge)
{
    std::string text = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n";
    const std::string chunk((kLimits.max_body_bytes / 2) + 1, 'x');
    for (int i = 0; i < 2; ++i)
    {
        text += "201\r\n" + chunk + "\r\n";
    }
    text += "0\r\n\r\n";
    ExpectFault(text, Fault::BodyTooLarge);
    EXPECT_TRUE(m_parser.IsTooLarge());
    EXPECT_LE(m_parser.Body().size(), kLimits.max_body_bytes);
}

TEST_P(Http1ResponseParserTest, ContentLength_OverMax_FailsBeforeReadingTheBody)
{
    const auto consumed = Feed("HTTP/1.1 200 OK\r\nContent-Length: 1025\r\n\r\nxx");
    EXPECT_EQ(m_parser.GetFault(), Fault::BodyTooLarge);
    EXPECT_TRUE(m_parser.Body().empty());
    EXPECT_LT(consumed, 45U) << "nothing of the body is consumed";
}

TEST_P(Http1ResponseParserTest, ContentLength_AtMax_Accepted)
{
    std::string text = "HTTP/1.1 200 OK\r\nContent-Length: 1024\r\n\r\n";
    text.append(kLimits.max_body_bytes, 'b');
    Feed(text);
    ASSERT_EQ(m_parser.GetState(), State::Complete);
    EXPECT_EQ(m_parser.Body().size(), kLimits.max_body_bytes);
}

TEST_P(Http1ResponseParserTest, ContentLength_Malformed_Fails)
{
    for (const std::string_view text : {
             "HTTP/1.1 200 OK\r\nContent-Length: abc\r\n\r\n",
             "HTTP/1.1 200 OK\r\nContent-Length: -1\r\n\r\n",
             "HTTP/1.1 200 OK\r\nContent-Length: \r\n\r\n",
             "HTTP/1.1 200 OK\r\nContent-Length: 1, 1\r\n\r\n",
             "HTTP/1.1 200 OK\r\nContent-Length: 1\r\nContent-Length: 2\r\n\r\n",
             "HTTP/1.1 200 OK\r\nContent-Length: 99999999999999999999999\r\n\r\n",
         })
    {
        Parser parser{kLimits};
        FeedAll(parser, text, GetParam());
        EXPECT_EQ(parser.GetFault(), Fault::MalformedContentLength) << text;
    }
}

TEST_P(Http1ResponseParserTest, ContentLength_RepeatedIdentical_Accepted)
{
    Feed("HTTP/1.1 200 OK\r\nContent-Length: 2\r\nContent-Length: 2\r\n\r\nok");
    ASSERT_EQ(m_parser.GetState(), State::Complete);
    EXPECT_EQ(BodyText(m_parser), "ok");
}

TEST_P(Http1ResponseParserTest, ContentLengthAndTransferEncoding_Fails)
{
    ExpectFault("HTTP/1.1 200 OK\r\nContent-Length: 2\r\nTransfer-Encoding: chunked\r\n\r\n",
                Fault::ConflictingFraming);
}

TEST_P(Http1ResponseParserTest, TransferEncoding_OtherThanChunked_Fails)
{
    for (const std::string_view text : {
             "HTTP/1.1 200 OK\r\nTransfer-Encoding: gzip\r\n\r\n",
             "HTTP/1.1 200 OK\r\nTransfer-Encoding: gzip, chunked\r\n\r\n",
             "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nTransfer-Encoding: chunked\r\n\r\n",
         })
    {
        Parser parser{kLimits};
        FeedAll(parser, text, GetParam());
        EXPECT_EQ(parser.GetFault(), Fault::UnsupportedTransferEncoding) << text;
    }
}

TEST_P(Http1ResponseParserTest, NoContentStatuses_HaveNoBody)
{
    for (const std::string_view text : {"HTTP/1.1 204 No Content\r\nContent-Length: 5\r\n\r\n",
                                        "HTTP/1.1 304 Not Modified\r\n\r\n"})
    {
        Parser parser{kLimits};
        EXPECT_EQ(FeedAll(parser, text, GetParam()), text.size());
        EXPECT_EQ(parser.GetState(), State::Complete) << text;
        EXPECT_TRUE(parser.Body().empty());
        EXPECT_TRUE(parser.KeepAlive());
    }
}

TEST_P(Http1ResponseParserTest, CloseDelimitedBody_CompletesOnEof)
{
    Feed("HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\n\r\nuntil close");
    EXPECT_EQ(m_parser.GetState(), State::NeedMore);
    m_parser.FeedEof();
    ASSERT_EQ(m_parser.GetState(), State::Complete);
    EXPECT_EQ(BodyText(m_parser), "until close");
    EXPECT_FALSE(m_parser.KeepAlive()) << "the close is the framing";
}

TEST_P(Http1ResponseParserTest, CloseDelimitedBody_OverMax_FailsTooLarge)
{
    const std::string_view head = "HTTP/1.1 200 OK\r\n\r\n";
    std::string text{head};
    text.append(kLimits.max_body_bytes + 1, 'c');
    const auto consumed = Feed(text);
    m_parser.FeedEof();
    EXPECT_EQ(m_parser.GetFault(), Fault::BodyTooLarge);
    // Found by http1_response_fuzz: the whole feed stopped before the body and
    // the bytewise one at the cap. Both now stop at the cap.
    EXPECT_EQ(m_parser.Body().size(), kLimits.max_body_bytes);
    EXPECT_EQ(consumed, head.size() + kLimits.max_body_bytes);
}

// ---------------------------------------------------------------------------
// Keep-alive versus close
// ---------------------------------------------------------------------------

TEST_P(Http1ResponseParserTest, ConnectionClose_EndsKeepAlive)
{
    for (const std::string_view text : {
             "HTTP/1.1 200 OK\r\nConnection: close\r\nContent-Length: 0\r\n\r\n",
             "HTTP/1.1 200 OK\r\nConnection: keep-alive, Close\r\nContent-Length: 0\r\n\r\n",
         })
    {
        Parser parser{kLimits};
        FeedAll(parser, text, GetParam());
        ASSERT_EQ(parser.GetState(), State::Complete) << text;
        EXPECT_FALSE(parser.KeepAlive()) << text;
    }
}

TEST_P(Http1ResponseParserTest, Http10_EndsKeepAlive)
{
    Feed("HTTP/1.0 200 OK\r\nConnection: keep-alive\r\nContent-Length: 0\r\n\r\n");
    ASSERT_EQ(m_parser.GetState(), State::Complete);
    EXPECT_FALSE(m_parser.KeepAlive()) << "ICP 0042: an HTTP/1.0 status line ends keep-alive";
}

TEST_P(Http1ResponseParserTest, BytesAfterTheResponse_AreNotConsumed)
{
    const std::string_view first = "HTTP/1.1 200 OK\r\nContent-Length: 1\r\n\r\na";
    const std::string text = std::string{first} + "HTTP/1.1 200 OK\r\n";
    EXPECT_EQ(Feed(text), first.size());
    EXPECT_EQ(m_parser.GetState(), State::Complete);
}

TEST_P(Http1ResponseParserTest, Reset_ParsesTheNextResponse)
{
    Feed("HTTP/1.1 500 Internal Server Error\r\nContent-Length: 3\r\nX-A: 1\r\n\r\nbad");
    ASSERT_EQ(m_parser.GetState(), State::Complete);
    m_parser.Reset();
    EXPECT_EQ(m_parser.GetState(), State::NeedMore);
    EXPECT_FALSE(m_parser.HasStarted());
    Feed("HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok");
    ASSERT_EQ(m_parser.GetState(), State::Complete);
    EXPECT_EQ(m_parser.StatusCode(), 200);
    EXPECT_EQ(BodyText(m_parser), "ok");
    EXPECT_EQ(FindHeader(m_parser, "x-a"), "<absent>") << "the previous response's fields are gone";
}

// ---------------------------------------------------------------------------
// End of stream
// ---------------------------------------------------------------------------

TEST_P(Http1ResponseParserTest, Eof_MidHeadOrBody_IsTruncated)
{
    for (const std::string_view text : {
             "HTTP/1.1 200 OK\r\nContent-Le",
             "HTTP/1.1 200 OK\r\nContent-Length: 10\r\n\r\nabc",
             "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nab",
         })
    {
        Parser parser{kLimits};
        FeedAll(parser, text, GetParam());
        EXPECT_EQ(parser.GetState(), State::NeedMore);
        parser.FeedEof();
        EXPECT_EQ(parser.GetFault(), Fault::Truncated) << text;
        EXPECT_TRUE(parser.HasStarted());
    }
}

TEST_P(Http1ResponseParserTest, Eof_BeforeAnyByte_IsTruncatedButNotStarted)
{
    m_parser.FeedEof();
    EXPECT_EQ(m_parser.GetFault(), Fault::Truncated);
    EXPECT_FALSE(m_parser.HasStarted()) << "the transport tells a quiet close from a cut one";
}

TEST(Http1ResponseParserStandaloneTest, Describe_NamesEveryFault)
{
    for (const auto fault : {Fault::MalformedStatusLine,
                             Fault::MalformedHeader,
                             Fault::ObsFold,
                             Fault::HeadersTooLarge,
                             Fault::ConflictingFraming,
                             Fault::UnsupportedTransferEncoding,
                             Fault::MalformedContentLength,
                             Fault::MalformedChunk,
                             Fault::TrailersTooLarge,
                             Fault::BodyTooLarge,
                             Fault::UnexpectedUpgrade,
                             Fault::Truncated})
    {
        EXPECT_FALSE(Parser::Describe(fault).empty());
    }
}
