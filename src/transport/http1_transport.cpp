// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#include "http1_transport.hpp"

#include "microtel/error.hpp"
#include "microtel/internal/transport.hpp"
#include "microtel/log_sink.hpp"

#include "common/internal_log.hpp"
#include "transport/nosignal_io.hpp"

#include <openssl/err.h>
#include <openssl/ssl.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstring>
#include <exception>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include <sys/socket.h>

namespace microtel::transport
{

namespace
{

/// ALPN `http/1.1`, in wire format.
constexpr std::array<std::uint8_t, 9> kAlpnHttp11{8, 'h', 't', 't', 'p', '/', '1', '.', '1'};
constexpr std::string_view kHttp11{"http/1.1"};

constexpr auto kPollInterval = std::chrono::milliseconds(50);
constexpr auto kDestructorCloseTimeout = std::chrono::milliseconds(2000);
constexpr std::size_t kReadChunkBytes = std::size_t{16} * 1024;
constexpr std::size_t kHeadReserveBytes = 256;
constexpr std::size_t kCountBufferBytes = 48;

/// @brief An already-resolved handle for a request the transport would not
///        take. Handle id 0, which `Cancel` ignores.
[[nodiscard]] internal::RequestHandle RefuseRequest(microtel::Error error, bool busy) noexcept
{
    std::promise<internal::TransportResult> p;
    internal::TransportResult result;
    result.error = std::move(error);
    result.transport_busy = busy;
    p.set_value(std::move(result));
    return internal::RequestHandle{0, p.get_future()};
}

[[nodiscard]] microtel::Error MakeError(microtel::Error::Kind kind, std::string message)
{
    return microtel::Error{.kind = kind, .message = std::move(message)};
}

[[nodiscard]] bool EqualsIgnoreCase(std::string_view a, std::string_view b) noexcept
{
    return std::ranges::equal(
        a,
        b,
        [](char x, char y)
        {
            const auto lower = [](char c)
            { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + ('a' - 'A')) : c; };
            return lower(x) == lower(y);
        });
}

[[nodiscard]] std::string_view FindField(const std::vector<internal::HeaderField>& headers,
                                         std::string_view name) noexcept
{
    const auto it = std::ranges::find(headers, name, &internal::HeaderField::name);
    return it == headers.end() ? std::string_view{} : std::string_view{it->value};
}

/// @brief The request as it goes on the wire: head, then the payload, copied.
///
/// `POST <:path> HTTP/1.1`, `Host` from `:authority`, then every other field
/// except the pseudo-headers and any `content-length`, then our own
/// `Content-Length` (ICP 0042 §3). The codecs have already rejected CR, LF and
/// NUL in every field (`wire::ValidateRequestHeaders`), so nothing here can
/// end a line early.
[[nodiscard]] std::vector<std::byte> SerializeRequest(const internal::RequestSpec& spec)
{
    const std::string_view method = FindField(spec.headers, ":method");
    const std::string_view path = FindField(spec.headers, ":path");

    std::string head;
    head.reserve(kHeadReserveBytes);
    head.append(method.empty() ? std::string_view{"POST"} : method);
    head.append(" ");
    head.append(path.empty() ? std::string_view{"/"} : path);
    head.append(" HTTP/1.1\r\nHost: ");
    head.append(FindField(spec.headers, ":authority"));
    head.append("\r\n");
    for (const auto& field : spec.headers)
    {
        if (field.name.starts_with(':') || EqualsIgnoreCase(field.name, "content-length"))
        {
            continue;
        }
        head.append(field.name).append(": ").append(field.value).append("\r\n");
    }
    head.append("Content-Length: ").append(std::to_string(spec.payload.size())).append("\r\n\r\n");

    std::vector<std::byte> wire(head.size() + spec.payload.size());
    std::memcpy(wire.data(), head.data(), head.size());
    if (!spec.payload.empty())
    {
        std::memcpy(wire.data() + head.size(), spec.payload.data(), spec.payload.size());
    }
    return wire;
}

/// Log @p prefix followed by @p detail. Building the line allocates; if it
/// cannot be built, the prefix is logged alone rather than terminate a
/// `noexcept` frame.
void Log(microtel::LogLevel level, std::string_view prefix, std::string_view detail = {}) noexcept
{
    try
    {
        std::string line{prefix};
        line.append(detail);
        internal::LogImpl(level, line);
    }
    catch (const std::exception&)
    {
        internal::LogImpl(level, prefix);
    }
}

/// A count, or two, as text in a fixed buffer.
class CountText
{
public:
    explicit CountText(std::uint64_t a) noexcept
    {
        Set(std::snprintf(m_buf.data(), m_buf.size(), "%llu", static_cast<unsigned long long>(a)));
    }
    CountText(std::uint64_t a, std::uint64_t b) noexcept
    {
        Set(std::snprintf(m_buf.data(),
                          m_buf.size(),
                          "%llu / %llu",
                          static_cast<unsigned long long>(a),
                          static_cast<unsigned long long>(b)));
    }
    [[nodiscard]] std::string_view View() const noexcept
    {
        return std::string_view{m_buf.data(), m_len};
    }

private:
    void Set(int written) noexcept
    {
        m_len = written > 0 ? std::min(static_cast<std::size_t>(written), m_buf.size() - 1) : 0;
    }

    std::array<char, kCountBufferBytes> m_buf{};
    std::size_t m_len = 0;
};

[[nodiscard]] bool SocketHasError(int fd) noexcept
{
    int err = 0;
    socklen_t len = sizeof(err);
    return ::getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) != 0 || err != 0;
}

}  // namespace

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

Http1Transport::Http1Transport(std::unique_ptr<internal::IReactor> reactor) noexcept
    : m_reactor(std::move(reactor))
{
}

// static
microtel::Expected<std::unique_ptr<Http1Transport>, microtel::Error> Http1Transport::Create(
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
        auto t = std::unique_ptr<Http1Transport>(new Http1Transport(std::move(reactor)));
        t->m_io_thread = std::thread(&Http1Transport::IoThreadLoop, t.get());
        return t;
    }
    // As in Http2Transport::Create: std::thread throws std::system_error when
    // no thread can be spawned.
    catch (const std::exception&)
    {
        return microtel::Unexpected<microtel::Error>{microtel::Error{
            .kind = microtel::Error::Kind::InternalFailure, .message = "OOM or EAGAIN"}};
    }
}

Http1Transport::~Http1Transport() noexcept
{
    (void)Close(kDestructorCloseTimeout);
}

microtel::ConnectionState Http1Transport::GetState() const noexcept
{
    return m_state.load(std::memory_order_acquire);
}

Http1Transport::Stats Http1Transport::GetStats() const noexcept
{
    return Stats{.background_reconnects = m_background_reconnects.load(std::memory_order_relaxed),
                 .stale_retries = m_stale_retries.load(std::memory_order_relaxed)};
}

// ---------------------------------------------------------------------------
// Connection setup
// ---------------------------------------------------------------------------

// static
microtel::Expected<Http1Transport::Connection, microtel::Error> Http1Transport::OpenConnection(
    const internal::ConnectOptions& opts, const EndpointInfo& endpoint)
{
    auto fd = TcpConnect(endpoint.host, endpoint.port, opts.connect_timeout);
    if (!fd)
    {
        return microtel::Unexpected<microtel::Error>{fd.error()};
    }
    Connection conn;
    conn.fd = std::move(*fd);
    if (!endpoint.use_tls)
    {
        return conn;
    }

    auto tls = TlsConnect(conn.fd.Get(), opts, endpoint.host, kAlpnHttp11);
    if (!tls)
    {
        return microtel::Unexpected<microtel::Error>{tls.error()};
    }
    // A receiver that ignores ALPN selects nothing, which is how most
    // HTTP/1.1-only servers answer; anything but http/1.1 or nothing is a
    // protocol this transport cannot speak.
    const std::string_view alpn = SelectedAlpn(tls->second.Get());
    if (!alpn.empty() && alpn != kHttp11)
    {
        return microtel::Unexpected<microtel::Error>{
            MakeError(microtel::Error::Kind::Protocol,
                      "TLS peer selected ALPN " + std::string{alpn} + ", not http/1.1")};
    }
    // HTTP/1.1 servers routinely close without a TLS close_notify. Read that
    // as an ordinary close, so a body delimited by the close can complete.
#ifdef SSL_OP_IGNORE_UNEXPECTED_EOF
    ::SSL_set_options(tls->second.Get(), SSL_OP_IGNORE_UNEXPECTED_EOF);
#endif
    conn.ssl_ctx = std::move(tls->first);
    conn.ssl = std::move(tls->second);
    return conn;
}

bool Http1Transport::RegisterConnection() noexcept
{
    try
    {
        return m_reactor
            ->Register(m_conn.fd.Get(),
                       internal::EventMask::Read | internal::EventMask::Error,
                       [this](int fd, internal::EventMask events) { OnIoEvent(fd, events); })
            .has_value();
    }
    catch (const std::exception&)
    {
        return false;
    }
}

void Http1Transport::CloseConnection() noexcept
{
    if (m_conn.fd.IsValid())
    {
        m_reactor->Unregister(m_conn.fd.Get());
    }
    // The TLS session first: its BIO reads and writes the socket.
    m_conn.ssl.Reset();
    m_conn.ssl_ctx.Reset();
    m_conn.fd.Close();
    m_conn.reused = false;
    m_parser.Reset();
}

microtel::Expected<void, microtel::Error> Http1Transport::Connect(
    const internal::ConnectOptions& opts)
{
    microtel::ConnectionState prior = microtel::ConnectionState::Disconnected;
    if (!ClaimConnectSlot(m_state, prior))
    {
        const auto* const msg = (prior == microtel::ConnectionState::Closed) ? "transport is closed"
                                                                             : "already connecting";
        return microtel::Unexpected<microtel::Error>{
            {.kind = microtel::Error::Kind::Network, .message = msg}};
    }

    auto endpoint = ParseEndpoint(opts.endpoint, opts.insecure);
    if (!endpoint)
    {
        m_state.store(prior, std::memory_order_release);
        return microtel::Unexpected<microtel::Error>{endpoint.error()};
    }
    auto conn = OpenConnection(opts, *endpoint);
    if (!conn)
    {
        m_state.store(prior, std::memory_order_release);
        return microtel::Unexpected<microtel::Error>{conn.error()};
    }

    m_opts = opts;
    m_endpoint = std::move(*endpoint);
    m_max_pending_requests.store(opts.max_pending_requests, std::memory_order_relaxed);
    m_parser = Http1ResponseParser{Http1ResponseLimits{.max_body_bytes = opts.max_response_bytes,
                                                       .max_header_bytes = opts.max_trailer_bytes}};
    m_conn = std::move(*conn);
    if (!RegisterConnection())
    {
        CloseConnection();
        m_state.store(prior, std::memory_order_release);
        return microtel::Unexpected<microtel::Error>{
            {.kind = microtel::Error::Kind::InternalFailure, .message = "reactor register failed"}};
    }

    m_state.store(microtel::ConnectionState::Connected, std::memory_order_release);
    return {};
}

microtel::Status Http1Transport::Close(std::chrono::milliseconds timeout) noexcept
{
    const auto prev =
        m_state.exchange(microtel::ConnectionState::Closed, std::memory_order_acq_rel);
    if (prev == microtel::ConnectionState::Closed)
    {
        return microtel::Status::AlreadyShutDown;
    }

    m_stop.store(true, std::memory_order_release);
    m_reactor->Wake();
    const bool exited_in_time = [&]
    {
        std::unique_lock lk{m_io_done_mu};
        return m_io_done_cv.wait_for(lk, timeout, [this] { return m_io_done; });
    }();
    // Joined either way, as in Http2Transport::Close: the loop touches `this`.
    if (m_io_thread.joinable())
    {
        m_io_thread.join();
    }

    FailAll(
        microtel::Error{.kind = microtel::Error::Kind::Cancelled, .message = "transport closed"});
    CloseConnection();
    return exited_in_time ? microtel::Status::Completed : microtel::Status::TimedOut;
}

// ---------------------------------------------------------------------------
// Request submission
// ---------------------------------------------------------------------------

internal::RequestHandle Http1Transport::Send(internal::RequestSpec spec) noexcept
{
    if (m_state.load(std::memory_order_acquire) != microtel::ConnectionState::Connected)
    {
        return RefuseRequest(
            microtel::Error{.kind = microtel::Error::Kind::Network, .message = "not connected"},
            false);
    }

    const std::uint64_t id = m_next_id.fetch_add(1, std::memory_order_relaxed);
    std::promise<internal::TransportResult> promise;
    auto future = promise.get_future();
    const auto deadline = std::chrono::steady_clock::now() + spec.deadline;
    {
        const std::scoped_lock lk{m_mu};
        if (m_queue.size() >= m_max_pending_requests.load(std::memory_order_relaxed))
        {
            return RefuseRequest(
                microtel::Error{.kind = microtel::Error::Kind::ResourceExhausted,
                                .message = "transport request queue full (max_pending_requests)"},
                true);
        }
        m_queue.push_back(PendingRequest{.id = id,
                                         .spec = std::move(spec),
                                         .promise = std::move(promise),
                                         .deadline = deadline});
    }
    m_reactor->Wake();
    return internal::RequestHandle{id, std::move(future)};
}

// Synchronous with respect to the payload: a queued request still borrows it,
// and is removed here; the request in flight has already copied it into its
// wire buffer, under the same lock, so marking it is enough.
void Http1Transport::Cancel(const internal::RequestHandle& handle) noexcept
{
    if (handle.Id() == 0)
    {
        return;
    }
    std::optional<PendingRequest> queued;
    {
        const std::scoped_lock lk{m_mu};
        const auto it = std::ranges::find(m_queue, handle.Id(), &PendingRequest::id);
        if (it != m_queue.end())
        {
            queued.emplace(std::move(*it));
            m_queue.erase(it);
        }
        else if (m_inflight_id == handle.Id())
        {
            m_inflight_cancelled = true;
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
    m_reactor->Wake();
}

// ---------------------------------------------------------------------------
// I/O thread
// ---------------------------------------------------------------------------

void Http1Transport::IoThreadLoop() noexcept
{
    while (!m_stop.load(std::memory_order_acquire))
    {
        m_reactor->WaitAndDispatch(std::chrono::steady_clock::now() + kPollInterval);
        Service();
    }
    {
        const std::scoped_lock lk{m_io_done_mu};
        m_io_done = true;
    }
    m_io_done_cv.notify_all();
}

void Http1Transport::Service() noexcept
{
    // The queue has its own lock, so expiry needs no connection.
    ExpireQueued();
    if (m_state.load(std::memory_order_acquire) != microtel::ConnectionState::Connected)
    {
        return;
    }
    CheckInFlight();
    StartNext();
}

void Http1Transport::ExpireQueued() noexcept
{
    const auto now = std::chrono::steady_clock::now();
    std::vector<PendingRequest> expired;
    {
        const std::scoped_lock lk{m_mu};
        const auto first = std::ranges::stable_partition(
            m_queue, [now](const PendingRequest& r) { return r.deadline > now; });
        std::ranges::move(first, std::back_inserter(expired));
        m_queue.erase(first.begin(), first.end());
    }
    for (auto& request : expired)
    {
        internal::TransportResult result;
        result.error = microtel::Error{.kind = microtel::Error::Kind::Cancelled,
                                       .message = "request deadline exceeded while queued"};
        request.promise.set_value(std::move(result));
    }
}

void Http1Transport::CheckInFlight() noexcept
{
    if (!m_inflight.active)
    {
        return;
    }
    bool cancelled = false;
    {
        const std::scoped_lock lk{m_mu};
        cancelled = m_inflight_cancelled;
    }
    // HTTP/1.1 has no stream reset: abandoning a request costs the connection.
    if (cancelled)
    {
        FailInFlight({.kind = microtel::Error::Kind::Cancelled, .message = "request cancelled"});
        CloseConnection();
    }
    else if (std::chrono::steady_clock::now() >= m_inflight.deadline)
    {
        FailInFlight(
            {.kind = microtel::Error::Kind::Cancelled, .message = "request deadline exceeded"});
        CloseConnection();
    }
}

void Http1Transport::StartNext() noexcept
{
    if (m_inflight.active)
    {
        return;
    }
    PendingRequest next;
    std::vector<std::byte> wire;
    bool built = false;
    {
        const std::scoped_lock lk{m_mu};
        if (m_queue.empty())
        {
            return;
        }
        next = std::move(m_queue.front());
        m_queue.pop_front();
        // Copied under the lock, so a Cancel that misses the queue knows the
        // payload is no longer read.
        try
        {
            wire = SerializeRequest(next.spec);
            built = true;
            m_inflight_id = next.id;
            m_inflight_cancelled = false;
        }
        catch (const std::exception&)
        {
            built = false;
        }
    }
    if (!built)
    {
        internal::TransportResult result;
        result.error = microtel::Error{.kind = microtel::Error::Kind::InternalFailure,
                                       .message = "out of memory"};
        next.promise.set_value(std::move(result));
        return;
    }

    m_inflight = InFlight{.active = true,
                          .id = next.id,
                          .promise = std::move(next.promise),
                          .deadline = next.deadline,
                          .wire = std::move(wire)};
    if (!m_conn.fd.IsValid() && !Reconnect())
    {
        return;
    }
    m_inflight.on_reused_connection = m_conn.reused;
    TryWrite();
}

microtel::Expected<void, microtel::Error> Http1Transport::OpenAndRegister()
{
    auto conn = OpenConnection(m_opts, m_endpoint);
    if (!conn)
    {
        return microtel::Unexpected<microtel::Error>{
            MakeError(conn.error().kind, "HTTP/1.1 reconnect failed: " + conn.error().message)};
    }
    m_conn = std::move(*conn);
    if (!RegisterConnection())
    {
        CloseConnection();
        return microtel::Unexpected<microtel::Error>{
            MakeError(microtel::Error::Kind::InternalFailure, "reactor register failed")};
    }
    return {};
}

bool Http1Transport::Reconnect() noexcept
{
    try
    {
        auto opened = OpenAndRegister();
        if (opened)
        {
            CountEvent(EventLogLimiter::Kind::BackgroundReconnect);
            return true;
        }
        GiveUpConnection(opened.error());
    }
    catch (const std::exception&)
    {
        CloseConnection();
        // Fits the SSO buffer, so building it cannot throw again.
        GiveUpConnection(
            {.kind = microtel::Error::Kind::InternalFailure, .message = "out of memory"});
    }
    return false;
}

// Everything waiting would wait for a connection that is not coming. Fail it,
// and hand recovery to the codec's next lazy Connect.
//
// In this order: the connection is released before `Reconnecting` lets a
// caller's Connect replace it, and the state is published before the failures
// are, so a codec woken by one sees `Reconnecting` and reconnects instead of
// queueing behind a connection that is gone.
void Http1Transport::GiveUpConnection(const microtel::Error& error) noexcept
{
    Log(microtel::LogLevel::Debug, "HTTP/1.1 transport: ", error.message);
    CloseConnection();
    auto expected = microtel::ConnectionState::Connected;
    m_state.compare_exchange_strong(expected,
                                    microtel::ConnectionState::Reconnecting,
                                    std::memory_order_acq_rel,
                                    std::memory_order_acquire);
    FailAll(error);
}

void Http1Transport::OnIoEvent(int fd, internal::EventMask events) noexcept
{
    if (m_state.load(std::memory_order_acquire) != microtel::ConnectionState::Connected ||
        fd != m_conn.fd.Get())
    {
        return;
    }
    const bool error = internal::HasEvent(events, internal::EventMask::Error);
    if (error || internal::HasEvent(events, internal::EventMask::Read))
    {
        OnReadable();
    }
    if (m_state.load(std::memory_order_acquire) != microtel::ConnectionState::Connected ||
        !m_conn.fd.IsValid())
    {
        return;
    }
    // An error with nothing to read: the next write would find it, but an idle
    // connection writes nothing, and the level-triggered error would spin.
    if (error && SocketHasError(m_conn.fd.Get()))
    {
        OnConnectionLost("connection lost");
        return;
    }
    if (internal::HasEvent(events, internal::EventMask::Write))
    {
        TryWrite();
    }
}

void Http1Transport::OnReadable() noexcept
{
    std::array<std::byte, kReadChunkBytes> buf{};
    while (m_conn.fd.IsValid())
    {
        std::size_t n = 0;
        switch (ReadSome(buf, n))
        {
            case IoStatus::Progress:
                OnResponseBytes(std::span<const std::byte>{buf.data(), n});
                break;
            case IoStatus::WouldBlock:
                return;
            case IoStatus::Closed:
                OnResponseEnd();
                return;
            case IoStatus::Failed:
                OnConnectionLost("connection lost");
                return;
        }
    }
}

void Http1Transport::OnResponseBytes(std::span<const std::byte> data) noexcept
{
    if (!m_inflight.active)
    {
        // Nothing was asked: an idle connection has nothing to say.
        Log(microtel::LogLevel::Debug,
            "HTTP/1.1 transport: unsolicited bytes on an idle connection; closing it");
        CloseConnection();
        return;
    }
    const std::size_t consumed = m_parser.Feed(data);
    OnParserState(consumed == data.size());
}

void Http1Transport::OnParserState(bool all_consumed) noexcept
{
    switch (m_parser.GetState())
    {
        case Http1ResponseParser::State::NeedMore:
            return;
        case Http1ResponseParser::State::Failed:
            FailMalformedResponse();
            return;
        case Http1ResponseParser::State::Complete:
            CompleteResponse(all_consumed);
            return;
    }
}

void Http1Transport::FailMalformedResponse() noexcept
{
    internal::TransportResult result;
    result.response_too_large = m_parser.IsTooLarge();
    try
    {
        result.error =
            MakeError(microtel::Error::Kind::Malformed,
                      "malformed HTTP/1.1 response: " +
                          std::string{Http1ResponseParser::Describe(m_parser.GetFault())});
    }
    catch (const std::exception&)
    {
        result.error =
            microtel::Error{.kind = microtel::Error::Kind::Malformed, .message = "malformed"};
    }
    Finish(std::move(result));
    CloseConnection();
}

void Http1Transport::CompleteResponse(bool all_consumed) noexcept
{
    // Bytes after the response, or a response before the request was fully
    // written, leave the connection in no state to reuse.
    const bool keep_alive =
        m_parser.KeepAlive() && all_consumed && m_inflight.written == m_inflight.wire.size();
    internal::TransportResult result;
    result.success = true;
    result.response_headers = std::move(m_parser.Headers());
    result.response_body = std::move(m_parser.Body());
    Finish(std::move(result));
    m_parser.Reset();
    m_conn.reused = true;
    if (!keep_alive)
    {
        CloseConnection();
    }
}

void Http1Transport::OnResponseEnd() noexcept
{
    if (!m_inflight.active)
    {
        // The peer closed an idle keep-alive connection. The next request
        // opens a new one.
        CloseConnection();
        return;
    }
    if (!m_parser.HasStarted())
    {
        OnConnectionLost("connection closed before the response");
        return;
    }
    // Completes a body delimited by the close; anything else incomplete is a
    // truncation, which is a lost connection rather than a bad response.
    m_parser.FeedEof();
    if (m_parser.GetFault() == Http1ResponseParser::Fault::Truncated)
    {
        FailInFlight({.kind = microtel::Error::Kind::Network,
                      .message = "connection closed before the response was complete"});
        CloseConnection();
        return;
    }
    OnParserState(true);
    CloseConnection();
}

// With response bytes in hand the request fails. With none, a request on a
// reused connection is retried once, on a new one: the peer's idle timeout
// raced the write (ICP 0042 §3).
void Http1Transport::OnConnectionLost(const char* message) noexcept
{
    if (!m_inflight.active)
    {
        CloseConnection();
        return;
    }
    const bool stale =
        m_inflight.on_reused_connection && !m_inflight.retried && !m_parser.HasStarted();
    if (stale)
    {
        RetryOnNewConnection();
        return;
    }
    FailInFlight({.kind = microtel::Error::Kind::Network, .message = message});
    CloseConnection();
}

void Http1Transport::RetryOnNewConnection() noexcept
{
    CloseConnection();
    m_inflight.retried = true;
    m_inflight.written = 0;
    m_inflight.on_reused_connection = false;
    CountEvent(EventLogLimiter::Kind::StaleRetry);
    // The write starts at the new socket's first Write event, not from here:
    // this can run inside TryWrite's own failure path.
    if (Reconnect())
    {
        WatchWrites(true);
    }
}

void Http1Transport::TryWrite() noexcept
{
    while (m_inflight.active && m_conn.fd.IsValid() && m_inflight.written < m_inflight.wire.size())
    {
        std::size_t n = 0;
        const auto rest = std::span<const std::byte>{m_inflight.wire}.subspan(m_inflight.written);
        switch (WriteSome(rest, n))
        {
            case IoStatus::Progress:
                m_inflight.written += n;
                break;
            case IoStatus::WouldBlock:
                WatchWrites(true);
                return;
            case IoStatus::Closed:
            case IoStatus::Failed:
                OnConnectionLost("connection lost while sending the request");
                return;
        }
    }
    WatchWrites(false);
}

void Http1Transport::WatchWrites(bool want_write) noexcept
{
    if (!m_conn.fd.IsValid())
    {
        return;
    }
    const auto base = internal::EventMask::Read | internal::EventMask::Error;
    m_reactor->Modify(m_conn.fd.Get(), want_write ? (base | internal::EventMask::Write) : base);
}

Http1Transport::IoStatus Http1Transport::WriteSome(std::span<const std::byte> data,
                                                   std::size_t& n) const noexcept
{
    const std::size_t len = std::min<std::size_t>(data.size(), INT_MAX);
    if (m_conn.ssl.IsValid())
    {
        ::ERR_clear_error();
        const int r = ::SSL_write(m_conn.ssl.Get(), data.data(), static_cast<int>(len));
        if (r > 0)
        {
            n = static_cast<std::size_t>(r);
            return IoStatus::Progress;
        }
        const int err = ::SSL_get_error(m_conn.ssl.Get(), r);
        return (err == SSL_ERROR_WANT_WRITE || err == SSL_ERROR_WANT_READ) ? IoStatus::WouldBlock
                                                                           : IoStatus::Failed;
    }
    const ssize_t r = SendNoSignal(m_conn.fd.Get(), data.data(), len);
    if (r >= 0)
    {
        n = static_cast<std::size_t>(r);
        return IoStatus::Progress;
    }
    return (errno == EAGAIN || errno == EWOULDBLOCK) ? IoStatus::WouldBlock : IoStatus::Failed;
}

Http1Transport::IoStatus Http1Transport::ReadSome(std::span<std::byte> buf,
                                                  std::size_t& n) const noexcept
{
    const std::size_t len = std::min<std::size_t>(buf.size(), INT_MAX);
    if (m_conn.ssl.IsValid())
    {
        ::ERR_clear_error();
        const int r = ::SSL_read(m_conn.ssl.Get(), buf.data(), static_cast<int>(len));
        if (r > 0)
        {
            n = static_cast<std::size_t>(r);
            return IoStatus::Progress;
        }
        const int err = ::SSL_get_error(m_conn.ssl.Get(), r);
        if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE)
        {
            return IoStatus::WouldBlock;
        }
        return (err == SSL_ERROR_ZERO_RETURN) ? IoStatus::Closed : IoStatus::Failed;
    }
    ssize_t r = ::recv(m_conn.fd.Get(), buf.data(), len, 0);
    while (r < 0 && errno == EINTR)
    {
        r = ::recv(m_conn.fd.Get(), buf.data(), len, 0);
    }
    if (r > 0)
    {
        n = static_cast<std::size_t>(r);
        return IoStatus::Progress;
    }
    if (r == 0)
    {
        return IoStatus::Closed;
    }
    return (errno == EAGAIN || errno == EWOULDBLOCK) ? IoStatus::WouldBlock : IoStatus::Failed;
}

// ---------------------------------------------------------------------------
// Completion
// ---------------------------------------------------------------------------

void Http1Transport::Finish(internal::TransportResult result) noexcept
{
    m_inflight.promise.set_value(std::move(result));
    m_inflight = InFlight{};
    const std::scoped_lock lk{m_mu};
    m_inflight_id = 0;
    m_inflight_cancelled = false;
}

void Http1Transport::FailInFlight(microtel::Error error) noexcept
{
    internal::TransportResult result;
    result.error = std::move(error);
    Finish(std::move(result));
}

void Http1Transport::FailAll(const microtel::Error& error) noexcept
{
    if (m_inflight.active)
    {
        FailInFlight(error);
    }
    std::deque<PendingRequest> queued;
    {
        const std::scoped_lock lk{m_mu};
        queued.swap(m_queue);
    }
    for (auto& request : queued)
    {
        internal::TransportResult result;
        result.error = error;
        request.promise.set_value(std::move(result));
    }
}

void Http1Transport::CountEvent(EventLogLimiter::Kind kind) noexcept
{
    const bool reconnect = kind == EventLogLimiter::Kind::BackgroundReconnect;
    auto& counter = reconnect ? m_background_reconnects : m_stale_retries;
    const std::uint64_t count = counter.fetch_add(1, std::memory_order_relaxed) + 1;
    switch (m_event_log.OnEvent(kind, std::chrono::steady_clock::now()))
    {
        case EventLogLimiter::Emit::First:
            Log(microtel::LogLevel::Info,
                reconnect ? "HTTP/1.1 transport: opened a new connection after the last one "
                            "closed; further reconnects are logged at Debug and summarised at "
                            "most once a minute"
                          : "HTTP/1.1 transport: a reused connection closed before responding, "
                            "so the request was sent again on a new connection and the "
                            "receiver may see it twice; further retries are logged at Debug "
                            "and summarised at most once a minute");
            break;
        case EventLogLimiter::Emit::Summary:
        {
            const auto stats = GetStats();
            Log(microtel::LogLevel::Info,
                "HTTP/1.1 transport: background reconnects / stale-connection retries so far: ",
                CountText{stats.background_reconnects, stats.stale_retries}.View());
            break;
        }
        case EventLogLimiter::Emit::Debug:
            Log(microtel::LogLevel::Debug,
                reconnect ? "HTTP/1.1 transport: background reconnect "
                          : "HTTP/1.1 transport: stale-connection retry ",
                CountText{count}.View());
            break;
    }
}

}  // namespace microtel::transport
