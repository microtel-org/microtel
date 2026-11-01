// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "microtel/error.hpp"
#include "microtel/expected.hpp"
#include "microtel/protocol.hpp"
#include "microtel/provider.hpp"  // ConnectionState
#include "microtel/status.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <future>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace microtel::internal
{

/// @brief Inputs to `ITransport::Connect`. Six independent timeouts per
/// `docs/configuration.md` §3.4 (the connect-relevant subset is on this struct;
/// others live on the exporter's per-request `RequestSpec`).
struct ConnectOptions
{
    std::string endpoint;  ///< host:port URL (https:// or http://)
    Protocol protocol = Protocol::Grpc;
    std::chrono::milliseconds connect_timeout = std::chrono::seconds(10);
    std::chrono::milliseconds tls_handshake_timeout = std::chrono::seconds(10);

    // TLS material; empty paths mean default behaviour.
    bool insecure = false;
    std::filesystem::path ca_bundle;
    std::filesystem::path client_cert;
    std::filesystem::path client_key;
    std::string sni_override;

    // HTTP/2 settings (sensible defaults; tunable via configuration).
    std::uint32_t max_concurrent_streams = 100;
    std::uint32_t initial_window_size = 1U << 20;  // 1 MiB

    /// @brief Ceiling on one response body, in bytes, enforced as the body is
    ///        accumulated (`MemoryLimitOptions::max_response_bytes`).
    ///
    /// A response that exceeds it stops being buffered, its stream is reset,
    /// and the request fails with `TransportResult::response_too_large`.
    ///
    /// A connection-level knob rather than a per-request one: it is a memory
    /// budget for the transport's own buffers, identical for every request a
    /// provider issues, and repeating it on each `RequestSpec` would give the
    /// codec a dial it has no reason to turn.
    std::uint32_t max_response_bytes = 1U << 20;  // 1 MiB
    /// @brief Ceiling on the summed name+value bytes of one response's
    ///        trailers (`MemoryLimitOptions::max_trailer_bytes`). Same
    ///        failure shape as `max_response_bytes`.
    ///
    /// Also the value advertised as `SETTINGS_MAX_HEADER_LIST_SIZE` at session
    /// setup, which is how the response *headers* get a budget: nghttp2 then
    /// enforces the same ceiling on the non-trailer HEADERS block, which the
    /// transport does not meter itself (issue #213). Both HEADERS frames on a
    /// stream are header lists, so one budget covers them.
    std::uint32_t max_trailer_bytes = 64U * 1024U;  // 64 KiB

    /// @brief Ceiling on how many requests may sit in the transport's request
    ///        queue waiting for the I/O thread to attach them to a stream
    ///        (`docs/threading-model.md` §3.2).
    ///
    /// `Send` refuses beyond it rather than queueing: the future resolves
    /// immediately with `TransportResult::transport_busy`, and the codec counts
    /// `DropReason::TransportBusy`. Without a bound, an I/O thread that stalls
    /// — a peer that stopped reading, a reconnect in backoff — lets the queue
    /// grow for as long as producers keep submitting.
    ///
    /// The default is generous on purpose. v1 has at most three exporter
    /// workers (traces, metrics, logs) and each blocks on its own completion,
    /// so a healthy process never queues more than three; the headroom is for
    /// the deadline-and-retry churn that accumulates while the I/O thread is
    /// the thing that is stuck, which is the case the bound exists for.
    std::uint32_t max_pending_requests = 64;
};

/// @brief HTTP/2 header (name, value) pair.
struct HeaderField
{
    std::string name;
    std::string value;
};

/// @brief Inputs to a single `ITransport::Send` call.
///
/// The bytes referenced by `payload` are **borrowed**; the caller (the wire
/// codec) retains ownership and must not free them until the request
/// completion fires. (LOCKED — `docs/memory-model.md` §3.3.)
struct RequestSpec
{
    std::vector<HeaderField> headers;
    std::span<const std::byte> payload;
    std::chrono::milliseconds deadline = std::chrono::seconds(10);
};

/// @brief Result of a single transport request.
struct TransportResult
{
    bool success = false;
    std::vector<HeaderField> response_headers;
    std::vector<HeaderField> response_trailers;
    /// Bounded by `ConnectOptions::max_response_bytes`. Empty when the cap was
    /// hit: the transport releases what it had buffered rather than hand up a
    /// truncated body that reads like a malformed one.
    std::vector<std::byte> response_body;
    std::optional<Error> error;
    /// @brief The response exceeded `ConnectOptions::max_response_bytes` or
    ///        `max_trailer_bytes`, so the transport stopped buffering it and
    ///        reset the stream.
    ///
    /// `success` is false and `error` names which cap. Distinguished from an
    /// ordinary transport failure because the retry classification differs:
    /// the peer sends the same oversized response next time, so the codec
    /// makes this a terminal `response_too_large` rather than a retry
    /// (`docs/error-model.md` §7.1).
    bool response_too_large = false;
    /// @brief The transport refused the request because its request queue was
    ///        already at `ConnectOptions::max_pending_requests`.
    ///
    /// `success` is false, `error` is `ResourceExhausted`, and nothing was
    /// queued or sent. Distinguished from an ordinary transport failure so the
    /// codec can count `DropReason::TransportBusy`; the classification is
    /// unchanged — a full queue is transient, so the request stays retryable.
    bool transport_busy = false;
};

/// @brief Move-only handle to an in-flight request.
///
/// The `future` resolves on completion (success, failure, cancellation).
/// `Cancel(handle)` requests cancellation; the future still resolves, with
/// `Error::Kind::Cancelled` populated. Methods are inline so mocks can
/// construct a `RequestHandle` without a separate translation unit.
class RequestHandle
{
public:
    RequestHandle() noexcept = default;
    explicit RequestHandle(std::uint64_t id, std::future<TransportResult> f) noexcept
        : m_id(id), m_future(std::move(f))
    {
    }

    RequestHandle(const RequestHandle&) = delete;
    RequestHandle& operator=(const RequestHandle&) = delete;
    RequestHandle(RequestHandle&&) noexcept = default;
    RequestHandle& operator=(RequestHandle&&) noexcept = default;

    [[nodiscard]] std::uint64_t Id() const noexcept
    {
        return m_id;
    }
    [[nodiscard]] std::future<TransportResult>& Future() noexcept
    {
        return m_future;
    }

private:
    std::uint64_t m_id = 0;
    std::future<TransportResult> m_future;
};

/// @brief A connection to one OTLP endpoint over HTTP/2, or over HTTP/1.1 for
///        OTLP/HTTP when the endpoint cannot speak HTTP/2 (ICP 0042).
///
/// One transport instance manages exactly one socket and one session: an
/// nghttp2 session, or an HTTP/1.1 keep-alive connection.
/// Reconnect is internal — clients do not see it.
///
/// `Send` is safe for concurrent callers (LOCKED, relaxed by ICP 0009): the
/// transport serialises submissions onto its single I/O thread. Each caller
/// still owns its own `IWireCodec`.
///
/// @threadsafety See per-method threading notes.
/// @see docs/interfaces.md §4.1
class ITransport
{
public:
    virtual ~ITransport() noexcept = default;

    /// @brief Open the connection (DNS + TCP + TLS + HTTP/2 SETTINGS).
    /// @threadsafety Caller-thread; called once during `Provider::Build` or
    ///               `Provider::Connect`.
    [[nodiscard]] virtual microtel::Expected<void, microtel::Error> Connect(
        const ConnectOptions& opts) = 0;

    /// @brief Submit a request. Returns immediately with a handle.
    /// @threadsafety Thread-safe for submission — any number of caller threads
    ///               may call `Send` concurrently; the transport serialises
    ///               submissions onto its single I/O thread. Each caller still
    ///               owns its own `IWireCodec`; codecs and `IOtlpEncoder`
    ///               remain single-caller. (ICP 0009.)
    [[nodiscard]] virtual RequestHandle Send(RequestSpec spec) noexcept = 0;

    /// @brief Cancel an in-flight request.
    /// @threadsafety Thread-safe.
    virtual void Cancel(const RequestHandle& handle) noexcept = 0;

    /// @threadsafety Thread-safe.
    [[nodiscard]] virtual ConnectionState GetState() const noexcept = 0;

    /// @brief Initiate orderly shutdown. Idempotent.
    /// @threadsafety Thread-safe.
    [[nodiscard]] virtual microtel::Status Close(std::chrono::milliseconds timeout) noexcept = 0;
};

}  // namespace microtel::internal
