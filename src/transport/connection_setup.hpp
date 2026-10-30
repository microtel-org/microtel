// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "microtel/error.hpp"
#include "microtel/expected.hpp"
#include "microtel/internal/transport.hpp"

#include "common/raii/ssl_ctx.hpp"
#include "common/raii/ssl_session.hpp"
#include "common/raii/unique_fd.hpp"

#include <openssl/ssl.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>

/// @file
/// @brief Connection setup shared by the transports: endpoint parsing, the
///        TCP connect and the TLS client handshake.

namespace microtel::transport
{

/// @brief An endpoint URL, split for `TcpConnect`.
struct EndpointInfo
{
    std::string host;
    std::string port;
    bool use_tls = true;
};

/// @brief Move the transport into `Connecting`, from either state that permits
///        it, reporting which one we came from.
///
/// `Disconnected → Connecting` is a first connect; `Reconnecting → Connecting`
/// recovers from a mid-connection drop (ICP 0018 §3). Two compare-exchange
/// attempts rather than one, because compare_exchange takes a single expected
/// value; the second re-reads, so a state that moved in between simply loses.
///
/// @param prior Set to the observed state — the one won from on success, or
///              the blocking state on failure.
[[nodiscard]] bool ClaimConnectSlot(std::atomic<microtel::ConnectionState>& state,
                                    microtel::ConnectionState& prior) noexcept;

/// @brief Split an endpoint URL into host, port and whether to use TLS.
///
/// `https://` and `http://` set TLS on and off; with no scheme, TLS is on
/// unless @p insecure. The port defaults to 443 or 80.
[[nodiscard]] microtel::Expected<EndpointInfo, microtel::Error> ParseEndpoint(
    const std::string& endpoint, bool insecure);

/// @brief Resolve @p host and connect a non-blocking TCP socket to it.
///
/// Tries each resolved address in turn within @p timeout. The socket has
/// `TCP_NODELAY` set and, where the platform has it, `SO_NOSIGPIPE`.
[[nodiscard]] microtel::Expected<common::raii::UniqueFd, microtel::Error> TcpConnect(
    const std::string& host, const std::string& port, std::chrono::milliseconds timeout);

/// @brief Run a TLS client handshake over the connected socket @p fd.
///
/// Loads the trust store and client credentials from @p opts (TLS 1.2
/// minimum), offers @p alpn_protos, sets SNI and hostname verification, and
/// completes the handshake within `opts.tls_handshake_timeout`. It does not
/// check which ALPN protocol was selected: that is the caller's decision
/// (`SelectedAlpn`).
///
/// @param fd          Borrowed; must outlive the returned session, whose BIO
///                    reads and writes it.
/// @param alpn_protos ALPN protocol list in wire format.
[[nodiscard]] microtel::Expected<std::pair<common::raii::SslCtx, common::raii::SslSession>,
                                 microtel::Error>
TlsConnect(int fd,
           const internal::ConnectOptions& opts,
           const std::string& host,
           std::span<const std::uint8_t> alpn_protos);

/// @param ssl Borrowed; not retained.
/// @return The protocol the TLS peer selected, borrowed from the session and
///         valid for its lifetime; empty if ALPN produced no agreement.
[[nodiscard]] std::string_view SelectedAlpn(const SSL* ssl) noexcept;

}  // namespace microtel::transport
