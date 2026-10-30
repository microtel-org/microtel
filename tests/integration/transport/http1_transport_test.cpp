// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0
//
// Integration test: Http1Transport against a scripted HTTP/1.1 peer over
// loopback, plaintext and TLS (ICP 0042 §3).
//
// The peer (`tests/helpers/fake_http1_peer.hpp`) serves one connection at a
// time and records every request, so a test can see how many connections the
// transport opened, what went on the wire, and whether a request was ever
// sent before the response to the previous one had arrived.

#include "transport/http1_transport.hpp"

#include "microtel/internal/encoded_payload.hpp"
#include "microtel/internal/transport.hpp"
#include "microtel/internal/wire_result.hpp"

#include "fakes/fake_auth_provider.hpp"
#include "helpers/fake_http1_peer.hpp"
#include "transport/epoll_reactor.hpp"
#include "wire/http/http_wire_codec.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <future>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace mt = microtel;
namespace mti = microtel::internal;
namespace mtt = microtel::transport;
namespace mtw = microtel::wire;
namespace mtfk = microtel::testing;

using mtfk::After;
using mtfk::FakeHttp1Peer;
using mtfk::Http1Reply;
using mtfk::Http1Request;
using mtfk::Http1Response;
using mtfk::PeerTls;

namespace
{

constexpr auto kWait = std::chrono::seconds(5);
constexpr auto kSlowReply = std::chrono::milliseconds(400);

std::string Authority(int port)
{
    return "127.0.0.1:" + std::to_string(port);
}

mti::ConnectOptions Options(int port, bool tls = false)
{
    mti::ConnectOptions opts;
    opts.endpoint = (tls ? "https://" : "http://") + Authority(port);
    opts.protocol = mt::Protocol::Http;
    opts.insecure = true;  // the peer's certificate is self-signed
    opts.connect_timeout = std::chrono::seconds(2);
    opts.tls_handshake_timeout = std::chrono::seconds(2);
    return opts;
}

std::unique_ptr<mtt::Http1Transport> MakeTransport()
{
    auto reactor = mtt::EpollReactor::Create();
    if (!reactor)
    {
        return nullptr;
    }
    auto transport = mtt::Http1Transport::Create(std::move(*reactor));
    return transport ? std::move(*transport) : nullptr;
}

/// A transport connected with @p opts, or null.
std::unique_ptr<mtt::Http1Transport> Connected(const mti::ConnectOptions& opts)
{
    auto t = MakeTransport();
    if (!t || !t->Connect(opts))
    {
        return nullptr;
    }
    return t;
}

mti::RequestSpec Spec(int port,
                      std::span<const std::byte> payload,
                      std::chrono::milliseconds deadline = std::chrono::seconds(5))
{
    return mti::RequestSpec{
        .headers = {{.name = ":method", .value = "POST"},
                    {.name = ":scheme", .value = "http"},
                    {.name = ":path", .value = "/v1/traces"},
                    {.name = ":authority", .value = Authority(port)},
                    {.name = "content-type", .value = "application/x-protobuf"}},
        .payload = payload,
        .deadline = deadline,
    };
}

std::vector<std::byte> Bytes(std::string_view s)
{
    std::vector<std::byte> out(s.size());
    for (std::size_t i = 0; i < s.size(); ++i)
    {
        out[i] = static_cast<std::byte>(s[i]);
    }
    return out;
}

std::string Text(const std::vector<std::byte>& bytes)
{
    std::string out(bytes.size(), '\0');
    for (std::size_t i = 0; i < bytes.size(); ++i)
    {
        out[i] = static_cast<char>(bytes[i]);
    }
    return out;
}

/// The result, or a failure naming the timeout if none arrives in time.
mti::TransportResult Await(mti::RequestHandle& handle)
{
    if (handle.Future().wait_for(kWait) != std::future_status::ready)
    {
        mti::TransportResult timed_out;
        timed_out.error =
            mt::Error{.kind = mt::Error::Kind::Unspecified, .message = "test timeout"};
        return timed_out;
    }
    return handle.Future().get();
}

mti::TransportResult SendAndWait(mtt::Http1Transport& t, int port, std::string_view body)
{
    const auto payload = Bytes(body);
    auto handle = t.Send(Spec(port, payload));
    return Await(handle);
}

std::string Message(const mti::TransportResult& r)
{
    return r.error ? r.error->message : std::string{};
}

std::string Status(const mti::TransportResult& r)
{
    return r.response_headers.empty() ? std::string{} : r.response_headers.front().value;
}

FakeHttp1Peer::Script Always(std::string reply, After after = After::KeepOpen)
{
    return [reply = std::move(reply), after](const Http1Request&, std::size_t)
    { return Http1Reply{.bytes = reply, .after = after}; };
}

}  // namespace

// ---------------------------------------------------------------------------
// Requests and keep-alive
// ---------------------------------------------------------------------------

TEST(Http1TransportTest, Plaintext_SerialRequestsShareOneKeepAliveConnection)
{
    FakeHttp1Peer peer{Always(Http1Response(200, "ok"))};
    const int port = peer.Start();
    ASSERT_GT(port, 0);
    auto t = Connected(Options(port));
    ASSERT_NE(t, nullptr);

    for (const char* body : {"one", "two", "three"})
    {
        const auto r = SendAndWait(*t, port, body);
        EXPECT_TRUE(r.success) << Message(r);
        EXPECT_EQ(Status(r), "200");
        EXPECT_EQ(Text(r.response_body), "ok");
    }

    const auto requests = peer.Requests();
    ASSERT_EQ(requests.size(), 3U);
    EXPECT_EQ(peer.Connections(), 1) << "keep-alive: one connection carries all three";
    EXPECT_EQ(t->GetStats().background_reconnects, 0U);
    EXPECT_EQ(requests[0].method, "POST");
    EXPECT_EQ(requests[0].target, "/v1/traces");
    EXPECT_EQ(requests[0].version, "HTTP/1.1");
    EXPECT_EQ(requests[0].Header("host"), Authority(port));
    EXPECT_EQ(requests[0].Header("content-length"), "3");
    EXPECT_EQ(requests[0].Header("content-type"), "application/x-protobuf");
    EXPECT_EQ(requests[0].HeaderCount(":path"), 0U) << "no pseudo-header goes on the wire";
    EXPECT_EQ(requests[2].body, "three");
    EXPECT_EQ(t->Close(kWait), mt::Status::Completed);
}

TEST(Http1TransportTest, ConcurrentSends_GoOutOneAtATime)
{
    FakeHttp1Peer peer{[](const Http1Request&, std::size_t)
                       {
                           return Http1Reply{.bytes = Http1Response(200),
                                             .after = After::KeepOpen,
                                             .delay = std::chrono::milliseconds(30)};
                       }};
    const int port = peer.Start();
    ASSERT_GT(port, 0);
    auto t = Connected(Options(port));
    ASSERT_NE(t, nullptr);

    const auto payload = Bytes("x");
    std::vector<mti::RequestHandle> handles;
    handles.reserve(4);
    for (int i = 0; i < 4; ++i)
    {
        handles.push_back(t->Send(Spec(port, payload)));
    }
    for (auto& h : handles)
    {
        const auto r = Await(h);
        EXPECT_TRUE(r.success) << Message(r);
    }

    const auto requests = peer.Requests();
    ASSERT_EQ(requests.size(), 4U);
    for (const auto& request : requests)
    {
        EXPECT_FALSE(request.pipelined) << "a request went out before the last response arrived";
    }
    EXPECT_EQ(peer.Connections(), 1);
    EXPECT_EQ(t->Close(kWait), mt::Status::Completed);
}

TEST(Http1TransportTest, ChunkedResponse_IsDecoded)
{
    FakeHttp1Peer peer{Always("HTTP/1.1 200 OK\r\nTransfer-Encoding: "
                              "chunked\r\n\r\n5\r\nhello\r\n6\r\n world\r\n0\r\n\r\n")};
    const int port = peer.Start();
    ASSERT_GT(port, 0);
    auto t = Connected(Options(port));
    ASSERT_NE(t, nullptr);

    for (int i = 0; i < 2; ++i)
    {
        const auto r = SendAndWait(*t, port, "x");
        EXPECT_TRUE(r.success) << Message(r);
        EXPECT_EQ(Text(r.response_body), "hello world");
    }
    EXPECT_EQ(peer.Connections(), 1) << "a chunked body keeps the connection";
    EXPECT_EQ(t->Close(kWait), mt::Status::Completed);
}

TEST(Http1TransportTest, ConnectionClose_ReconnectsInTheBackground)
{
    FakeHttp1Peer peer{Always(Http1Response(200, "ok", "Connection: close\r\n"), After::Close)};
    const int port = peer.Start();
    ASSERT_GT(port, 0);
    auto t = Connected(Options(port));
    ASSERT_NE(t, nullptr);

    for (int i = 0; i < 3; ++i)
    {
        const auto r = SendAndWait(*t, port, "x");
        EXPECT_TRUE(r.success) << Message(r);
        EXPECT_EQ(t->GetState(), mt::ConnectionState::Connected)
            << "a closed keep-alive connection is not a lost transport";
    }
    EXPECT_EQ(peer.Connections(), 3);
    EXPECT_EQ(t->GetStats().background_reconnects, 2U);
    EXPECT_EQ(t->GetStats().stale_retries, 0U);
    EXPECT_EQ(t->Close(kWait), mt::Status::Completed);
}

TEST(Http1TransportTest, ConnectionCloseField_ClientClosesEvenIfThePeerDoesNot)
{
    // The peer says close and then leaves the socket open. The client must
    // not send on it again: the next request goes on a new connection.
    FakeHttp1Peer peer{Always(Http1Response(200, "ok", "Connection: close\r\n"))};
    const int port = peer.Start();
    ASSERT_GT(port, 0);
    auto t = Connected(Options(port));
    ASSERT_NE(t, nullptr);

    EXPECT_TRUE(SendAndWait(*t, port, "a").success);
    EXPECT_TRUE(SendAndWait(*t, port, "b").success);
    const auto requests = peer.Requests();
    ASSERT_EQ(requests.size(), 2U);
    EXPECT_EQ(requests[1].connection, 1);
    EXPECT_EQ(t->GetStats().stale_retries, 0U);
    EXPECT_EQ(t->Close(kWait), mt::Status::Completed);
}

TEST(Http1TransportTest, Http10Response_EndsKeepAlive)
{
    FakeHttp1Peer peer{Always("HTTP/1.0 200 OK\r\nContent-Length: 2\r\n\r\nok", After::Close)};
    const int port = peer.Start();
    ASSERT_GT(port, 0);
    auto t = Connected(Options(port));
    ASSERT_NE(t, nullptr);

    EXPECT_TRUE(SendAndWait(*t, port, "a").success);
    EXPECT_TRUE(SendAndWait(*t, port, "b").success);
    EXPECT_EQ(peer.Connections(), 2);
    EXPECT_EQ(t->Close(kWait), mt::Status::Completed);
}

TEST(Http1TransportTest, CloseDelimitedBody_CompletesAtTheClose)
{
    FakeHttp1Peer peer{Always("HTTP/1.1 200 OK\r\n\r\nall of it", After::Close)};
    const int port = peer.Start();
    ASSERT_GT(port, 0);
    auto t = Connected(Options(port));
    ASSERT_NE(t, nullptr);

    const auto r = SendAndWait(*t, port, "x");
    EXPECT_TRUE(r.success) << Message(r);
    EXPECT_EQ(Text(r.response_body), "all of it");
    EXPECT_EQ(t->Close(kWait), mt::Status::Completed);
}

// ---------------------------------------------------------------------------
// TLS
// ---------------------------------------------------------------------------

TEST(Http1TransportTest, Tls_PeerSelectsHttp11)
{
    FakeHttp1Peer peer{Always(Http1Response(200, "tls")), PeerTls::AlpnHttp11};
    const int port = peer.Start();
    ASSERT_GT(port, 0);
    auto t = Connected(Options(port, true));
    ASSERT_NE(t, nullptr);

    for (int i = 0; i < 2; ++i)
    {
        const auto r = SendAndWait(*t, port, "x");
        EXPECT_TRUE(r.success) << Message(r);
        EXPECT_EQ(Text(r.response_body), "tls");
    }
    EXPECT_EQ(peer.Connections(), 1);
    EXPECT_EQ(t->Close(kWait), mt::Status::Completed);
}

TEST(Http1TransportTest, Tls_PeerIgnoresAlpn)
{
    FakeHttp1Peer peer{Always(Http1Response(200, "tls")), PeerTls::AlpnNone};
    const int port = peer.Start();
    ASSERT_GT(port, 0);
    auto t = Connected(Options(port, true));
    ASSERT_NE(t, nullptr);

    const auto r = SendAndWait(*t, port, "x");
    EXPECT_TRUE(r.success) << Message(r);
    EXPECT_EQ(t->Close(kWait), mt::Status::Completed);
}

TEST(Http1TransportTest, Tls_ConnectionClose_ReconnectsWithTls)
{
    FakeHttp1Peer peer{Always(Http1Response(200, "ok", "Connection: close\r\n"), After::Close),
                       PeerTls::AlpnHttp11};
    const int port = peer.Start();
    ASSERT_GT(port, 0);
    auto t = Connected(Options(port, true));
    ASSERT_NE(t, nullptr);

    EXPECT_TRUE(SendAndWait(*t, port, "a").success);
    EXPECT_TRUE(SendAndWait(*t, port, "b").success);
    EXPECT_EQ(peer.Connections(), 2);
    EXPECT_EQ(t->GetStats().background_reconnects, 1U);
    EXPECT_EQ(t->Close(kWait), mt::Status::Completed);
}

// ---------------------------------------------------------------------------
// Failures
// ---------------------------------------------------------------------------

TEST(Http1TransportTest, OversizedResponse_FailsAsTooLargeAndTheNextRequestReconnects)
{
    FakeHttp1Peer peer{
        [](const Http1Request&, std::size_t index)
        {
            return Http1Reply{.bytes = index == 0 ? Http1Response(200, std::string(4096, 'B'))
                                                  : Http1Response(200, "small")};
        }};
    const int port = peer.Start();
    ASSERT_GT(port, 0);
    auto opts = Options(port);
    opts.max_response_bytes = 1024;
    auto t = Connected(opts);
    ASSERT_NE(t, nullptr);

    const auto big = SendAndWait(*t, port, "x");
    EXPECT_FALSE(big.success);
    EXPECT_TRUE(big.response_too_large);
    EXPECT_TRUE(big.response_body.empty());
    EXPECT_EQ(big.error ? big.error->kind : mt::Error::Kind::Unspecified,
              mt::Error::Kind::Malformed);

    const auto next = SendAndWait(*t, port, "y");
    EXPECT_TRUE(next.success) << Message(next);
    EXPECT_EQ(t->Close(kWait), mt::Status::Completed);
}

TEST(Http1TransportTest, MalformedResponse_FailsAndClosesTheConnection)
{
    FakeHttp1Peer peer{[](const Http1Request&, std::size_t index) {
        return Http1Reply{.bytes =
                              index == 0 ? std::string{"NOT HTTP\r\n\r\n"} : Http1Response(200)};
    }};
    const int port = peer.Start();
    ASSERT_GT(port, 0);
    auto t = Connected(Options(port));
    ASSERT_NE(t, nullptr);

    const auto bad = SendAndWait(*t, port, "x");
    EXPECT_FALSE(bad.success);
    EXPECT_FALSE(bad.response_too_large);
    EXPECT_NE(Message(bad).find("malformed HTTP/1.1 response"), std::string::npos) << Message(bad);
    EXPECT_TRUE(SendAndWait(*t, port, "y").success);
    EXPECT_EQ(peer.Connections(), 2);
    EXPECT_EQ(t->Close(kWait), mt::Status::Completed);
}

TEST(Http1TransportTest, BytesAfterTheResponse_CostTheConnection)
{
    // Nothing was pipelined, so anything after the response is the peer
    // talking out of turn: the response stands, the connection does not.
    FakeHttp1Peer peer{Always(Http1Response(200, "ok") + "HTTP/1.1 200 OK\r\n")};
    const int port = peer.Start();
    ASSERT_GT(port, 0);
    auto t = Connected(Options(port));
    ASSERT_NE(t, nullptr);

    const auto r = SendAndWait(*t, port, "a");
    EXPECT_TRUE(r.success) << Message(r);
    EXPECT_EQ(Text(r.response_body), "ok");
    EXPECT_TRUE(SendAndWait(*t, port, "b").success);
    EXPECT_EQ(peer.Connections(), 2);
    EXPECT_EQ(t->Close(kWait), mt::Status::Completed);
}

TEST(Http1TransportTest, PeerResetsMidResponse_FailsWithoutARetry)
{
    FakeHttp1Peer peer{
        Always("HTTP/1.1 200 OK\r\nContent-Length: 100\r\n\r\npartial", After::Reset)};
    const int port = peer.Start();
    ASSERT_GT(port, 0);
    auto t = Connected(Options(port));
    ASSERT_NE(t, nullptr);

    const auto r = SendAndWait(*t, port, "x");
    EXPECT_FALSE(r.success);
    EXPECT_FALSE(r.response_too_large);
    EXPECT_EQ(r.error ? r.error->kind : mt::Error::Kind::Unspecified, mt::Error::Kind::Network);
    EXPECT_EQ(t->GetStats().stale_retries, 0U) << "response bytes arrived: never retried";
    EXPECT_EQ(peer.Requests().size(), 1U);
    EXPECT_EQ(t->Close(kWait), mt::Status::Completed);
}

TEST(Http1TransportTest, PeerClosesMidResponse_FailsAsTruncated)
{
    FakeHttp1Peer peer{
        Always("HTTP/1.1 200 OK\r\nContent-Length: 100\r\n\r\npartial", After::Close)};
    const int port = peer.Start();
    ASSERT_GT(port, 0);
    auto t = Connected(Options(port));
    ASSERT_NE(t, nullptr);

    const auto r = SendAndWait(*t, port, "x");
    EXPECT_FALSE(r.success);
    EXPECT_EQ(Message(r), "connection closed before the response was complete");
    EXPECT_EQ(t->Close(kWait), mt::Status::Completed);
}

// ---------------------------------------------------------------------------
// Stale-connection retry (ICP 0042 §3)
// ---------------------------------------------------------------------------

TEST(Http1TransportTest, ReusedConnectionClosedBeforeAnyResponse_IsRetriedOnce)
{
    // Request 1 is answered; request 2 lands on the kept-alive connection just
    // as the peer gives up on it, and gets no answer at all.
    FakeHttp1Peer peer{[](const Http1Request&, std::size_t index)
                       {
                           return index == 1 ? Http1Reply{.bytes = {}, .after = After::Close}
                                             : Http1Reply{.bytes = Http1Response(200, "ok")};
                       }};
    const int port = peer.Start();
    ASSERT_GT(port, 0);
    auto t = Connected(Options(port));
    ASSERT_NE(t, nullptr);

    EXPECT_TRUE(SendAndWait(*t, port, "first").success);
    const auto r = SendAndWait(*t, port, "second");
    EXPECT_TRUE(r.success) << Message(r);

    const auto requests = peer.Requests();
    ASSERT_EQ(requests.size(), 3U);
    EXPECT_EQ(requests[1].body, "second");
    EXPECT_EQ(requests[2].body, "second") << "the receiver may see the retried request twice";
    EXPECT_EQ(requests[2].connection, 1);
    EXPECT_EQ(t->GetStats().stale_retries, 1U);
    EXPECT_EQ(t->GetStats().background_reconnects, 1U);
    EXPECT_EQ(t->Close(kWait), mt::Status::Completed);
}

TEST(Http1TransportTest, StaleRetry_HappensOnlyOnce)
{
    FakeHttp1Peer peer{[](const Http1Request&, std::size_t index)
                       {
                           return index == 0 ? Http1Reply{.bytes = Http1Response(200)}
                                             : Http1Reply{.bytes = {}, .after = After::Close};
                       }};
    const int port = peer.Start();
    ASSERT_GT(port, 0);
    auto t = Connected(Options(port));
    ASSERT_NE(t, nullptr);

    EXPECT_TRUE(SendAndWait(*t, port, "first").success);
    const auto r = SendAndWait(*t, port, "second");
    EXPECT_FALSE(r.success);
    EXPECT_EQ(r.error ? r.error->kind : mt::Error::Kind::Unspecified, mt::Error::Kind::Network);
    EXPECT_EQ(peer.Requests().size(), 3U) << "the request, and one retry";
    EXPECT_EQ(t->GetStats().stale_retries, 1U);
    EXPECT_EQ(t->Close(kWait), mt::Status::Completed);
}

TEST(Http1TransportTest, FreshConnectionClosedBeforeAnyResponse_IsNotRetried)
{
    FakeHttp1Peer peer{[](const Http1Request&, std::size_t index)
                       {
                           return index == 0 ? Http1Reply{.bytes = {}, .after = After::Close}
                                             : Http1Reply{.bytes = Http1Response(200)};
                       }};
    const int port = peer.Start();
    ASSERT_GT(port, 0);
    auto t = Connected(Options(port));
    ASSERT_NE(t, nullptr);

    const auto r = SendAndWait(*t, port, "x");
    EXPECT_FALSE(r.success) << "a connection that never answered is not a stale one";
    EXPECT_EQ(t->GetStats().stale_retries, 0U);
    EXPECT_EQ(peer.Requests().size(), 1U);
    EXPECT_EQ(t->Close(kWait), mt::Status::Completed);
}

// ---------------------------------------------------------------------------
// Reconnect failure
// ---------------------------------------------------------------------------

TEST(Http1TransportTest, ReconnectFails_FailsTheRequestAndHandsBackToTheCodec)
{
    FakeHttp1Peer peer{Always(Http1Response(200, "ok", "Connection: close\r\n"), After::Close)};
    const int port = peer.Start();
    ASSERT_GT(port, 0);
    auto t = Connected(Options(port));
    ASSERT_NE(t, nullptr);

    EXPECT_TRUE(SendAndWait(*t, port, "a").success);
    peer.Stop();  // the next connect is refused

    const auto r = SendAndWait(*t, port, "b");
    EXPECT_FALSE(r.success);
    EXPECT_NE(Message(r).find("HTTP/1.1 reconnect failed"), std::string::npos) << Message(r);
    EXPECT_EQ(t->GetState(), mt::ConnectionState::Reconnecting);
    EXPECT_EQ(t->Send(Spec(port, {})).Id(), 0U) << "refused until the codec reconnects";
    EXPECT_EQ(t->Close(kWait), mt::Status::Completed);
}

// ---------------------------------------------------------------------------
// Queue, deadlines, cancel
// ---------------------------------------------------------------------------

TEST(Http1TransportTest, DeadlinePassesWhileQueued_FailsAsATimeout)
{
    FakeHttp1Peer peer{[](const Http1Request&, std::size_t) {
        return Http1Reply{
            .bytes = Http1Response(200), .after = After::KeepOpen, .delay = kSlowReply};
    }};
    const int port = peer.Start();
    ASSERT_GT(port, 0);
    auto t = Connected(Options(port));
    ASSERT_NE(t, nullptr);

    const auto payload = Bytes("x");
    auto slow = t->Send(Spec(port, payload));
    ASSERT_TRUE(peer.WaitForRequests(1, kWait));
    auto queued = t->Send(Spec(port, payload, std::chrono::milliseconds(50)));

    const auto expired = Await(queued);
    EXPECT_FALSE(expired.success);
    EXPECT_EQ(Message(expired), "request deadline exceeded while queued");
    EXPECT_TRUE(Await(slow).success);
    EXPECT_EQ(peer.Requests().size(), 1U) << "the expired request was never sent";
    EXPECT_EQ(t->Close(kWait), mt::Status::Completed);
}

TEST(Http1TransportTest, CancelQueued_ResolvesBeforeReturning)
{
    FakeHttp1Peer peer{[](const Http1Request&, std::size_t) {
        return Http1Reply{
            .bytes = Http1Response(200), .after = After::KeepOpen, .delay = kSlowReply};
    }};
    const int port = peer.Start();
    ASSERT_GT(port, 0);
    auto t = Connected(Options(port));
    ASSERT_NE(t, nullptr);

    const auto payload = Bytes("x");
    auto slow = t->Send(Spec(port, payload));
    ASSERT_TRUE(peer.WaitForRequests(1, kWait));
    auto queued_payload = std::make_unique<std::vector<std::byte>>(Bytes("queued"));
    auto queued = t->Send(Spec(port, *queued_payload));
    t->Cancel(queued);
    queued_payload.reset();  // what the codec's deadline path does (#508)

    ASSERT_EQ(queued.Future().wait_for(std::chrono::milliseconds(0)), std::future_status::ready);
    const auto r = queued.Future().get();
    EXPECT_EQ(r.error ? r.error->kind : mt::Error::Kind::Unspecified, mt::Error::Kind::Cancelled);
    EXPECT_TRUE(Await(slow).success);
    EXPECT_EQ(peer.Requests().size(), 1U);
    EXPECT_EQ(t->Close(kWait), mt::Status::Completed);
}

TEST(Http1TransportTest, CancelInFlight_ClosesTheConnection)
{
    FakeHttp1Peer peer{[](const Http1Request&, std::size_t index)
                       {
                           return Http1Reply{.bytes = Http1Response(200),
                                             .after = After::KeepOpen,
                                             .delay = index == 0 ? kSlowReply
                                                                 : std::chrono::milliseconds(0)};
                       }};
    const int port = peer.Start();
    ASSERT_GT(port, 0);
    auto t = Connected(Options(port));
    ASSERT_NE(t, nullptr);

    const auto payload = Bytes("x");
    auto handle = t->Send(Spec(port, payload));
    ASSERT_TRUE(peer.WaitForRequests(1, kWait));
    t->Cancel(handle);
    const auto r = Await(handle);
    EXPECT_EQ(Message(r), "request cancelled");

    EXPECT_TRUE(SendAndWait(*t, port, "y").success);
    EXPECT_EQ(peer.Connections(), 2) << "HTTP/1.1 has no reset: the cancel cost the connection";
    EXPECT_EQ(t->Close(kWait), mt::Status::Completed);
}

TEST(Http1TransportTest, InFlightDeadline_FailsAndClosesTheConnection)
{
    FakeHttp1Peer peer{[](const Http1Request&, std::size_t) {
        return Http1Reply{
            .bytes = Http1Response(200), .after = After::KeepOpen, .delay = kSlowReply};
    }};
    const int port = peer.Start();
    ASSERT_GT(port, 0);
    auto t = Connected(Options(port));
    ASSERT_NE(t, nullptr);

    const auto payload = Bytes("x");
    auto handle = t->Send(Spec(port, payload, std::chrono::milliseconds(100)));
    const auto r = Await(handle);
    EXPECT_EQ(Message(r), "request deadline exceeded");
    EXPECT_EQ(t->Close(kWait), mt::Status::Completed);
}

TEST(Http1TransportTest, QueueAtCapacity_RefusesWithTransportBusy)
{
    FakeHttp1Peer peer{[](const Http1Request&, std::size_t) {
        return Http1Reply{
            .bytes = Http1Response(200), .after = After::KeepOpen, .delay = kSlowReply};
    }};
    const int port = peer.Start();
    ASSERT_GT(port, 0);
    auto opts = Options(port);
    opts.max_pending_requests = 1;
    auto t = Connected(opts);
    ASSERT_NE(t, nullptr);

    const auto payload = Bytes("x");
    auto in_flight = t->Send(Spec(port, payload));
    ASSERT_TRUE(peer.WaitForRequests(1, kWait));
    auto queued = t->Send(Spec(port, payload));
    auto refused = t->Send(Spec(port, payload));

    EXPECT_EQ(refused.Id(), 0U);
    const auto r = Await(refused);
    EXPECT_TRUE(r.transport_busy);
    EXPECT_EQ(r.error ? r.error->kind : mt::Error::Kind::Unspecified,
              mt::Error::Kind::ResourceExhausted);
    EXPECT_TRUE(Await(in_flight).success);
    EXPECT_TRUE(Await(queued).success);
    EXPECT_EQ(t->Close(kWait), mt::Status::Completed);
}

TEST(Http1TransportTest, Close_FailsWhatIsLeftAndIsIdempotent)
{
    FakeHttp1Peer peer{[](const Http1Request&, std::size_t) {
        return Http1Reply{
            .bytes = Http1Response(200), .after = After::KeepOpen, .delay = kSlowReply};
    }};
    const int port = peer.Start();
    ASSERT_GT(port, 0);
    auto t = Connected(Options(port));
    ASSERT_NE(t, nullptr);

    const auto payload = Bytes("x");
    auto in_flight = t->Send(Spec(port, payload));
    ASSERT_TRUE(peer.WaitForRequests(1, kWait));
    auto queued = t->Send(Spec(port, payload));

    EXPECT_EQ(t->Close(kWait), mt::Status::Completed);
    EXPECT_EQ(Message(Await(in_flight)), "transport closed");
    EXPECT_EQ(Message(Await(queued)), "transport closed");
    EXPECT_EQ(t->Close(kWait), mt::Status::AlreadyShutDown);
    EXPECT_EQ(t->GetState(), mt::ConnectionState::Closed);
    EXPECT_FALSE(t->Connect(Options(port)).has_value());
}

// ---------------------------------------------------------------------------
// Connect
// ---------------------------------------------------------------------------

TEST(Http1TransportTest, Connect_Refused_StaysDisconnected)
{
    FakeHttp1Peer peer{Always(Http1Response(200))};
    const int port = peer.Start();
    ASSERT_GT(port, 0);
    peer.Stop();
    auto t = MakeTransport();
    ASSERT_NE(t, nullptr);

    EXPECT_FALSE(t->Connect(Options(port)).has_value());
    EXPECT_EQ(t->GetState(), mt::ConnectionState::Disconnected);
    EXPECT_EQ(t->Send(Spec(port, {})).Id(), 0U);
}

TEST(Http1TransportTest, Connect_BadEndpoint_Fails)
{
    auto t = MakeTransport();
    ASSERT_NE(t, nullptr);
    const mti::ConnectOptions opts;
    EXPECT_FALSE(t->Connect(opts).has_value());
    EXPECT_EQ(t->GetState(), mt::ConnectionState::Disconnected);
}

TEST(Http1TransportTest, Create_NullReactor_Fails)
{
    EXPECT_FALSE(mtt::Http1Transport::Create(nullptr).has_value());
}

// ---------------------------------------------------------------------------
// HttpWireCodec over Http1Transport: status classification and header
// validation (ICP 0042 §3)
// ---------------------------------------------------------------------------

namespace
{

mti::EncodedPayload Payload()
{
    constexpr std::size_t kBytes = 4;
    return mti::EncodedPayload{std::make_unique<std::byte[]>(kBytes), kBytes};
}

mti::WireResult ExportOnce(mtt::Http1Transport& t, int port, mti::IAuthProvider* auth = nullptr)
{
    const mtw::HttpWireCodecConfig config{
        .host = Authority(port),
        .scheme = "http",
        .path = "",
        .signal_path = {},
        .extra_headers = {},
    };
    mtw::HttpWireCodec codec{&t, config, auth, nullptr, nullptr, Options(port)};
    return codec.Send(Payload(), std::chrono::milliseconds(5000));
}

}  // namespace

TEST(Http1TransportCodecTest, Success)
{
    FakeHttp1Peer peer{Always(Http1Response(200, "", "Content-Type: application/x-protobuf\r\n"))};
    const int port = peer.Start();
    ASSERT_GT(port, 0);
    auto t = Connected(Options(port));
    ASSERT_NE(t, nullptr);

    const auto r = ExportOnce(*t, port);
    EXPECT_TRUE(r.success) << (r.error ? r.error->message : "");
    EXPECT_EQ(t->Close(kWait), mt::Status::Completed);
}

TEST(Http1TransportCodecTest, RetryableStatus_IsRetryable)
{
    FakeHttp1Peer peer{Always(Http1Response(503))};
    const int port = peer.Start();
    ASSERT_GT(port, 0);
    auto t = Connected(Options(port));
    ASSERT_NE(t, nullptr);

    const auto r = ExportOnce(*t, port);
    EXPECT_FALSE(r.success);
    EXPECT_TRUE(r.retryable);
    EXPECT_EQ(t->Close(kWait), mt::Status::Completed);
}

TEST(Http1TransportCodecTest, NonRetryableStatus_IsNotRetryable)
{
    FakeHttp1Peer peer{Always(Http1Response(400))};
    const int port = peer.Start();
    ASSERT_GT(port, 0);
    auto t = Connected(Options(port));
    ASSERT_NE(t, nullptr);

    const auto r = ExportOnce(*t, port);
    EXPECT_FALSE(r.success);
    EXPECT_FALSE(r.retryable);
    EXPECT_EQ(t->Close(kWait), mt::Status::Completed);
}

TEST(Http1TransportCodecTest, LazyConnect_ThroughTheCodec)
{
    FakeHttp1Peer peer{Always(Http1Response(200))};
    const int port = peer.Start();
    ASSERT_GT(port, 0);
    auto t = MakeTransport();
    ASSERT_NE(t, nullptr);

    const auto r = ExportOnce(*t, port);
    EXPECT_TRUE(r.success) << (r.error ? r.error->message : "");
    EXPECT_EQ(t->Close(kWait), mt::Status::Completed);
}

TEST(Http1TransportCodecTest, AuthTokenWithCrLf_PeerReceivesNoRequest)
{
    FakeHttp1Peer peer{Always(Http1Response(200))};
    const int port = peer.Start();
    ASSERT_GT(port, 0);
    auto t = Connected(Options(port));
    ASSERT_NE(t, nullptr);

    mtfk::FakeAuthProvider auth;
    auth.scripted_responses.emplace_back(std::optional<std::string>{"Bearer a\r\nx-injected: 1"});
    auth.scripted_responses.emplace_back(std::optional<std::string>{"Bearer a"});

    const auto rejected = ExportOnce(*t, port, &auth);
    EXPECT_FALSE(rejected.success);
    EXPECT_FALSE(rejected.retryable);
    EXPECT_TRUE(peer.Requests().empty()) << "the injected header must not reach the peer";

    const auto accepted = ExportOnce(*t, port, &auth);
    EXPECT_TRUE(accepted.success) << (accepted.error ? accepted.error->message : "");
    const auto requests = peer.Requests();
    ASSERT_EQ(requests.size(), 1U);
    EXPECT_EQ(requests[0].Header("authorization"), "Bearer a");
    EXPECT_EQ(requests[0].HeaderCount("x-injected"), 0U);
    EXPECT_EQ(t->Close(kWait), mt::Status::Completed);
}
