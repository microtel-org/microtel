// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0
//
// Integration test: Http2Transport::Send over a real loopback TCP socket.
// Spins an in-process minimal nghttp2 server on a random port, completes
// the SETTINGS exchange, receives one POST request and replies with 200.
// No TLS (insecure=true).

#include "microtel/internal/encoded_payload.hpp"
#include "microtel/internal/wire_result.hpp"
#include "microtel/provider.hpp"

#include "fakes/fake_auth_provider.hpp"
#include "fakes/fake_diagnostics_sink.hpp"
#include "fakes/fake_reactor.hpp"
#include "transport/epoll_reactor.hpp"
#include "transport/http2_transport.hpp"
#include "wire/grpc/grpc_wire_codec.hpp"
#include "wire/http/http_wire_codec.hpp"

#include <gtest/gtest.h>
#include <nghttp2/nghttp2.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <future>
#include <memory>
#include <mutex>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace mtt = microtel::transport;
namespace mti = microtel::internal;
namespace mtw = microtel::wire;
namespace mtfk = microtel::testing;

// ---------------------------------------------------------------------------
// Minimal in-process HTTP/2 server: completes SETTINGS, responds to one POST
// ---------------------------------------------------------------------------

namespace
{

/// What the server does once the SETTINGS exchange is complete.
enum class ServerScript : std::uint8_t
{
    /// Read one POST and answer it with 200.
    RespondToRequest,
    /// Stop reading and reset the connection as soon as the client's first
    /// request bytes arrive — a collector restarting under an export, which is
    /// the shape issue #177 was reported against. Nothing is ever answered.
    ResetOnFirstRequest,
    /// Answer the first completed request with `GOAWAY(last_stream_id = 0,
    /// NO_ERROR)` and then hold the socket open. Nothing the client sent was
    /// accepted, so every in-flight stream is refused
    /// (`docs/sequences/goaway-handling.md`, variant 2 with `last = 0`).
    /// Holding the socket open is what makes this a test of GOAWAY rather
    /// than of the EOF path that would otherwise retire the connection.
    GoawayRefusesInFlight,
    /// Same, with a non-zero error code — the `ENHANCE_YOUR_CALM` row of the
    /// error-code table in `goaway-handling.md`.
    GoawayEnhanceYourCalm,
    /// `GOAWAY(last_stream_id = <the in-flight stream>)` followed by that
    /// stream's 200: the peer drains what it already accepted before going
    /// away (`goaway-handling.md` happy path / annotation 1).
    GoawayDrainsInFlight,
    /// `RST_STREAM(INTERNAL_ERROR)` on the first request, 200 on the second —
    /// a stream-level error must not take the connection with it
    /// (`docs/grpc-wire-protocol.md` §2.6).
    RstStreamOnFirstRequest,
    /// A gRPC response whose 5-byte length prefix straddles two DATA frames.
    GrpcSplitPrefix,
    /// A gRPC response whose message body straddles two DATA frames.
    GrpcSplitBody,
    /// A 200 whose DATA body is far larger than the cap the client connects
    /// with — the `max_response_bytes` row of `error-model.md` §7.1.
    OversizedResponseBody,
    /// A 200 whose trailers carry one deliberately huge value, for
    /// `max_trailer_bytes`. The body is empty: what is over budget here is the
    /// trailing HEADERS frame, not the DATA that precedes it.
    OversizedTrailer,
};

struct RequestServerCtx
{
    int fd = -1;
    std::atomic<bool> settings_ack_received{false};
    std::atomic<bool> response_sent{false};
    int32_t request_stream_id = -1;
    ServerScript script = ServerScript::RespondToRequest;
    /// Completed requests seen on this connection — the RST_STREAM script
    /// treats the first and the second differently.
    int requests_seen = 0;
    /// How long to keep pumping after the script's work is done, so the
    /// client observes the frame the script sent and not the close that
    /// would otherwise follow it.
    std::chrono::milliseconds hold_open{0};

    // --- split-DATA-frame response state -----------------------------------
    std::vector<std::uint8_t> body;
    std::size_t body_offset = 0;
    /// Bytes handed to nghttp2 on the first read callback, and so the length
    /// of the first DATA frame.
    std::size_t first_chunk = 0;
    int32_t response_stream_id = -1;
    /// Set once the read callback has parked the remainder with
    /// `NGHTTP2_ERR_DEFERRED`; cleared by nobody — one pause per response.
    bool deferred = false;
    /// Set once the server loop has un-parked the deferred body, so it asks
    /// nghttp2 to resume exactly once.
    bool resume_sent = false;
    std::chrono::steady_clock::time_point resume_at;
    /// Counts DATA frames actually produced, so a test can prove the response
    /// really was split rather than coalesced into one frame. Borrowed from
    /// the server object; never null once `RunSession` has run.
    std::atomic<int>* data_frames = nullptr;
    /// Counts request HEADERS frames begun, so a test can prove a request
    /// never reached the peer. Borrowed like `data_frames`.
    std::atomic<int>* requests_begun = nullptr;
};

ssize_t SrvSendCb(
    nghttp2_session* /*s*/, const uint8_t* data, size_t len, int /*flags*/, void* ud) noexcept
{
    const int fd = static_cast<RequestServerCtx*>(ud)->fd;
    ssize_t n = ::write(fd, data, len);
    while (n < 0 && errno == EINTR)
    {
        n = ::write(fd, data, len);
    }
    if (n < 0)
    {
        return (errno == EAGAIN || errno == EWOULDBLOCK)
                   ? static_cast<ssize_t>(NGHTTP2_ERR_WOULDBLOCK)
                   : static_cast<ssize_t>(NGHTTP2_ERR_CALLBACK_FAILURE);
    }
    return n;
}

ssize_t SrvRecvCb(
    nghttp2_session* /*s*/, uint8_t* buf, size_t len, int /*flags*/, void* ud) noexcept
{
    const int fd = static_cast<RequestServerCtx*>(ud)->fd;
    ssize_t n = ::read(fd, buf, len);
    while (n < 0 && errno == EINTR)
    {
        n = ::read(fd, buf, len);
    }
    if (n == 0)
    {
        return NGHTTP2_ERR_CALLBACK_FAILURE;
    }
    if (n < 0)
    {
        return (errno == EAGAIN || errno == EWOULDBLOCK) ? NGHTTP2_ERR_WOULDBLOCK
                                                         : NGHTTP2_ERR_CALLBACK_FAILURE;
    }
    return n;
}

// Record the stream id when a request HEADERS frame begins.
int SrvOnBeginHeadersCb(nghttp2_session* /*s*/, const nghttp2_frame* frame, void* ud) noexcept
{
    auto* ctx = static_cast<RequestServerCtx*>(ud);
    if (frame->hd.type == NGHTTP2_HEADERS && frame->headers.cat == NGHTTP2_HCAT_REQUEST)
    {
        ctx->request_stream_id = frame->hd.stream_id;
        ctx->requests_begun->fetch_add(1, std::memory_order_relaxed);
    }
    return 0;
}

/// Build an `nghttp2_nv` over a string literal. nghttp2's fields are
/// non-const although the library never writes through them, and the literals
/// passed here outlive the session.
nghttp2_nv SrvNv(std::string_view name, std::string_view value) noexcept
{
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast)
    auto* const n = reinterpret_cast<uint8_t*>(const_cast<char*>(name.data()));
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast)
    auto* const v = reinterpret_cast<uint8_t*>(const_cast<char*>(value.data()));
    return nghttp2_nv{.name = n,
                      .value = v,
                      .namelen = name.size(),
                      .valuelen = value.size(),
                      .flags = NGHTTP2_NV_FLAG_NONE};
}

void SrvSubmit200(nghttp2_session* s, RequestServerCtx& ctx, int32_t stream_id) noexcept
{
    const std::array<nghttp2_nv, 1> nva{SrvNv(":status", "200")};
    ::nghttp2_submit_response(s, stream_id, nva.data(), nva.size(), nullptr);
    ctx.response_sent.store(true, std::memory_order_release);
}

// ---------------------------------------------------------------------------
// gRPC response body, delivered in two DATA frames
//
// ExportTraceServiceResponse { partial_success { rejected_spans: 42 } }, under
// the 5-byte gRPC length prefix: compression flag 0x00 then the big-endian
// length. Nine bytes in all, which is what makes an off-by-one in the client's
// accumulation visible as a parse failure rather than as a plausible number.
// ---------------------------------------------------------------------------

constexpr std::array<std::uint8_t, 4> kRejected42Proto{0x0A, 0x02, 0x08, 0x2A};
constexpr std::uint32_t kRejected42Count = 42;
/// First DATA frame ends inside the 5-byte prefix.
constexpr std::size_t kSplitInsidePrefix = 3;
/// First DATA frame ends inside the message body.
constexpr std::size_t kSplitInsideBody = 7;
constexpr int kSplitPauseMs = 30;

std::vector<std::uint8_t> GrpcFramedRejected42()
{
    std::vector<std::uint8_t> out;
    out.reserve(5U + kRejected42Proto.size());
    out.push_back(0x00U);
    out.push_back(0x00U);
    out.push_back(0x00U);
    out.push_back(0x00U);
    out.push_back(static_cast<std::uint8_t>(kRejected42Proto.size()));
    out.insert(out.end(), kRejected42Proto.begin(), kRejected42Proto.end());
    return out;
}

void SrvSubmitGrpcTrailers(nghttp2_session* s, int32_t stream_id) noexcept
{
    const std::array<nghttp2_nv, 1> nva{SrvNv("grpc-status", "0")};
    ::nghttp2_submit_trailer(s, stream_id, nva.data(), nva.size());
}

/// Hands the body out in two pieces, pausing in between so the remainder
/// leaves in its own TCP write a poll() later: the client has to accumulate
/// across `recv()` calls, not merely across callbacks within one.
ssize_t SrvSplitBodyReadCb(nghttp2_session* s,
                           int32_t stream_id,
                           uint8_t* buf,
                           size_t length,
                           uint32_t* data_flags,
                           nghttp2_data_source* source,
                           void* /*ud*/) noexcept
{
    auto* ctx = static_cast<RequestServerCtx*>(source->ptr);
    if (ctx->body_offset == 0)
    {
        const size_t n = std::min(ctx->first_chunk, length);
        std::memcpy(buf, ctx->body.data(), n);
        ctx->body_offset = n;
        ctx->data_frames->fetch_add(1, std::memory_order_relaxed);
        return static_cast<ssize_t>(n);
    }
    if (!ctx->deferred)
    {
        ctx->deferred = true;
        ctx->resume_at =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(kSplitPauseMs);
        return NGHTTP2_ERR_DEFERRED;
    }

    const size_t n = std::min(ctx->body.size() - ctx->body_offset, length);
    std::memcpy(buf, ctx->body.data() + ctx->body_offset, n);
    ctx->body_offset += n;
    ctx->data_frames->fetch_add(1, std::memory_order_relaxed);
    if (ctx->body_offset == ctx->body.size())
    {
        // NO_END_STREAM because the gRPC status rides in trailers after the
        // last DATA frame (`docs/grpc-wire-protocol.md` §2.4).
        // NOLINTNEXTLINE(hicpp-signed-bitwise)
        *data_flags |= NGHTTP2_DATA_FLAG_EOF | NGHTTP2_DATA_FLAG_NO_END_STREAM;
        SrvSubmitGrpcTrailers(s, stream_id);
        ctx->response_sent.store(true, std::memory_order_release);
    }
    return static_cast<ssize_t>(n);
}

void SrvSubmitGrpcResponse(nghttp2_session* s,
                           RequestServerCtx& ctx,
                           int32_t stream_id,
                           std::size_t first_chunk)
{
    ctx.body = GrpcFramedRejected42();
    ctx.body_offset = 0;
    ctx.first_chunk = first_chunk;
    ctx.deferred = false;
    ctx.response_stream_id = stream_id;

    const std::array<nghttp2_nv, 2> nva{SrvNv(":status", "200"),
                                        SrvNv("content-type", "application/grpc")};
    nghttp2_data_provider prd{};
    prd.source.ptr = &ctx;
    prd.read_callback = SrvSplitBodyReadCb;
    ::nghttp2_submit_response(s, stream_id, nva.data(), nva.size(), &prd);
}

// ---------------------------------------------------------------------------
// Oversized responses — response-size limits (`max_response_bytes`,
// `max_trailer_bytes`)
//
// Both sizes are comfortably under HTTP/2's default 64 KiB connection window
// and under any peer's header-list limit: the point is to exceed the *client's*
// configured cap, not to stress the protocol.
// ---------------------------------------------------------------------------

constexpr std::size_t kOversizedBodyBytes = std::size_t{32} * 1024;
constexpr std::size_t kOversizedTrailerBytes = std::size_t{4} * 1024;
/// What the client is configured to accept in the oversized-response tests —
/// small enough that the very first DATA frame overruns it.
constexpr std::uint32_t kTinyResponseCap = 1024;
constexpr std::uint32_t kTinyTrailerCap = 512;

/// Hands out the whole body as fast as nghttp2 will take it.
ssize_t SrvBulkBodyReadCb(nghttp2_session* /*s*/,
                          int32_t /*stream_id*/,
                          uint8_t* buf,
                          size_t length,
                          uint32_t* data_flags,
                          nghttp2_data_source* source,
                          void* /*ud*/) noexcept
{
    auto* ctx = static_cast<RequestServerCtx*>(source->ptr);
    const size_t n = std::min(ctx->body.size() - ctx->body_offset, length);
    std::memcpy(buf, ctx->body.data() + ctx->body_offset, n);
    ctx->body_offset += n;
    ctx->data_frames->fetch_add(1, std::memory_order_relaxed);
    if (ctx->body_offset == ctx->body.size())
    {
        *data_flags |= NGHTTP2_DATA_FLAG_EOF;
        ctx->response_sent.store(true, std::memory_order_release);
    }
    return static_cast<ssize_t>(n);
}

void SrvSubmitOversizedBody(nghttp2_session* s, RequestServerCtx& ctx, int32_t stream_id)
{
    ctx.body.assign(kOversizedBodyBytes, static_cast<std::uint8_t>('B'));
    ctx.body_offset = 0;
    ctx.response_stream_id = stream_id;

    const std::array<nghttp2_nv, 2> nva{SrvNv(":status", "200"),
                                        SrvNv("content-type", "application/grpc")};
    nghttp2_data_provider prd{};
    prd.source.ptr = &ctx;
    prd.read_callback = SrvBulkBodyReadCb;
    ::nghttp2_submit_response(s, stream_id, nva.data(), nva.size(), &prd);
}

/// nghttp2 copies header names and values at submit time, so the oversized
/// value may live on this stack frame.
void SrvSubmitOversizedTrailers(nghttp2_session* s, int32_t stream_id)
{
    const std::string huge(kOversizedTrailerBytes, 'T');
    const std::array<nghttp2_nv, 2> nva{SrvNv("grpc-status", "0"), SrvNv("grpc-message", huge)};
    ::nghttp2_submit_trailer(s, stream_id, nva.data(), nva.size());
}

/// Ends the (empty) body without ending the stream, so the trailing HEADERS
/// frame is a genuine trailer rather than part of the response headers.
ssize_t SrvEmptyBodyThenTrailersCb(nghttp2_session* s,
                                   int32_t stream_id,
                                   uint8_t* /*buf*/,
                                   size_t /*length*/,
                                   uint32_t* data_flags,
                                   nghttp2_data_source* source,
                                   void* /*ud*/) noexcept
{
    auto* ctx = static_cast<RequestServerCtx*>(source->ptr);
    // NOLINTNEXTLINE(hicpp-signed-bitwise)
    *data_flags |= NGHTTP2_DATA_FLAG_EOF | NGHTTP2_DATA_FLAG_NO_END_STREAM;
    SrvSubmitOversizedTrailers(s, stream_id);
    ctx->response_sent.store(true, std::memory_order_release);
    return 0;
}

void SrvSubmitOversizedTrailerResponse(nghttp2_session* s, RequestServerCtx& ctx, int32_t stream_id)
{
    ctx.response_stream_id = stream_id;
    const std::array<nghttp2_nv, 2> nva{SrvNv(":status", "200"),
                                        SrvNv("content-type", "application/grpc")};
    nghttp2_data_provider prd{};
    prd.source.ptr = &ctx;
    prd.read_callback = SrvEmptyBodyThenTrailersCb;
    ::nghttp2_submit_response(s, stream_id, nva.data(), nva.size(), &prd);
}

void SrvSubmitGoaway(nghttp2_session* s,
                     RequestServerCtx& ctx,
                     int32_t last_stream_id,
                     uint32_t error_code) noexcept
{
    ::nghttp2_submit_goaway(s, NGHTTP2_FLAG_NONE, last_stream_id, error_code, nullptr, 0);
    ctx.response_sent.store(true, std::memory_order_release);
}

/// First request: reset the stream. Second: answer it, which is the assertion
/// that the connection outlived the stream-level error.
void SrvRunRstStreamScript(nghttp2_session* s, RequestServerCtx& ctx, int32_t stream_id) noexcept
{
    if (ctx.requests_seen == 1)
    {
        ::nghttp2_submit_rst_stream(s, NGHTTP2_FLAG_NONE, stream_id, NGHTTP2_INTERNAL_ERROR);
        return;
    }
    SrvSubmit200(s, ctx, stream_id);
}

void SrvHandleCompletedRequest(nghttp2_session* s, RequestServerCtx& ctx, int32_t stream_id)
{
    ctx.requests_seen += 1;
    switch (ctx.script)
    {
        case ServerScript::GoawayRefusesInFlight:
            SrvSubmitGoaway(s, ctx, 0, NGHTTP2_NO_ERROR);
            break;
        case ServerScript::GoawayEnhanceYourCalm:
            SrvSubmitGoaway(s, ctx, 0, NGHTTP2_ENHANCE_YOUR_CALM);
            break;
        case ServerScript::GoawayDrainsInFlight:
            ::nghttp2_submit_goaway(s, NGHTTP2_FLAG_NONE, stream_id, NGHTTP2_NO_ERROR, nullptr, 0);
            SrvSubmit200(s, ctx, stream_id);
            break;
        case ServerScript::RstStreamOnFirstRequest:
            SrvRunRstStreamScript(s, ctx, stream_id);
            break;
        case ServerScript::GrpcSplitPrefix:
            SrvSubmitGrpcResponse(s, ctx, stream_id, kSplitInsidePrefix);
            break;
        case ServerScript::GrpcSplitBody:
            SrvSubmitGrpcResponse(s, ctx, stream_id, kSplitInsideBody);
            break;
        case ServerScript::OversizedResponseBody:
            SrvSubmitOversizedBody(s, ctx, stream_id);
            break;
        case ServerScript::OversizedTrailer:
            SrvSubmitOversizedTrailerResponse(s, ctx, stream_id);
            break;
        case ServerScript::RespondToRequest:
            SrvSubmit200(s, ctx, stream_id);
            break;
        case ServerScript::ResetOnFirstRequest:
            // A request can complete in the same read as the SETTINGS ACK.
            // Answering it would let one request succeed before the reset
            // lands (issue #282); this peer answers nothing.
            break;
    }
}

bool SrvIsCompletedRequest(const nghttp2_frame& frame, const RequestServerCtx& ctx) noexcept
{
    const bool is_our_stream =
        (frame.hd.stream_id == ctx.request_stream_id) && (ctx.request_stream_id > 0);
    const bool end_stream = (frame.hd.flags & NGHTTP2_FLAG_END_STREAM) != 0U;
    return is_our_stream && end_stream && !ctx.response_sent.load(std::memory_order_acquire);
}

// On SETTINGS_ACK: mark handshake done.
// On END_STREAM for our request stream: run whatever the script says.
int SrvOnFrameRecvCb(nghttp2_session* s, const nghttp2_frame* frame, void* ud) noexcept
{
    auto* ctx = static_cast<RequestServerCtx*>(ud);

    if (frame->hd.type == NGHTTP2_SETTINGS && (frame->hd.flags & NGHTTP2_FLAG_ACK) != 0U)
    {
        ctx->settings_ack_received.store(true, std::memory_order_release);
    }

    if (SrvIsCompletedRequest(*frame, *ctx))
    {
        SrvHandleCompletedRequest(s, *ctx, frame->hd.stream_id);
    }
    return 0;
}

class MinimalHttp2RequestServer
{
public:
    MinimalHttp2RequestServer() = default;
    ~MinimalHttp2RequestServer()
    {
        Stop();
    }

    MinimalHttp2RequestServer(const MinimalHttp2RequestServer&) = delete;
    MinimalHttp2RequestServer& operator=(const MinimalHttp2RequestServer&) = delete;
    MinimalHttp2RequestServer(MinimalHttp2RequestServer&&) = delete;
    MinimalHttp2RequestServer& operator=(MinimalHttp2RequestServer&&) = delete;

    // Bind to 127.0.0.1:0, listen, start accept thread. Returns port or -1.
    int Start(ServerScript script = ServerScript::RespondToRequest)
    {
        m_script = script;
        m_listen_fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (m_listen_fd < 0)
        {
            return -1;
        }

        const int opt = 1;
        ::setsockopt(m_listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;

        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
        if (::bind(m_listen_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0)
        {
            ::close(m_listen_fd);
            m_listen_fd = -1;
            return -1;
        }
        if (::listen(m_listen_fd, 1) < 0)
        {
            ::close(m_listen_fd);
            m_listen_fd = -1;
            return -1;
        }

        socklen_t len = sizeof(addr);
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
        ::getsockname(m_listen_fd, reinterpret_cast<sockaddr*>(&addr), &len);
        m_port = ntohs(addr.sin_port);

        m_thread = std::thread([this] { ServerThread(); });
        return m_port;
    }

    /// DATA frames the response provider produced. Two means the message
    /// really did straddle a frame boundary.
    [[nodiscard]] int DataFramesSent() const noexcept
    {
        return m_data_frames.load(std::memory_order_relaxed);
    }

    /// Request HEADERS frames the server has seen begin.
    [[nodiscard]] int RequestsBegun() const noexcept
    {
        return m_requests_begun.load(std::memory_order_relaxed);
    }

    bool WaitForResponse(std::chrono::milliseconds timeout) const
    {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (!m_response_done.load(std::memory_order_acquire))
        {
            if (std::chrono::steady_clock::now() >= deadline)
            {
                return false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return true;
    }

    void Stop()
    {
        // Wake a blocked accept() without closing the fd under it: a close here
        // races the accept thread's read of m_listen_fd and lets another socket
        // reuse the number first. Close only once the thread has joined.
        if (m_listen_fd >= 0)
        {
            ::shutdown(m_listen_fd, SHUT_RDWR);
        }
        if (m_thread.joinable())
        {
            m_thread.join();
        }
        if (m_listen_fd >= 0)
        {
            ::close(m_listen_fd);
            m_listen_fd = -1;
        }
    }

private:
    void ServerThread()
    {
        const int client_fd = ::accept(m_listen_fd, nullptr, nullptr);
        if (client_fd < 0)
        {
            m_response_done.store(true, std::memory_order_release);
            return;
        }
        if (RunSession(client_fd))
        {
            // Zero linger turns close() into an RST, so the client's in-flight
            // writes fail immediately instead of only when the peer's kernel
            // gets round to answering them.
            const linger reset{.l_onoff = 1, .l_linger = 0};
            ::setsockopt(client_fd, SOL_SOCKET, SO_LINGER, &reset, sizeof(reset));
        }
        ::close(client_fd);
        m_response_done.store(true, std::memory_order_release);
    }

    /// @return true when the connection must be reset rather than closed.
    bool RunSession(int fd)
    {
        // Non-blocking so nghttp2 recv_callback returns WOULDBLOCK instead of blocking.
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg,hicpp-vararg,hicpp-signed-bitwise)
        ::fcntl(fd, F_SETFL, ::fcntl(fd, F_GETFL) | O_NONBLOCK);

        RequestServerCtx ctx;
        ctx.fd = fd;
        ctx.script = m_script;
        ctx.hold_open = HoldOpenFor(m_script);
        ctx.data_frames = &m_data_frames;
        ctx.requests_begun = &m_requests_begun;

        nghttp2_session_callbacks* cbs = nullptr;
        ::nghttp2_session_callbacks_new(&cbs);
        ::nghttp2_session_callbacks_set_send_callback(cbs, SrvSendCb);
        ::nghttp2_session_callbacks_set_recv_callback(cbs, SrvRecvCb);
        ::nghttp2_session_callbacks_set_on_frame_recv_callback(cbs, SrvOnFrameRecvCb);
        ::nghttp2_session_callbacks_set_on_begin_headers_callback(cbs, SrvOnBeginHeadersCb);

        nghttp2_session* session = nullptr;
        ::nghttp2_session_server_new(&session, cbs, &ctx);
        ::nghttp2_session_callbacks_del(cbs);

        const nghttp2_settings_entry iv[1] = {{NGHTTP2_SETTINGS_MAX_CONCURRENT_STREAMS, 100U}};
        ::nghttp2_submit_settings(session, NGHTTP2_FLAG_NONE, iv, 1);

        const bool reset = (m_script == ServerScript::ResetOnFirstRequest)
                               ? RunUntilFirstRequestByte(session, ctx, fd)
                               : RunUntilResponseSent(session, ctx, fd);

        ::nghttp2_session_del(session);
        return reset;
    }

    /// How long a script keeps the connection alive after its work is done.
    /// The GOAWAY scripts need it: a close on the heels of the GOAWAY would
    /// let the client's EOF path retire the connection and the test would
    /// pass without any GOAWAY handling at all. The split-frame scripts need
    /// it because their second DATA frame is deliberately late.
    static std::chrono::milliseconds HoldOpenFor(ServerScript script) noexcept
    {
        switch (script)
        {
            case ServerScript::GoawayRefusesInFlight:
            case ServerScript::GoawayEnhanceYourCalm:
            case ServerScript::GoawayDrainsInFlight:
                return std::chrono::milliseconds(kHoldOpenMs);
            // The oversized scripts share the split scripts' hold: it keeps the
            // client's RST_STREAM landing on a live connection, where a close
            // on the heels of the response would let the EOF path fail the
            // request and the cap would never be the reason.
            case ServerScript::GrpcSplitPrefix:
            case ServerScript::GrpcSplitBody:
            case ServerScript::OversizedResponseBody:
            case ServerScript::OversizedTrailer:
                return std::chrono::milliseconds(kSplitHoldOpenMs);
            case ServerScript::RespondToRequest:
            case ServerScript::ResetOnFirstRequest:
            case ServerScript::RstStreamOnFirstRequest:
                break;
        }
        return std::chrono::milliseconds(0);
    }

    /// Un-park a body the read callback deferred, once its pause has elapsed.
    static void MaybeResumeBody(nghttp2_session* session, RequestServerCtx& ctx) noexcept
    {
        if (!ctx.deferred || ctx.resume_sent || std::chrono::steady_clock::now() < ctx.resume_at)
        {
            return;
        }
        ctx.resume_sent = true;
        ::nghttp2_session_resume_data(session, ctx.response_stream_id);
    }

    /// The stock script: pump the session until the 200 has gone out.
    /// @return false — this connection ends with an ordinary close.
    static bool RunUntilResponseSent(nghttp2_session* session, RequestServerCtx& ctx, int fd)
    {
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(kTimeoutMs);

        while (!ctx.response_sent.load(std::memory_order_acquire) &&
               std::chrono::steady_clock::now() < deadline)
        {
            Pump(session, ctx, fd);
        }

        // Flush any remaining output after response is submitted.
        ::nghttp2_session_send(session);
        ::nghttp2_session_send(session);

        const auto hold_until = std::chrono::steady_clock::now() + ctx.hold_open;
        while (std::chrono::steady_clock::now() < hold_until)
        {
            Pump(session, ctx, fd);
        }
        return false;
    }

    /// One send / poll / recv turn of the server loop.
    static void Pump(nghttp2_session* session, RequestServerCtx& ctx, int fd) noexcept
    {
        ::nghttp2_session_send(session);
        MaybeResumeBody(session, ctx);
        pollfd pfd{.fd = fd, .events = POLLIN, .revents = 0};
        if (::poll(&pfd, 1, kPollMs) > 0)
        {
            ::nghttp2_session_recv(session);
            ::nghttp2_session_send(session);
        }
    }

    /// Complete the SETTINGS exchange, then stop reading. The first byte the
    /// client sends afterwards is the head of its request burst, and returning
    /// here resets the connection in the middle of that burst — which is what
    /// puts a reset socket under a write the client has already started.
    /// @return true — this connection ends with an RST.
    static bool RunUntilFirstRequestByte(nghttp2_session* session, RequestServerCtx& ctx, int fd)
    {
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(kTimeoutMs);

        while (std::chrono::steady_clock::now() < deadline)
        {
            ::nghttp2_session_send(session);
            pollfd pfd{.fd = fd, .events = POLLIN, .revents = 0};
            if (::poll(&pfd, 1, kPollMs) <= 0)
            {
                continue;
            }
            if (ctx.settings_ack_received.load(std::memory_order_acquire))
            {
                return true;
            }
            ::nghttp2_session_recv(session);
            // The burst can arrive in the same read as the SETTINGS ACK. Once
            // it has been read there is nothing left to wake the next poll,
            // so waiting for "the first request byte" would stall until the
            // client gave up (issue #282). Reset now instead.
            if (ctx.request_stream_id > 0)
            {
                return true;
            }
            ::nghttp2_session_send(session);
        }
        return true;
    }

    static constexpr int kPollMs = 50;
    static constexpr int kTimeoutMs = 10000;
    /// Long enough that a client which only notices the peer is gone when the
    /// socket closes cannot be mistaken for one that handled the GOAWAY.
    static constexpr int kHoldOpenMs = 1500;
    static constexpr int kSplitHoldOpenMs = 150;

    ServerScript m_script = ServerScript::RespondToRequest;
    int m_listen_fd = -1;
    int m_port = 0;
    std::thread m_thread;
    std::atomic<bool> m_response_done{false};
    std::atomic<int> m_data_frames{0};
    std::atomic<int> m_requests_begun{0};
};

}  // namespace

// ---------------------------------------------------------------------------
// Test
// ---------------------------------------------------------------------------

TEST(Http2TransportSendIntegrationTest, Send_InsecureLoopback_Succeeds)
{
    MinimalHttp2RequestServer server;
    const int port = server.Start();
    ASSERT_GT(port, 0);

    auto reactor_result = mtt::EpollReactor::Create();
    ASSERT_TRUE(reactor_result.has_value());

    auto transport_result = mtt::Http2Transport::Create(std::move(*reactor_result));
    ASSERT_TRUE(transport_result.has_value());
    auto& t = *transport_result;

    mti::ConnectOptions opts;
    opts.endpoint = "http://127.0.0.1:" + std::to_string(port);
    opts.insecure = true;
    opts.connect_timeout = std::chrono::milliseconds(5000);

    const auto connect_result = t->Connect(opts);
    ASSERT_TRUE(connect_result.has_value())
        << (connect_result.has_value() ? "" : connect_result.error().message);

    // Build a minimal POST request.
    const std::string authority = "127.0.0.1:" + std::to_string(port);
    mti::RequestSpec spec;
    spec.headers.push_back({":method", "POST"});
    spec.headers.push_back({":scheme", "http"});
    spec.headers.push_back({":path", "/test"});
    spec.headers.push_back({":authority", authority});
    spec.headers.push_back({"content-type", "application/grpc+proto"});
    spec.deadline = std::chrono::milliseconds(8000);

    // Small payload so we test the data-provider path.
    const std::array<std::byte, 4> payload_bytes{
        std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4}};
    spec.payload = std::span<const std::byte>{payload_bytes};

    auto handle = t->Send(std::move(spec));
    EXPECT_NE(handle.Id(), 0U);
    ASSERT_TRUE(handle.Future().valid());

    const auto status = handle.Future().wait_for(std::chrono::seconds(8));
    ASSERT_EQ(status, std::future_status::ready);

    const auto result = handle.Future().get();
    EXPECT_TRUE(result.success) << (result.error ? result.error->message : "");

    bool found_status_200 = false;
    for (const auto& hdr : result.response_headers)
    {
        if (hdr.name == ":status" && hdr.value == "200")
        {
            found_status_200 = true;
            break;
        }
    }
    EXPECT_TRUE(found_status_200);

    EXPECT_TRUE(server.WaitForResponse(std::chrono::milliseconds(5000)));
    (void)t->Close(std::chrono::milliseconds(1000));
    server.Stop();
}

// ---------------------------------------------------------------------------
// Concurrent Send (ICP 0009)
//
// ICP 0009 proposed relaxing ITransport::Send from "single-caller — only the
// exporter worker may call" to "safe for concurrent callers", so the M12
// metrics pipeline could share one transport with traces. The relaxation
// shipped — SdkBuilder builds three codecs over one transport, each driven by
// its own exporter worker — but the ICP was never accepted, interfaces.md §4.1
// went on marking the single-caller contract LOCKED, and the test 0009 asked
// for was never written.
//
// This is that test. It is the thing that turns "mechanically MPSC-safe" from
// an assertion into a checked property, and it is meaningful under TSAN
// (-DMICROTEL_SANITIZER=tsan).
// ---------------------------------------------------------------------------

namespace
{

constexpr int kConcurrentSenders = 4;
constexpr int kSendsPerSender = 5;

/// One sender thread: issue kSendsPerSender requests, then await them.
///
/// The payload buffers must outlive the transport's use of them:
/// `RequestSpec::payload` is a *borrowed* span (`memory-model.md` §3.3), and
/// the I/O thread reads those bytes from `PayloadReadCb` until the stream
/// completes. An earlier version of this test let each payload die at the end
/// of its loop iteration, and TSAN caught the use-after-free immediately —
/// a write in `operator delete` here against a read in `PayloadReadCb` there.
/// Keeping them in a vector that outlives the awaits is the fix.
void SendRepeatedly(mtt::Http2Transport& transport,
                    std::vector<std::uint64_t>& out_ids,
                    std::mutex& out_mu)
{
    std::vector<std::vector<std::byte>> payloads;
    std::vector<mti::RequestHandle> handles;
    payloads.reserve(kSendsPerSender);
    handles.reserve(kSendsPerSender);

    for (int i = 0; i < kSendsPerSender; ++i)
    {
        payloads.emplace_back(16, std::byte{0x5A});
        mti::RequestSpec spec{
            .headers = {{.name = ":method", .value = "POST"},
                        {.name = ":scheme", .value = "http"},
                        {.name = ":path", .value = "/v1/traces"},
                        {.name = "content-type", .value = "application/x-protobuf"}},
            .payload = std::span<const std::byte>{payloads.back().data(), payloads.back().size()},
            .deadline = std::chrono::milliseconds(3000),
        };
        handles.push_back(transport.Send(std::move(spec)));
    }

    // Await before `payloads` goes out of scope.
    for (auto& h : handles)
    {
        (void)h.Future().wait_for(std::chrono::seconds(3));
        const std::scoped_lock lk{out_mu};
        out_ids.push_back(h.Id());
    }
}

}  // namespace

TEST(Http2TransportIntegrationTest, ConcurrentSendFromMultipleThreads)
{
    MinimalHttp2RequestServer server;
    const int port = server.Start();
    ASSERT_GT(port, 0);

    auto reactor_result = mtt::EpollReactor::Create();
    ASSERT_TRUE(reactor_result.has_value());
    auto transport_result = mtt::Http2Transport::Create(std::move(*reactor_result));
    ASSERT_TRUE(transport_result.has_value());
    auto& t = *transport_result;

    mti::ConnectOptions opts;
    opts.endpoint = "http://127.0.0.1:" + std::to_string(port);
    opts.insecure = true;
    opts.connect_timeout = std::chrono::milliseconds(5000);
    ASSERT_TRUE(t->Connect(opts).has_value());

    // Four threads submitting concurrently — the shape SdkBuilder produces with
    // three exporter workers, plus margin.
    std::vector<std::uint64_t> ids_seen;
    std::mutex ids_mu;
    std::vector<std::thread> senders;
    senders.reserve(kConcurrentSenders);
    for (int i = 0; i < kConcurrentSenders; ++i)
    {
        senders.emplace_back(SendRepeatedly, std::ref(*t), std::ref(ids_seen), std::ref(ids_mu));
    }
    for (auto& s : senders)
    {
        s.join();
    }

    EXPECT_EQ(ids_seen.size(), static_cast<std::size_t>(kConcurrentSenders * kSendsPerSender));

    // Every submission must have been allocated a distinct handle id. A torn
    // read or a lost update on m_next_handle_id would collide here, and a
    // collision would route a response to the wrong waiter.
    const std::set<std::uint64_t> unique_ids{ids_seen.begin(), ids_seen.end()};
    EXPECT_EQ(unique_ids.size(), ids_seen.size())
        << "handle ids collided across concurrent Send calls";

    EXPECT_EQ(t->Close(std::chrono::milliseconds(2000)), microtel::Status::Completed);
}

// ---------------------------------------------------------------------------
// A peer that resets under an in-flight export (issue #177)
//
// This is the I/O thread's write path, the counterpart to the caller-thread
// one in http2_connect_test.cpp. Two details make it reproduce the fault
// rather than merely describe it:
//
//   * A *burst*, not one request. Linux hands the first write after a reset
//     `ECONNRESET` and only the second one `EPIPE` — and `EPIPE` is what
//     raises `SIGPIPE`. `DrainPendingRequests` submits every queued request in
//     one pass with no error check in between, so a burst is what puts two
//     writes on a reset socket; a single request never could.
//   * The server resets on the client's *first request byte*, which lands the
//     reset in the middle of that pass rather than before it, where the
//     reactor's `EPOLLERR` handling would have retired the connection first.
//
// The assertion that matters is reaching the end of the function at all.
// ---------------------------------------------------------------------------

namespace
{

constexpr int kBurstRequests = 256;
constexpr std::size_t kBurstPayloadBytes = 256;

[[nodiscard]] bool WaitForState(const mtt::Http2Transport& transport,
                                microtel::ConnectionState want,
                                std::chrono::milliseconds timeout)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline)
    {
        if (transport.GetState() == want)
        {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return transport.GetState() == want;
}

/// A request the peer reset took down: lost with the connection or failed by
/// `Close` (`Cancelled`), refused because the transport was no longer
/// connected (`Network`), or refused at submission because the burst
/// outran `max_pending_requests` (`ResourceExhausted`, flagged busy). Any
/// other kind — `Protocol`, `Malformed`, `InternalFailure` — would mean the
/// transport misread the reset as something the peer said.
[[nodiscard]] bool IsPeerResetFailure(const mti::TransportResult& result)
{
    if (!result.error.has_value())
    {
        return false;
    }
    switch (result.error->kind)
    {
        case microtel::Error::Kind::Network:
        case microtel::Error::Kind::Cancelled:
            return true;
        case microtel::Error::Kind::ResourceExhausted:
            return result.transport_busy;
        default:
            return false;
    }
}

}  // namespace

TEST(Http2TransportSendIntegrationTest, Send_PeerResetsMidBurst_ProcessSurvives)
{
    MinimalHttp2RequestServer server;
    const int port = server.Start(ServerScript::ResetOnFirstRequest);
    ASSERT_GT(port, 0);

    auto reactor_result = mtt::EpollReactor::Create();
    ASSERT_TRUE(reactor_result.has_value());
    auto transport_result = mtt::Http2Transport::Create(std::move(*reactor_result));
    ASSERT_TRUE(transport_result.has_value());
    auto& t = *transport_result;

    mti::ConnectOptions opts;
    opts.endpoint = "http://127.0.0.1:" + std::to_string(port);
    opts.insecure = true;
    opts.connect_timeout = std::chrono::milliseconds(5000);
    ASSERT_TRUE(t->Connect(opts).has_value());

    // Borrowed for the lifetime of every request below (memory-model.md §3.3),
    // so it must outlive the awaits at the bottom of the test.
    const std::vector<std::byte> payload(kBurstPayloadBytes, std::byte{0x5A});
    const std::string authority = "127.0.0.1:" + std::to_string(port);

    std::vector<mti::RequestHandle> handles;
    handles.reserve(kBurstRequests);
    for (int i = 0; i < kBurstRequests; ++i)
    {
        mti::RequestSpec spec{
            .headers = {{.name = ":method", .value = "POST"},
                        {.name = ":scheme", .value = "http"},
                        {.name = ":path", .value = "/v1/traces"},
                        {.name = ":authority", .value = authority},
                        {.name = "content-type", .value = "application/x-protobuf"}},
            .payload = std::span<const std::byte>{payload},
            .deadline = std::chrono::milliseconds(3000),
        };
        handles.push_back(t->Send(std::move(spec)));
    }

    EXPECT_TRUE(WaitForState(*t, microtel::ConnectionState::Reconnecting, std::chrono::seconds(5)))
        << "a peer reset must retire the connection, not the process — state was "
        << static_cast<int>(t->GetState());

    // Close fulfils whatever the drop did not: nothing may be left waiting.
    EXPECT_EQ(t->Close(std::chrono::milliseconds(2000)), microtel::Status::Completed);

    for (auto& handle : handles)
    {
        if (!handle.Future().valid())
        {
            continue;
        }
        ASSERT_EQ(handle.Future().wait_for(std::chrono::seconds(5)), std::future_status::ready);
        const mti::TransportResult result = handle.Future().get();
        EXPECT_FALSE(result.success) << "the peer answered nothing; no request can have succeeded";
        EXPECT_TRUE(IsPeerResetFailure(result))
            << "a request the reset took down must fail as a network, cancelled or busy "
               "error — kind was "
            << (result.error ? static_cast<int>(result.error->kind) : -1);
    }

    server.Stop();
}

// ---------------------------------------------------------------------------
// Peer GOAWAY (`docs/grpc-wire-protocol.md` §6.2)
//
// `docs/sequences/goaway-handling.md` has specified this since M0 and nothing
// implemented it: the frame-recv callback branched on SETTINGS alone, so a
// GOAWAY was absorbed in silence. The transport then sat in `Connected` on a
// session nghttp2 had already marked draining — every later `Send` refused by
// nghttp2 with no reconnect to recover it — until the peer happened to close
// the socket and the EOF path retired the connection. These tests hold the
// socket open precisely so that EOF path cannot stand in for GOAWAY handling.
// ---------------------------------------------------------------------------

namespace
{

constexpr std::size_t kSmallPayloadBytes = 4;
/// Shorter than the server's post-GOAWAY hold, so a transport that only
/// notices the peer when the socket closes cannot pass for one that handled
/// the frame.
constexpr int kGoawayWaitMs = 700;

mti::RequestSpec MakeRequestSpec(const std::string& authority, std::span<const std::byte> payload)
{
    return mti::RequestSpec{
        .headers = {{.name = ":method", .value = "POST"},
                    {.name = ":scheme", .value = "http"},
                    {.name = ":path", .value = "/v1/traces"},
                    {.name = ":authority", .value = authority},
                    {.name = "content-type", .value = "application/x-protobuf"}},
        .payload = payload,
        .deadline = std::chrono::milliseconds(8000),
    };
}

/// The message a failed result carries, or empty when it carried none.
/// A guarded accessor rather than `result.error->message` after an
/// `ASSERT_TRUE`, because clang-tidy's optional analysis cannot see through
/// gtest's macros (`bugprone-unchecked-optional-access`).
std::string ErrorMessage(const mti::TransportResult& result)
{
    return result.error ? result.error->message : std::string{};
}

/// The kind a failed result carries; `Unspecified` when it carried no error.
microtel::Error::Kind ErrorKind(const mti::TransportResult& result)
{
    return result.error ? result.error->kind : microtel::Error::Kind::Unspecified;
}

mti::ConnectOptions MakeConnectOptions(int port)
{
    mti::ConnectOptions opts;
    opts.endpoint = "http://127.0.0.1:" + std::to_string(port);
    opts.insecure = true;
    opts.connect_timeout = std::chrono::milliseconds(5000);
    return opts;
}

/// Connect a transport to @p port over loopback. Null on any failure, which
/// the caller turns into a test failure.
std::unique_ptr<mtt::Http2Transport> ConnectedTransport(int port)
{
    auto reactor_result = mtt::EpollReactor::Create();
    if (!reactor_result)
    {
        return nullptr;
    }
    auto transport_result = mtt::Http2Transport::Create(std::move(*reactor_result));
    if (!transport_result)
    {
        return nullptr;
    }
    auto transport = std::move(*transport_result);
    if (!transport->Connect(MakeConnectOptions(port)))
    {
        return nullptr;
    }
    return transport;
}

}  // namespace

TEST(Http2TransportSendIntegrationTest, Send_PeerGoawayRefusesStream_FailsNamingGoaway)
{
    MinimalHttp2RequestServer server;
    const int port = server.Start(ServerScript::GoawayRefusesInFlight);
    ASSERT_GT(port, 0);

    auto t = ConnectedTransport(port);
    ASSERT_NE(t, nullptr);

    const std::vector<std::byte> payload(kSmallPayloadBytes, std::byte{0x5A});
    const std::string authority = "127.0.0.1:" + std::to_string(port);
    auto handle = t->Send(MakeRequestSpec(authority, payload));

    // Resolved, not hung: GOAWAY(last_stream_id = 0) refuses every stream the
    // client opened, and each one must be fulfilled rather than abandoned.
    ASSERT_EQ(handle.Future().wait_for(std::chrono::seconds(5)), std::future_status::ready);
    const auto result = handle.Future().get();
    EXPECT_FALSE(result.success);
    const std::string message = ErrorMessage(result);
    EXPECT_NE(message.find("GOAWAY"), std::string::npos)
        << "the error an operator reads must name the frame that caused it — was: " << message;
    EXPECT_NE(message.find("last_stream_id=0"), std::string::npos) << message;
    EXPECT_NE(message.find("NO_ERROR"), std::string::npos) << message;

    // The connection is retired even though the socket is still open, because
    // nghttp2 will not open another stream on it.
    EXPECT_TRUE(WaitForState(
        *t, microtel::ConnectionState::Reconnecting, std::chrono::milliseconds(kGoawayWaitMs)))
        << "GOAWAY must move the transport to Reconnecting while the socket lives — state was "
        << static_cast<int>(t->GetState());

    EXPECT_EQ(t->Close(std::chrono::milliseconds(2000)), microtel::Status::Completed);
    server.Stop();
}

TEST(Http2TransportSendIntegrationTest, Send_PeerGoawayWithErrorCode_NamesTheCode)
{
    MinimalHttp2RequestServer server;
    const int port = server.Start(ServerScript::GoawayEnhanceYourCalm);
    ASSERT_GT(port, 0);

    auto t = ConnectedTransport(port);
    ASSERT_NE(t, nullptr);

    const std::vector<std::byte> payload(kSmallPayloadBytes, std::byte{0x5A});
    const std::string authority = "127.0.0.1:" + std::to_string(port);
    auto handle = t->Send(MakeRequestSpec(authority, payload));

    ASSERT_EQ(handle.Future().wait_for(std::chrono::seconds(5)), std::future_status::ready);
    const auto result = handle.Future().get();
    EXPECT_FALSE(result.success);
    // goaway-handling.md's error-code table: the code is what lets an operator
    // correlate the drain with a peer-side incident, so it has to survive into
    // the message rather than being flattened to "connection lost".
    const std::string message = ErrorMessage(result);
    EXPECT_NE(message.find("ENHANCE_YOUR_CALM"), std::string::npos) << message;

    EXPECT_EQ(t->Close(std::chrono::milliseconds(2000)), microtel::Status::Completed);
    server.Stop();
}

TEST(Http2TransportSendIntegrationTest, Send_PeerGoawayAfterAccepting_CompletesThenReconnects)
{
    MinimalHttp2RequestServer server;
    const int port = server.Start(ServerScript::GoawayDrainsInFlight);
    ASSERT_GT(port, 0);

    auto t = ConnectedTransport(port);
    ASSERT_NE(t, nullptr);

    const std::vector<std::byte> payload(kSmallPayloadBytes, std::byte{0x5A});
    const std::string authority = "127.0.0.1:" + std::to_string(port);
    auto handle = t->Send(MakeRequestSpec(authority, payload));

    ASSERT_EQ(handle.Future().wait_for(std::chrono::seconds(5)), std::future_status::ready);
    const auto result = handle.Future().get();
    // Streams at or below last_stream_id were accepted by the peer and must be
    // allowed to finish (goaway-handling.md annotation 1). Failing them here
    // would turn a graceful drain into a lost batch.
    EXPECT_TRUE(result.success) << (result.error ? result.error->message : "");

    // ...and once the last accepted stream is done, the connection retires.
    EXPECT_TRUE(WaitForState(
        *t, microtel::ConnectionState::Reconnecting, std::chrono::milliseconds(kGoawayWaitMs)))
        << "a drained GOAWAY connection must retire, not linger in Connected — state was "
        << static_cast<int>(t->GetState());

    // Recovery: the next connect is the exporter's lazy one (ICP 0017), and it
    // must succeed against the peer that came back.
    MinimalHttp2RequestServer restarted;
    const int new_port = restarted.Start();
    ASSERT_GT(new_port, 0);
    const auto reconnected = t->Connect(MakeConnectOptions(new_port));
    ASSERT_TRUE(reconnected.has_value())
        << (reconnected.has_value() ? "" : reconnected.error().message);

    const std::string new_authority = "127.0.0.1:" + std::to_string(new_port);
    auto retry = t->Send(MakeRequestSpec(new_authority, payload));
    ASSERT_EQ(retry.Future().wait_for(std::chrono::seconds(5)), std::future_status::ready);
    const auto retry_result = retry.Future().get();
    EXPECT_TRUE(retry_result.success) << (retry_result.error ? retry_result.error->message : "");

    EXPECT_EQ(t->Close(std::chrono::milliseconds(2000)), microtel::Status::Completed);
    server.Stop();
    restarted.Stop();
}

// ---------------------------------------------------------------------------
// Peer RST_STREAM (`docs/grpc-wire-protocol.md` §2.6)
//
// A stream-level error. The request it kills must fail; the connection under
// it must not. The only peer-reset coverage before this was TCP-level (an RST
// via SO_LINGER), which is a different thing entirely — that one does retire
// the connection.
// ---------------------------------------------------------------------------

TEST(Http2TransportSendIntegrationTest, Send_PeerRstStream_FailsRequestKeepsConnection)
{
    MinimalHttp2RequestServer server;
    const int port = server.Start(ServerScript::RstStreamOnFirstRequest);
    ASSERT_GT(port, 0);

    auto t = ConnectedTransport(port);
    ASSERT_NE(t, nullptr);

    const std::vector<std::byte> payload(kSmallPayloadBytes, std::byte{0x5A});
    const std::string authority = "127.0.0.1:" + std::to_string(port);

    auto first = t->Send(MakeRequestSpec(authority, payload));
    ASSERT_EQ(first.Future().wait_for(std::chrono::seconds(5)), std::future_status::ready);
    const auto first_result = first.Future().get();
    EXPECT_FALSE(first_result.success);
    ASSERT_TRUE(first_result.error.has_value()) << "a reset stream must carry an error";
    // error-model.md classifies a request that got no response as a transport
    // failure: Network, and retryable at the codec above.
    EXPECT_EQ(ErrorKind(first_result), microtel::Error::Kind::Network);

    // The half that matters: RST_STREAM is stream-level, so the connection is
    // still usable and no reconnect is needed to use it.
    EXPECT_EQ(t->GetState(), microtel::ConnectionState::Connected);

    auto second = t->Send(MakeRequestSpec(authority, payload));
    ASSERT_EQ(second.Future().wait_for(std::chrono::seconds(5)), std::future_status::ready);
    const auto second_result = second.Future().get();
    EXPECT_TRUE(second_result.success) << "a reset stream must not cost the connection — "
                                       << (second_result.error ? second_result.error->message : "");
    // No state assertion after this point: the script is finished, so the
    // server closes, and the transport is then right to report Reconnecting.
    // Asserting Connected here would be asserting a race.

    EXPECT_EQ(t->Close(std::chrono::milliseconds(2000)), microtel::Status::Completed);
    server.Stop();
}

// ---------------------------------------------------------------------------
// A gRPC message split across DATA frames (grpc-wire-protocol.md §2.3, LOCKED:
// "the parser must
// not assume a gRPC message corresponds to a single HTTP/2 DATA frame")
//
// The accumulation this exercises lives in the transport, below the codec's
// view — a `FakeTransport` hands the codec one finished body and so can never
// reach it. These drive the real codec over the real transport against a
// server that deliberately parks the remainder of the message for 30 ms: the
// two halves arrive in separate DATA frames, in separate reads.
//
// A nine-byte body is the point: `rejected_spans: 42` only decodes if every
// one of those bytes was accumulated, in order.
// ---------------------------------------------------------------------------

namespace
{

mti::EncodedPayload MakeEncodedPayload(std::size_t n = kSmallPayloadBytes)
{
    auto buf = std::make_unique<std::byte[]>(n);
    return mti::EncodedPayload{std::move(buf), n};
}

/// Run one OTLP/gRPC export against @p port and return what the codec made of
/// the response. The codec connects the transport lazily (ICP 0017).
mti::WireResult ExportOverGrpc(mtt::Http2Transport& transport, int port)
{
    const mtw::GrpcWireCodecConfig config{
        .host = "127.0.0.1:" + std::to_string(port),
        .scheme = "http",
        .extra_headers = {},
        .service_path = {},
    };
    mtw::GrpcWireCodec codec{
        &transport, config, nullptr, nullptr, nullptr, MakeConnectOptions(port)};
    return codec.Send(MakeEncodedPayload(), std::chrono::milliseconds(5000));
}

}  // namespace

TEST(Http2TransportSendIntegrationTest, GrpcResponse_SplitMidPrefix_Accumulates)
{
    MinimalHttp2RequestServer server;
    const int port = server.Start(ServerScript::GrpcSplitPrefix);
    ASSERT_GT(port, 0);

    auto reactor_result = mtt::EpollReactor::Create();
    ASSERT_TRUE(reactor_result.has_value());
    auto transport_result = mtt::Http2Transport::Create(std::move(*reactor_result));
    ASSERT_TRUE(transport_result.has_value());
    auto& t = *transport_result;

    const auto result = ExportOverGrpc(*t, port);
    EXPECT_TRUE(result.success) << (result.error ? result.error->message : "");
    EXPECT_EQ(result.partial_success_rejected, kRejected42Count)
        << "the 5-byte length prefix straddled two DATA frames and must be reassembled";
    EXPECT_GE(server.DataFramesSent(), 2) << "the server did not actually split the response";

    EXPECT_EQ(t->Close(std::chrono::milliseconds(2000)), microtel::Status::Completed);
    server.Stop();
}

TEST(Http2TransportSendIntegrationTest, GrpcResponse_SplitMidBody_Accumulates)
{
    MinimalHttp2RequestServer server;
    const int port = server.Start(ServerScript::GrpcSplitBody);
    ASSERT_GT(port, 0);

    auto reactor_result = mtt::EpollReactor::Create();
    ASSERT_TRUE(reactor_result.has_value());
    auto transport_result = mtt::Http2Transport::Create(std::move(*reactor_result));
    ASSERT_TRUE(transport_result.has_value());
    auto& t = *transport_result;

    const auto result = ExportOverGrpc(*t, port);
    EXPECT_TRUE(result.success) << (result.error ? result.error->message : "");
    EXPECT_EQ(result.partial_success_rejected, kRejected42Count)
        << "the message body straddled two DATA frames and must be reassembled";
    EXPECT_GE(server.DataFramesSent(), 2) << "the server did not actually split the response";

    EXPECT_EQ(t->Close(std::chrono::milliseconds(2000)), microtel::Status::Completed);
    server.Stop();
}

// ---------------------------------------------------------------------------
// Response memory caps — memory-model.md §6, issue #181
//
// `max_response_bytes` and `max_trailer_bytes` were declared in
// `MemoryLimitOptions`, documented in `error-model.md` §7.1, asserted by three
// source comments — and enforced nowhere: `OnResponseData` appended every byte
// a peer cared to send and `OnResponseHeader` accumulated trailers without
// bound. A hostile or broken collector could therefore grow the transport's
// per-stream buffers without limit.
//
// The enforcement point has to be here, in the transport, because that is
// where the memory is spent; by the time a codec sees a `TransportResult` the
// bytes have already been buffered. These tests drive the real transport
// against a server that deliberately overruns the cap the client connected
// with.
// ---------------------------------------------------------------------------

namespace
{

mti::ConnectOptions MakeCappedConnectOptions(int port,
                                             std::uint32_t response_cap,
                                             std::uint32_t trailer_cap)
{
    auto opts = MakeConnectOptions(port);
    opts.max_response_bytes = response_cap;
    opts.max_trailer_bytes = trailer_cap;
    return opts;
}

/// Connect with caps small enough for the oversized scripts to breach, then
/// issue one raw request and return what the transport made of the response.
std::unique_ptr<mtt::Http2Transport> CappedTransport(int port,
                                                     std::uint32_t response_cap,
                                                     std::uint32_t trailer_cap)
{
    auto reactor_result = mtt::EpollReactor::Create();
    if (!reactor_result)
    {
        return nullptr;
    }
    auto transport_result = mtt::Http2Transport::Create(std::move(*reactor_result));
    if (!transport_result)
    {
        return nullptr;
    }
    auto transport = std::move(*transport_result);
    if (!transport->Connect(MakeCappedConnectOptions(port, response_cap, trailer_cap)))
    {
        return nullptr;
    }
    return transport;
}

}  // namespace

TEST(Http2TransportSendIntegrationTest, Send_ResponseOverMaxResponseBytes_FailsAndDropsTheBody)
{
    MinimalHttp2RequestServer server;
    const int port = server.Start(ServerScript::OversizedResponseBody);
    ASSERT_GT(port, 0);

    auto t = CappedTransport(port, kTinyResponseCap, kTinyTrailerCap);
    ASSERT_NE(t, nullptr);

    const std::array<std::byte, kSmallPayloadBytes> payload{};
    auto handle = t->Send(MakeRequestSpec("127.0.0.1:" + std::to_string(port), payload));
    ASSERT_EQ(handle.Future().wait_for(std::chrono::seconds(8)), std::future_status::ready);
    const auto result = handle.Future().get();

    EXPECT_FALSE(result.success) << "a response over the cap must not be reported as delivered";
    EXPECT_TRUE(result.response_too_large)
        << "the failure must name the cap, not look like a reset";
    EXPECT_TRUE(result.response_body.empty())
        << "the buffered prefix must be released, not handed up as a truncated body";
    EXPECT_NE(ErrorMessage(result).find("max_response_bytes"), std::string::npos)
        << "observed message: " << ErrorMessage(result);

    EXPECT_EQ(t->Close(std::chrono::milliseconds(2000)), microtel::Status::Completed);
    server.Stop();
}

TEST(Http2TransportSendIntegrationTest, Send_TrailersOverMaxTrailerBytes_Fails)
{
    MinimalHttp2RequestServer server;
    const int port = server.Start(ServerScript::OversizedTrailer);
    ASSERT_GT(port, 0);

    auto t = CappedTransport(port, kTinyResponseCap, kTinyTrailerCap);
    ASSERT_NE(t, nullptr);

    const std::array<std::byte, kSmallPayloadBytes> payload{};
    auto handle = t->Send(MakeRequestSpec("127.0.0.1:" + std::to_string(port), payload));
    ASSERT_EQ(handle.Future().wait_for(std::chrono::seconds(8)), std::future_status::ready);
    const auto result = handle.Future().get();

    EXPECT_FALSE(result.success);
    EXPECT_TRUE(result.response_too_large);
    EXPECT_NE(ErrorMessage(result).find("max_trailer_bytes"), std::string::npos)
        << "observed message: " << ErrorMessage(result);

    EXPECT_EQ(t->Close(std::chrono::milliseconds(2000)), microtel::Status::Completed);
    server.Stop();
}

TEST(Http2TransportSendIntegrationTest, GrpcExport_OversizedResponse_IsTerminalAndCounted)
{
    MinimalHttp2RequestServer server;
    const int port = server.Start(ServerScript::OversizedResponseBody);
    ASSERT_GT(port, 0);

    auto reactor_result = mtt::EpollReactor::Create();
    ASSERT_TRUE(reactor_result.has_value());
    auto transport_result = mtt::Http2Transport::Create(std::move(*reactor_result));
    ASSERT_TRUE(transport_result.has_value());
    auto& t = *transport_result;

    // End to end: the codec connects the transport lazily with these options
    // (ICP 0017), so the cap reaches the transport the way it does in a built
    // Provider, and the counter is the one `GetExporterHealth()` reports.
    mtfk::FakeDiagnosticsSink sink;
    const mtw::GrpcWireCodecConfig config{
        .host = "127.0.0.1:" + std::to_string(port),
        .scheme = "http",
        .extra_headers = {},
        .service_path = {},
    };
    mtw::GrpcWireCodec codec{&*t,
                             config,
                             nullptr,
                             &sink,
                             nullptr,
                             MakeCappedConnectOptions(port, kTinyResponseCap, kTinyTrailerCap)};
    const auto result = codec.Send(MakeEncodedPayload(), std::chrono::milliseconds(5000));

    EXPECT_FALSE(result.success);
    EXPECT_FALSE(result.retryable)
        << "the peer answers the retry with the same oversized response (error-model.md §7.1)";
    EXPECT_GE(
        sink.drop_counters.at(static_cast<std::size_t>(microtel::DropReason::ResponseTooLarge)),
        1U);

    EXPECT_EQ(t->Close(std::chrono::milliseconds(2000)), microtel::Status::Completed);
    server.Stop();
}

// ---------------------------------------------------------------------------
// Bounded request queue — threading-model.md §3.2, issue #181
//
// `Send` pushed onto `m_pending_queue` with no capacity check, so a stalled
// I/O thread (a peer that stopped reading, a reconnect backing off) let the
// queue grow for as long as producers kept submitting, and
// `DropReason::TransportBusy` was declared and never incremented.
//
// The I/O thread is the seam these tests need under control: a real reactor
// drains the queue microseconds after `Send` wakes it, so the depth at which
// the bound trips is unobservable. `FakeReactor::HoldDispatch()` parks the I/O
// thread inside `WaitAndDispatch` — engaged before `Create`, so the thread
// parks on its first iteration and nothing is ever drained — which makes the
// queue depth exactly what the test pushed.
// ---------------------------------------------------------------------------

namespace
{

constexpr std::uint32_t kTinyPendingCap = 2;
constexpr int kDrainProbeAttempts = 200;
constexpr auto kDrainProbeInterval = std::chrono::milliseconds(10);

/// @brief Wait for the released I/O thread to empty the request queue.
///
/// The queue depth is not observable from outside the transport, so its
/// emptiness is read the only way a caller can: `Send` starts being accepted
/// again. Accepted probes are appended to @p probes, which must outlive the
/// call — `RequestSpec::payload` is borrowed (`memory-model.md` §3.3).
[[nodiscard]] bool WaitForQueueToDrain(mtt::Http2Transport& transport,
                                       const std::string& authority,
                                       std::span<const std::byte> payload,
                                       std::vector<mti::RequestHandle>& probes)
{
    for (int attempt = 0; attempt < kDrainProbeAttempts; ++attempt)
    {
        probes.push_back(transport.Send(MakeRequestSpec(authority, payload)));
        if (probes.back().Id() != 0)
        {
            return true;
        }
        std::this_thread::sleep_for(kDrainProbeInterval);
    }
    return false;
}

}  // namespace

TEST(Http2TransportSendIntegrationTest, Send_PendingQueueAtCapacity_RefusesWithTransportBusy)
{
    MinimalHttp2RequestServer server;
    const int port = server.Start();
    ASSERT_GT(port, 0);

    auto reactor = std::make_unique<mtfk::FakeReactor>();
    auto* const held = reactor.get();
    held->HoldDispatch();  // released at the end, before the transport dies

    auto transport_result = mtt::Http2Transport::Create(std::move(reactor));
    ASSERT_TRUE(transport_result.has_value());
    auto& t = *transport_result;

    auto opts = MakeConnectOptions(port);
    opts.max_pending_requests = kTinyPendingCap;
    ASSERT_TRUE(t->Connect(opts).has_value());

    // EXPECT, not ASSERT, from here on: an early return would leave the I/O
    // thread parked and the transport's destructor would never join it.
    const std::array<std::byte, kSmallPayloadBytes> payload{};
    const std::string authority = "127.0.0.1:" + std::to_string(port);

    std::vector<mti::RequestHandle> accepted;
    for (std::uint32_t i = 0; i < kTinyPendingCap; ++i)
    {
        accepted.push_back(t->Send(MakeRequestSpec(authority, payload)));
        EXPECT_NE(accepted.back().Id(), 0U) << "request " << i << " should have been queued";
    }

    auto refused = t->Send(MakeRequestSpec(authority, payload));
    EXPECT_EQ(refused.Id(), 0U) << "a refused request never gets a stream, so it has no id";
    // Checked before `get()`, never asserted around it: a transport that
    // queued the request instead of refusing it leaves a future nothing will
    // resolve while the I/O thread is held, and `get()` would hang CI rather
    // than fail it.
    const bool resolved =
        refused.Future().wait_for(std::chrono::seconds(2)) == std::future_status::ready;
    EXPECT_TRUE(resolved) << "the refusal must resolve immediately, not hang the worker";

    const auto result = resolved ? refused.Future().get() : mti::TransportResult{};
    EXPECT_FALSE(result.success);
    EXPECT_TRUE(result.transport_busy) << "the failure must name the queue, not look like a reset";
    EXPECT_EQ(ErrorKind(result), microtel::Error::Kind::ResourceExhausted);
    EXPECT_NE(ErrorMessage(result).find("max_pending_requests"), std::string::npos)
        << "observed message: " << ErrorMessage(result);

    // The bound is a high-water mark, not a one-way latch: once the I/O thread
    // drains the queue, `Send` accepts again. A transport that stayed busy
    // after the pressure passed would turn one stall into a dead exporter.
    held->ReleaseDispatch();
    EXPECT_TRUE(WaitForQueueToDrain(*t, authority, payload, accepted))
        << "the queue drained; the transport must accept work again";

    EXPECT_EQ(t->Close(std::chrono::milliseconds(2000)), microtel::Status::Completed);
    server.Stop();
}

// ---------------------------------------------------------------------------
// Request header validation over a real HTTP/2 connection (ICP 0042 §3)
//
// The auth token is produced at send time, after Build() has checked the
// static headers. A token carrying CR/LF must stop in the codec: the peer sees
// no request for it, and the next export, with a clean token, goes through on
// the same connection.
// ---------------------------------------------------------------------------

TEST(Http2TransportSendIntegrationTest, HttpCodec_AuthTokenWithCrLf_PeerReceivesNoRequest)
{
    MinimalHttp2RequestServer server;
    const int port = server.Start(ServerScript::RespondToRequest);
    ASSERT_GT(port, 0);

    auto reactor_result = mtt::EpollReactor::Create();
    ASSERT_TRUE(reactor_result.has_value());
    auto transport_result = mtt::Http2Transport::Create(std::move(*reactor_result));
    ASSERT_TRUE(transport_result.has_value());
    auto& t = *transport_result;

    mtfk::FakeAuthProvider auth;
    auth.scripted_responses.emplace_back(std::optional<std::string>{"Bearer a\r\nx-injected: 1"});
    auth.scripted_responses.emplace_back(std::optional<std::string>{"Bearer a"});
    const mtw::HttpWireCodecConfig config{
        .host = "127.0.0.1:" + std::to_string(port),
        .scheme = "http",
        .path = "",
        .signal_path = {},
        .extra_headers = {},
    };
    mtw::HttpWireCodec codec{&*t, config, &auth, nullptr, nullptr, MakeConnectOptions(port)};

    const auto rejected = codec.Send(MakeEncodedPayload(), std::chrono::milliseconds(5000));
    EXPECT_FALSE(rejected.success);
    EXPECT_FALSE(rejected.retryable);
    EXPECT_EQ(server.RequestsBegun(), 0) << "the bad header must not reach the peer";

    const auto accepted = codec.Send(MakeEncodedPayload(), std::chrono::milliseconds(5000));
    EXPECT_TRUE(accepted.success) << (accepted.error ? accepted.error->message : "");
    EXPECT_EQ(server.RequestsBegun(), 1) << "only the clean request reached the peer";

    EXPECT_EQ(t->Close(std::chrono::milliseconds(2000)), microtel::Status::Completed);
    server.Stop();
}
