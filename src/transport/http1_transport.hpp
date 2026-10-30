// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "microtel/error.hpp"
#include "microtel/expected.hpp"
#include "microtel/internal/reactor.hpp"
#include "microtel/internal/transport.hpp"
#include "microtel/provider.hpp"
#include "microtel/status.hpp"

#include "common/raii/ssl_ctx.hpp"
#include "common/raii/ssl_session.hpp"
#include "common/raii/unique_fd.hpp"
#include "transport/connection_setup.hpp"
#include "transport/event_log_limiter.hpp"
#include "transport/http1_response_parser.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <future>
#include <memory>
#include <mutex>
#include <span>
#include <thread>
#include <vector>

namespace microtel::transport
{

/// @brief HTTP/1.1 implementation of `ITransport`, for OTLP/HTTP receivers
///        that do not speak HTTP/2 (ICP 0042).
///
/// One keep-alive connection, plaintext or TLS (ALPN `http/1.1`), and one I/O
/// thread. Requests are **serial**: the I/O thread writes the next request
/// only once the previous response is complete. There is no pool and no
/// pipelining, so throughput needs h2 or gRPC.
///
/// **Reconnects.** A peer that ends keep-alive (`Connection: close`, an
/// HTTP/1.0 response, a close-delimited body, or an idle close) costs the
/// connection, not the transport: the I/O thread opens a new one for the next
/// request and the state stays `Connected`. Only a failed reconnect fails the
/// waiting requests and moves the state to `Reconnecting`, so the codec's next
/// lazy `Connect` takes over. A cancel or deadline on the request in flight
/// also closes the connection, because HTTP/1.1 has no stream reset.
///
/// **Stale-connection retry.** A request written on a reused connection that
/// closes before any response byte arrives is retried once on a new
/// connection: the usual race with a peer's idle timeout. The receiver may
/// see it twice, as OTLP retry semantics allow.
///
/// Both are counted (`GetStats`) and logged through `EventLogLimiter`. There
/// is no public counter for either yet (#515).
///
/// **The payload is copied** into the request's wire buffer when the I/O
/// thread takes the request, under the queue lock. So `Cancel` is
/// synchronous with respect to the payload: once it returns, the payload is
/// never read again (the contract `Http2Transport` gained in #508).
///
/// @threadsafety As `ITransport`: `Send`, `Cancel`, `GetState` and `Close` are
///               thread-safe; `Connect` is caller-thread. The connection is
///               touched by the caller thread only in `Connect` (state
///               `Connecting`) and after `Close` has joined the I/O thread,
///               and by the I/O thread only while the state is `Connected`.
class Http1Transport final : public internal::ITransport
{
public:
    /// @brief Counts of the transport's recovery events, for tests and logs.
    struct Stats
    {
        /// Connections the I/O thread opened after the first one.
        std::uint64_t background_reconnects = 0;
        /// Requests re-sent after a stale connection closed under them.
        std::uint64_t stale_retries = 0;
    };

    /// @brief Factory: creates the transport and starts its I/O thread.
    [[nodiscard]] static microtel::Expected<std::unique_ptr<Http1Transport>, microtel::Error>
    Create(std::unique_ptr<internal::IReactor> reactor) noexcept;

    ~Http1Transport() noexcept override;

    Http1Transport(const Http1Transport&) = delete;
    Http1Transport& operator=(const Http1Transport&) = delete;
    Http1Transport(Http1Transport&&) = delete;
    Http1Transport& operator=(Http1Transport&&) = delete;

    /// @brief DNS, TCP and, for `https://`, a TLS handshake offering ALPN
    ///        `http/1.1`.
    ///
    /// A TLS peer must select `http/1.1` or no protocol at all; anything else
    /// fails with `Error::Kind::Protocol`.
    [[nodiscard]] microtel::Expected<void, microtel::Error> Connect(
        const internal::ConnectOptions& opts) override;

    /// @brief Queue a request; the I/O thread sends it when the one before it
    ///        has completed.
    ///
    /// Refused unless `Connected`, and with `transport_busy` once
    /// `max_pending_requests` are queued. A request whose deadline passes
    /// while it is queued fails as a timeout.
    [[nodiscard]] internal::RequestHandle Send(internal::RequestSpec spec) noexcept override;

    /// @brief Abandon a request; synchronous with respect to its payload.
    ///
    /// A queued request is failed `Cancelled` before this returns. The request
    /// in flight is failed by the I/O thread, which closes the connection.
    void Cancel(const internal::RequestHandle& handle) noexcept override;

    [[nodiscard]] microtel::ConnectionState GetState() const noexcept override;

    /// @brief Stop the I/O thread, fail what is left, and close the
    ///        connection. Idempotent: a second call returns `AlreadyShutDown`.
    [[nodiscard]] microtel::Status Close(std::chrono::milliseconds timeout) noexcept override;

    /// @brief The recovery counts so far. Thread-safe.
    [[nodiscard]] Stats GetStats() const noexcept;

private:
    /// A request queued by `Send`.
    struct PendingRequest
    {
        std::uint64_t id = 0;
        internal::RequestSpec spec;
        std::promise<internal::TransportResult> promise;
        std::chrono::steady_clock::time_point deadline;
    };

    /// The request on the wire. I/O-thread-only.
    struct InFlight
    {
        /// False when no request is in flight; the other fields are then unused.
        bool active = false;
        std::uint64_t id = 0;
        std::promise<internal::TransportResult> promise;
        std::chrono::steady_clock::time_point deadline;
        /// The request head and body, owned: the payload was copied in.
        std::vector<std::byte> wire;
        std::size_t written = 0;
        /// Written on a connection that had already carried a response.
        bool on_reused_connection = false;
        /// Already re-sent once after a stale connection.
        bool retried = false;
    };

    /// One TCP connection, with its TLS session when the endpoint is TLS.
    struct Connection
    {
        common::raii::UniqueFd fd;
        common::raii::SslCtx ssl_ctx;
        common::raii::SslSession ssl;
        /// At least one response has completed on it.
        bool reused = false;
    };

    /// What one read or write on the connection did.
    enum class IoStatus : std::uint8_t
    {
        Progress,
        WouldBlock,
        Closed,  ///< The peer closed the connection (reads only).
        Failed,
    };

    explicit Http1Transport(std::unique_ptr<internal::IReactor> reactor) noexcept;

    [[nodiscard]] static microtel::Expected<Connection, microtel::Error> OpenConnection(
        const internal::ConnectOptions& opts, const EndpointInfo& endpoint);
    [[nodiscard]] bool RegisterConnection() noexcept;
    void CloseConnection() noexcept;

    void IoThreadLoop() noexcept;
    void Service() noexcept;
    void ExpireQueued() noexcept;
    void CheckInFlight() noexcept;
    void StartNext() noexcept;
    [[nodiscard]] microtel::Expected<void, microtel::Error> OpenAndRegister();
    [[nodiscard]] bool Reconnect() noexcept;
    void GiveUpConnection(const microtel::Error& error) noexcept;
    void OnIoEvent(int fd, internal::EventMask events) noexcept;
    void OnReadable() noexcept;
    void OnResponseBytes(std::span<const std::byte> data) noexcept;
    void OnParserState(bool all_consumed) noexcept;
    void CompleteResponse(bool all_consumed) noexcept;
    void FailMalformedResponse() noexcept;
    void OnResponseEnd() noexcept;
    void OnConnectionLost(const char* message) noexcept;
    void RetryOnNewConnection() noexcept;
    void TryWrite() noexcept;
    void WatchWrites(bool want_write) noexcept;
    [[nodiscard]] IoStatus WriteSome(std::span<const std::byte> data,
                                     std::size_t& n) const noexcept;
    [[nodiscard]] IoStatus ReadSome(std::span<std::byte> buf, std::size_t& n) const noexcept;
    void Finish(internal::TransportResult result) noexcept;
    void FailInFlight(microtel::Error error) noexcept;
    void FailAll(const microtel::Error& error) noexcept;
    void CountEvent(EventLogLimiter::Kind kind) noexcept;

    std::unique_ptr<internal::IReactor> m_reactor;
    std::atomic<microtel::ConnectionState> m_state{microtel::ConnectionState::Disconnected};
    std::atomic<bool> m_stop{false};
    std::thread m_io_thread;
    std::mutex m_io_done_mu;
    std::condition_variable m_io_done_cv;
    bool m_io_done = false;

    // Written by `Connect` before it publishes `Connected`; read by the I/O
    // thread after it observes `Connected`.
    internal::ConnectOptions m_opts;
    EndpointInfo m_endpoint;
    Http1ResponseParser m_parser{Http1ResponseLimits{}};

    // The connection and the request on it: see @threadsafety.
    Connection m_conn;
    InFlight m_inflight;

    // Shared between `Send` / `Cancel` and the I/O thread.
    std::mutex m_mu;
    std::deque<PendingRequest> m_queue;
    std::uint64_t m_inflight_id = 0;
    bool m_inflight_cancelled = false;
    std::atomic<std::uint32_t> m_max_pending_requests{
        internal::ConnectOptions{}.max_pending_requests};

    std::atomic<std::uint64_t> m_next_id{1};
    std::atomic<std::uint64_t> m_background_reconnects{0};
    std::atomic<std::uint64_t> m_stale_retries{0};
    EventLogLimiter m_event_log;  // I/O-thread-only
};

}  // namespace microtel::transport
