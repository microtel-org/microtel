// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0
//
// A scripted HTTP/1.1 server on loopback, plaintext or TLS, for the transport
// tests (ICP 0042).
//
// One connection at a time, served on its own thread: it reads a request
// (head and `Content-Length` body), records it, and writes whatever the
// script returns for it, then keeps the connection open, closes it, or resets
// it. That is enough to play a keep-alive receiver, a `Connection: close`
// receiver, one whose idle timeout races a request, and one that dies
// mid-response. It also notes when a request arrived pipelined behind the one
// before it, which a serial client must never do.
//
// Not a fake in the `tests/fakes/` sense: it does real socket I/O on its own
// thread, which is why it lives here.

#pragma once

#include <openssl/evp.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

namespace microtel::testing
{

/// One request as the peer read it.
struct Http1Request
{
    std::string method;
    std::string target;
    std::string version;
    /// Field names lowercased, in order.
    std::vector<std::pair<std::string, std::string>> headers;
    std::string body;
    /// Which accepted connection it arrived on, from 0.
    int connection = 0;
    /// More bytes were already waiting behind it when it was read.
    bool pipelined = false;

    /// The first value of field @p name (lowercase), or empty.
    [[nodiscard]] std::string Header(std::string_view name) const
    {
        const auto it =
            std::ranges::find(headers, name, &std::pair<std::string, std::string>::first);
        return it == headers.end() ? std::string{} : it->second;
    }
    [[nodiscard]] std::size_t HeaderCount(std::string_view name) const
    {
        return static_cast<std::size_t>(
            std::ranges::count(headers, name, &std::pair<std::string, std::string>::first));
    }
};

/// What the peer does with the connection after writing a reply.
enum class After : std::uint8_t
{
    KeepOpen,
    Close,
    /// Close with an RST (zero linger).
    Reset,
};

/// The script's answer to one request.
struct Http1Reply
{
    std::string bytes;
    After after = After::KeepOpen;
    /// Wait this long before writing.
    std::chrono::milliseconds delay{0};
};

/// What the TLS peer answers to the client's ALPN offer.
enum class PeerTls : std::uint8_t
{
    None,        ///< Plaintext.
    AlpnHttp11,  ///< TLS, selecting `http/1.1`.
    AlpnNone,    ///< TLS, ignoring ALPN, as most HTTP/1.1-only servers do.
};

/// A complete response with a `Content-Length` body.
inline std::string Http1Response(int status,
                                 std::string_view body = {},
                                 std::string_view extra_fields = {})
{
    return "HTTP/1.1 " + std::to_string(status) +
           " X\r\nContent-Length: " + std::to_string(body.size()) + "\r\n" +
           std::string{extra_fields} + "\r\n" + std::string{body};
}

class FakeHttp1Peer
{
public:
    /// @param script Called on the peer thread with each request and its
    ///               index across all connections, from 0.
    using Script = std::function<Http1Reply(const Http1Request&, std::size_t)>;

    explicit FakeHttp1Peer(Script script, PeerTls tls = PeerTls::None)
        : m_script(std::move(script)), m_tls(tls)
    {
    }
    ~FakeHttp1Peer()
    {
        Stop();
    }
    FakeHttp1Peer(const FakeHttp1Peer&) = delete;
    FakeHttp1Peer& operator=(const FakeHttp1Peer&) = delete;
    FakeHttp1Peer(FakeHttp1Peer&&) = delete;
    FakeHttp1Peer& operator=(FakeHttp1Peer&&) = delete;

    /// Listen on 127.0.0.1 and start serving. @return The port, or -1.
    int Start()
    {
        if (m_tls != PeerTls::None && !SetUpTls())
        {
            return -1;
        }
        m_listen_fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
        const int one = 1;
        ::setsockopt(m_listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        // NOLINTBEGIN(cppcoreguidelines-pro-type-reinterpret-cast)
        if (::bind(m_listen_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 ||
            ::listen(m_listen_fd, kBacklog) != 0)
        {
            return -1;
        }
        socklen_t len = sizeof(addr);
        ::getsockname(m_listen_fd, reinterpret_cast<sockaddr*>(&addr), &len);
        // NOLINTEND(cppcoreguidelines-pro-type-reinterpret-cast)
        m_thread = std::thread([this] { Run(); });
        return ntohs(addr.sin_port);
    }

    /// Stop serving and close the listener, so later connects are refused.
    void Stop()
    {
        m_stop.store(true, std::memory_order_release);
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

    [[nodiscard]] std::vector<Http1Request> Requests() const
    {
        const std::scoped_lock lk{m_mu};
        return m_requests;
    }

    [[nodiscard]] int Connections() const noexcept
    {
        return m_connections.load(std::memory_order_acquire);
    }

    /// Wait until at least @p n requests have been read.
    [[nodiscard]] bool WaitForRequests(std::size_t n, std::chrono::milliseconds timeout) const
    {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline)
        {
            {
                const std::scoped_lock lk{m_mu};
                if (m_requests.size() >= n)
                {
                    return true;
                }
            }
            std::this_thread::sleep_for(kPollStep);
        }
        return false;
    }

private:
    static constexpr int kBacklog = 8;
    static constexpr int kAcceptPollMs = 20;
    static constexpr std::chrono::milliseconds kPollStep{5};
    static constexpr long kRecvTimeoutUs = 20000;
    static constexpr long kCertLifetimeSeconds = 3600;
    static constexpr std::size_t kReadChunk = 4096;

    struct SslCtxFree
    {
        void operator()(SSL_CTX* p) const noexcept
        {
            ::SSL_CTX_free(p);
        }
    };
    struct SslFree
    {
        void operator()(SSL* p) const noexcept
        {
            ::SSL_free(p);
        }
    };

    /// One accepted connection: a socket, and a TLS session over it if any.
    struct Conn
    {
        int fd = -1;
        std::unique_ptr<SSL, SslFree> ssl;
        std::string buffer;  ///< Read but not yet consumed.
    };

    static int SelectHttp11(SSL* /*ssl*/,
                            const unsigned char** out,
                            unsigned char* outlen,
                            const unsigned char* in,
                            unsigned int inlen,
                            void* /*arg*/) noexcept
    {
        static constexpr std::array<unsigned char, 9> kHttp11{
            8, 'h', 't', 't', 'p', '/', '1', '.', '1'};
        unsigned char* selected = nullptr;
        if (::SSL_select_next_proto(&selected, outlen, kHttp11.data(), kHttp11.size(), in, inlen) !=
            OPENSSL_NPN_NEGOTIATED)
        {
            return SSL_TLSEXT_ERR_NOACK;
        }
        *out = selected;
        return SSL_TLSEXT_ERR_OK;
    }

    bool SetUpTls()
    {
        const std::unique_ptr<EVP_PKEY, decltype(&::EVP_PKEY_free)> key{
            ::EVP_PKEY_Q_keygen(nullptr, nullptr, "EC", "P-256"), &::EVP_PKEY_free};
        const std::unique_ptr<X509, decltype(&::X509_free)> cert{::X509_new(), &::X509_free};
        m_ctx.reset(::SSL_CTX_new(::TLS_server_method()));
        if (!key || !cert || !m_ctx)
        {
            return false;
        }
        ::X509_gmtime_adj(::X509_getm_notBefore(cert.get()), 0);
        ::X509_gmtime_adj(::X509_getm_notAfter(cert.get()), kCertLifetimeSeconds);
        ::X509_set_pubkey(cert.get(), key.get());
        ::X509_sign(cert.get(), key.get(), ::EVP_sha256());
        if (::SSL_CTX_use_certificate(m_ctx.get(), cert.get()) != 1 ||
            ::SSL_CTX_use_PrivateKey(m_ctx.get(), key.get()) != 1)
        {
            return false;
        }
        if (m_tls == PeerTls::AlpnHttp11)
        {
            ::SSL_CTX_set_alpn_select_cb(m_ctx.get(), SelectHttp11, nullptr);
        }
        return true;
    }

    void Run()
    {
        // The TLS session writes with write(2): a client that has gone must
        // cost an EPIPE here, not a SIGPIPE that ends the test process.
        sigset_t pipe{};
        ::sigemptyset(&pipe);
        ::sigaddset(&pipe, SIGPIPE);
        ::pthread_sigmask(SIG_BLOCK, &pipe, nullptr);

        std::size_t index = 0;
        while (!m_stop.load(std::memory_order_acquire))
        {
            pollfd pfd{.fd = m_listen_fd, .events = POLLIN, .revents = 0};
            if (::poll(&pfd, 1, kAcceptPollMs) <= 0)
            {
                continue;
            }
            Conn conn;
            conn.fd = ::accept(m_listen_fd, nullptr, nullptr);
            if (conn.fd < 0)
            {
                continue;
            }
            const int connection = m_connections.fetch_add(1, std::memory_order_acq_rel);
            Serve(conn, connection, index);
        }
    }

    void Serve(Conn& conn, int connection, std::size_t& index)
    {
        const timeval tv{.tv_sec = 0, .tv_usec = kRecvTimeoutUs};
        ::setsockopt(conn.fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        After after = After::Close;
        if (m_tls == PeerTls::None || Accept(conn))
        {
            after = ServeRequests(conn, connection, index);
        }
        Finish(conn, after);
    }

    bool Accept(Conn& conn)
    {
        conn.ssl.reset(::SSL_new(m_ctx.get()));
        ::SSL_set_fd(conn.ssl.get(), conn.fd);
        while (!m_stop.load(std::memory_order_acquire))
        {
            const int r = ::SSL_accept(conn.ssl.get());
            if (r == 1)
            {
                return true;
            }
            if (::SSL_get_error(conn.ssl.get(), r) != SSL_ERROR_WANT_READ)
            {
                return false;
            }
        }
        return false;
    }

    After ServeRequests(Conn& conn, int connection, std::size_t& index)
    {
        while (!m_stop.load(std::memory_order_acquire))
        {
            auto request = ReadRequest(conn);
            if (!request)
            {
                return After::Close;
            }
            request->connection = connection;
            {
                const std::scoped_lock lk{m_mu};
                m_requests.push_back(*request);
            }
            const Http1Reply reply = m_script(*request, index++);
            std::this_thread::sleep_for(reply.delay);
            Write(conn, reply.bytes);
            if (reply.after != After::KeepOpen)
            {
                return reply.after;
            }
        }
        return After::Close;
    }

    /// Read more bytes into the buffer. False on close, error or stop.
    bool Fill(Conn& conn)
    {
        std::array<char, kReadChunk> buf{};
        while (!m_stop.load(std::memory_order_acquire))
        {
            const int n = conn.ssl ? ::SSL_read(conn.ssl.get(), buf.data(), buf.size())
                                   : static_cast<int>(::recv(conn.fd, buf.data(), buf.size(), 0));
            if (n > 0)
            {
                conn.buffer.append(buf.data(), static_cast<std::size_t>(n));
                return true;
            }
            const bool timed_out = conn.ssl
                                       ? ::SSL_get_error(conn.ssl.get(), n) == SSL_ERROR_WANT_READ
                                       : (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK));
            if (!timed_out)
            {
                return false;
            }
        }
        return false;
    }

    std::optional<Http1Request> ReadRequest(Conn& conn)
    {
        std::size_t head_end = std::string::npos;
        while ((head_end = conn.buffer.find("\r\n\r\n")) == std::string::npos)
        {
            if (!Fill(conn))
            {
                return std::nullopt;
            }
        }
        Http1Request request = ParseHead(std::string_view{conn.buffer}.substr(0, head_end));
        const std::size_t body_start = head_end + 4;
        const std::size_t body_len = std::stoul("0" + request.Header("content-length"));
        while (conn.buffer.size() < body_start + body_len)
        {
            if (!Fill(conn))
            {
                return std::nullopt;
            }
        }
        request.body = conn.buffer.substr(body_start, body_len);
        conn.buffer.erase(0, body_start + body_len);
        request.pipelined = !conn.buffer.empty() || Pending(conn);
        return request;
    }

    /// Bytes already waiting on the socket, without waiting for more.
    static bool Pending(const Conn& conn)
    {
        if (conn.ssl && ::SSL_pending(conn.ssl.get()) > 0)
        {
            return true;
        }
        pollfd pfd{.fd = conn.fd, .events = POLLIN, .revents = 0};
        return ::poll(&pfd, 1, 0) > 0;
    }

    static Http1Request ParseHead(std::string_view head)
    {
        Http1Request request;
        const auto line_end = head.find("\r\n");
        const std::string_view start = head.substr(0, line_end);
        const auto sp1 = start.find(' ');
        const auto sp2 = start.find(' ', sp1 + 1);
        request.method = std::string{start.substr(0, sp1)};
        request.target = std::string{start.substr(sp1 + 1, sp2 - sp1 - 1)};
        request.version = std::string{start.substr(sp2 + 1)};
        std::string_view rest =
            line_end == std::string_view::npos ? std::string_view{} : head.substr(line_end + 2);
        while (!rest.empty())
        {
            const auto end = rest.find("\r\n");
            const std::string_view line = rest.substr(0, end);
            rest = end == std::string_view::npos ? std::string_view{} : rest.substr(end + 2);
            const auto colon = line.find(':');
            std::string name{line.substr(0, colon)};
            std::ranges::transform(
                name,
                name.begin(),
                [](char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c; });
            std::string_view value =
                colon == std::string_view::npos ? std::string_view{} : line.substr(colon + 1);
            while (!value.empty() && value.front() == ' ')
            {
                value.remove_prefix(1);
            }
            request.headers.emplace_back(std::move(name), std::string{value});
        }
        return request;
    }

    static void Write(Conn& conn, const std::string& bytes)
    {
        std::size_t off = 0;
        while (off < bytes.size())
        {
            const int n =
                conn.ssl
                    ? ::SSL_write(
                          conn.ssl.get(), bytes.data() + off, static_cast<int>(bytes.size() - off))
                    : static_cast<int>(
                          ::send(conn.fd, bytes.data() + off, bytes.size() - off, MSG_NOSIGNAL));
            if (n <= 0)
            {
                return;
            }
            off += static_cast<std::size_t>(n);
        }
    }

    static void Finish(Conn& conn, After after)
    {
        if (after == After::Reset)
        {
            const linger reset{.l_onoff = 1, .l_linger = 0};
            ::setsockopt(conn.fd, SOL_SOCKET, SO_LINGER, &reset, sizeof(reset));
        }
        conn.ssl.reset();
        ::close(conn.fd);
    }

    Script m_script;
    PeerTls m_tls;
    std::unique_ptr<SSL_CTX, SslCtxFree> m_ctx;
    int m_listen_fd = -1;
    std::thread m_thread;
    std::atomic<bool> m_stop{false};
    std::atomic<int> m_connections{0};
    mutable std::mutex m_mu;
    std::vector<Http1Request> m_requests;
};

}  // namespace microtel::testing
