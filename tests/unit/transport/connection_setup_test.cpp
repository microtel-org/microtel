// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0
//
// Unit test: the connection setup shared by the transports
// (`src/transport/connection_setup.hpp`).
//
// The integration tests under `tests/integration/transport/` cover the paths
// that end in a working connection. These cover the ones that end in an
// error: endpoints that do not parse, hosts that do not resolve or listen, and
// TLS material or peers that cannot complete a handshake. Each uses loopback
// only.

#include "transport/connection_setup.hpp"

#include "microtel/internal/transport.hpp"

#include <gtest/gtest.h>
#include <openssl/bio.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <thread>

#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

namespace mt = microtel;
namespace mti = microtel::internal;
namespace mtt = microtel::transport;

namespace
{

constexpr std::array<std::uint8_t, 9> kAlpnHttp11{8, 'h', 't', 't', 'p', '/', '1', '.', '1'};
constexpr auto kShortTimeout = std::chrono::milliseconds(200);
constexpr long kCertLifetimeSeconds = 3600;
constexpr int kBacklog = 4;

/// A loopback listener on an ephemeral port. Connections complete in the
/// kernel's backlog whether or not anything accepts them.
class Listener
{
public:
    Listener() : m_fd(::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0))
    {
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
        const bool bound = ::bind(m_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0;
        EXPECT_TRUE(bound && ::listen(m_fd, kBacklog) == 0);
        socklen_t len = sizeof(addr);
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
        ::getsockname(m_fd, reinterpret_cast<sockaddr*>(&addr), &len);
        m_port = std::to_string(ntohs(addr.sin_port));
    }
    ~Listener()
    {
        ::close(m_fd);
    }
    Listener(const Listener&) = delete;
    Listener& operator=(const Listener&) = delete;
    Listener(Listener&&) = delete;
    Listener& operator=(Listener&&) = delete;

    [[nodiscard]] int Fd() const noexcept
    {
        return m_fd;
    }
    [[nodiscard]] const std::string& Port() const noexcept
    {
        return m_port;
    }

private:
    int m_fd = -1;
    std::string m_port;
};

/// A port nothing listens on: bound once, then released.
std::string ClosedPort()
{
    const Listener listener;
    return listener.Port();
}

/// A temporary file holding @p contents, removed on destruction.
class TempFile
{
public:
    explicit TempFile(const std::string& stem)
    {
        static std::atomic<int> s_counter{0};
        m_path = std::filesystem::temp_directory_path() /
                 ("microtel-setup-test-" + std::to_string(::getpid()) + "-" +
                  std::to_string(s_counter.fetch_add(1)) + "-" + stem + ".pem");
    }
    ~TempFile()
    {
        std::error_code ec;
        std::filesystem::remove(m_path, ec);
    }
    TempFile(const TempFile&) = delete;
    TempFile& operator=(const TempFile&) = delete;
    TempFile(TempFile&&) = delete;
    TempFile& operator=(TempFile&&) = delete;

    [[nodiscard]] const std::filesystem::path& Path() const noexcept
    {
        return m_path;
    }

private:
    std::filesystem::path m_path;
};

/// Write a self-signed certificate (and nothing else) to @p file.
bool WriteSelfSignedCert(const TempFile& file)
{
    const std::unique_ptr<EVP_PKEY, decltype(&::EVP_PKEY_free)> key{
        ::EVP_PKEY_Q_keygen(nullptr, nullptr, "EC", "P-256"), &::EVP_PKEY_free};
    const std::unique_ptr<X509, decltype(&::X509_free)> cert{::X509_new(), &::X509_free};
    if (!key || !cert)
    {
        return false;
    }
    ::X509_gmtime_adj(::X509_getm_notBefore(cert.get()), 0);
    ::X509_gmtime_adj(::X509_getm_notAfter(cert.get()), kCertLifetimeSeconds);
    ::X509_set_pubkey(cert.get(), key.get());
    ::X509_sign(cert.get(), key.get(), ::EVP_sha256());

    const std::unique_ptr<BIO, decltype(&::BIO_free)> out{::BIO_new_file(file.Path().c_str(), "w"),
                                                          &::BIO_free};
    return out && ::PEM_write_bio_X509(out.get(), cert.get()) == 1;
}

mti::ConnectOptions TlsOptions()
{
    mti::ConnectOptions opts;
    opts.insecure = true;
    opts.tls_handshake_timeout = kShortTimeout;
    return opts;
}

}  // namespace

// ---------------------------------------------------------------------------
// ParseEndpoint
// ---------------------------------------------------------------------------

TEST(ParseEndpointTest, Https_UsesTlsAndDefaultPort443)
{
    const auto ep = mtt::ParseEndpoint("https://collector.example", false);
    ASSERT_TRUE(ep.has_value());
    EXPECT_EQ(ep->host, "collector.example");
    EXPECT_EQ(ep->port, "443");
    EXPECT_TRUE(ep->use_tls);
}

TEST(ParseEndpointTest, Http_IsPlaintextWithDefaultPort80)
{
    const auto ep = mtt::ParseEndpoint("http://collector.example", false);
    ASSERT_TRUE(ep.has_value());
    EXPECT_EQ(ep->port, "80");
    EXPECT_FALSE(ep->use_tls);
}

TEST(ParseEndpointTest, NoScheme_TlsUnlessInsecure)
{
    const auto secure = mtt::ParseEndpoint("collector.example:4318", false);
    const auto insecure = mtt::ParseEndpoint("collector.example:4318", true);
    ASSERT_TRUE(secure.has_value());
    ASSERT_TRUE(insecure.has_value());
    EXPECT_TRUE(secure->use_tls);
    EXPECT_FALSE(insecure->use_tls);
    EXPECT_EQ(insecure->host, "collector.example");
    EXPECT_EQ(insecure->port, "4318");
}

TEST(ParseEndpointTest, Empty_IsAnError)
{
    const auto ep = mtt::ParseEndpoint("", false);
    ASSERT_FALSE(ep.has_value());
    EXPECT_EQ(ep.error().message, "empty endpoint");
}

TEST(ParseEndpointTest, MissingHostOrPort_IsMalformed)
{
    for (const char* endpoint : {"http://:4318", "http://collector.example:"})
    {
        const auto ep = mtt::ParseEndpoint(endpoint, false);
        ASSERT_FALSE(ep.has_value()) << endpoint;
        EXPECT_EQ(ep.error().message, "malformed endpoint") << endpoint;
    }
}

// ---------------------------------------------------------------------------
// TcpConnect
// ---------------------------------------------------------------------------

TEST(TcpConnectTest, ListeningPeer_Connects)
{
    const Listener listener;
    const auto fd = mtt::TcpConnect("127.0.0.1", listener.Port(), kShortTimeout);
    ASSERT_TRUE(fd.has_value()) << fd.error().message;
    EXPECT_TRUE(fd->IsValid());
}

TEST(TcpConnectTest, NothingListening_IsRefused)
{
    const auto fd = mtt::TcpConnect("127.0.0.1", ClosedPort(), kShortTimeout);
    ASSERT_FALSE(fd.has_value());
    EXPECT_EQ(fd.error().kind, mt::Error::Kind::Network);
    EXPECT_EQ(fd.error().message, "connection refused");
}

TEST(TcpConnectTest, UnresolvableHost_FailsDns)
{
    // RFC 6761 reserves .invalid: it never resolves.
    const auto fd = mtt::TcpConnect("microtel-test.invalid", "80", kShortTimeout);
    ASSERT_FALSE(fd.has_value());
    EXPECT_EQ(fd.error().message, "DNS resolution failed");
}

// ---------------------------------------------------------------------------
// TlsConnect
// ---------------------------------------------------------------------------

TEST(TlsConnectTest, MissingCaBundle_FailsBeforeTheHandshake)
{
    const Listener listener;
    auto fd = mtt::TcpConnect("127.0.0.1", listener.Port(), kShortTimeout);
    ASSERT_TRUE(fd.has_value());
    auto opts = TlsOptions();
    opts.ca_bundle = "/nonexistent/microtel-ca.pem";

    const auto tls = mtt::TlsConnect(fd->Get(), opts, "localhost", kAlpnHttp11);
    ASSERT_FALSE(tls.has_value());
    EXPECT_EQ(tls.error().message, "CA bundle load failed");
}

TEST(TlsConnectTest, MissingClientCert_FailsBeforeTheHandshake)
{
    const Listener listener;
    auto fd = mtt::TcpConnect("127.0.0.1", listener.Port(), kShortTimeout);
    ASSERT_TRUE(fd.has_value());
    auto opts = TlsOptions();
    opts.client_cert = "/nonexistent/microtel-client.pem";

    const auto tls = mtt::TlsConnect(fd->Get(), opts, "localhost", kAlpnHttp11);
    ASSERT_FALSE(tls.has_value());
    EXPECT_EQ(tls.error().message, "client cert load failed");
}

TEST(TlsConnectTest, ClientCertWithoutItsKey_FailsBeforeTheHandshake)
{
    const Listener listener;
    auto fd = mtt::TcpConnect("127.0.0.1", listener.Port(), kShortTimeout);
    ASSERT_TRUE(fd.has_value());
    const TempFile cert{"client-cert"};
    ASSERT_TRUE(WriteSelfSignedCert(cert));
    auto opts = TlsOptions();
    opts.client_cert = cert.Path();
    opts.client_key = "/nonexistent/microtel-client-key.pem";

    const auto tls = mtt::TlsConnect(fd->Get(), opts, "localhost", kAlpnHttp11);
    ASSERT_FALSE(tls.has_value());
    EXPECT_EQ(tls.error().message, "client key load failed");
}

TEST(TlsConnectTest, PeerThatNeverAnswers_TimesOut)
{
    // Accepted by the kernel, never by a TLS server: the ClientHello is read
    // by nobody, so the handshake waits until its deadline.
    const Listener listener;
    auto fd = mtt::TcpConnect("127.0.0.1", listener.Port(), kShortTimeout);
    ASSERT_TRUE(fd.has_value());

    const auto tls = mtt::TlsConnect(fd->Get(), TlsOptions(), "localhost", kAlpnHttp11);
    ASSERT_FALSE(tls.has_value());
    EXPECT_EQ(tls.error().kind, mt::Error::Kind::Cancelled);
    EXPECT_EQ(tls.error().message, "TLS handshake timeout");
}

TEST(TlsConnectTest, PeerThatAnswersInPlaintext_FailsTheHandshake)
{
    const Listener listener;
    std::thread peer{[&listener]
                     {
                         const int conn = ::accept(listener.Fd(), nullptr, nullptr);
                         if (conn < 0)
                         {
                             return;
                         }
                         constexpr std::string_view kReply = "HTTP/1.1 400 Bad Request\r\n\r\n";
                         (void)::send(conn, kReply.data(), kReply.size(), MSG_NOSIGNAL);
                         ::close(conn);
                     }};
    auto fd = mtt::TcpConnect("127.0.0.1", listener.Port(), kShortTimeout);
    ASSERT_TRUE(fd.has_value());
    auto opts = TlsOptions();
    opts.tls_handshake_timeout = std::chrono::seconds(5);

    const auto tls = mtt::TlsConnect(fd->Get(), opts, "localhost", kAlpnHttp11);
    peer.join();
    ASSERT_FALSE(tls.has_value());
    EXPECT_EQ(tls.error().kind, mt::Error::Kind::Network);
    EXPECT_EQ(tls.error().message, "TLS handshake failed");
}
