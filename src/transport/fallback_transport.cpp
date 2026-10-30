// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#include "fallback_transport.hpp"

#include "microtel/log_sink.hpp"
#include "microtel/protocol.hpp"

#include "common/internal_log.hpp"

#include <exception>
#include <future>
#include <utility>

namespace microtel::transport
{

namespace
{

/// How long the switch waits for the HTTP/2 I/O thread before it is told it
/// timed out. The join happens either way (`Http2Transport::Close`).
constexpr auto kSwitchCloseTimeout = std::chrono::milliseconds(2000);
constexpr auto kDestructorCloseTimeout = std::chrono::milliseconds(2000);

/// Which inner transport issued a handle, in the id's top byte.
constexpr unsigned kGenerationShift = 56;
constexpr std::uint64_t kInnerIdMask = (std::uint64_t{1} << kGenerationShift) - 1;
constexpr std::uint64_t kHttp2Generation = 1;
constexpr std::uint64_t kHttp1Generation = 2;

constexpr const char* kFallbackMessage =
    "endpoint speaks HTTP/1.1 only; OTLP/HTTP continues over HTTP/1.1, one request at a time. "
    "For throughput use https:// (ALPN h2) or OTLP/gRPC.";

[[nodiscard]] internal::RequestHandle Tag(internal::RequestHandle handle,
                                          std::uint64_t generation) noexcept
{
    const std::uint64_t id = handle.Id();
    if (id == 0)
    {
        return handle;  // refused: nothing to cancel
    }
    return internal::RequestHandle{(generation << kGenerationShift) | (id & kInnerIdMask),
                                   std::move(handle.Future())};
}

[[nodiscard]] internal::RequestHandle Refuse(const char* message) noexcept
{
    std::promise<internal::TransportResult> p;
    internal::TransportResult result;
    result.error = microtel::Error{.kind = microtel::Error::Kind::Network, .message = message};
    p.set_value(std::move(result));
    return internal::RequestHandle{0, p.get_future()};
}

[[nodiscard]] microtel::Error ClosedError()
{
    return microtel::Error{.kind = microtel::Error::Kind::Network,
                           .message = "transport is closed"};
}

}  // namespace

FallbackTransport::FallbackTransport(ReactorFactory reactor_factory,
                                     std::unique_ptr<Http2Transport> http2) noexcept
    : m_reactor_factory(std::move(reactor_factory)), m_http2(std::move(http2))
{
}

// static
microtel::Expected<std::unique_ptr<FallbackTransport>, microtel::Error> FallbackTransport::Create(
    ReactorFactory reactor_factory) noexcept
{
    try
    {
        if (!reactor_factory)
        {
            return microtel::Unexpected<microtel::Error>{microtel::Error{
                .kind = microtel::Error::Kind::InternalFailure, .message = "no reactor factory"}};
        }
        auto reactor = reactor_factory();
        if (!reactor)
        {
            return microtel::Unexpected<microtel::Error>{reactor.error()};
        }
        auto http2 = Http2Transport::Create(std::move(*reactor));
        if (!http2)
        {
            return microtel::Unexpected<microtel::Error>{http2.error()};
        }
        // Private constructor; make_unique can't reach it.
        // NOLINTNEXTLINE(cppcoreguidelines-owning-memory)
        return std::unique_ptr<FallbackTransport>(
            new FallbackTransport(std::move(reactor_factory), std::move(*http2)));
    }
    catch (const std::exception&)
    {
        return microtel::Unexpected<microtel::Error>{microtel::Error{
            .kind = microtel::Error::Kind::InternalFailure, .message = "OOM or EAGAIN"}};
    }
}

FallbackTransport::~FallbackTransport() noexcept
{
    (void)Close(kDestructorCloseTimeout);
}

internal::ITransport* FallbackTransport::ActiveLocked() const noexcept
{
    if (m_http1)
    {
        return m_http1.get();
    }
    return m_http2.get();
}

bool FallbackTransport::IsHttp1() const noexcept
{
    return m_fell_back.load(std::memory_order_acquire);
}

microtel::Expected<void, microtel::Error> FallbackTransport::Connect(
    const internal::ConnectOptions& opts)
{
    const std::unique_lock connect_lk{m_connect_mu, std::try_to_lock};
    if (!connect_lk.owns_lock())
    {
        return microtel::Unexpected<microtel::Error>{
            {.kind = microtel::Error::Kind::Network, .message = "already connecting"}};
    }
    // m_http2 and m_http1 change only under m_connect_mu, which this call
    // holds, so they can be used outside m_mu from here on.
    {
        const std::scoped_lock lk{m_mu};
        if (m_closed)
        {
            return microtel::Unexpected<microtel::Error>{ClosedError()};
        }
    }
    if (m_fell_back.load(std::memory_order_acquire))
    {
        return ConnectHttp1(opts);
    }

    auto connected = m_http2->Connect(opts);
    if (connected || opts.protocol != Protocol::Http || !m_http2->PeerSpeaksOnlyHttp1())
    {
        return connected;
    }
    return SwitchToHttp1(opts);
}

// ICP 0042 §2: close, destroy, then create, strictly in that order, so the
// HTTP/2 I/O thread has been joined before the HTTP/1.1 one starts.
microtel::Expected<void, microtel::Error> FallbackTransport::SwitchToHttp1(
    const internal::ConnectOptions& opts)
{
    std::unique_ptr<Http2Transport> retiring;
    {
        const std::scoped_lock lk{m_mu};
        if (m_closed)
        {
            return microtel::Unexpected<microtel::Error>{ClosedError()};
        }
        m_switching = true;
        retiring = std::move(m_http2);
        m_retiring = retiring.get();
    }
    // 1. Joins the HTTP/2 I/O thread, whatever the timeout, and fails what it
    //    held with the retryable "transport closed".
    (void)retiring->Close(kSwitchCloseTimeout);
    {
        const std::scoped_lock lk{m_mu};
        m_retiring = nullptr;
    }
    // 2. No Cancel can reach it now.
    retiring.reset();

    internal::LogImpl(microtel::LogLevel::Info, kFallbackMessage);
    m_fell_back.store(true, std::memory_order_release);
    return ConnectHttp1(opts);
}

// 3. The HTTP/1.1 transport and its thread. Also the retry after a switch
//    whose Http1Transport could not be created.
microtel::Expected<void, microtel::Error> FallbackTransport::ConnectHttp1(
    const internal::ConnectOptions& opts)
{
    bool need_transport = false;
    {
        const std::scoped_lock lk{m_mu};
        need_transport = !m_http1;
    }
    if (need_transport)
    {
        auto reactor = m_reactor_factory();
        auto http1 = reactor ? Http1Transport::Create(std::move(*reactor))
                             : microtel::Expected<std::unique_ptr<Http1Transport>, microtel::Error>{
                                   microtel::Unexpected<microtel::Error>{reactor.error()}};
        const std::scoped_lock lk{m_mu};
        m_switching = false;
        if (!http1)
        {
            return microtel::Unexpected<microtel::Error>{http1.error()};
        }
        if (m_closed)
        {
            return microtel::Unexpected<microtel::Error>{ClosedError()};
        }
        m_http1 = std::move(*http1);
    }
    return m_http1->Connect(opts);
}

internal::RequestHandle FallbackTransport::Send(internal::RequestSpec spec) noexcept
{
    const std::scoped_lock lk{m_mu};
    if (m_switching)
    {
        // Retryable, like "already connecting": the codec's next attempt
        // finds the HTTP/1.1 transport.
        return Refuse("switching to HTTP/1.1");
    }
    if (m_http1)
    {
        return Tag(m_http1->Send(std::move(spec)), kHttp1Generation);
    }
    if (m_http2)
    {
        return Tag(m_http2->Send(std::move(spec)), kHttp2Generation);
    }
    return Refuse("not connected");
}

void FallbackTransport::Cancel(const internal::RequestHandle& handle) noexcept
{
    const std::uint64_t generation = handle.Id() >> kGenerationShift;
    const internal::RequestHandle inner{handle.Id() & kInnerIdMask, {}};
    const std::scoped_lock lk{m_mu};
    if (generation == kHttp1Generation && m_http1)
    {
        m_http1->Cancel(inner);
        return;
    }
    if (generation != kHttp2Generation)
    {
        return;
    }
    if (m_http2)
    {
        m_http2->Cancel(inner);
    }
    else if (m_retiring != nullptr)
    {
        m_retiring->Cancel(inner);
    }
}

microtel::ConnectionState FallbackTransport::GetState() const noexcept
{
    const std::scoped_lock lk{m_mu};
    if (m_switching)
    {
        return microtel::ConnectionState::Connecting;
    }
    const auto* const active = ActiveLocked();
    if (active == nullptr)
    {
        return m_closed ? microtel::ConnectionState::Closed
                        : microtel::ConnectionState::Disconnected;
    }
    return active->GetState();
}

microtel::Status FallbackTransport::Close(std::chrono::milliseconds timeout) noexcept
{
    internal::ITransport* active = nullptr;
    {
        const std::scoped_lock lk{m_mu};
        if (m_closed)
        {
            return microtel::Status::AlreadyShutDown;
        }
        m_closed = true;
        active = ActiveLocked();
    }
    // Outside m_mu, so Send and GetState do not wait on the join. `active`
    // outlives this: the inner transports are destroyed only by the switch,
    // which stops at m_closed, and by the destructor, after this returns.
    return active == nullptr ? microtel::Status::Completed : active->Close(timeout);
}

}  // namespace microtel::transport
