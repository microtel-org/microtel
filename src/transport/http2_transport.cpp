// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#include "http2_transport.hpp"

#include "microtel/error.hpp"
#include "microtel/internal/transport.hpp"

#include "transport/connection_setup.hpp"
#include "transport/nosignal_io.hpp"

#include <nghttp2/nghttp2.h>
#include <openssl/ssl.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <future>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <poll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

namespace microtel::transport
{

// ---------------------------------------------------------------------------
// Internal helpers (anonymous namespace)
// ---------------------------------------------------------------------------

namespace
{

/// What a plaintext endpoint that turns out to speak HTTP/1.1 reports.
///
/// A plaintext microtel endpoint is h2c with prior knowledge, and the common
/// case on the other end — the OpenTelemetry Collector's `:4318` receiver —
/// serves HTTP/1.1 only. There is no fallback to offer, so the error names the
/// two configurations that do work. See issue #166.
constexpr const char* kHttp1PeerMessage =
    "peer answered the HTTP/2 preface with an HTTP/1.1 response - endpoint appears to be "
    "HTTP/1.1-only; use https:// (ALPN h2) or OTLP/gRPC; see docs/compatibility-matrix.md";

/// What a peer that hung up under our own write reports.
///
/// The generic message below blames the read that noticed the loss, which
/// sends an operator looking at nghttp2. When it was a write that discovered
/// the peer had gone — an EPIPE or ECONNRESET from `NgHttp2DoSend` — say so.
constexpr const char* kPeerClosedMessage = "peer closed the connection during the HTTP/2 handshake";

/// The only ALPN protocol microtel offers, and the only one it can use.
constexpr std::string_view kAlpnH2{"h2"};

microtel::Error AlpnMismatchError(std::string_view selected)
{
    const std::string_view negotiated = selected.empty() ? std::string_view{"none"} : selected;
    return microtel::Error{
        .kind = microtel::Error::Kind::Protocol,
        .message =
            "TLS peer did not negotiate h2 via ALPN (negotiated: " + std::string{negotiated} +
            ") - this endpoint is not an HTTP/2 receiver; use OTLP/gRPC or an h2-capable "
            "OTLP/HTTP endpoint; see docs/compatibility-matrix.md"};
}

/// @brief Submit the client's initial SETTINGS frame.
///
/// `SETTINGS_MAX_HEADER_LIST_SIZE` is the response *headers* budget (issue
/// #213). nghttp2 applies no receive-side default of its own — the 64 KiB and
/// 4 KiB defaults in its docs are send-side — so without it a peer's HEADERS
/// block is bounded only by what it chooses to send, and `response_headers`
/// grows in step with it. `max_trailer_bytes` is the value: trailers are a
/// header list, and that budget already caps the other HEADERS frame on the
/// stream. Advertising it makes nghttp2 enforce the cap, and tells the peer
/// the limit rather than leaving it to be discovered mid-response.
///
/// @param session borrowed; not retained.
/// @param opts borrowed; read for the three advertised values.
void SubmitClientSettings(nghttp2_session* session,
                          const microtel::internal::ConnectOptions& opts) noexcept
{
    const nghttp2_settings_entry iv[3] = {
        {.settings_id = NGHTTP2_SETTINGS_MAX_CONCURRENT_STREAMS,
         .value = opts.max_concurrent_streams},
        {.settings_id = NGHTTP2_SETTINGS_INITIAL_WINDOW_SIZE, .value = opts.initial_window_size},
        {.settings_id = NGHTTP2_SETTINGS_MAX_HEADER_LIST_SIZE, .value = opts.max_trailer_bytes},
    };
    ::nghttp2_submit_settings(session, NGHTTP2_FLAG_NONE, iv, std::size(iv));
}

// ---------------------------------------------------------------------------
// SSL helpers (reduce nesting depth and cognitive complexity)
// ---------------------------------------------------------------------------

std::ptrdiff_t SslSend(SSL* ssl, const std::uint8_t* data, std::size_t len) noexcept
{
    const int n = ::SSL_write(ssl, data, static_cast<int>(len));
    if (n <= 0)
    {
        const int err = ::SSL_get_error(ssl, n);
        if (err == SSL_ERROR_WANT_WRITE || err == SSL_ERROR_WANT_READ)
        {
            return NGHTTP2_ERR_WOULDBLOCK;
        }
        return NGHTTP2_ERR_CALLBACK_FAILURE;
    }
    return n;
}

std::ptrdiff_t SslRecv(SSL* ssl, std::uint8_t* buf, std::size_t len) noexcept
{
    const int n = ::SSL_read(ssl, buf, static_cast<int>(len));
    if (n <= 0)
    {
        const int err = ::SSL_get_error(ssl, n);
        if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE)
        {
            return NGHTTP2_ERR_WOULDBLOCK;
        }
        return NGHTTP2_ERR_CALLBACK_FAILURE;
    }
    return n;
}

// ---------------------------------------------------------------------------
// nghttp2 C-callback trampolines — userdata is Http2Transport*
// ---------------------------------------------------------------------------

ssize_t NgHttp2SendCb(
    nghttp2_session* /*s*/, const uint8_t* data, size_t len, int /*flags*/, void* ud) noexcept
{
    return static_cast<ssize_t>(static_cast<Http2Transport*>(ud)->NgHttp2DoSend(data, len));
}

ssize_t NgHttp2RecvCb(
    nghttp2_session* /*s*/, uint8_t* buf, size_t len, int /*flags*/, void* ud) noexcept
{
    return static_cast<ssize_t>(static_cast<Http2Transport*>(ud)->NgHttp2DoRecv(buf, len));
}

int NgHttp2OnFrameRecvCb(nghttp2_session* /*s*/, const nghttp2_frame* frame, void* ud) noexcept
{
    auto* const transport = static_cast<Http2Transport*>(ud);
    if (frame->hd.type == NGHTTP2_SETTINGS && (frame->hd.flags & NGHTTP2_FLAG_ACK) != 0U)
    {
        transport->OnSettingsAck();
    }
    else if (frame->hd.type == NGHTTP2_GOAWAY)
    {
        transport->OnGoaway(frame->goaway.last_stream_id, frame->goaway.error_code);
    }
    return 0;
}

// NOLINTNEXTLINE(readability-function-size) — 8-param signature imposed by nghttp2 C API
int NgHttp2OnHeaderCb(nghttp2_session* /*s*/,
                      const nghttp2_frame* frame,
                      const uint8_t* name,
                      size_t namelen,
                      const uint8_t* value,
                      size_t valuelen,
                      uint8_t /*flags*/,
                      void* ud) noexcept
{
    const bool is_trailer = (frame->headers.cat == NGHTTP2_HCAT_HEADERS);
    static_cast<Http2Transport*>(ud)->OnResponseHeader(
        frame->hd.stream_id,
        is_trailer,
        std::string_view{reinterpret_cast<const char*>(name), namelen},
        std::string_view{reinterpret_cast<const char*>(value), valuelen});
    return 0;
}

int NgHttp2OnDataChunkRecvCb(nghttp2_session* /*s*/,
                             uint8_t /*flags*/,
                             int32_t stream_id,
                             const uint8_t* data,
                             size_t len,
                             void* ud) noexcept
{
    static_cast<Http2Transport*>(ud)->OnResponseData(stream_id, data, len);
    return 0;
}

int NgHttp2OnStreamCloseCb(nghttp2_session* /*s*/,
                           int32_t stream_id,
                           uint32_t error_code,
                           void* ud) noexcept
{
    static_cast<Http2Transport*>(ud)->OnStreamClose(stream_id, error_code);
    return 0;
}

// Cast a const string's data pointer to the non-const uint8_t* nghttp2 expects.
// nghttp2 never writes through these pointers.
uint8_t* MutStr(const std::string& s) noexcept
{
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast)
    return reinterpret_cast<uint8_t*>(const_cast<char*>(s.data()));
}

// nghttp2 data-provider read callback: feeds payload bytes from StreamState.
ssize_t PayloadReadCb(nghttp2_session* /*session*/,
                      int32_t /*stream_id*/,
                      uint8_t* buf,
                      size_t length,
                      uint32_t* data_flags,
                      nghttp2_data_source* source,
                      void* user_data) noexcept
{
    auto* state = static_cast<Http2Transport::StreamState*>(source->ptr);
    return static_cast<Http2Transport*>(user_data)->ReadPayload(*state, buf, length, data_flags);
}


constexpr int kPollIntervalMs = 50;

}  // namespace

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

Http2Transport::Http2Transport(std::unique_ptr<internal::IReactor> reactor) noexcept
    : m_reactor(std::move(reactor))
{
}

// static
microtel::Expected<std::unique_ptr<Http2Transport>, microtel::Error> Http2Transport::Create(
    std::unique_ptr<internal::IReactor> reactor) noexcept
{
    if (!reactor)
    {
        return microtel::Unexpected<microtel::Error>{microtel::Error{
            .kind = microtel::Error::Kind::InternalFailure, .message = "null reactor"}};
    }
    try
    {
        // Private constructor; make_unique can't reach it.
        // NOLINTNEXTLINE(cppcoreguidelines-owning-memory)
        auto t = std::unique_ptr<Http2Transport>(new Http2Transport(std::move(reactor)));
        t->m_io_thread = std::thread(&Http2Transport::IoThreadLoop, t.get());
        return t;
    }
    // Not just bad_alloc: std::thread's constructor throws std::system_error
    // when the process cannot spawn another thread (EAGAIN — a container pid
    // limit, say), which is a far likelier failure here than heap exhaustion.
    // Create is noexcept, so that exception used to terminate the host process
    // instead of being reported as a build error.
    catch (const std::exception&)
    {
        return microtel::Unexpected<microtel::Error>{
            microtel::Error{.kind = microtel::Error::Kind::InternalFailure,
                            // Must fit the SSO buffer — see EpollReactor::Create.
                            // Covers both causes: heap exhaustion and EAGAIN
                            // from std::thread.
                            .message = "OOM or EAGAIN"}};
    }
}

Http2Transport::~Http2Transport() noexcept
{
    (void)Close(std::chrono::milliseconds(2000));
}

// ---------------------------------------------------------------------------
// ITransport — state
// ---------------------------------------------------------------------------

microtel::ConnectionState Http2Transport::GetState() const noexcept
{
    return m_state.load(std::memory_order_acquire);
}

// ---------------------------------------------------------------------------
// ITransport — lifecycle
// ---------------------------------------------------------------------------

microtel::Expected<void, microtel::Error> Http2Transport::Connect(
    const internal::ConnectOptions& opts)
{
    // Claim the right to connect. `prior` is the state we won from.
    microtel::ConnectionState prior = microtel::ConnectionState::Disconnected;
    if (!ClaimConnectSlot(m_state, prior))
    {
        const auto* const msg = (prior == microtel::ConnectionState::Closed) ? "transport is closed"
                                                                             : "already connecting";
        return microtel::Unexpected<microtel::Error>{
            {.kind = microtel::Error::Kind::Network, .message = msg}};
    }

    // Restore whichever state we won from, not unconditionally Disconnected:
    // a failed *re*connect leaves the transport still recovering from a drop,
    // and reporting Disconnected would tell an operator the connection never
    // came up. `prior` is Disconnected or Reconnecting here — the CAS cannot
    // have succeeded from any other value.
    auto rollback = [this, prior]() noexcept
    {
        m_nghttp2_session.Reset();
        m_ssl_session.Reset();
        m_ssl_ctx.Reset();
        m_socket.Close();
        m_state.store(prior, std::memory_order_release);
    };

    AdoptBudgets(opts);

    auto ep = ParseEndpoint(opts.endpoint, opts.insecure);
    if (!ep)
    {
        rollback();
        return microtel::Unexpected<microtel::Error>{ep.error()};
    }

    auto fd = TcpConnect(ep->host, ep->port, opts.connect_timeout);
    if (!fd)
    {
        rollback();
        return microtel::Unexpected<microtel::Error>{fd.error()};
    }
    m_socket = std::move(*fd);

    if (ep->use_tls)
    {
        auto tls = TlsHandshake(opts, ep->host);
        if (!tls)
        {
            rollback();
            return microtel::Unexpected<microtel::Error>{tls.error()};
        }
        m_ssl_ctx = std::move(tls->first);
        m_ssl_session = std::move(tls->second);
    }

    auto h2 = Http2Handshake(opts);
    if (!h2)
    {
        rollback();
        return microtel::Unexpected<microtel::Error>{h2.error()};
    }
    m_nghttp2_session = std::move(*h2);

    auto reg = m_reactor->Register(m_socket.Get(),
                                   internal::EventMask::Read | internal::EventMask::Error,
                                   [this](int fd, internal::EventMask ev) { OnIoEvent(fd, ev); });
    if (!reg)
    {
        rollback();
        return microtel::Unexpected<microtel::Error>{
            {.kind = microtel::Error::Kind::InternalFailure, .message = "reactor register failed"}};
    }

    m_state.store(microtel::ConnectionState::Connected, std::memory_order_release);
    return {};
}

microtel::Status Http2Transport::Close(std::chrono::milliseconds timeout) noexcept
{
    const auto prev =
        m_state.exchange(microtel::ConnectionState::Closed, std::memory_order_acq_rel);
    if (prev == microtel::ConnectionState::Closed)
    {
        return microtel::Status::AlreadyShutDown;
    }

    m_stop.store(true, std::memory_order_release);
    m_reactor->Wake();

    // Wait for the loop to publish its exit, bounded by the caller's timeout.
    // Previously the timeout parameter was not even named and this was a bare
    // join(), so a wedged I/O thread hung Provider::Shutdown -- and through it
    // the host application's exit -- with no way for the caller to find out.
    const bool exited_in_time = [&]
    {
        std::unique_lock lk{m_io_done_mu};
        return m_io_done_cv.wait_for(lk, timeout, [this] { return m_io_done; });
    }();

    // Joined either way: the loop touches members of `this`, so detaching a
    // thread that has not finished would leave it running against a destroyed
    // transport. The timeout governs how long the caller waits before being
    // told the truth, not whether the join happens.
    if (m_io_thread.joinable())
    {
        m_io_thread.join();
    }

    // I/O thread has stopped — fulfill any streams it didn't get to close.
    AbandonInFlight("transport closed");

    // Fulfill any requests that were queued but never submitted.
    {
        const std::scoped_lock lk{m_pending_mu};
        for (auto& pending : m_pending_queue)
        {
            internal::TransportResult result;
            result.error = microtel::Error{.kind = microtel::Error::Kind::Cancelled,
                                           .message = "transport closed"};
            pending.promise.set_value(std::move(result));
        }
        m_pending_queue.clear();
    }

    // Safe to tear down connection resources.
    if (m_nghttp2_session.IsValid())
    {
        m_reactor->Unregister(m_socket.Get());
    }
    m_nghttp2_session.Reset();
    m_ssl_session.Reset();
    m_ssl_ctx.Reset();
    m_socket.Close();

    return exited_in_time ? microtel::Status::Completed : microtel::Status::TimedOut;
}

// ---------------------------------------------------------------------------
// ITransport — request handling
// ---------------------------------------------------------------------------

namespace
{

/// @brief An already-resolved handle for a request the transport would not
///        take: no stream, no id, a future the caller can read immediately.
///
/// Handle id 0 is the "never reached the wire" id — `Cancel` ignores it,
/// because there is nothing on the wire to reset.
[[nodiscard]] internal::RequestHandle RefuseRequest(microtel::Error error, bool busy) noexcept
{
    std::promise<internal::TransportResult> p;
    internal::TransportResult result;
    result.error = std::move(error);
    result.transport_busy = busy;
    p.set_value(std::move(result));
    return internal::RequestHandle{0, p.get_future()};
}

}  // namespace

internal::RequestHandle Http2Transport::Send(internal::RequestSpec spec) noexcept
{
    if (m_state.load(std::memory_order_acquire) != microtel::ConnectionState::Connected)
    {
        return RefuseRequest(
            microtel::Error{.kind = microtel::Error::Kind::Network, .message = "not connected"},
            false);
    }

    const std::uint64_t id = m_next_handle_id.fetch_add(1, std::memory_order_relaxed);
    std::promise<internal::TransportResult> p;
    auto future = p.get_future();
    {
        const std::scoped_lock lk{m_pending_mu};
        // Bounded per threading-model.md §3.2. The check is inside the same
        // lock as the push, so concurrent submitters (ICP 0009) cannot both
        // see room for the last slot.
        if (m_pending_queue.size() >= m_max_pending_requests.load(std::memory_order_relaxed))
        {
            return RefuseRequest(
                microtel::Error{.kind = microtel::Error::Kind::ResourceExhausted,
                                .message = "transport request queue full (max_pending_requests)"},
                true);
        }
        m_pending_queue.push_back(
            PendingRequest{.spec = std::move(spec), .promise = std::move(p), .handle_id = id});
    }
    m_reactor->Wake();
    return internal::RequestHandle{id, std::move(future)};
}

// Synchronous with respect to the payload (issue #508): when this returns,
// the transport will not read the request's payload again, so the codec may
// free it. A request still queued is taken out and failed here. One the I/O
// thread has already taken is listed in m_cancel_queue, which ReadPayload
// checks under the same lock it copies under; the I/O thread then resets it.
void Http2Transport::Cancel(const internal::RequestHandle& handle) noexcept
{
    if (handle.Id() == 0)
    {
        return;
    }
    std::optional<PendingRequest> queued;
    {
        const std::scoped_lock lk{m_pending_mu};
        const auto it = std::ranges::find(m_pending_queue, handle.Id(), &PendingRequest::handle_id);
        if (it != m_pending_queue.end())
        {
            queued.emplace(std::move(*it));
            m_pending_queue.erase(it);
        }
    }
    if (queued)
    {
        internal::TransportResult result;
        result.error =
            microtel::Error{.kind = microtel::Error::Kind::Cancelled, .message = "cancelled"};
        queued->promise.set_value(std::move(result));
        return;
    }
    {
        const std::scoped_lock lk{m_cancel_mu};
        m_cancel_queue.push_back(handle.Id());
    }
    m_reactor->Wake();
}

std::ptrdiff_t Http2Transport::ReadPayload(StreamState& state,
                                           std::uint8_t* buf,
                                           std::size_t length,
                                           std::uint32_t* data_flags) noexcept
{
    const std::scoped_lock lk{m_cancel_mu};
    if (state.cancelled ||
        std::ranges::find(m_cancel_queue, state.handle_id) != m_cancel_queue.end())
    {
        return NGHTTP2_ERR_TEMPORAL_CALLBACK_FAILURE;
    }
    const auto& payload = state.spec.payload;
    const std::size_t to_copy = std::min(payload.size() - state.payload_offset, length);
    if (to_copy > 0)
    {
        std::memcpy(buf, payload.data() + state.payload_offset, to_copy);
        state.payload_offset += to_copy;
    }
    if (state.payload_offset >= payload.size())
    {
        *data_flags |= NGHTTP2_DATA_FLAG_EOF;
    }
    return static_cast<std::ptrdiff_t>(to_copy);
}

// ---------------------------------------------------------------------------
// I/O-thread request lifecycle helpers
// ---------------------------------------------------------------------------

void Http2Transport::DrainPendingRequests() noexcept
{
    std::vector<PendingRequest> local;
    {
        const std::scoped_lock lk{m_pending_mu};
        local.swap(m_pending_queue);
    }
    for (auto& req : local)
    {
        SubmitStream(std::move(req));
    }
}

void Http2Transport::DrainCancelQueue() noexcept
{
    std::vector<std::uint64_t> local;
    {
        const std::scoped_lock lk{m_cancel_mu};
        local.swap(m_cancel_queue);
    }
    for (const auto& handle_id : local)
    {
        const auto it = m_handle_to_stream.find(handle_id);
        if (it == m_handle_to_stream.end())
        {
            continue;
        }
        const std::int32_t stream_id = it->second;
        // Off the queue now, so ReadPayload needs the flag to keep refusing
        // until nghttp2 has sent the reset and closed the stream.
        if (const auto stream = m_streams.find(stream_id); stream != m_streams.end())
        {
            stream->second->cancelled = true;
        }
        ::nghttp2_submit_rst_stream(
            m_nghttp2_session.Get(), NGHTTP2_FLAG_NONE, stream_id, NGHTTP2_CANCEL);
        ::nghttp2_session_send(m_nghttp2_session.Get());
        // on_stream_close_callback fires during nghttp2_session_send above
        // and calls FulfillStream, which removes the stream from the maps.
    }
}

void Http2Transport::SubmitStream(PendingRequest req) noexcept
{
    auto state = std::make_unique<StreamState>();
    state->spec = std::move(req.spec);
    state->handle_id = req.handle_id;

    std::vector<nghttp2_nv> nvs;
    nvs.reserve(state->spec.headers.size());
    for (auto& hdr : state->spec.headers)
    {
        nghttp2_nv nv{};
        nv.name = MutStr(hdr.name);
        nv.namelen = hdr.name.size();
        nv.value = MutStr(hdr.value);
        nv.valuelen = hdr.value.size();
        nv.flags = NGHTTP2_NV_FLAG_NONE;
        nvs.push_back(nv);
    }

    nghttp2_data_provider prd{};
    const nghttp2_data_provider* prd_ptr = nullptr;
    if (!state->spec.payload.empty())
    {
        prd.source.ptr = state.get();
        prd.read_callback = PayloadReadCb;
        prd_ptr = &prd;
    }

    const std::int32_t stream_id = ::nghttp2_submit_request(
        m_nghttp2_session.Get(), nullptr, nvs.data(), nvs.size(), prd_ptr, nullptr);

    if (stream_id < 0)
    {
        internal::TransportResult result;
        result.error = microtel::Error{.kind = microtel::Error::Kind::Network,
                                       .message = "nghttp2_submit_request failed"};
        req.promise.set_value(std::move(result));
        return;
    }

    const std::uint64_t handle_id = state->handle_id;
    state->promise = std::move(req.promise);
    m_handle_to_stream[handle_id] = stream_id;
    m_streams[stream_id] = std::move(state);

    ::nghttp2_session_send(m_nghttp2_session.Get());
}

namespace
{

/// @brief The error a stream carries when it breached a response memory cap.
///
/// `Malformed` rather than a kind of its own: the same choice
/// `HttpWireCodec::BodyFailure` makes for the decompression ceiling — the
/// counter and the message carry the distinction, and `Error::Kind` is public
/// surface not worth churning for it. The message names the cap because that
/// is the setting an operator would raise.
/// @brief Copy one nghttp2 header field into an owned `HeaderField`.
///
/// nghttp2's buffers are recycled on the next read, so the bytes have to be
/// copied here or not at all.
[[nodiscard]] internal::HeaderField MakeHeaderField(std::string_view name, std::string_view value)
{
    return internal::HeaderField{.name = std::string{name}, .value = std::string{value}};
}

[[nodiscard]] microtel::Error OversizedError(Http2Transport::ResponseOverflow kind)
{
    const auto* const message = (kind == Http2Transport::ResponseOverflow::Trailers)
                                    ? "response trailers exceed max_trailer_bytes"
                                    : "response exceeds max_response_bytes";
    return microtel::Error{.kind = microtel::Error::Kind::Malformed, .message = message};
}

}  // namespace

void Http2Transport::FulfillStream(std::int32_t stream_id,
                                   std::uint32_t nghttp2_error_code) noexcept
{
    const auto it = m_streams.find(stream_id);
    if (it == m_streams.end())
    {
        return;
    }
    auto state = std::move(it->second);
    m_streams.erase(it);
    m_handle_to_stream.erase(state->handle_id);

    // Checked before the nghttp2 code: the stream closes with whatever code
    // our own RST_STREAM carried, or even cleanly if the peer's END_STREAM won
    // the race, and neither says why we stopped reading it.
    if (state->overflow != ResponseOverflow::None)
    {
        state->result.response_too_large = true;
        state->result.error = OversizedError(state->overflow);
        state->promise.set_value(std::move(state->result));
        return;
    }

    if (nghttp2_error_code == 0)
    {
        state->result.success = true;
    }
    else if (nghttp2_error_code == NGHTTP2_CANCEL)
    {
        state->result.error = microtel::Error{.kind = microtel::Error::Kind::Cancelled,
                                              .message = "request cancelled"};
    }
    else
    {
        state->result.error =
            microtel::Error{.kind = microtel::Error::Kind::Network, .message = "stream error"};
    }
    state->promise.set_value(std::move(state->result));
}

// ---------------------------------------------------------------------------
// nghttp2 response callbacks (called from trampolines on I/O thread)
// ---------------------------------------------------------------------------

void Http2Transport::OnStreamClose(std::int32_t stream_id, std::uint32_t error_code) noexcept
{
    FulfillStream(stream_id, error_code);
}

namespace
{

/// @brief Mnemonic for the GOAWAY error codes `goaway-handling.md` tabulates.
///
/// v1 treats every code as the same category — drain and reconnect — so the
/// name is purely diagnostic: it is what lets an operator correlate the drain
/// with a peer-side incident. Codes outside the table are reported by number.
[[nodiscard]] const char* GoawayErrorName(std::uint32_t error_code) noexcept
{
    switch (error_code)
    {
        case NGHTTP2_NO_ERROR:
            return "NO_ERROR";
        case NGHTTP2_PROTOCOL_ERROR:
            return "PROTOCOL_ERROR";
        case NGHTTP2_INTERNAL_ERROR:
            return "INTERNAL_ERROR";
        case NGHTTP2_ENHANCE_YOUR_CALM:
            return "ENHANCE_YOUR_CALM";
        default:
            return "unnamed";
    }
}

}  // namespace

void Http2Transport::OnGoaway(std::int32_t last_stream_id, std::uint32_t error_code) noexcept
{
    // snprintf into the member buffer rather than building a std::string: this
    // runs inside a noexcept nghttp2 callback, where an allocation failure
    // would take the process rather than the batch.
    (void)std::snprintf(m_goaway_detail.data(),
                        m_goaway_detail.size(),
                        "peer sent GOAWAY last_stream_id=%d error=%u (%s)",
                        last_stream_id,
                        error_code,
                        GoawayErrorName(error_code));
    m_goaway_received.store(true, std::memory_order_release);

    // Only the refusal happens here. The state change waits for `OnIoEvent`,
    // where nghttp2 has finished with the socket for this turn — see
    // `FinishGoawayDrainIfIdle`.
    RefuseStreamsAbove(last_stream_id);
}

void Http2Transport::RefuseStreamsAbove(std::int32_t last_stream_id) noexcept
{
    for (auto it = m_streams.begin(); it != m_streams.end();)
    {
        if (it->first <= last_stream_id)
        {
            // The peer accepted this one and may still answer it: leave it
            // alone (`goaway-handling.md` annotation 1).
            ++it;
            continue;
        }
        auto state = std::move(it->second);
        it = m_streams.erase(it);
        m_handle_to_stream.erase(state->handle_id);
        // Network, matching what `FulfillStream` makes of the REFUSED_STREAM
        // close that nghttp2 is about to deliver for this stream — the codec
        // above retries it either way. Only the message changes, and the
        // message is the whole operator-visible explanation.
        state->result.error = microtel::Error{.kind = microtel::Error::Kind::Network,
                                              .message = m_goaway_detail.data()};
        state->promise.set_value(std::move(state->result));
    }
}

bool Http2Transport::FinishGoawayDrainIfIdle() noexcept
{
    if (!m_goaway_received.load(std::memory_order_acquire) || !m_streams.empty())
    {
        return false;
    }
    // Compare-exchange rather than a store: `Close` may have published Closed
    // while the peer's GOAWAY was in flight, and a blind store would resurrect
    // a closed transport into Reconnecting.
    auto expected = microtel::ConnectionState::Connected;
    return m_state.compare_exchange_strong(expected,
                                           microtel::ConnectionState::Reconnecting,
                                           std::memory_order_acq_rel,
                                           std::memory_order_acquire);
}

void Http2Transport::AdoptBudgets(const internal::ConnectOptions& opts) noexcept
{
    m_max_response_bytes.store(opts.max_response_bytes, std::memory_order_relaxed);
    m_max_trailer_bytes.store(opts.max_trailer_bytes, std::memory_order_relaxed);
    m_max_pending_requests.store(opts.max_pending_requests, std::memory_order_relaxed);
}

void Http2Transport::FailOversizedStream(std::int32_t stream_id,
                                         StreamState& state,
                                         ResponseOverflow kind) noexcept
{
    state.overflow = kind;
    // Release rather than keep: the budget exists to stop this memory being
    // held, and a truncated body would reach the codec looking like a
    // malformed one. `shrink_to_fit` is what actually returns the capacity.
    state.result.response_body.clear();
    state.result.response_body.shrink_to_fit();
    state.result.response_trailers.clear();
    state.result.response_trailers.shrink_to_fit();

    // Tell the peer to stop. Submitted, not sent: this runs inside an nghttp2
    // callback, and the `nghttp2_session_send` that ends the turn flushes it.
    ::nghttp2_submit_rst_stream(
        m_nghttp2_session.Get(), NGHTTP2_FLAG_NONE, stream_id, NGHTTP2_CANCEL);
}

void Http2Transport::OnResponseHeader(std::int32_t stream_id,
                                      bool is_trailer,
                                      std::string_view name,
                                      std::string_view value) noexcept
{
    const auto it = m_streams.find(stream_id);
    if (it == m_streams.end())
    {
        return;
    }
    auto& state = *it->second;
    if (state.overflow != ResponseOverflow::None)
    {
        return;  // already over budget; buffer nothing further
    }
    // Only trailers are metered: `max_trailer_bytes` is a trailer budget, and
    // the cap is checked before the strings are copied, so an over-budget
    // field is never materialised.
    if (is_trailer)
    {
        state.trailer_bytes += name.size() + value.size();
        if (state.trailer_bytes > m_max_trailer_bytes.load(std::memory_order_relaxed))
        {
            FailOversizedStream(stream_id, state, ResponseOverflow::Trailers);
            return;
        }
        state.result.response_trailers.push_back(MakeHeaderField(name, value));
        return;
    }
    state.result.response_headers.push_back(MakeHeaderField(name, value));
}

void Http2Transport::OnResponseData(std::int32_t stream_id,
                                    const std::uint8_t* data,
                                    std::size_t len) noexcept
{
    const auto it = m_streams.find(stream_id);
    if (it == m_streams.end())
    {
        return;
    }
    auto& state = *it->second;
    if (state.overflow != ResponseOverflow::None)
    {
        return;  // already over budget; the peer's RST_STREAM is in flight
    }
    auto& body = state.result.response_body;
    if (body.size() + len > m_max_response_bytes.load(std::memory_order_relaxed))
    {
        FailOversizedStream(stream_id, state, ResponseOverflow::Body);
        return;
    }
    const auto* bytes = reinterpret_cast<const std::byte*>(data);
    body.insert(body.end(), bytes, bytes + len);
}

// ---------------------------------------------------------------------------
// TLS handshake
// ---------------------------------------------------------------------------

microtel::Expected<std::pair<common::raii::SslCtx, common::raii::SslSession>, microtel::Error>
Http2Transport::TlsHandshake(const internal::ConnectOptions& opts, const std::string& host)
{
    // Advertise HTTP/2 via ALPN.
    static constexpr std::array<std::uint8_t, 3> kAlpn{2, 'h', '2'};
    auto tls = TlsConnect(m_socket.Get(), opts, host, kAlpn);
    if (!tls)
    {
        return microtel::Unexpected<microtel::Error>{tls.error()};
    }

    // Advertising h2 is not the same as getting it. A TLS receiver that serves
    // HTTP/1.1 only typically ignores ALPN altogether, and without this check
    // the connection went on to the HTTP/2 preface and died there as a generic
    // nghttp2 failure naming neither ALPN nor the endpoint's real protocol.
    const std::string_view alpn = SelectedAlpn(tls->second.Get());
    if (alpn != kAlpnH2)
    {
        return microtel::Unexpected<microtel::Error>{AlpnMismatchError(alpn)};
    }

    return tls;
}

// ---------------------------------------------------------------------------
// nghttp2 SETTINGS exchange
// ---------------------------------------------------------------------------

microtel::Expected<common::raii::Nghttp2Session, microtel::Error> Http2Transport::Http2Handshake(
    const internal::ConnectOptions& opts)
{
    // Per-attempt detection state, reset before anything can read from the
    // socket. Reconnect is a first-class path (ICP 0018 §3) and this is the
    // only reader of both flags, so clearing them here — rather than once at
    // construction — is what stops one peer's diagnosis reaching the next.
    m_first_plaintext_recv.store(true, std::memory_order_release);
    m_peer_spoke_http1.store(false, std::memory_order_release);
    m_peer_closed_on_send.store(false, std::memory_order_release);
    // Likewise the GOAWAY: the new session has had none, and inheriting the
    // old one's would retire this connection the moment a stream finished.
    m_goaway_received.store(false, std::memory_order_release);

    nghttp2_session_callbacks* cbs = nullptr;
    ::nghttp2_session_callbacks_new(&cbs);
    ::nghttp2_session_callbacks_set_send_callback(cbs, NgHttp2SendCb);
    ::nghttp2_session_callbacks_set_recv_callback(cbs, NgHttp2RecvCb);
    ::nghttp2_session_callbacks_set_on_frame_recv_callback(cbs, NgHttp2OnFrameRecvCb);
    ::nghttp2_session_callbacks_set_on_header_callback(cbs, NgHttp2OnHeaderCb);
    ::nghttp2_session_callbacks_set_on_data_chunk_recv_callback(cbs, NgHttp2OnDataChunkRecvCb);
    ::nghttp2_session_callbacks_set_on_stream_close_callback(cbs, NgHttp2OnStreamCloseCb);

    nghttp2_session* raw = nullptr;
    ::nghttp2_session_client_new(&raw, cbs, this);
    ::nghttp2_session_callbacks_del(cbs);

    common::raii::Nghttp2Session session{raw};
    if (!session.IsValid())
    {
        return microtel::Unexpected<microtel::Error>{
            {.kind = microtel::Error::Kind::InternalFailure,
             .message = "nghttp2_session_client_new failed"}};
    }

    SubmitClientSettings(session.Get(), opts);

    // Send client connection preface + initial SETTINGS.
    ::nghttp2_session_send(session.Get());

    m_settings_ack_received.store(false, std::memory_order_release);
    const auto deadline = std::chrono::steady_clock::now() + opts.connect_timeout;

    while (!m_settings_ack_received.load(std::memory_order_acquire))
    {
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline)
        {
            return microtel::Unexpected<microtel::Error>{
                {.kind = microtel::Error::Kind::Cancelled, .message = "SETTINGS ACK timeout"}};
        }
        const auto remaining =
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
        const int poll_ms =
            (remaining < kPollIntervalMs) ? static_cast<int>(remaining) : kPollIntervalMs;
        pollfd pfd{.fd = m_socket.Get(), .events = POLLIN, .revents = 0};
        const int rv = (::poll(&pfd, 1, poll_ms) > 0) ? ::nghttp2_session_recv(session.Get()) : 0;
        // nghttp2_session_recv may have fired OnSettingsAck (m_settings_ack_received = true)
        // and then read a FIN if the server closed immediately after — that EOF is not an error.
        if (!m_settings_ack_received.load(std::memory_order_acquire) && rv != 0 &&
            rv != NGHTTP2_ERR_WOULDBLOCK)
        {
            return microtel::Unexpected<microtel::Error>{Http2HandshakeFailure()};
        }
        ::nghttp2_session_send(session.Get());
    }

    return session;
}

microtel::Error Http2Transport::Http2HandshakeFailure() const
{
    if (m_peer_spoke_http1.load(std::memory_order_acquire))
    {
        return microtel::Error{.kind = microtel::Error::Kind::Protocol,
                               .message = kHttp1PeerMessage};
    }
    if (m_peer_closed_on_send.load(std::memory_order_acquire))
    {
        return microtel::Error{.kind = microtel::Error::Kind::Network,
                               .message = kPeerClosedMessage};
    }
    return microtel::Error{.kind = microtel::Error::Kind::Network,
                           .message = "nghttp2 recv failed during SETTINGS exchange"};
}

// ---------------------------------------------------------------------------
// nghttp2 send / recv — called from the C trampolines
// ---------------------------------------------------------------------------

std::ptrdiff_t Http2Transport::NgHttp2DoSend(const std::uint8_t* data, std::size_t len) noexcept
{
    if (m_ssl_session.IsValid())
    {
        return SslSend(m_ssl_session.Get(), data, len);
    }

    const ssize_t n = SendNoSignal(m_socket.Get(), data, len);
    if (n < 0)
    {
        if (errno == EAGAIN || errno == EWOULDBLOCK)
        {
            return NGHTTP2_ERR_WOULDBLOCK;
        }
        // EPIPE and ECONNRESET are the peer-closed cases, and with MSG_NOSIGNAL
        // they arrive as ordinary errno values rather than as a signal that
        // would have terminated the host process (issue #177). nghttp2 has no
        // finer code to return, so recovery is unchanged — the reactor still
        // retires the connection — but recording which one it was is what lets
        // a handshake failure name the peer instead of blaming nghttp2.
        m_peer_closed_on_send.store(errno == EPIPE || errno == ECONNRESET,
                                    std::memory_order_release);
        return NGHTTP2_ERR_CALLBACK_FAILURE;
    }
    return static_cast<std::ptrdiff_t>(n);
}

std::ptrdiff_t Http2Transport::NgHttp2DoRecv(std::uint8_t* buf, std::size_t len) noexcept
{
    if (m_ssl_session.IsValid())
    {
        return SslRecv(m_ssl_session.Get(), buf, len);
    }

    ssize_t n = ::read(m_socket.Get(), buf, len);
    while (n < 0 && errno == EINTR)
    {
        n = ::read(m_socket.Get(), buf, len);
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
    if (SniffHttp1Response(buf, static_cast<std::size_t>(n)))
    {
        return NGHTTP2_ERR_CALLBACK_FAILURE;
    }
    return static_cast<std::ptrdiff_t>(n);
}

bool Http2Transport::SniffHttp1Response(const std::uint8_t* buf, std::size_t len) noexcept
{
    if (!m_first_plaintext_recv.exchange(false, std::memory_order_acq_rel))
    {
        return false;
    }
    constexpr std::string_view kHttp1Prefix{"HTTP/1."};
    if (len < kHttp1Prefix.size() ||
        std::memcmp(buf, kHttp1Prefix.data(), kHttp1Prefix.size()) != 0)
    {
        return false;
    }
    // Left for Http2Handshake to turn into an error: this callback can only
    // report "the read failed", and losing the reason here is what left the
    // whole class of failures wearing a generic nghttp2 message (issue #166).
    m_peer_spoke_http1.store(true, std::memory_order_release);
    return true;
}

void Http2Transport::OnSettingsAck() noexcept
{
    m_settings_ack_received.store(true, std::memory_order_release);
}

// ---------------------------------------------------------------------------
// I/O thread
// ---------------------------------------------------------------------------

void Http2Transport::IoThreadLoop() noexcept
{
    while (!m_stop.load(std::memory_order_acquire))
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
        m_reactor->WaitAndDispatch(deadline);
        // Gate on m_state before touching the connection objects, exactly as
        // OnIoEvent does. Connect writes m_socket / m_ssl_* / m_nghttp2_session
        // and *then* release-stores Connected; reading the session without the
        // matching acquire is a data race against a concurrent reconnect.
        //
        // Harmless while a transport connected once and never again, which is
        // why it went unnoticed — implementing Reconnecting (ICP 0018 §3) made
        // reconnect a first-class path and TSAN found it immediately.
        if (m_state.load(std::memory_order_acquire) == microtel::ConnectionState::Connected &&
            m_nghttp2_session.IsValid())
        {
            DrainPendingRequests();
            DrainCancelQueue();
            // A cancel can retire the last stream on a GOAWAY'd connection,
            // and no read event need follow it: without this the transport
            // would sit in Connected on a session nghttp2 refuses to open
            // streams on, which is the wedge the GOAWAY handling exists to
            // avoid. Safe here — nghttp2 is between turns, not mid-recv.
            (void)FinishGoawayDrainIfIdle();
        }
    }
    // Publish loop exit so Close's wait can be bounded by its timeout.
    {
        const std::scoped_lock lk{m_io_done_mu};
        m_io_done = true;
    }
    m_io_done_cv.notify_all();
}

void Http2Transport::AbandonInFlight(const char* message) noexcept
{
    for (auto& [stream_id, state] : m_streams)
    {
        internal::TransportResult result;
        result.error =
            microtel::Error{.kind = microtel::Error::Kind::Cancelled, .message = message};
        state->promise.set_value(std::move(result));
    }
    m_streams.clear();
    m_handle_to_stream.clear();
}

void Http2Transport::OnIoEvent(int fd, internal::EventMask events) noexcept
{
    if (m_state.load(std::memory_order_acquire) != microtel::ConnectionState::Connected)
    {
        return;
    }

    if (internal::HasEvent(events, internal::EventMask::Error))
    {
        // Fulfil before publishing the state change. Without this, every
        // in-flight promise was simply abandoned: the HTTP codec waits with a
        // deadline and recovers, but GrpcWireCodec::Send used an unbounded
        // Future().get() and blocked forever on a promise nobody would set
        // (ICP 0018).
        AbandonInFlight("connection lost");
        // Reconnecting, not Disconnected: this transport *was* connected, and
        // the next export's lazy connect will re-establish it (ICP 0018).
        // Distinguishing the two is the point of the state — "never came up"
        // is a config or network problem, "dropped and recovering" is a peer
        // problem, and an operator needs to tell them apart.
        m_state.store(microtel::ConnectionState::Reconnecting, std::memory_order_release);
        return;
    }

    if (internal::HasEvent(events, internal::EventMask::Read))
    {
        if (::nghttp2_session_recv(m_nghttp2_session.Get()) != 0)
        {
            AbandonInFlight("connection lost");
            m_state.store(microtel::ConnectionState::Reconnecting, std::memory_order_release);
            return;
        }
        // A GOAWAY seen during that recv retires the connection — but here,
        // not in the callback that saw it. Publishing Reconnecting from inside
        // the callback tells the caller thread it may reconnect while nghttp2
        // is still reading this socket, and the reconnect's `close` then races
        // the `read` still in flight (TSAN finds it immediately).
        if (FinishGoawayDrainIfIdle())
        {
            return;
        }
    }

    ::nghttp2_session_send(m_nghttp2_session.Get());

    const bool want_write = ::nghttp2_session_want_write(m_nghttp2_session.Get()) != 0;
    const auto interest =
        want_write
            ? (internal::EventMask::Read | internal::EventMask::Write | internal::EventMask::Error)
            : (internal::EventMask::Read | internal::EventMask::Error);
    m_reactor->Modify(fd, interest);
}

}  // namespace microtel::transport
