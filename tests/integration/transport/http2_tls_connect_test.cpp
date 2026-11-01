// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0
//
// Integration test: TLS server-certificate verification (ICP 0022).
//
// Spins an in-process HTTP/2-over-TLS server on a loopback port, fronted by a
// certificate generated at runtime, and drives Http2Transport::Connect against
// it under four trust configurations.  Everything is generated in-process:
// there is no checked-in key material and nothing shells out to `openssl`.

#include "microtel/error.hpp"
#include "microtel/protocol.hpp"

#include "transport/epoll_reactor.hpp"
#include "transport/http2_transport.hpp"

#include <gtest/gtest.h>
#include <nghttp2/nghttp2.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <future>
#include <memory>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

namespace mtt = microtel::transport;
namespace mti = microtel::internal;

namespace
{

// ---------------------------------------------------------------------------
// RAII wrappers for the OpenSSL objects this test owns.
//
// src/common/raii/ carries SslCtx and SslSession, but not X509, EVP_PKEY or
// BIO; a unique_ptr with a deleter covers all five uniformly and keeps every
// one of these types rule-of-zero.
// ---------------------------------------------------------------------------

struct SslCtxDeleter
{
    void operator()(SSL_CTX* p) const noexcept
    {
        ::SSL_CTX_free(p);
    }
};

struct SslDeleter
{
    void operator()(SSL* p) const noexcept
    {
        ::SSL_free(p);
    }
};

struct PkeyDeleter
{
    void operator()(EVP_PKEY* p) const noexcept
    {
        ::EVP_PKEY_free(p);
    }
};

struct X509Deleter
{
    void operator()(X509* p) const noexcept
    {
        ::X509_free(p);
    }
};

struct BioDeleter
{
    void operator()(BIO* p) const noexcept
    {
        ::BIO_free(p);
    }
};

using SslCtxPtr = std::unique_ptr<SSL_CTX, SslCtxDeleter>;
using SslPtr = std::unique_ptr<SSL, SslDeleter>;
using PkeyPtr = std::unique_ptr<EVP_PKEY, PkeyDeleter>;
using X509Ptr = std::unique_ptr<X509, X509Deleter>;
using BioPtr = std::unique_ptr<BIO, BioDeleter>;

/// A key pair and the self-signed certificate that carries its public half.
struct Credential
{
    PkeyPtr key;
    X509Ptr cert;
};

// ---------------------------------------------------------------------------
// Runtime certificate generation
// ---------------------------------------------------------------------------

constexpr long kCertLifetimeSeconds = 3600;
constexpr long kCertSerial = 1;
constexpr long kX509Version3 = 2;  // X.509 v3 is encoded as 2.

void AddExtension(X509* cert, int nid, const char* value) noexcept
{
    X509V3_CTX ext_ctx{};
    ::X509V3_set_ctx(&ext_ctx, cert, cert, nullptr, nullptr, 0);
    X509_EXTENSION* const ext = ::X509V3_EXT_conf_nid(nullptr, &ext_ctx, nid, value);
    if (ext != nullptr)
    {
        ::X509_add_ext(cert, ext, -1);
        ::X509_EXTENSION_free(ext);
    }
}

/// Generate a self-signed P-256 certificate for @p common_name carrying @p san
/// (an X509v3 `subjectAltName` value such as `"DNS:localhost"`).
///
/// `CA:TRUE` is set so the certificate can also serve as its own trust anchor
/// when a test hands it to the client as a `ca_bundle`.
Credential MakeSelfSignedCert(const std::string& common_name, const char* san)
{
    Credential cred;
    cred.key.reset(::EVP_PKEY_Q_keygen(nullptr, nullptr, "EC", "P-256"));
    if (!cred.key)
    {
        return cred;
    }

    cred.cert.reset(::X509_new());
    if (!cred.cert)
    {
        return cred;
    }

    X509* const cert = cred.cert.get();
    ::X509_set_version(cert, kX509Version3);
    ::ASN1_INTEGER_set(::X509_get_serialNumber(cert), kCertSerial);
    ::X509_gmtime_adj(::X509_getm_notBefore(cert), 0);
    ::X509_gmtime_adj(::X509_getm_notAfter(cert), kCertLifetimeSeconds);
    ::X509_set_pubkey(cert, cred.key.get());

    X509_NAME* const name = ::X509_get_subject_name(cert);
    const auto* const cn = reinterpret_cast<const unsigned char*>(common_name.c_str());
    ::X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC, cn, -1, -1, 0);
    ::X509_set_issuer_name(cert, name);

    AddExtension(cert, NID_basic_constraints, "critical,CA:TRUE");
    AddExtension(cert, NID_subject_alt_name, san);

    ::X509_sign(cert, cred.key.get(), ::EVP_sha256());
    return cred;
}

/// A PEM copy of a certificate on disk, for the `ConnectOptions::ca_bundle`
/// path, removed when the test that made it goes out of scope.
class TempPemFile
{
public:
    explicit TempPemFile(X509* cert)
    {
        static std::atomic<int> s_counter{0};
        m_path = std::filesystem::temp_directory_path() /
                 ("microtel-tls-test-" + std::to_string(::getpid()) + "-" +
                  std::to_string(s_counter.fetch_add(1)) + ".pem");

        const BioPtr bio{::BIO_new_file(m_path.string().c_str(), "wb")};
        if (bio)
        {
            ::PEM_write_bio_X509(bio.get(), cert);
        }
    }

    ~TempPemFile()
    {
        std::error_code ec;
        std::filesystem::remove(m_path, ec);
    }

    TempPemFile(const TempPemFile&) = delete;
    TempPemFile& operator=(const TempPemFile&) = delete;
    TempPemFile(TempPemFile&&) = delete;
    TempPemFile& operator=(TempPemFile&&) = delete;

    [[nodiscard]] const std::filesystem::path& Path() const noexcept
    {
        return m_path;
    }

private:
    std::filesystem::path m_path;
};

// ---------------------------------------------------------------------------
// Minimal in-process HTTP/2-over-TLS server (loopback only)
// ---------------------------------------------------------------------------

struct TlsServerCtx
{
    SSL* ssl = nullptr;
    std::atomic<bool> settings_ack_received{false};
    /// A client request's HEADERS arrived while the server was still waiting
    /// for its SETTINGS ACK: the client had its ACK and burst in flight together.
    bool request_seen = false;
};

ssize_t SrvTlsSend(
    nghttp2_session* /*s*/, const uint8_t* data, size_t len, int /*flags*/, void* ud) noexcept
{
    SSL* const ssl = static_cast<TlsServerCtx*>(ud)->ssl;
    const int n = ::SSL_write(ssl, data, static_cast<int>(len));
    if (n <= 0)
    {
        const int err = ::SSL_get_error(ssl, n);
        return (err == SSL_ERROR_WANT_WRITE || err == SSL_ERROR_WANT_READ)
                   ? static_cast<ssize_t>(NGHTTP2_ERR_WOULDBLOCK)
                   : static_cast<ssize_t>(NGHTTP2_ERR_CALLBACK_FAILURE);
    }
    return n;
}

ssize_t SrvTlsRecv(
    nghttp2_session* /*s*/, uint8_t* buf, size_t len, int /*flags*/, void* ud) noexcept
{
    SSL* const ssl = static_cast<TlsServerCtx*>(ud)->ssl;
    const int n = ::SSL_read(ssl, buf, static_cast<int>(len));
    if (n <= 0)
    {
        const int err = ::SSL_get_error(ssl, n);
        return (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE)
                   ? static_cast<ssize_t>(NGHTTP2_ERR_WOULDBLOCK)
                   : static_cast<ssize_t>(NGHTTP2_ERR_CALLBACK_FAILURE);
    }
    return n;
}

int SrvTlsOnFrameRecv(nghttp2_session* /*s*/, const nghttp2_frame* frame, void* ud) noexcept
{
    if (frame->hd.type == NGHTTP2_SETTINGS && (frame->hd.flags & NGHTTP2_FLAG_ACK) != 0U)
    {
        static_cast<TlsServerCtx*>(ud)->settings_ack_received.store(true,
                                                                    std::memory_order_release);
    }
    if (frame->hd.type == NGHTTP2_HEADERS)
    {
        static_cast<TlsServerCtx*>(ud)->request_seen = true;
    }
    return 0;
}

/// What this server answers the client's ALPN offer with.
///
/// `Http2Transport` advertises only `h2`; a receiver that answers anything
/// else — or ignores ALPN entirely, which is what a stock HTTP/1.1-only TLS
/// endpoint does — cannot carry an HTTP/2 session (issue #166).
enum class ServerAlpn : std::uint8_t
{
    H2,      ///< "h2": the protocol Http2Transport requires
    Http11,  ///< "http/1.1": an HTTP/1.1-only receiver that does speak ALPN
    None,    ///< no ALPN callback at all: no protocol in the ServerHello
};

/// When and how this server tears the connection down.
///
/// The two reset modes exist for issue #177: microtel's TLS writes go through
/// OpenSSL, so `MSG_NOSIGNAL` is only reachable there via a custom `BIO`, and
/// these are the two write paths that BIO has to cover — the handshake's and
/// the record layer's.
enum class ServerClose : std::uint8_t
{
    /// Hold the connection open until `Stop()`: what every other test wants.
    HoldOpen,
    /// Reset the moment the connection is accepted, before `SSL_accept`. The
    /// client is inside `SSL_connect`, so this hits the handshake write path.
    ResetOnAccept,
    /// Serve the handshakes, then reset on the client's first post-handshake
    /// byte — its export burst, written with `SSL_write`.
    ResetOnFirstRequest,
};

/// How this server constrains the TLS protocol version it will negotiate.
///
/// Both non-default modes exist for issue #216. microtel pins a TLS 1.2 floor,
/// and a floor is only worth anything if both of its halves hold: a receiver
/// below it is refused, and a receiver sitting exactly on it still works.
enum class ServerTls : std::uint8_t
{
    Default,   ///< Whatever this OpenSSL build and the host policy permit.
    Tls11Max,  ///< Refuses TLS 1.2 and up: a legacy receiver, below the floor.
    Tls12Max,  ///< Refuses TLS 1.3: a receiver sitting exactly on the floor.
};

/// Cipher list that survives `SECLEVEL=0`, for the pre-1.2 server only.
constexpr const char* kLegacyCipherList = "ALL:@SECLEVEL=0";

/// Constrain @p ctx per @p tls.
///
/// `Tls11Max` has to fight the library to get there, and only the *test
/// server* does so — it exists to impersonate a legacy receiver, which is
/// exactly what the client under test must refuse to talk to. A stock
/// OpenSSL 3.x rejects TLS 1.1 twice over: the default security level (2 on
/// both Fedora and Ubuntu) excludes it, and the host crypto policy pins
/// `TLS.MinProtocol = TLSv1.2` into every context built from the system
/// `openssl.cnf`, which would otherwise leave this context with min > max and
/// nothing at all to negotiate. Level 0 plus an explicit floor undoes both,
/// and the cipher list restores the suites TLS 1.1 needs.
///
/// @return false when this OpenSSL cannot offer the requested version at all,
///         which is the case for a build configured with `no-tls1_1`.
[[nodiscard]] bool ApplyServerTlsVersion(SSL_CTX* ctx, ServerTls tls)
{
    if (tls == ServerTls::Default)
    {
        return true;
    }
    if (tls == ServerTls::Tls12Max)
    {
        return ::SSL_CTX_set_max_proto_version(ctx, TLS1_2_VERSION) == 1;
    }

    ::SSL_CTX_set_security_level(ctx, 0);
    return ::SSL_CTX_set_min_proto_version(ctx, TLS1_VERSION) == 1 &&
           ::SSL_CTX_set_max_proto_version(ctx, TLS1_1_VERSION) == 1 &&
           ::SSL_CTX_set_cipher_list(ctx, kLegacyCipherList) == 1 &&
           ::SSL_CTX_get_max_proto_version(ctx) == TLS1_1_VERSION;
}

/// True when this OpenSSL can be made to offer TLS 1.1 server-side at all.
///
/// A build configured with `no-tls1_1` cannot, and there the negative floor
/// test has no legacy receiver to point the client at.
[[nodiscard]] bool ServerCanOfferTls11()
{
    const SslCtxPtr ctx{::SSL_CTX_new(::TLS_server_method())};
    return ctx && ApplyServerTlsVersion(ctx.get(), ServerTls::Tls11Max);
}

/// The wire bytes of "http/1.1", without ALPN's length prefix. Static storage:
/// OpenSSL keeps the pointer the select callback hands back.
constexpr std::array<unsigned char, 8> kHttp11Wire{'h', 't', 't', 'p', '/', '1', '.', '1'};

/// Answer the client's ALPN offer per the `ServerAlpn` mode in @p arg.
int SrvAlpnSelect(SSL* /*ssl*/,
                  const unsigned char** out,
                  unsigned char* outlen,
                  const unsigned char* in,
                  unsigned int inlen,
                  void* arg) noexcept
{
    if (*static_cast<const ServerAlpn*>(arg) == ServerAlpn::Http11)
    {
        *out = kHttp11Wire.data();
        *outlen = static_cast<unsigned char>(kHttp11Wire.size());
        return SSL_TLSEXT_ERR_OK;
    }

    constexpr unsigned int kH2Len = 2;
    unsigned int i = 0;
    while (i < inlen)
    {
        const unsigned int len = in[i];
        const bool is_h2 =
            len == kH2Len && i + 1 + len <= inlen && in[i + 1] == 'h' && in[i + 2] == '2';
        if (is_h2)
        {
            *out = in + i + 1;
            *outlen = static_cast<unsigned char>(kH2Len);
            return SSL_TLSEXT_ERR_OK;
        }
        i += len + 1;
    }
    return SSL_TLSEXT_ERR_ALERT_FATAL;
}

/// @param alpn Borrowed; must outlive every connection made on the returned
///             context, since OpenSSL passes it to the select callback.
SslCtxPtr MakeServerCtx(const Credential& cred, const ServerAlpn* alpn, ServerTls tls)
{
    SslCtxPtr ctx{::SSL_CTX_new(::TLS_server_method())};
    if (!ctx)
    {
        return ctx;
    }
    if (!ApplyServerTlsVersion(ctx.get(), tls))
    {
        return SslCtxPtr{};
    }
    if (::SSL_CTX_use_certificate(ctx.get(), cred.cert.get()) != 1 ||
        ::SSL_CTX_use_PrivateKey(ctx.get(), cred.key.get()) != 1)
    {
        return SslCtxPtr{};
    }
    // No TLS 1.3 session tickets. A client that hangs up the instant the
    // handshake completes — which is exactly what the ALPN tests below make it
    // do — leaves the server writing NewSessionTicket into a closed socket,
    // and the EPIPE from that raises SIGPIPE in this single-process test.
    ::SSL_CTX_set_num_tickets(ctx.get(), 0);
    if (*alpn != ServerAlpn::None)
    {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast)
        ::SSL_CTX_set_alpn_select_cb(ctx.get(), SrvAlpnSelect, const_cast<ServerAlpn*>(alpn));
    }
    return ctx;
}

class TlsHttp2Server
{
public:
    TlsHttp2Server() = default;

    ~TlsHttp2Server()
    {
        Stop();
    }

    TlsHttp2Server(const TlsHttp2Server&) = delete;
    TlsHttp2Server& operator=(const TlsHttp2Server&) = delete;
    TlsHttp2Server(TlsHttp2Server&&) = delete;
    TlsHttp2Server& operator=(TlsHttp2Server&&) = delete;

    /// Bind 127.0.0.1:0, listen, start the accept thread.  Returns the
    /// assigned port, or -1.
    int Start(const Credential& cred,
              ServerAlpn alpn = ServerAlpn::H2,
              ServerClose close = ServerClose::HoldOpen,
              ServerTls tls = ServerTls::Default)
    {
        m_alpn = alpn;
        m_close = close;
        m_ctx = MakeServerCtx(cred, &m_alpn, tls);
        if (!m_ctx)
        {
            return -1;
        }

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

        if (::bind(m_listen_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0 ||
            ::listen(m_listen_fd, 1) < 0)
        {
            ::close(m_listen_fd);
            m_listen_fd = -1;
            return -1;
        }

        socklen_t len = sizeof(addr);
        ::getsockname(m_listen_fd, reinterpret_cast<sockaddr*>(&addr), &len);
        m_port = ntohs(addr.sin_port);

        m_thread = std::thread([this] { ServerThread(); });
        return m_port;
    }

    /// True once a client has completed both the TLS handshake and the HTTP/2
    /// SETTINGS exchange.
    [[nodiscard]] bool WaitForHandshake(std::chrono::milliseconds timeout) const
    {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (!m_handshake_done.load(std::memory_order_acquire))
        {
            if (std::chrono::steady_clock::now() >= deadline)
            {
                return false;
            }
            std::this_thread::sleep_for(kPollInterval);
        }
        return true;
    }

    /// True once this server has reset the client's connection. Only the reset
    /// modes ever set it; every other mode's connection closes cleanly.
    [[nodiscard]] bool WaitForReset(std::chrono::milliseconds timeout) const
    {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (!m_reset_sent.load(std::memory_order_acquire))
        {
            if (std::chrono::steady_clock::now() >= deadline)
            {
                return false;
            }
            std::this_thread::sleep_for(kPollInterval);
        }
        return true;
    }

    void Stop()
    {
        m_stop.store(true, std::memory_order_release);
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
    static constexpr auto kPollInterval = std::chrono::milliseconds(5);
    static constexpr int kPollMs = 50;
    static constexpr int kHttp2TimeoutMs = 5000;
    static constexpr int kAcceptTimeoutSec = 5;
    static constexpr std::uint32_t kMaxConcurrent = 100;

    void ServerThread()
    {
        const int client_fd = ::accept(m_listen_fd, nullptr, nullptr);
        if (client_fd < 0)
        {
            return;
        }
        const bool reset = RunConnection(client_fd);
        if (reset)
        {
            // Zero linger turns close() into an RST, so the client's in-flight
            // writes fail now rather than whenever the peer's kernel gets round
            // to answering them.
            const linger zero_linger{.l_onoff = 1, .l_linger = 0};
            ::setsockopt(client_fd, SOL_SOCKET, SO_LINGER, &zero_linger, sizeof(zero_linger));
        }
        ::close(client_fd);
        m_reset_sent.store(reset, std::memory_order_release);
    }

    /// @return true when the connection must be reset rather than closed.
    bool RunConnection(int fd)
    {
        if (m_close == ServerClose::ResetOnAccept)
        {
            return true;
        }

        // Bound the blocking SSL_accept: a client that rejects our certificate
        // may hang up without a word, and the accept thread must still exit.
        const timeval tv{.tv_sec = kAcceptTimeoutSec, .tv_usec = 0};
        ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        const SslPtr ssl{::SSL_new(m_ctx.get())};
        if (!ssl)
        {
            return false;
        }
        ::SSL_set_fd(ssl.get(), fd);
        if (::SSL_accept(ssl.get()) != 1)
        {
            // Expected whenever the client refuses the certificate.
            return false;
        }

        SetNonBlocking(fd);
        bool request_seen = false;
        if (m_alpn == ServerAlpn::H2)
        {
            // A receiver that did not agree to h2 would not run an HTTP/2
            // session either — and the client hangs up as soon as it reads
            // the ALPN answer, so there would be nobody to talk to.
            request_seen = RunHttp2(ssl.get(), fd);
        }
        m_handshake_done.store(true, std::memory_order_release);

        if (m_close == ServerClose::ResetOnFirstRequest)
        {
            // The client's burst can land in the same read as its SETTINGS
            // ACK, and then the SETTINGS loop has already consumed all of it:
            // a client blocked on its concurrent-stream limit sends nothing
            // more, so waiting for "the next byte" would wait until Stop and
            // no reset would ever happen (issue #274). The burst has started
            // either way, so reset now.
            return request_seen || WaitForClientBytes(fd);
        }

        // Hold the connection open until Stop(): closing straight after the
        // handshake makes the client's Connected state transient and races the
        // assertions (see http2_connect_test.cpp).
        while (!m_stop.load(std::memory_order_acquire))
        {
            std::this_thread::sleep_for(kPollInterval);
        }
        return false;
    }

    /// Stop reading and wait for the client's first post-handshake byte, so the
    /// reset lands in the middle of its export burst rather than before it.
    /// @return true — this connection always ends with an RST.
    [[nodiscard]] bool WaitForClientBytes(int fd) const
    {
        while (!m_stop.load(std::memory_order_acquire))
        {
            pollfd pfd{.fd = fd, .events = POLLIN, .revents = 0};
            if (::poll(&pfd, 1, kPollMs) > 0)
            {
                return true;
            }
        }
        return true;
    }

    static void SetNonBlocking(int fd) noexcept
    {
        const int flags = ::fcntl(fd, F_GETFL);
        // NOLINTNEXTLINE(hicpp-signed-bitwise)
        ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    }

    /// Serve the HTTP/2 SETTINGS exchange.
    /// @return true if a client request arrived during it.
    static bool RunHttp2(SSL* ssl, int fd)
    {
        TlsServerCtx ctx;
        ctx.ssl = ssl;

        nghttp2_session_callbacks* cbs = nullptr;
        ::nghttp2_session_callbacks_new(&cbs);
        ::nghttp2_session_callbacks_set_send_callback(cbs, SrvTlsSend);
        ::nghttp2_session_callbacks_set_recv_callback(cbs, SrvTlsRecv);
        ::nghttp2_session_callbacks_set_on_frame_recv_callback(cbs, SrvTlsOnFrameRecv);

        nghttp2_session* session = nullptr;
        ::nghttp2_session_server_new(&session, cbs, &ctx);
        ::nghttp2_session_callbacks_del(cbs);

        const nghttp2_settings_entry iv[1] = {
            {.settings_id = NGHTTP2_SETTINGS_MAX_CONCURRENT_STREAMS, .value = kMaxConcurrent}};
        ::nghttp2_submit_settings(session, NGHTTP2_FLAG_NONE, iv, 1);

        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(kHttp2TimeoutMs);
        while (!ctx.settings_ack_received.load(std::memory_order_acquire) &&
               std::chrono::steady_clock::now() < deadline)
        {
            ::nghttp2_session_send(session);
            pollfd pfd{.fd = fd, .events = POLLIN, .revents = 0};
            if (::poll(&pfd, 1, kPollMs) > 0)
            {
                ::nghttp2_session_recv(session);
                ::nghttp2_session_send(session);
            }
        }

        ::nghttp2_session_del(session);
        return ctx.request_seen;
    }

    SslCtxPtr m_ctx;
    ServerAlpn m_alpn = ServerAlpn::H2;
    ServerClose m_close = ServerClose::HoldOpen;
    int m_listen_fd = -1;
    int m_port = 0;
    std::thread m_thread;
    std::atomic<bool> m_handshake_done{false};
    std::atomic<bool> m_reset_sent{false};
    std::atomic<bool> m_stop{false};
};

// ---------------------------------------------------------------------------
// Client-side helper
// ---------------------------------------------------------------------------

constexpr auto kConnectTimeout = std::chrono::milliseconds(5000);

struct ConnectOutcome
{
    bool connected = false;
    microtel::Error::Kind kind = microtel::Error::Kind::Unspecified;
    std::string message;
    /// `Http2Transport::PeerSpeaksOnlyHttp1` after the attempt (ICP 0042).
    bool http1_only = false;
};

/// Build a transport, run one `Connect`, close it, and report what happened.
ConnectOutcome ConnectOnce(const mti::ConnectOptions& opts)
{
    auto reactor_result = mtt::EpollReactor::Create();
    if (!reactor_result)
    {
        return ConnectOutcome{.connected = false,
                              .kind = microtel::Error::Kind::InternalFailure,
                              .message = "EpollReactor::Create failed"};
    }
    auto transport_result = mtt::Http2Transport::Create(std::move(*reactor_result));
    if (!transport_result)
    {
        return ConnectOutcome{.connected = false,
                              .kind = microtel::Error::Kind::InternalFailure,
                              .message = "Http2Transport::Create failed"};
    }
    auto& transport = *transport_result;

    const auto result = transport->Connect(opts);
    ConnectOutcome outcome;
    outcome.connected = result.has_value();
    if (!result)
    {
        outcome.kind = result.error().kind;
        outcome.message = result.error().message;
    }
    outcome.http1_only = transport->PeerSpeaksOnlyHttp1();
    (void)transport->Close(std::chrono::milliseconds(1000));
    return outcome;
}

/// Size of the export burst the reset tests fire. Big enough that the I/O
/// thread is still draining it when the peer's RST lands.
constexpr int kBurstRequests = 256;
constexpr std::size_t kBurstPayloadBytes = 256;
constexpr auto kStatePollInterval = std::chrono::milliseconds(20);

/// Poll until @p transport reports @p want, or the deadline passes: the drop
/// is noticed on the I/O thread, so this cannot be a bare read.
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
        std::this_thread::sleep_for(kStatePollInterval);
    }
    return transport.GetState() == want;
}

mti::ConnectOptions MakeOptions(int port)
{
    mti::ConnectOptions opts;
    opts.endpoint = "https://127.0.0.1:" + std::to_string(port);
    opts.insecure = false;
    opts.connect_timeout = kConnectTimeout;
    opts.tls_handshake_timeout = kConnectTimeout;
    return opts;
}

}  // namespace

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

// The gap ICP 0022 closes: without SSL_CTX_set_verify, OpenSSL's
// SSL_VERIFY_NONE default means the trust store loaded from `ca_bundle` is
// never consulted and the handshake completes against any certificate at all.
TEST(Http2TlsConnectTest, UntrustedCert_ConnectFails)
{
    const Credential server_cred = MakeSelfSignedCert("localhost", "DNS:localhost");
    ASSERT_TRUE(server_cred.cert);
    const Credential other_ca = MakeSelfSignedCert("unrelated-ca.invalid", "DNS:unrelated.invalid");
    ASSERT_TRUE(other_ca.cert);
    const TempPemFile trust{other_ca.cert.get()};

    TlsHttp2Server server;
    const int port = server.Start(server_cred);
    ASSERT_GT(port, 0);

    auto opts = MakeOptions(port);
    opts.ca_bundle = trust.Path();
    // Pin the name so the only thing left for the client to object to is the
    // chain: this test is about trust, not about hostnames.
    opts.sni_override = "localhost";

    const auto outcome = ConnectOnce(opts);
    EXPECT_FALSE(outcome.connected)
        << "a certificate signed by a CA outside ca_bundle must not be accepted";
    EXPECT_NE(outcome.message.find("certificate verification failed"), std::string::npos)
        << "error was: " << outcome.message;
}

// The positive half of the pair with HostnameMismatch_ConnectFails below: same
// certificate, same trust anchor, and the connection succeeds precisely
// because `sni_override` names what the certificate was issued to.
TEST(Http2TlsConnectTest, TrustedCa_ConnectSucceeds)
{
    const Credential server_cred = MakeSelfSignedCert("localhost", "DNS:localhost");
    ASSERT_TRUE(server_cred.cert);
    const TempPemFile trust{server_cred.cert.get()};

    TlsHttp2Server server;
    const int port = server.Start(server_cred);
    ASSERT_GT(port, 0);

    auto opts = MakeOptions(port);
    opts.ca_bundle = trust.Path();
    opts.sni_override = "localhost";

    const auto outcome = ConnectOnce(opts);
    EXPECT_TRUE(outcome.connected) << "error was: " << outcome.message;
    EXPECT_TRUE(server.WaitForHandshake(kConnectTimeout))
        << "the server never completed TLS + SETTINGS";
}

// `insecure = true` keeps the old, unverified behaviour on purpose: the
// connection is still TLS (the server does an SSL_accept), but nothing about
// the certificate is checked.
TEST(Http2TlsConnectTest, Insecure_UntrustedCert_ConnectSucceeds)
{
    const Credential server_cred = MakeSelfSignedCert("localhost", "DNS:localhost");
    ASSERT_TRUE(server_cred.cert);

    TlsHttp2Server server;
    const int port = server.Start(server_cred);
    ASSERT_GT(port, 0);

    auto opts = MakeOptions(port);
    opts.insecure = true;  // no ca_bundle, untrusted certificate, accepted anyway

    const auto outcome = ConnectOnce(opts);
    EXPECT_TRUE(outcome.connected) << "error was: " << outcome.message;
    EXPECT_TRUE(server.WaitForHandshake(kConnectTimeout))
        << "insecure = true must still speak TLS, just without verification";
}

// Trust is fine and the name is not: the certificate is issued to `localhost`
// and the client connects to 127.0.0.1 without an override.  Compare against
// TrustedCa_ConnectSucceeds, which differs only in setting `sni_override` —
// together they show that the verified name follows the override.
TEST(Http2TlsConnectTest, HostnameMismatch_ConnectFails)
{
    const Credential server_cred = MakeSelfSignedCert("localhost", "DNS:localhost");
    ASSERT_TRUE(server_cred.cert);
    const TempPemFile trust{server_cred.cert.get()};

    TlsHttp2Server server;
    const int port = server.Start(server_cred);
    ASSERT_GT(port, 0);

    auto opts = MakeOptions(port);
    opts.ca_bundle = trust.Path();

    const auto outcome = ConnectOnce(opts);
    EXPECT_FALSE(outcome.connected)
        << "a certificate issued to another name must not be accepted for this endpoint";
    EXPECT_NE(outcome.message.find("certificate verification failed"), std::string::npos)
        << "error was: " << outcome.message;
}

// ---------------------------------------------------------------------------
// ALPN (issue #166)
//
// For OTLP/gRPC, `Http2Transport` advertises only `h2`, so a TLS endpoint that
// answers with anything else — or ignores ALPN entirely, which is what an
// HTTP/1.1-only TLS receiver does — cannot carry a session. (For OTLP/HTTP it
// offers `h2,http/1.1`, and those answers are the fallback signal; see the
// ICP 0042 tests below.) Nothing
// checked the negotiated protocol before these tests: the connection went on
// to the HTTP/2 preface and died there as a generic nghttp2 recv failure,
// which named neither ALPN nor the endpoint's actual protocol.
// ---------------------------------------------------------------------------

// A receiver that answers ALPN with `http/1.1` — a protocol microtel never
// offered, since it offers only `h2`.
//
// Which layer catches this depends on the linked OpenSSL, and both are correct
// outcomes: OpenSSL 3.2+ enforces RFC 7301's "must be one the client offered"
// and aborts the handshake before any microtel code runs (`Network`), while
// 3.0 and 3.1 let the ServerHello through and microtel's own ALPN check is
// what refuses it (`Protocol`). CI runs the first, Fedora the second. The
// assertion is on what holds either way — the connection is refused — plus,
// where microtel is the one refusing, that its message names what the peer
// chose.
TEST(Http2TlsConnectTest, AlpnAnswersHttp11_ConnectFails)
{
    const Credential server_cred = MakeSelfSignedCert("localhost", "DNS:localhost");
    ASSERT_TRUE(server_cred.cert);

    TlsHttp2Server server;
    const int port = server.Start(server_cred, ServerAlpn::Http11);
    ASSERT_GT(port, 0);

    auto opts = MakeOptions(port);
    opts.insecure = true;  // the certificate is not what this test is about

    const auto outcome = ConnectOnce(opts);
    ASSERT_FALSE(outcome.connected) << "a peer that answered http/1.1 cannot speak HTTP/2";
    if (outcome.kind == microtel::Error::Kind::Protocol)
    {
        EXPECT_NE(outcome.message.find("http/1.1"), std::string::npos)
            << "microtel's ALPN check must name what was negotiated; error was: "
            << outcome.message;
    }
    else
    {
        EXPECT_EQ(outcome.kind, microtel::Error::Kind::Network)
            << "the only other acceptable refusal is OpenSSL failing the handshake; error was: "
            << outcome.message;
    }
}

TEST(Http2TlsConnectTest, AlpnNotNegotiated_ConnectFailsWithProtocolError)
{
    const Credential server_cred = MakeSelfSignedCert("localhost", "DNS:localhost");
    ASSERT_TRUE(server_cred.cert);

    TlsHttp2Server server;
    const int port = server.Start(server_cred, ServerAlpn::None);
    ASSERT_GT(port, 0);

    auto opts = MakeOptions(port);
    opts.insecure = true;

    const auto outcome = ConnectOnce(opts);
    EXPECT_FALSE(outcome.connected) << "no ALPN answer means no agreement to speak h2";
    EXPECT_EQ(outcome.kind, microtel::Error::Kind::Protocol);
    EXPECT_NE(outcome.message.find("none"), std::string::npos) << "error was: " << outcome.message;
}

// ---------------------------------------------------------------------------
// ALPN for OTLP/HTTP: h2, then http/1.1 (ICP 0042 §1)
//
// For `Protocol::Http` the transport also offers http/1.1, so an HTTP/1.1-only
// receiver can answer honestly, and reports such an answer, or none, through
// `PeerSpeaksOnlyHttp1` for `FallbackTransport`. gRPC still offers h2 alone.
// ---------------------------------------------------------------------------

TEST(Http2TlsConnectTest, OtlpHttp_H2Server_StillConnectsOverH2)
{
    const Credential server_cred = MakeSelfSignedCert("localhost", "DNS:localhost");
    ASSERT_TRUE(server_cred.cert);
    TlsHttp2Server server;
    const int port = server.Start(server_cred, ServerAlpn::H2);
    ASSERT_GT(port, 0);

    auto opts = MakeOptions(port);
    opts.insecure = true;
    opts.protocol = microtel::Protocol::Http;
    const auto outcome = ConnectOnce(opts);
    EXPECT_TRUE(outcome.connected) << outcome.message;
    EXPECT_FALSE(outcome.http1_only);
}

TEST(Http2TlsConnectTest, OtlpHttp_Http11Server_IsReportedHttp1Only)
{
    const Credential server_cred = MakeSelfSignedCert("localhost", "DNS:localhost");
    ASSERT_TRUE(server_cred.cert);
    TlsHttp2Server server;
    const int port = server.Start(server_cred, ServerAlpn::Http11);
    ASSERT_GT(port, 0);

    auto opts = MakeOptions(port);
    opts.insecure = true;
    opts.protocol = microtel::Protocol::Http;
    const auto outcome = ConnectOnce(opts);
    EXPECT_FALSE(outcome.connected);
    EXPECT_EQ(outcome.kind, microtel::Error::Kind::Protocol) << outcome.message;
    EXPECT_TRUE(outcome.http1_only);
}

TEST(Http2TlsConnectTest, OtlpHttp_NoAlpn_IsReportedHttp1Only)
{
    const Credential server_cred = MakeSelfSignedCert("localhost", "DNS:localhost");
    ASSERT_TRUE(server_cred.cert);
    TlsHttp2Server server;
    const int port = server.Start(server_cred, ServerAlpn::None);
    ASSERT_GT(port, 0);

    auto opts = MakeOptions(port);
    opts.insecure = true;
    opts.protocol = microtel::Protocol::Http;
    const auto outcome = ConnectOnce(opts);
    EXPECT_FALSE(outcome.connected);
    EXPECT_TRUE(outcome.http1_only);
}

TEST(Http2TlsConnectTest, Grpc_NoAlpn_IsNotReportedHttp1Only)
{
    const Credential server_cred = MakeSelfSignedCert("localhost", "DNS:localhost");
    ASSERT_TRUE(server_cred.cert);
    TlsHttp2Server server;
    const int port = server.Start(server_cred, ServerAlpn::None);
    ASSERT_GT(port, 0);

    auto opts = MakeOptions(port);
    opts.insecure = true;
    opts.protocol = microtel::Protocol::Grpc;
    const auto outcome = ConnectOnce(opts);
    EXPECT_FALSE(outcome.connected);
    EXPECT_FALSE(outcome.http1_only) << "gRPC cannot use HTTP/1.1, so it never asks";
}

// ---------------------------------------------------------------------------
// The TLS 1.2 floor (issue #216)
//
// microtel pins the minimum protocol version itself rather than inheriting
// whatever the linked OpenSSL happens to permit.  Without that, the same
// source linked two ways negotiates two different security floors: TLS 1.2
// against an OpenSSL 3.x under a strict host crypto policy, TLS 1.0 against a
// default-built OpenSSL 1.1.1 — which microtel still accepts.
//
// Note on what these two prove where.  A host whose OpenSSL refuses TLS 1.1
// outright — seclevel 2 by default plus `TLS.MinProtocol = TLSv1.2` from the
// system openssl.cnf, which is Fedora and Ubuntu both — was already doing
// microtel's job for it, so the negative test passed there before the floor
// existed in code.  It is load-bearing on the permissive builds, and it is a
// regression test everywhere: it fails the moment the floor is removed on a
// host that would otherwise allow the downgrade.
// ---------------------------------------------------------------------------

TEST(Http2TlsConnectTest, TlsFloor_ServerLimitedToTls11_ConnectFails)
{
    if (!ServerCanOfferTls11())
    {
        GTEST_SKIP() << "this OpenSSL build cannot offer TLS 1.1 server-side";
    }

    const Credential server_cred = MakeSelfSignedCert("localhost", "DNS:localhost");
    ASSERT_TRUE(server_cred.cert);
    const TempPemFile trust{server_cred.cert.get()};

    TlsHttp2Server server;
    const int port =
        server.Start(server_cred, ServerAlpn::H2, ServerClose::HoldOpen, ServerTls::Tls11Max);
    ASSERT_GT(port, 0);

    auto opts = MakeOptions(port);
    // Trust anchor and name are both in order, so the protocol version is the
    // only thing left for the client to refuse over.
    opts.ca_bundle = trust.Path();
    opts.sni_override = "localhost";

    const auto outcome = ConnectOnce(opts);
    EXPECT_FALSE(outcome.connected) << "a receiver that will not go above TLS 1.1 must be refused";
    EXPECT_EQ(outcome.kind, microtel::Error::Kind::Network);
    EXPECT_NE(outcome.message.find("TLS handshake failed"), std::string::npos)
        << "the refusal belongs to the handshake, not to certificate verification; error was: "
        << outcome.message;
}

// The other half of the floor: 1.2 is the minimum, not the minimum plus one.
// A receiver capped at exactly TLS 1.2 is an ordinary conservative deployment
// and must still connect — this fails if the floor is ever raised to
// TLS1_3_VERSION by mistake.
TEST(Http2TlsConnectTest, TlsFloor_Tls12Server_StillConnects)
{
    const Credential server_cred = MakeSelfSignedCert("localhost", "DNS:localhost");
    ASSERT_TRUE(server_cred.cert);
    const TempPemFile trust{server_cred.cert.get()};

    TlsHttp2Server server;
    const int port =
        server.Start(server_cred, ServerAlpn::H2, ServerClose::HoldOpen, ServerTls::Tls12Max);
    ASSERT_GT(port, 0);

    auto opts = MakeOptions(port);
    opts.ca_bundle = trust.Path();
    opts.sni_override = "localhost";

    const auto outcome = ConnectOnce(opts);
    EXPECT_TRUE(outcome.connected) << "error was: " << outcome.message;
    EXPECT_TRUE(server.WaitForHandshake(kConnectTimeout))
        << "a TLS 1.2 receiver sits on the floor, not below it";
}

// ---------------------------------------------------------------------------
// A peer that resets a TLS connection (issue #177)
//
// The plaintext paths take `MSG_NOSIGNAL` at the `::send` call; OpenSSL owns
// the TLS ones, so microtel hands `SSL` a custom `BIO` that does the same.
// These two tests walk the two write paths that BIO carries — the handshake's
// (`SSL_connect`) and the record layer's (`SSL_write`) — with a peer that
// resets underneath them.
//
// In both, the assertion that matters is reaching the end of the function at
// all: with OpenSSL's stock socket BIO the second write on a reset connection
// raises `SIGPIPE`, whose default disposition terminates the test binary.
// ---------------------------------------------------------------------------

TEST(Http2TlsConnectTest, PeerResetsDuringTlsHandshake_ProcessSurvives)
{
    const Credential server_cred = MakeSelfSignedCert("localhost", "DNS:localhost");
    ASSERT_TRUE(server_cred.cert);

    TlsHttp2Server server;
    const int port = server.Start(server_cred, ServerAlpn::H2, ServerClose::ResetOnAccept);
    ASSERT_GT(port, 0);

    auto opts = MakeOptions(port);
    opts.insecure = true;  // the certificate is not what this test is about

    const auto outcome = ConnectOnce(opts);
    EXPECT_FALSE(outcome.connected) << "a peer that reset cannot have completed a TLS handshake";
    EXPECT_FALSE(outcome.message.empty()) << "the failure must still be reported to the caller";
}

TEST(Http2TlsConnectTest, PeerResetsUnderTlsExportBurst_ProcessSurvives)
{
    const Credential server_cred = MakeSelfSignedCert("localhost", "DNS:localhost");
    ASSERT_TRUE(server_cred.cert);

    TlsHttp2Server server;
    const int port = server.Start(server_cred, ServerAlpn::H2, ServerClose::ResetOnFirstRequest);
    ASSERT_GT(port, 0);

    auto reactor_result = mtt::EpollReactor::Create();
    ASSERT_TRUE(reactor_result.has_value());
    auto transport_result = mtt::Http2Transport::Create(std::move(*reactor_result));
    ASSERT_TRUE(transport_result.has_value());
    auto& transport = *transport_result;

    auto opts = MakeOptions(port);
    opts.insecure = true;
    ASSERT_TRUE(transport->Connect(opts).has_value());

    // A burst, because Linux answers the first write on a reset socket with
    // ECONNRESET and only the second with EPIPE — see the equivalent test in
    // http2_send_test.cpp. Borrowed payload: it must outlive the awaits below.
    const std::vector<std::byte> payload(kBurstPayloadBytes, std::byte{0x5A});
    const std::string authority = "127.0.0.1:" + std::to_string(port);

    std::vector<mti::RequestHandle> handles;
    handles.reserve(kBurstRequests);
    for (int i = 0; i < kBurstRequests; ++i)
    {
        mti::RequestSpec spec{
            .headers = {{.name = ":method", .value = "POST"},
                        {.name = ":scheme", .value = "https"},
                        {.name = ":path", .value = "/v1/traces"},
                        {.name = ":authority", .value = authority},
                        {.name = "content-type", .value = "application/x-protobuf"}},
            .payload = std::span<const std::byte>{payload},
            .deadline = std::chrono::milliseconds(3000),
        };
        handles.push_back(transport->Send(std::move(spec)));
    }

    // The server's half first: without it, a Connected below could mean the
    // transport missed a reset, or that no reset was ever sent (issue #274).
    EXPECT_TRUE(server.WaitForReset(kConnectTimeout))
        << "the server must reset the connection while the export burst is in flight";
    EXPECT_TRUE(
        WaitForState(*transport, microtel::ConnectionState::Reconnecting, std::chrono::seconds(5)))
        << "a peer reset must retire the connection, not the process — state was "
        << static_cast<int>(transport->GetState());

    EXPECT_EQ(transport->Close(std::chrono::milliseconds(2000)), microtel::Status::Completed);

    for (auto& handle : handles)
    {
        if (!handle.Future().valid())
        {
            continue;
        }
        ASSERT_EQ(handle.Future().wait_for(std::chrono::seconds(5)), std::future_status::ready);
        EXPECT_FALSE(handle.Future().get().success)
            << "the peer went away; no request can have succeeded";
    }

    server.Stop();
}
