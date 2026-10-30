// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0
//
// Integration test: FallbackTransport against HTTP/1.1-only peers, plaintext
// and TLS (ICP 0042 §1-2).
//
// The peer is `tests/helpers/fake_http1_peer.hpp`. To a plaintext HTTP/2
// attempt it looks like the OpenTelemetry Collector's :4318 receiver: the
// connection preface arrives as a request line `PRI * HTTP/2.0`, and it
// answers with an HTTP/1.1 400. The HTTP/2 path (a peer that does speak h2)
// is covered in http2_send_test.cpp, next to the HTTP/2 server.

#include "transport/fallback_transport.hpp"

#include "microtel/internal/encoded_payload.hpp"
#include "microtel/internal/transport.hpp"
#include "microtel/internal/wire_result.hpp"
#include "microtel/protocol.hpp"

#include "helpers/fake_http1_peer.hpp"
#include "helpers/thread_ids.hpp"
#include "transport/epoll_reactor.hpp"
#include "wire/http/http_wire_codec.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <future>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
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
constexpr auto kThreadGone = std::chrono::seconds(2);

std::string Authority(int port)
{
    return "127.0.0.1:" + std::to_string(port);
}

mti::ConnectOptions Options(int port, bool tls = false, mt::Protocol protocol = mt::Protocol::Http)
{
    mti::ConnectOptions opts;
    opts.endpoint = (tls ? "https://" : "http://") + Authority(port);
    opts.protocol = protocol;
    opts.insecure = true;  // the peer's certificate is self-signed
    opts.connect_timeout = std::chrono::seconds(2);
    opts.tls_handshake_timeout = std::chrono::seconds(2);
    return opts;
}

mtt::FallbackTransport::ReactorFactory EpollFactory()
{
    return []() -> mt::Expected<std::unique_ptr<mti::IReactor>, mt::Error>
    {
        auto reactor = mtt::EpollReactor::Create();
        if (!reactor)
        {
            return mt::Unexpected<mt::Error>{reactor.error()};
        }
        return std::unique_ptr<mti::IReactor>{std::move(*reactor)};
    };
}

std::unique_ptr<mtt::FallbackTransport> MakeTransport(
    mtt::FallbackTransport::ReactorFactory factory = EpollFactory())
{
    auto t = mtt::FallbackTransport::Create(std::move(factory));
    return t ? std::move(*t) : nullptr;
}

/// An HTTP/1.1-only receiver: the HTTP/2 preface gets a 400 and a close,
/// anything else @p reply.
FakeHttp1Peer::Script Http1Only(std::string reply = Http1Response(200, "ok"))
{
    return [reply = std::move(reply)](const Http1Request& request, std::size_t)
    {
        if (request.method == "PRI")
        {
            return Http1Reply{.bytes = Http1Response(400), .after = After::Close};
        }
        return Http1Reply{.bytes = reply};
    };
}

std::size_t PrefaceAttempts(const FakeHttp1Peer& peer)
{
    std::size_t n = 0;
    for (const auto& request : peer.Requests())
    {
        n += request.method == "PRI" ? 1U : 0U;
    }
    return n;
}

mti::RequestSpec Spec(int port, std::span<const std::byte> payload)
{
    return mti::RequestSpec{
        .headers = {{.name = ":method", .value = "POST"},
                    {.name = ":scheme", .value = "http"},
                    {.name = ":path", .value = "/v1/traces"},
                    {.name = ":authority", .value = Authority(port)},
                    {.name = "content-type", .value = "application/x-protobuf"}},
        .payload = payload,
        .deadline = std::chrono::seconds(5),
    };
}

mti::TransportResult SendAndWait(mti::ITransport& t, int port)
{
    const std::vector<std::byte> payload(4, std::byte{0x5A});
    auto handle = t.Send(Spec(port, payload));
    if (handle.Future().wait_for(kWait) != std::future_status::ready)
    {
        mti::TransportResult timed_out;
        timed_out.error =
            mt::Error{.kind = mt::Error::Kind::Unspecified, .message = "test timeout"};
        return timed_out;
    }
    return handle.Future().get();
}

std::string Message(const mti::TransportResult& r)
{
    return r.error ? r.error->message : std::string{};
}

/// Wait until thread @p id has left /proc/self/task. A joined thread leaves
/// within moments; a running one never does.
bool ThreadGone(const std::string& id)
{
    const auto deadline = std::chrono::steady_clock::now() + kThreadGone;
    while (std::chrono::steady_clock::now() < deadline)
    {
        if (!mtfk::ThreadIds().contains(id))
        {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return false;
}

std::set<std::string> NewThreads(const std::set<std::string>& before)
{
    std::set<std::string> out;
    for (const auto& id : mtfk::ThreadIds())
    {
        if (!before.contains(id))
        {
            out.insert(id);
        }
    }
    return out;
}

}  // namespace

// ---------------------------------------------------------------------------
// When to fall back
// ---------------------------------------------------------------------------

TEST(FallbackTransportTest, Plaintext_Http1OnlyPeer_FallsBackInsideConnect)
{
    FakeHttp1Peer peer{Http1Only()};
    const int port = peer.Start();
    ASSERT_GT(port, 0);
    auto t = MakeTransport();
    ASSERT_NE(t, nullptr);

    const auto connected = t->Connect(Options(port));
    ASSERT_TRUE(connected.has_value()) << connected.error().message;
    EXPECT_TRUE(t->IsHttp1());
    EXPECT_EQ(t->GetState(), mt::ConnectionState::Connected);

    const auto r = SendAndWait(*t, port);
    EXPECT_TRUE(r.success) << Message(r);
    EXPECT_EQ(PrefaceAttempts(peer), 1U);
    const auto requests = peer.Requests();
    ASSERT_EQ(requests.size(), 2U);
    EXPECT_EQ(requests[1].method, "POST");
    EXPECT_EQ(requests[1].version, "HTTP/1.1");
    EXPECT_EQ(t->Close(kWait), mt::Status::Completed);
}

TEST(FallbackTransportTest, Tls_PeerSelectsHttp11_FallsBack)
{
    FakeHttp1Peer peer{Http1Only(), PeerTls::AlpnHttp11};
    const int port = peer.Start();
    ASSERT_GT(port, 0);
    auto t = MakeTransport();
    ASSERT_NE(t, nullptr);

    const auto connected = t->Connect(Options(port, true));
    ASSERT_TRUE(connected.has_value()) << connected.error().message;
    EXPECT_TRUE(t->IsHttp1());
    EXPECT_TRUE(SendAndWait(*t, port).success);
    EXPECT_EQ(PrefaceAttempts(peer), 0U) << "TLS decides on ALPN, before any HTTP/2 byte";
    EXPECT_EQ(peer.Connections(), 2) << "the HTTP/1.1 path repeats the handshake";
    EXPECT_EQ(t->Close(kWait), mt::Status::Completed);
}

TEST(FallbackTransportTest, Tls_PeerSelectsNoAlpn_FallsBack)
{
    FakeHttp1Peer peer{Http1Only(), PeerTls::AlpnNone};
    const int port = peer.Start();
    ASSERT_GT(port, 0);
    auto t = MakeTransport();
    ASSERT_NE(t, nullptr);

    const auto connected = t->Connect(Options(port, true));
    ASSERT_TRUE(connected.has_value()) << connected.error().message;
    EXPECT_TRUE(t->IsHttp1());
    EXPECT_TRUE(SendAndWait(*t, port).success);
    EXPECT_EQ(t->Close(kWait), mt::Status::Completed);
}

TEST(FallbackTransportTest, Grpc_NeverFallsBack_Plaintext)
{
    FakeHttp1Peer peer{Http1Only()};
    const int port = peer.Start();
    ASSERT_GT(port, 0);
    auto t = MakeTransport();
    ASSERT_NE(t, nullptr);

    const auto connected = t->Connect(Options(port, false, mt::Protocol::Grpc));
    ASSERT_FALSE(connected.has_value());
    EXPECT_EQ(connected.error().kind, mt::Error::Kind::Protocol);
    EXPECT_FALSE(t->IsHttp1());
    EXPECT_EQ(PrefaceAttempts(peer), 1U);
    EXPECT_EQ(peer.Requests().size(), 1U) << "no HTTP/1.1 request was sent";
    EXPECT_EQ(t->Close(kWait), mt::Status::Completed);
}

TEST(FallbackTransportTest, Grpc_NeverFallsBack_Tls)
{
    FakeHttp1Peer peer{Http1Only(), PeerTls::AlpnNone};
    const int port = peer.Start();
    ASSERT_GT(port, 0);
    auto t = MakeTransport();
    ASSERT_NE(t, nullptr);

    const auto connected = t->Connect(Options(port, true, mt::Protocol::Grpc));
    ASSERT_FALSE(connected.has_value());
    EXPECT_EQ(connected.error().kind, mt::Error::Kind::Protocol);
    EXPECT_FALSE(t->IsHttp1());
    EXPECT_EQ(peer.Connections(), 1);
    EXPECT_EQ(t->Close(kWait), mt::Status::Completed);
}

// ---------------------------------------------------------------------------
// The decision is remembered
// ---------------------------------------------------------------------------

TEST(FallbackTransportTest, DecisionIsRememberedAcrossReconnects)
{
    auto peer = std::make_unique<FakeHttp1Peer>(Http1Only());
    const int port = peer->Start();
    ASSERT_GT(port, 0);
    auto t = MakeTransport();
    ASSERT_NE(t, nullptr);
    ASSERT_TRUE(t->Connect(Options(port)).has_value());
    ASSERT_TRUE(t->IsHttp1());

    // Lose the receiver, so the next request's reconnect fails and the
    // transport hands back to the caller ...
    peer.reset();
    EXPECT_FALSE(SendAndWait(*t, port).success);
    EXPECT_EQ(t->GetState(), mt::ConnectionState::Reconnecting);

    // ... then bring it back on the same port, and reconnect as a codec does.
    FakeHttp1Peer again{Http1Only()};
    ASSERT_EQ(again.Start(port), port);
    const auto reconnected = t->Connect(Options(port));
    ASSERT_TRUE(reconnected.has_value()) << reconnected.error().message;
    EXPECT_TRUE(SendAndWait(*t, port).success);
    EXPECT_EQ(PrefaceAttempts(again), 0U) << "a transport that fell back never tries HTTP/2 again";
    EXPECT_EQ(t->Close(kWait), mt::Status::Completed);
}

// ---------------------------------------------------------------------------
// The switch: one I/O thread at a time, and Send during it
// ---------------------------------------------------------------------------

namespace
{

/// What the switch looked like from inside: the reactor factory's second
/// call, which the switch makes just before `Http1Transport::Create` starts
/// the new thread.
struct SwitchProbe
{
    mtt::FallbackTransport* transport = nullptr;
    int port = 0;
    std::string http2_thread;
    int factory_calls = 0;
    bool http2_thread_gone = false;
    bool send_resolved_at_once = false;
    mti::TransportResult send_result;
    mt::ConnectionState state = mt::ConnectionState::Disconnected;

    void OnFactoryCall()
    {
        if (++factory_calls != 2)
        {
            return;
        }
        http2_thread_gone = ThreadGone(http2_thread);
        const std::vector<std::byte> payload(4, std::byte{0x5A});
        auto handle = transport->Send(Spec(port, payload));
        send_resolved_at_once =
            handle.Future().wait_for(std::chrono::seconds(0)) == std::future_status::ready;
        if (send_resolved_at_once)
        {
            send_result = handle.Future().get();
        }
        state = transport->GetState();
    }
};

}  // namespace

TEST(FallbackTransportTest, Switch_NeverRunsTwoIoThreads_AndRefusesSendRetryably)
{
    FakeHttp1Peer peer{Http1Only()};
    const int port = peer.Start();
    ASSERT_GT(port, 0);

    const auto before = mtfk::ThreadIds();
    SwitchProbe probe;
    probe.port = port;
    auto t = MakeTransport(
        [&probe]() -> mt::Expected<std::unique_ptr<mti::IReactor>, mt::Error>
        {
            probe.OnFactoryCall();
            return EpollFactory()();
        });
    ASSERT_NE(t, nullptr);
    probe.transport = t.get();
    const auto after_create = NewThreads(before);
    ASSERT_EQ(after_create.size(), 1U) << "Create starts exactly one I/O thread";
    probe.http2_thread = *after_create.begin();

    ASSERT_TRUE(t->Connect(Options(port)).has_value());
    EXPECT_EQ(probe.factory_calls, 2);
    EXPECT_TRUE(probe.http2_thread_gone)
        << "the HTTP/2 I/O thread was still running when the HTTP/1.1 one was created";

    EXPECT_TRUE(probe.send_resolved_at_once) << "Send during the switch must not block";
    EXPECT_FALSE(probe.send_result.success);
    EXPECT_FALSE(probe.send_result.transport_busy);
    EXPECT_EQ(probe.send_result.error ? probe.send_result.error->kind
                                      : mt::Error::Kind::Unspecified,
              mt::Error::Kind::Network)
        << "Network: the codec classifies it retryable";
    EXPECT_EQ(probe.state, mt::ConnectionState::Connecting);

    const auto after_switch = NewThreads(before);
    EXPECT_FALSE(after_switch.contains(probe.http2_thread));
    EXPECT_EQ(after_switch.size(), 1U) << "one I/O thread after the switch";
    EXPECT_EQ(t->Close(kWait), mt::Status::Completed);
}

TEST(FallbackTransportTest, ConcurrentConnect_OneConnectsTheOtherIsTurnedAway)
{
    FakeHttp1Peer peer{Http1Only()};
    const int port = peer.Start();
    ASSERT_GT(port, 0);
    auto t = MakeTransport();
    ASSERT_NE(t, nullptr);

    std::atomic<int> succeeded{0};
    std::atomic<int> busy{0};
    std::vector<std::thread> callers;
    callers.reserve(3);
    for (int i = 0; i < 3; ++i)
    {
        callers.emplace_back(
            [&]
            {
                const auto r = t->Connect(Options(port));
                (r ? succeeded : busy).fetch_add(1);
            });
    }
    for (auto& c : callers)
    {
        c.join();
    }
    EXPECT_GE(succeeded.load(), 1);
    EXPECT_EQ(succeeded.load() + busy.load(), 3);
    EXPECT_TRUE(t->IsHttp1());
    EXPECT_EQ(PrefaceAttempts(peer), 1U) << "only one caller attempted HTTP/2";
    EXPECT_TRUE(SendAndWait(*t, port).success);
    EXPECT_EQ(t->Close(kWait), mt::Status::Completed);
}

TEST(FallbackTransportTest, Cancel_ReachesOnlyTheTransportThatIssuedTheHandle)
{
    FakeHttp1Peer peer{[](const Http1Request& request, std::size_t)
                       {
                           if (request.method == "PRI")
                           {
                               return Http1Reply{.bytes = Http1Response(400),
                                                 .after = After::Close};
                           }
                           return Http1Reply{.bytes = Http1Response(200),
                                             .after = After::KeepOpen,
                                             .delay = std::chrono::milliseconds(300)};
                       }};
    const int port = peer.Start();
    ASSERT_GT(port, 0);
    auto t = MakeTransport();
    ASSERT_NE(t, nullptr);
    ASSERT_TRUE(t->Connect(Options(port)).has_value());

    const std::vector<std::byte> payload(4, std::byte{0x5A});
    auto in_flight = t->Send(Spec(port, payload));
    ASSERT_TRUE(peer.WaitForRequests(2, kWait));
    auto queued = t->Send(Spec(port, payload));

    // A handle the retired HTTP/2 transport could have issued, with the same
    // inner id as the request in flight: it must cancel nothing.
    constexpr unsigned kGenerationShift = 56;
    const std::uint64_t inner = in_flight.Id() & ((std::uint64_t{1} << kGenerationShift) - 1);
    t->Cancel(mti::RequestHandle{(std::uint64_t{1} << kGenerationShift) | inner, {}});

    t->Cancel(queued);
    ASSERT_EQ(queued.Future().wait_for(std::chrono::seconds(0)), std::future_status::ready)
        << "a queued request is cancelled before Cancel returns, after the switch too";
    EXPECT_EQ(Message(queued.Future().get()), "cancelled");
    ASSERT_EQ(in_flight.Future().wait_for(kWait), std::future_status::ready);
    const auto r = in_flight.Future().get();
    EXPECT_TRUE(r.success) << Message(r);
    EXPECT_EQ(t->Close(kWait), mt::Status::Completed);
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

TEST(FallbackTransportTest, Close_AfterFallback_ClosesTheHttp1Transport)
{
    FakeHttp1Peer peer{Http1Only()};
    const int port = peer.Start();
    ASSERT_GT(port, 0);
    auto t = MakeTransport();
    ASSERT_NE(t, nullptr);
    ASSERT_TRUE(t->Connect(Options(port)).has_value());

    EXPECT_EQ(t->Close(kWait), mt::Status::Completed);
    EXPECT_EQ(t->Close(kWait), mt::Status::AlreadyShutDown);
    EXPECT_EQ(t->GetState(), mt::ConnectionState::Closed);
    EXPECT_FALSE(t->Connect(Options(port)).has_value());
    EXPECT_EQ(t->Send(Spec(port, {})).Id(), 0U);
}

TEST(FallbackTransportTest, Create_WithoutAFactory_Fails)
{
    EXPECT_FALSE(mtt::FallbackTransport::Create(nullptr).has_value());
}

TEST(FallbackTransportTest, Create_FactoryFails_Fails)
{
    auto factory = []() -> mt::Expected<std::unique_ptr<mti::IReactor>, mt::Error>
    {
        return mt::Unexpected<mt::Error>{
            mt::Error{.kind = mt::Error::Kind::InternalFailure, .message = "no reactor"}};
    };
    EXPECT_FALSE(mtt::FallbackTransport::Create(factory).has_value());
}

TEST(FallbackTransportTest, Switch_FactoryFails_ConnectFailsAndTheNextConnectRetries)
{
    FakeHttp1Peer peer{Http1Only()};
    const int port = peer.Start();
    ASSERT_GT(port, 0);
    int calls = 0;
    auto factory = [&]() -> mt::Expected<std::unique_ptr<mti::IReactor>, mt::Error>
    {
        if (++calls == 2)
        {
            return mt::Unexpected<mt::Error>{
                mt::Error{.kind = mt::Error::Kind::InternalFailure, .message = "no reactor"}};
        }
        return EpollFactory()();
    };
    auto t = MakeTransport(factory);
    ASSERT_NE(t, nullptr);

    EXPECT_FALSE(t->Connect(Options(port)).has_value());
    EXPECT_TRUE(t->IsHttp1()) << "the endpoint's answer is remembered even so";
    EXPECT_EQ(t->GetState(), mt::ConnectionState::Disconnected);
    EXPECT_EQ(t->Send(Spec(port, {})).Id(), 0U);

    ASSERT_TRUE(t->Connect(Options(port)).has_value());
    EXPECT_TRUE(SendAndWait(*t, port).success);
    EXPECT_EQ(PrefaceAttempts(peer), 1U);
    EXPECT_EQ(t->Close(kWait), mt::Status::Completed);
}

// ---------------------------------------------------------------------------
// Through the codec: the first export succeeds
// ---------------------------------------------------------------------------

TEST(FallbackTransportTest, HttpCodec_FirstExportToAnHttp1OnlyReceiverSucceeds)
{
    FakeHttp1Peer peer{Http1Only(Http1Response(200))};
    const int port = peer.Start();
    ASSERT_GT(port, 0);
    auto t = MakeTransport();
    ASSERT_NE(t, nullptr);

    const mtw::HttpWireCodecConfig config{
        .host = Authority(port),
        .scheme = "http",
        .path = "",
        .signal_path = {},
        .extra_headers = {},
    };
    mtw::HttpWireCodec codec{t.get(), config, nullptr, nullptr, nullptr, Options(port)};
    constexpr std::size_t kBytes = 4;
    const auto r = codec.Send(mti::EncodedPayload{std::make_unique<std::byte[]>(kBytes), kBytes},
                              std::chrono::milliseconds(5000));
    EXPECT_TRUE(r.success) << (r.error ? r.error->message : "");
    EXPECT_EQ(t->Close(kWait), mt::Status::Completed);
}
