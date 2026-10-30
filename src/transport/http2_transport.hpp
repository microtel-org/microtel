// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "microtel/error.hpp"
#include "microtel/expected.hpp"
#include "microtel/internal/reactor.hpp"
#include "microtel/internal/transport.hpp"
#include "microtel/provider.hpp"
#include "microtel/status.hpp"

#include "common/raii/nghttp2_session.hpp"
#include "common/raii/ssl_ctx.hpp"
#include "common/raii/ssl_session.hpp"
#include "common/raii/unique_fd.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace microtel::transport
{

/// @brief nghttp2 + OpenSSL implementation of `ITransport`.
///
/// One socket, one TLS session, one nghttp2 client session, one I/O thread.
/// Reconnect is internal; callers see only `ConnectionState` transitions.
///
/// The reactor is injected so unit tests can drive the I/O loop from a script
/// without a real socket (`FakeReactor`). Production code injects an
/// `EpollReactor` created by `EpollReactor::Create()`.
///
/// @threadsafety `GetState` is thread-safe. `Connect` and `Close` are
///               caller-thread (single caller at a time). `Send` is
///               single-threaded; only the exporter worker may call it.
///               (All threading contracts LOCKED — `interfaces.md` §4.1.)
/// @see docs/interfaces.md §4.1
class Http2Transport final : public internal::ITransport
{
public:
    /// @brief Factory — creates the transport and starts the I/O thread.
    ///
    /// Returns an error if `reactor` is null or memory allocation fails.
    [[nodiscard]] static microtel::Expected<std::unique_ptr<Http2Transport>, microtel::Error>
    Create(std::unique_ptr<internal::IReactor> reactor) noexcept;

    ~Http2Transport() noexcept override;

    Http2Transport(const Http2Transport&) = delete;
    Http2Transport& operator=(const Http2Transport&) = delete;
    Http2Transport(Http2Transport&&) = delete;
    Http2Transport& operator=(Http2Transport&&) = delete;

    /// @brief DNS + TCP + optional TLS + nghttp2 SETTINGS exchange.
    [[nodiscard]] microtel::Expected<void, microtel::Error> Connect(
        const internal::ConnectOptions& opts) override;

    /// @brief Stub — stream submission in M3-D5.
    [[nodiscard]] internal::RequestHandle Send(internal::RequestSpec spec) noexcept override;

    /// @brief Abandon a request; synchronous with respect to its payload.
    ///
    /// Once this returns the transport reads no more of the request's
    /// payload, so the caller may free it (issue #508). A request still queued
    /// is failed `Cancelled` before this returns; one already on the wire is
    /// reset by the I/O thread, which then fulfils it.
    void Cancel(const internal::RequestHandle& handle) noexcept override;

    /// @brief True when the last `Connect` failed because the peer speaks
    ///        HTTP/1.1 only (ICP 0042 §1).
    ///
    /// Set by a plaintext peer that answered the HTTP/2 preface with an
    /// HTTP/1.1 response, or, for `Protocol::Http`, by a TLS peer that
    /// selected ALPN `http/1.1` or nothing. `FallbackTransport` reads it so
    /// that nothing has to match error text. Reset by every `Connect`.
    /// Thread-safe.
    [[nodiscard]] bool PeerSpeaksOnlyHttp1() const noexcept;

    /// @brief Returns the current connection state. Thread-safe.
    [[nodiscard]] microtel::ConnectionState GetState() const noexcept override;

    /// @brief Signal the I/O thread to stop, wait up to `timeout` for it to
    /// join, then return the outcome. Idempotent — second call returns
    /// `AlreadyShutDown`.
    [[nodiscard]] microtel::Status Close(std::chrono::milliseconds timeout) noexcept override;

    // Called by the nghttp2 C-callback trampolines defined in the .cpp file.
    // Not part of the public API; must be public so the free-function trampolines
    // can reach them without friendship (C-linkage functions cannot be friends).
    std::ptrdiff_t NgHttp2DoSend(const std::uint8_t* data, std::size_t len) noexcept;
    std::ptrdiff_t NgHttp2DoRecv(std::uint8_t* buf, std::size_t len) noexcept;
    void OnSettingsAck() noexcept;

    /// @brief Handle a peer `GOAWAY` (`docs/sequences/goaway-handling.md`).
    ///
    /// Records what the peer said, fulfils every stream the peer will not
    /// serve, and retires the connection once the ones it accepted have
    /// drained. nghttp2 marks the session draining on its own and will open no
    /// further streams on it, so a transport that stayed `Connected` here
    /// would refuse every later `Send` with no reconnect to recover it.
    ///
    /// @param last_stream_id Highest stream id the peer accepted; streams above
    ///                       it are refused.
    /// @param error_code     HTTP/2 error code carried by the frame.
    void OnGoaway(std::int32_t last_stream_id, std::uint32_t error_code) noexcept;

    void OnStreamClose(std::int32_t stream_id, std::uint32_t error_code) noexcept;
    void OnResponseHeader(std::int32_t stream_id,
                          bool is_trailer,
                          std::string_view name,
                          std::string_view value) noexcept;
    void OnResponseData(std::int32_t stream_id, const std::uint8_t* data, std::size_t len) noexcept;

    struct StreamState;

    /// @brief Copy the next request body bytes of @p state into @p buf.
    ///
    /// Called by nghttp2's data-provider trampoline. Refuses, so nghttp2
    /// resets the stream, once the request has been cancelled: the payload is
    /// borrowed, and its owner may free it as soon as `Cancel` returns
    /// (issue #508).
    ///
    /// @return Bytes copied, or `NGHTTP2_ERR_TEMPORAL_CALLBACK_FAILURE`.
    std::ptrdiff_t ReadPayload(StreamState& state,
                               std::uint8_t* buf,
                               std::size_t length,
                               std::uint32_t* data_flags) noexcept;

    /// @brief Which response memory cap a stream exceeded, if any.
    ///
    /// Recorded on the stream rather than acted on where it is detected: both
    /// detection points sit inside an nghttp2 callback, and the request is
    /// completed from `FulfillStream` once nghttp2 closes the stream.
    enum class ResponseOverflow : std::uint8_t
    {
        None = 0,
        /// Body accumulation passed `ConnectOptions::max_response_bytes`.
        Body = 1,
        /// Trailer accumulation passed `ConnectOptions::max_trailer_bytes`.
        Trailers = 2,
    };

    /// @brief Per-stream state owned by the I/O thread.
    ///
    /// Public so the `PayloadReadCb` C trampoline can cast `source->ptr`
    /// to `Http2Transport::StreamState*`.
    struct StreamState
    {
        internal::RequestSpec spec;
        std::size_t payload_offset = 0;
        std::promise<internal::TransportResult> promise;
        internal::TransportResult result;
        std::uint64_t handle_id = 0;
        /// Summed name+value bytes of the trailers seen so far.
        std::size_t trailer_bytes = 0;
        /// Set once, by whichever cap this stream breached first.
        ResponseOverflow overflow = ResponseOverflow::None;
        /// Set by `DrainCancelQueue`; `ReadPayload` reads no more of the
        /// payload once it is.
        bool cancelled = false;
    };

private:
    /// Size of the formatted GOAWAY diagnostic, including its terminator.
    static constexpr std::size_t kGoawayDetailMax = 96;

    /// @brief Request queued by Send(); drained by the I/O thread.
    struct PendingRequest
    {
        internal::RequestSpec spec;
        std::promise<internal::TransportResult> promise;
        std::uint64_t handle_id = 0;
    };

    explicit Http2Transport(std::unique_ptr<internal::IReactor> reactor) noexcept;

    void IoThreadLoop() noexcept;
    void OnIoEvent(int fd, internal::EventMask events) noexcept;
    void DrainPendingRequests() noexcept;
    void DrainCancelQueue() noexcept;
    void SubmitStream(PendingRequest req) noexcept;
    void FulfillStream(std::int32_t stream_id, std::uint32_t nghttp2_error_code) noexcept;

    [[nodiscard]] microtel::Expected<std::pair<common::raii::SslCtx, common::raii::SslSession>,
                                     microtel::Error>
    TlsHandshake(const internal::ConnectOptions& opts, const std::string& host);

    /// @brief Fulfil and drop every in-flight stream with @p message.
    ///
    /// Called from `Close` (after the I/O thread is joined) and from the
    /// mid-connection drop path in `OnIoEvent` (on the I/O thread itself).
    /// `m_streams` is I/O-thread-only and `Close` runs after the join, so
    /// neither caller needs a lock.
    ///
    /// @param message Must fit `std::string`'s SSO buffer (15 chars on
    ///        libstdc++). This can run under memory pressure, and a message
    ///        that allocates would throw out of a `noexcept` frame — see #150.
    void AbandonInFlight(const char* message) noexcept;

    /// @brief Take this connection's memory and queue budgets from @p opts.
    ///
    /// Called at the top of `Connect`, before anything can read from the
    /// socket, so the caps govern the very first response the connection
    /// carries — and so a reconnect picks up whatever the caller passed this
    /// time rather than inheriting the previous connection's.
    void AdoptBudgets(const internal::ConnectOptions& opts) noexcept;

    /// @brief Stop buffering an over-budget response and reset its stream.
    ///
    /// Releases what was accumulated — a truncated body would read like a
    /// malformed one upstream — records which cap was breached, and sends
    /// `RST_STREAM(CANCEL)` so the peer stops sending. The request itself is
    /// completed by `FulfillStream` when nghttp2 closes the stream, which is
    /// what keeps the completion path single.
    ///
    /// Called from inside nghttp2 callbacks, so it submits the reset and
    /// leaves the flush to the `nghttp2_session_send` that ends the turn.
    ///
    /// @param stream_id the offending stream.
    /// @param state its state; borrowed, still owned by `m_streams`.
    /// @param kind which cap was breached. Never `None`.
    void FailOversizedStream(std::int32_t stream_id,
                             StreamState& state,
                             ResponseOverflow kind) noexcept;

    /// @brief Fulfil and drop every stream the peer's GOAWAY refused.
    ///
    /// Streams above `last_stream_id` were never processed by the peer
    /// (`goaway-handling.md`, variant 2). nghttp2 is about to close them with
    /// `REFUSED_STREAM`; completing them here first is what lets the error say
    /// GOAWAY rather than the generic stream error `FulfillStream` would give.
    /// I/O-thread-only, like the rest of `m_streams`.
    void RefuseStreamsAbove(std::int32_t last_stream_id) noexcept;

    /// @brief Move to `Reconnecting` once a GOAWAY'd connection has no streams
    ///        left on it. No-op before a GOAWAY, or while one is still
    ///        draining.
    ///
    /// I/O-thread-only, and specifically only between nghttp2 turns — never
    /// from inside an nghttp2 callback. The state change is what releases the
    /// caller thread to reconnect, and a reconnect closes the socket nghttp2
    /// may still be reading.
    ///
    /// @return true if this call retired the connection.
    [[nodiscard]] bool FinishGoawayDrainIfIdle() noexcept;

    [[nodiscard]] microtel::Expected<common::raii::Nghttp2Session, microtel::Error> Http2Handshake(
        const internal::ConnectOptions& opts);

    /// @brief Classify a failed SETTINGS exchange.
    ///
    /// Returns the targeted HTTP/1.1-peer error if the first plaintext read of
    /// this connection was an HTTP/1.1 response (issue #166), the peer-closed
    /// message when a send failed with EPIPE or ECONNRESET (issue #177), and
    /// the generic nghttp2 failure otherwise.
    [[nodiscard]] microtel::Error Http2HandshakeFailure() const;

    /// @brief Inspect the first plaintext read of a connection for an HTTP/1.1
    ///        status line.
    ///
    /// A plaintext microtel endpoint is h2c with prior knowledge; an
    /// HTTP/1.1-only receiver answers the connection preface with a response
    /// nghttp2 cannot parse. Only the first read is eligible — later bytes are
    /// DATA-frame payload that may legitimately start with anything.
    ///
    /// @param buf Borrowed; the bytes just read. Not retained.
    /// @return true if the peer answered with HTTP/1.1, in which case
    ///         `m_peer_spoke_http1` is set for `Http2HandshakeFailure`.
    [[nodiscard]] bool SniffHttp1Response(const std::uint8_t* buf, std::size_t len) noexcept;

    std::unique_ptr<internal::IReactor> m_reactor;
    std::atomic<microtel::ConnectionState> m_state{microtel::ConnectionState::Disconnected};
    std::atomic<bool> m_stop{false};
    std::thread m_io_thread;
    /// Signalled by the I/O thread as its loop exits, so `Close` can bound how
    /// long it waits. `std::thread` has no timed join, and detaching is not an
    /// option here because the loop touches members of `this` — so the wait is
    /// what the timeout actually governs, and a timeout is reported honestly
    /// rather than silently ignored (`Provider::Shutdown` contract, CLAUDE.md
    /// rule 15). Same shape as `OtlpExporter::Shutdown`.
    std::mutex m_io_done_mu;
    std::condition_variable m_io_done_cv;
    bool m_io_done{false};

    // Connection resources — written by Connect(), read by I/O-thread callbacks.
    // Thread-safety: Connect() writes these before the release-store of
    // m_state = Connected; I/O callbacks acquire-load m_state before use.
    common::raii::UniqueFd m_socket;
    common::raii::SslCtx m_ssl_ctx;
    common::raii::SslSession m_ssl_session;
    common::raii::Nghttp2Session m_nghttp2_session;
    std::atomic<bool> m_settings_ack_received{false};
    /// True while the next successful plaintext read is still the first of the
    /// connection. Reset by every `Http2Handshake`, cleared by the read that
    /// claims it. Atomic because `Connect` writes it on the caller thread while
    /// the I/O thread may be in a recv callback.
    std::atomic<bool> m_first_plaintext_recv{false};
    /// Set when that first read turned out to be an HTTP/1.1 response, or when
    /// a TLS peer for `Protocol::Http` selected ALPN `http/1.1` or nothing
    /// (`PeerSpeaksOnlyHttp1`). Reset by every `Connect` and `Http2Handshake`,
    /// so a later reconnect cannot inherit it.
    std::atomic<bool> m_peer_spoke_http1{false};
    /// Set when a send failed with EPIPE or ECONNRESET — the peer hung up under
    /// our own write. Since issue #177 those arrive as errno values instead of
    /// a SIGPIPE, which is what makes them reportable at all. Reset by every
    /// `Http2Handshake`; written from the caller thread during the handshake
    /// and from the I/O thread afterwards, hence atomic.
    std::atomic<bool> m_peer_closed_on_send{false};
    /// Set when the peer's GOAWAY has been seen on the current connection.
    /// Reset by every `Http2Handshake`, so a reconnect does not inherit it.
    /// Atomic for the same reason as its neighbours: the frame can arrive
    /// during the caller-thread handshake as well as on the I/O thread.
    std::atomic<bool> m_goaway_received{false};
    /// Response memory budget for the current connection, from
    /// `ConnectOptions`. Written by `Connect` on the caller thread and read by
    /// the I/O thread's nghttp2 callbacks, hence atomic — they are plain
    /// scalars guarding nothing else, so relaxed ordering is enough.
    std::atomic<std::uint32_t> m_max_response_bytes{internal::ConnectOptions{}.max_response_bytes};
    std::atomic<std::uint32_t> m_max_trailer_bytes{internal::ConnectOptions{}.max_trailer_bytes};
    /// Request-queue bound for the current connection (`threading-model.md`
    /// §3.2). Same ownership story as the two response budgets, except the
    /// readers are the submitting threads rather than the I/O thread.
    std::atomic<std::uint32_t> m_max_pending_requests{
        internal::ConnectOptions{}.max_pending_requests};
    /// The GOAWAY diagnostic, formatted once on receipt.
    ///
    /// The transport owns no diagnostics sink — "its callers record what they
    /// observe" (`error-model.md` §5) — so what it saw reaches an operator
    /// through the `Error` carried by every request the GOAWAY refused. A
    /// fixed buffer because the formatting happens inside a `noexcept`
    /// nghttp2 callback.
    std::array<char, kGoawayDetailMax> m_goaway_detail{};

    // Send queues — caller-thread writes, I/O thread drains.
    std::mutex m_pending_mu;
    std::vector<PendingRequest> m_pending_queue;
    /// Also held by `ReadPayload` while it copies, so `Cancel` cannot return
    /// while a copy from the payload it cancels is under way.
    std::mutex m_cancel_mu;
    /// Requests cancelled after the I/O thread took them from
    /// `m_pending_queue`. `ReadPayload` refuses any id listed here.
    std::vector<std::uint64_t> m_cancel_queue;

    // I/O-thread-only stream tracking (no mutex needed).
    std::unordered_map<std::int32_t, std::unique_ptr<StreamState>> m_streams;
    std::unordered_map<std::uint64_t, std::int32_t> m_handle_to_stream;

    std::atomic<std::uint64_t> m_next_handle_id{1};
};

}  // namespace microtel::transport
