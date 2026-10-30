// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#include "connection_setup.hpp"

#include "transport/connect_error.hpp"
#include "transport/nosignal_io.hpp"

#include <openssl/ssl.h>
#include <openssl/x509.h>

#include <cerrno>
#include <chrono>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include <fcntl.h>
#include <netdb.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/types.h>

namespace microtel::transport
{

namespace
{

// Disable Nagle's algorithm: batching is done at the BSP/exporter layer;
// small trailing DATA frames must not stall 40ms on delayed-ACK interaction.
void SetTcpNoDelay(int fd) noexcept
{
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg,hicpp-vararg)
    static constexpr int kEnable = 1;
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg,hicpp-vararg)
    ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &kEnable, sizeof(kEnable));
}

// Suppress SIGPIPE for the whole socket where the platform offers it. v1 is
// Linux-only, where this compiles out entirely and MSG_NOSIGNAL on each write
// is the mechanism (issue #177); the BSDs and macOS have the socket option
// instead, and pre-paying it here is three lines.
void SetNoSigPipe(int fd) noexcept
{
#ifdef SO_NOSIGPIPE
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg,hicpp-vararg)
    static constexpr int kEnable = 1;
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg,hicpp-vararg)
    ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &kEnable, sizeof(kEnable));
#else
    (void)fd;
#endif
}

/// @return The socket's pending error (`SO_ERROR`), which reading clears: 0
///         once a non-blocking connect has succeeded, else its errno.
int PendingSocketError(int fd) noexcept
{
    int sock_err = 0;
    socklen_t sock_err_len = sizeof(sock_err);
    ::getsockopt(fd, SOL_SOCKET, SO_ERROR, &sock_err, &sock_err_len);
    return sock_err;
}

microtel::Expected<void, microtel::Error> LoadSslCtxCredentials(
    SSL_CTX* ctx, const internal::ConnectOptions& opts)
{
    // TLS_client_method() on its own means "whatever range this OpenSSL build
    // permits", so the floor would be set by the linked library and the host
    // crypto policy rather than by microtel -- the same source linked two ways
    // negotiating two different security floors. Pin it here: TLS 1.2 minimum,
    // 1.3 preferred by OpenSSL's own version negotiation. Unconditional,
    // including under `insecure`: that option skips peer *verification*, which
    // is a deliberate trust decision, and says nothing about accepting a
    // downgrade to a protocol version with known weaknesses. See issue #216.
    //
    // Unchecked, like the neighbouring `SSL_CTX_set_default_verify_paths` and
    // `SSL_CTX_set_alpn_protos`: this call only fails when the version is
    // outside the range the build supports, and every OpenSSL microtel
    // supports (1.1.1 and newer) has TLS 1.2.
    ::SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);

    if (!opts.ca_bundle.empty())
    {
        if (::SSL_CTX_load_verify_locations(ctx, opts.ca_bundle.string().c_str(), nullptr) != 1)
        {
            return microtel::Unexpected<microtel::Error>{
                {.kind = microtel::Error::Kind::Network, .message = "CA bundle load failed"}};
        }
    }
    else
    {
        ::SSL_CTX_set_default_verify_paths(ctx);
    }

    // An OpenSSL client defaults to SSL_VERIFY_NONE: without this the trust
    // store above is never consulted and any certificate is accepted, which
    // makes a configured `ca_bundle` look like it is doing something while it
    // does nothing at all.  See ICP 0022.
    if (!opts.insecure)
    {
        ::SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, nullptr);
    }

    if (!opts.client_cert.empty())
    {
        if (::SSL_CTX_use_certificate_file(
                ctx, opts.client_cert.string().c_str(), SSL_FILETYPE_PEM) != 1)
        {
            return microtel::Unexpected<microtel::Error>{
                {.kind = microtel::Error::Kind::Network, .message = "client cert load failed"}};
        }
        if (::SSL_CTX_use_PrivateKey_file(
                ctx, opts.client_key.string().c_str(), SSL_FILETYPE_PEM) != 1)
        {
            return microtel::Unexpected<microtel::Error>{
                {.kind = microtel::Error::Kind::Network, .message = "client key load failed"}};
        }
    }
    return {};
}

// A failed handshake is reported differently depending on whether the peer's
// certificate was the reason: "the certificate is not acceptable" and "the peer
// hung up mid-handshake" are different operational problems, and this message
// is what reaches `HealthSnapshot::last_error_message`.
std::string TlsFailureMessage(const SSL* ssl)
{
    const long verify = ::SSL_get_verify_result(ssl);
    return (verify == X509_V_OK) ? std::string{"TLS handshake failed"}
                                 : "TLS certificate verification failed: " +
                                       std::string{::X509_verify_cert_error_string(verify)};
}

/// Give @p ssl a transport that cannot raise `SIGPIPE`.
///
/// Not `SSL_set_fd`: OpenSSL's stock socket BIO writes with `write(2)`, which
/// raises `SIGPIPE` the moment the peer goes away mid-export and takes the host
/// application down with it. This BIO writes with `MSG_NOSIGNAL` instead, and
/// covers the handshake writes inside `SSL_connect` as well as `SSL_write`
/// (issue #177).
///
/// @param ssl Borrowed; adopts the BIO on success — `SSL_set_bio` takes
///            ownership, the same BIO for both directions consumes a single
///            reference, and `SSL_free` releases it.
/// @param fd  Borrowed; the BIO reads and writes it and never closes it, so
///            the socket's owner must outlive the session.
/// @return false if OpenSSL could not allocate the BIO, in which case nothing
///         was transferred and nothing leaked.
[[nodiscard]] bool InstallNoSignalBio(SSL* ssl, int fd) noexcept
{
    BioPtr bio = MakeNoSignalBio(fd);
    if (!bio)
    {
        return false;
    }
    BIO* const raw_bio = bio.release();
    ::SSL_set_bio(ssl, raw_bio, raw_bio);
    return true;
}

microtel::Expected<void, microtel::Error> SslConnectLoop(
    SSL* ssl, int fd, std::chrono::steady_clock::time_point deadline)
{
    while (true)
    {
        const int ret = ::SSL_connect(ssl);
        if (ret == 1)
        {
            return {};
        }
        const int err = ::SSL_get_error(ssl, ret);
        const int ev = (err == SSL_ERROR_WANT_READ) ? POLLIN : POLLOUT;

        if (err != SSL_ERROR_WANT_READ && err != SSL_ERROR_WANT_WRITE)
        {
            return microtel::Unexpected<microtel::Error>{
                {.kind = microtel::Error::Kind::Network, .message = TlsFailureMessage(ssl)}};
        }

        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline)
        {
            return microtel::Unexpected<microtel::Error>{
                {.kind = microtel::Error::Kind::Cancelled, .message = "TLS handshake timeout"}};
        }
        const auto ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
        pollfd pfd{.fd = fd, .events = static_cast<short>(ev), .revents = 0};
        if (::poll(&pfd, 1, static_cast<int>(ms)) <= 0)
        {
            return microtel::Unexpected<microtel::Error>{
                {.kind = microtel::Error::Kind::Cancelled, .message = "TLS handshake timeout"}};
        }
    }
}

}  // namespace

std::string_view SelectedAlpn(const SSL* ssl) noexcept
{
    const unsigned char* proto = nullptr;
    unsigned int proto_len = 0;
    ::SSL_get0_alpn_selected(ssl, &proto, &proto_len);
    if (proto == nullptr || proto_len == 0)
    {
        return {};
    }
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    return std::string_view{reinterpret_cast<const char*>(proto), proto_len};
}

microtel::Expected<EndpointInfo, microtel::Error> ParseEndpoint(const std::string& endpoint,
                                                                bool insecure)
{
    if (endpoint.empty())
    {
        return microtel::Unexpected<microtel::Error>{
            {.kind = microtel::Error::Kind::Network, .message = "empty endpoint"}};
    }

    std::string_view sv{endpoint};
    bool use_tls = !insecure;

    if (sv.starts_with("https://"))
    {
        use_tls = true;
        sv.remove_prefix(8);
    }
    else if (sv.starts_with("http://"))
    {
        use_tls = false;
        sv.remove_prefix(7);
    }

    const auto colon = sv.rfind(':');
    std::string host;
    std::string port;
    if (colon == std::string_view::npos)
    {
        host = std::string{sv};
        port = use_tls ? "443" : "80";
    }
    else
    {
        host = std::string{sv.substr(0, colon)};
        port = std::string{sv.substr(colon + 1)};
    }

    if (host.empty() || port.empty())
    {
        return microtel::Unexpected<microtel::Error>{
            {.kind = microtel::Error::Kind::Network, .message = "malformed endpoint"}};
    }
    return EndpointInfo{.host = std::move(host), .port = std::move(port), .use_tls = use_tls};
}

microtel::Expected<common::raii::UniqueFd, microtel::Error> TcpConnect(
    const std::string& host, const std::string& port, std::chrono::milliseconds timeout)
{
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    addrinfo* res = nullptr;
    if (::getaddrinfo(host.c_str(), port.c_str(), &hints, &res) != 0)
    {
        return microtel::Unexpected<microtel::Error>{
            {.kind = microtel::Error::Kind::Network, .message = "DNS resolution failed"}};
    }
    const std::unique_ptr<addrinfo, decltype(&::freeaddrinfo)> guard{res, &::freeaddrinfo};

    const auto deadline = std::chrono::steady_clock::now() + timeout;
    int last_errno = 0;

    for (const addrinfo* ai = res; ai != nullptr; ai = ai->ai_next)
    {
        // NOLINTBEGIN(cppcoreguidelines-pro-type-vararg,hicpp-vararg,hicpp-signed-bitwise)
        common::raii::UniqueFd fd{
            ::socket(ai->ai_family, ai->ai_socktype | SOCK_CLOEXEC, ai->ai_protocol)};
        // NOLINTEND(cppcoreguidelines-pro-type-vararg,hicpp-vararg,hicpp-signed-bitwise)
        if (!fd.IsValid())
        {
            continue;
        }

        SetTcpNoDelay(fd.Get());
        SetNoSigPipe(fd.Get());

        // Non-blocking connect so we can enforce the timeout.
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg,hicpp-vararg,hicpp-signed-bitwise)
        ::fcntl(fd.Get(), F_SETFL, ::fcntl(fd.Get(), F_GETFL) | O_NONBLOCK);

        const int ret = ::connect(fd.Get(), ai->ai_addr, ai->ai_addrlen);
        if (ret == 0)
        {
            return fd;
        }
        if (errno != EINPROGRESS)
        {
            last_errno = MergeConnectErrno(last_errno, errno);
            continue;
        }

        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline)
        {
            return microtel::Unexpected<microtel::Error>{
                {.kind = microtel::Error::Kind::Cancelled, .message = "connect timeout"}};
        }
        const auto ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
        pollfd pfd{.fd = fd.Get(), .events = POLLOUT, .revents = 0};
        if (::poll(&pfd, 1, static_cast<int>(ms)) <= 0)
        {
            continue;
        }

        const int sock_err = PendingSocketError(fd.Get());
        if (sock_err != 0)
        {
            last_errno = MergeConnectErrno(last_errno, sock_err);
            continue;
        }
        return fd;
    }

    return microtel::Unexpected<microtel::Error>{ConnectFailureError(last_errno)};
}

microtel::Expected<std::pair<common::raii::SslCtx, common::raii::SslSession>, microtel::Error>
TlsConnect(int fd,
           const internal::ConnectOptions& opts,
           const std::string& host,
           std::span<const std::uint8_t> alpn_protos)
{
    common::raii::SslCtx ssl_ctx{::SSL_CTX_new(::TLS_client_method())};
    if (!ssl_ctx.IsValid())
    {
        return microtel::Unexpected<microtel::Error>{
            {.kind = microtel::Error::Kind::Network, .message = "SSL_CTX_new failed"}};
    }

    auto creds = LoadSslCtxCredentials(ssl_ctx.Get(), opts);
    if (!creds)
    {
        return microtel::Unexpected<microtel::Error>{creds.error()};
    }

    ::SSL_CTX_set_alpn_protos(ssl_ctx.Get(), alpn_protos.data(), alpn_protos.size());

    common::raii::SslSession ssl{::SSL_new(ssl_ctx.Get())};
    if (!ssl.IsValid())
    {
        return microtel::Unexpected<microtel::Error>{
            {.kind = microtel::Error::Kind::Network, .message = "SSL_new failed"}};
    }

    if (!InstallNoSignalBio(ssl.Get(), fd))
    {
        return microtel::Unexpected<microtel::Error>{
            {.kind = microtel::Error::Kind::Network, .message = "BIO_new failed"}};
    }
    ::SSL_set_connect_state(ssl.Get());

    const std::string& sni = opts.sni_override.empty() ? host : opts.sni_override;
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    ::SSL_set_tlsext_host_name(ssl.Get(), reinterpret_cast<const void*>(sni.c_str()));

    // The certificate is checked against the same name that goes into SNI, so
    // a deliberate `sni_override` also moves the name the certificate must
    // carry -- that is what makes a cert issued to "localhost" usable when
    // connecting to 127.0.0.1. SSL_set1_host folds the check into chain
    // verification, so a mismatch fails SSL_connect rather than needing a
    // separate post-handshake test that a later edit could drop. See ICP 0022.
    if (!opts.insecure && ::SSL_set1_host(ssl.Get(), sni.c_str()) != 1)
    {
        return microtel::Unexpected<microtel::Error>{
            {.kind = microtel::Error::Kind::Network, .message = "TLS verify hostname rejected"}};
    }

    const auto deadline = std::chrono::steady_clock::now() + opts.tls_handshake_timeout;
    auto conn = SslConnectLoop(ssl.Get(), fd, deadline);
    if (!conn)
    {
        return microtel::Unexpected<microtel::Error>{conn.error()};
    }

    return std::make_pair(std::move(ssl_ctx), std::move(ssl));
}

}  // namespace microtel::transport
