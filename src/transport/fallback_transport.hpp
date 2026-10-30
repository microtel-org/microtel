// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "microtel/error.hpp"
#include "microtel/expected.hpp"
#include "microtel/internal/reactor.hpp"
#include "microtel/internal/transport.hpp"
#include "microtel/provider.hpp"
#include "microtel/status.hpp"

#include "transport/http1_transport.hpp"
#include "transport/http2_transport.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>

namespace microtel::transport
{

/// @brief The OTLP/HTTP transport: HTTP/2, falling back to HTTP/1.1 for a
///        receiver that speaks only that (ICP 0042 §1-2).
///
/// Built for `Protocol::Http` only; gRPC gets a bare `Http2Transport`, so it
/// cannot fall back by construction. It forwards to an `Http2Transport` until
/// a `Connect` finds the peer HTTP/1.1-only: a plaintext peer that answers the
/// HTTP/2 preface with an HTTP/1.1 response, or a TLS peer that selects ALPN
/// `http/1.1` or nothing. It then switches to an `Http1Transport` and
/// connects that, inside the same `Connect`, so the caller sees it succeed.
/// The decision lasts for the transport's lifetime.
///
/// **At most one I/O thread at any time.** `SwitchToHttp1` closes the
/// `Http2Transport`, which joins its thread, destroys it, and only then
/// creates the `Http1Transport`, whose factory starts the new thread.
///
/// **During the switch** `Send` fails at once, retryably, and `GetState`
/// reports `Connecting`. `Cancel` still reaches the transport being retired,
/// so the payload contract of #508 holds across the switch.
///
/// **Handles** carry which transport issued them in their top byte, so a
/// `Cancel` for an HTTP/2 request that arrives after the switch cannot cancel
/// an unrelated HTTP/1.1 request with the same inner id.
///
/// @threadsafety As `ITransport`. Concurrent `Connect` calls do not queue:
///               one connects and the others fail "already connecting", as
///               on `Http2Transport`.
class FallbackTransport final : public internal::ITransport
{
public:
    /// @brief Makes the reactor for each inner transport: one at `Create`,
    ///        one at the switch. Production passes `EpollReactor::Create`.
    using ReactorFactory =
        std::function<microtel::Expected<std::unique_ptr<internal::IReactor>, microtel::Error>()>;

    /// @brief Create the transport with its `Http2Transport`, which starts
    ///        the only I/O thread.
    [[nodiscard]] static microtel::Expected<std::unique_ptr<FallbackTransport>, microtel::Error>
    Create(ReactorFactory reactor_factory) noexcept;

    ~FallbackTransport() noexcept override;

    FallbackTransport(const FallbackTransport&) = delete;
    FallbackTransport& operator=(const FallbackTransport&) = delete;
    FallbackTransport(FallbackTransport&&) = delete;
    FallbackTransport& operator=(FallbackTransport&&) = delete;

    [[nodiscard]] microtel::Expected<void, microtel::Error> Connect(
        const internal::ConnectOptions& opts) override;
    [[nodiscard]] internal::RequestHandle Send(internal::RequestSpec spec) noexcept override;
    void Cancel(const internal::RequestHandle& handle) noexcept override;
    [[nodiscard]] microtel::ConnectionState GetState() const noexcept override;
    [[nodiscard]] microtel::Status Close(std::chrono::milliseconds timeout) noexcept override;

    /// @brief True once the transport has switched to HTTP/1.1. Thread-safe.
    [[nodiscard]] bool IsHttp1() const noexcept;

private:
    FallbackTransport(ReactorFactory reactor_factory,
                      std::unique_ptr<Http2Transport> http2) noexcept;

    [[nodiscard]] microtel::Expected<void, microtel::Error> SwitchToHttp1(
        const internal::ConnectOptions& opts);
    [[nodiscard]] microtel::Expected<void, microtel::Error> ConnectHttp1(
        const internal::ConnectOptions& opts);
    [[nodiscard]] internal::ITransport* ActiveLocked() const noexcept;

    ReactorFactory m_reactor_factory;
    /// Held for the whole of a `Connect`, by try-lock: it is what keeps
    /// `m_http2` and `m_http1` from being replaced under a `Connect`.
    std::mutex m_connect_mu;

    mutable std::mutex m_mu;
    std::unique_ptr<Http2Transport> m_http2;
    std::unique_ptr<Http1Transport> m_http1;
    /// The `Http2Transport` being closed by the switch, for `Cancel` only.
    Http2Transport* m_retiring = nullptr;
    bool m_switching = false;
    bool m_closed = false;
    /// Set once, when the switch has been logged.
    std::atomic<bool> m_fell_back{false};
};

}  // namespace microtel::transport
