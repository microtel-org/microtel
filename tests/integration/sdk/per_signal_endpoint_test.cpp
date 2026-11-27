// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0
//
// Integration test: the per-signal OTLP endpoint variables on the wire
// (`docs/configuration.md` §3.3). A real SdkBuilder-built Provider exports all
// three signals over its one loopback HTTP/2 connection to an in-process
// server that records each request's `:path`.
//
// The server is cut down from tests/integration/otelcpp_shim/
// wire_conformance_test.cpp's CapturingHttp2Server: it records paths only.

#include "microtel/log_record.hpp"
#include "microtel/logger.hpp"
#include "microtel/meter.hpp"
#include "microtel/provider.hpp"
#include "microtel/sdk_builder.hpp"
#include "microtel/status.hpp"

#include <gtest/gtest.h>
#include <nghttp2/nghttp2.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace
{

constexpr std::chrono::seconds kFlushTimeout{5};
constexpr std::chrono::seconds kShutdownTimeout{2};
constexpr std::size_t kThreeSignals = 3;

/// Sets an environment variable for the scope's lifetime, then unsets it.
class EnvVar
{
public:
    EnvVar(const char* name, const std::string& value) : m_name(name)
    {
        (void)::setenv(name, value.c_str(), 1);
    }
    ~EnvVar()
    {
        (void)::unsetenv(m_name);
    }
    EnvVar(const EnvVar&) = delete;
    EnvVar& operator=(const EnvVar&) = delete;
    EnvVar(EnvVar&&) = delete;
    EnvVar& operator=(EnvVar&&) = delete;

private:
    const char* m_name;
};

/// A one-connection h2c server on 127.0.0.1 that answers every request with an
/// empty 200 and records its `:path`.
class PathRecordingServer
{
public:
    PathRecordingServer() = default;
    ~PathRecordingServer()
    {
        Stop();
    }
    PathRecordingServer(const PathRecordingServer&) = delete;
    PathRecordingServer& operator=(const PathRecordingServer&) = delete;
    PathRecordingServer(PathRecordingServer&&) = delete;
    PathRecordingServer& operator=(PathRecordingServer&&) = delete;

    /// Bind, listen and start serving. Returns the port, or -1.
    int Start()
    {
        m_listen_fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (m_listen_fd < 0)
        {
            return -1;
        }
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        socklen_t len = sizeof(addr);
        // NOLINTBEGIN(cppcoreguidelines-pro-type-reinterpret-cast)
        if (::bind(m_listen_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0 ||
            ::listen(m_listen_fd, 1) < 0 ||
            ::getsockname(m_listen_fd, reinterpret_cast<sockaddr*>(&addr), &len) < 0)
        // NOLINTEND(cppcoreguidelines-pro-type-reinterpret-cast)
        {
            ::close(m_listen_fd);
            m_listen_fd = -1;
            return -1;
        }
        // The fd is passed by value: Stop() rewrites m_listen_fd from another thread.
        m_thread = std::thread([this, listen_fd = m_listen_fd] { Serve(listen_fd); });
        return ntohs(addr.sin_port);
    }

    /// Wait until @p count requests have arrived or @p timeout passes.
    [[nodiscard]] std::vector<std::string> WaitForPaths(std::size_t count,
                                                        std::chrono::milliseconds timeout) const
    {
        constexpr std::chrono::milliseconds kPoll{5};
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (Paths().size() < count && std::chrono::steady_clock::now() < deadline)
        {
            std::this_thread::sleep_for(kPoll);
        }
        return Paths();
    }

    void Stop()
    {
        m_stop.store(true, std::memory_order_release);
        if (m_listen_fd >= 0)
        {
            ::shutdown(m_listen_fd, SHUT_RDWR);
            ::close(m_listen_fd);
            m_listen_fd = -1;
        }
        if (m_thread.joinable())
        {
            m_thread.join();
        }
    }

private:
    struct SessionCtx
    {
        int fd = -1;
        PathRecordingServer* server = nullptr;
        std::unordered_map<std::int32_t, std::string> paths;
    };

    [[nodiscard]] std::vector<std::string> Paths() const
    {
        const std::scoped_lock lock{m_mu};
        return m_paths;
    }

    void Serve(int listen_fd)
    {
        const int fd = ::accept(listen_fd, nullptr, nullptr);
        if (fd < 0)
        {
            return;
        }
        RunSession(fd);
        ::close(fd);
    }

    void RunSession(int fd)
    {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg,hicpp-vararg,hicpp-signed-bitwise)
        ::fcntl(fd, F_SETFL, ::fcntl(fd, F_GETFL) | O_NONBLOCK);
        SessionCtx ctx{.fd = fd, .server = this, .paths = {}};

        nghttp2_session_callbacks* cbs = nullptr;
        ::nghttp2_session_callbacks_new(&cbs);
        ::nghttp2_session_callbacks_set_send_callback(cbs, SendCb);
        ::nghttp2_session_callbacks_set_recv_callback(cbs, RecvCb);
        ::nghttp2_session_callbacks_set_on_header_callback(cbs, OnHeaderCb);
        ::nghttp2_session_callbacks_set_on_frame_recv_callback(cbs, OnFrameRecvCb);
        nghttp2_session* session = nullptr;
        ::nghttp2_session_server_new(&session, cbs, &ctx);
        ::nghttp2_session_callbacks_del(cbs);
        ::nghttp2_submit_settings(session, NGHTTP2_FLAG_NONE, nullptr, 0);

        constexpr int kPollMs = 50;
        constexpr std::chrono::seconds kSessionLimit{10};
        const auto deadline = std::chrono::steady_clock::now() + kSessionLimit;
        while (!m_stop.load(std::memory_order_acquire) &&
               std::chrono::steady_clock::now() < deadline && PumpOnce(session, fd, kPollMs))
        {
        }
        ::nghttp2_session_send(session);
        ::nghttp2_session_del(session);
    }

    [[nodiscard]] static bool PumpOnce(nghttp2_session* session, int fd, int poll_ms)
    {
        ::nghttp2_session_send(session);
        pollfd pfd{.fd = fd, .events = POLLIN, .revents = 0};
        if (::poll(&pfd, 1, poll_ms) <= 0)
        {
            return true;
        }
        if (::nghttp2_session_recv(session) != 0)
        {
            return false;
        }
        ::nghttp2_session_send(session);
        return true;
    }

    static ssize_t SendCb(
        nghttp2_session* /*s*/, const std::uint8_t* data, std::size_t len, int /*flags*/, void* ud)
    {
        const int fd = static_cast<SessionCtx*>(ud)->fd;
        ssize_t n = ::write(fd, data, len);
        while (n < 0 && errno == EINTR)
        {
            n = ::write(fd, data, len);
        }
        if (n < 0)
        {
            return (errno == EAGAIN || errno == EWOULDBLOCK)
                       ? static_cast<ssize_t>(NGHTTP2_ERR_WOULDBLOCK)
                       : static_cast<ssize_t>(NGHTTP2_ERR_CALLBACK_FAILURE);
        }
        return n;
    }

    static ssize_t RecvCb(
        nghttp2_session* /*s*/, std::uint8_t* buf, std::size_t len, int /*flags*/, void* ud)
    {
        const int fd = static_cast<SessionCtx*>(ud)->fd;
        ssize_t n = ::read(fd, buf, len);
        while (n < 0 && errno == EINTR)
        {
            n = ::read(fd, buf, len);
        }
        if (n == 0)
        {
            return NGHTTP2_ERR_CALLBACK_FAILURE;
        }
        if (n < 0)
        {
            return (errno == EAGAIN || errno == EWOULDBLOCK) ? NGHTTP2_ERR_WOULDBLOCK
                                                             : NGHTTP2_ERR_CALLBACK_FAILURE;
        }
        return n;
    }

    // nghttp2_on_header_callback's 8-parameter signature is the C API's.
    // NOLINTNEXTLINE(readability-function-size)
    static int OnHeaderCb(nghttp2_session* /*s*/,
                          const nghttp2_frame* frame,
                          const std::uint8_t* name,
                          std::size_t namelen,
                          const std::uint8_t* value,
                          std::size_t valuelen,
                          std::uint8_t /*flags*/,
                          void* ud)
    {
        if (frame->hd.type != NGHTTP2_HEADERS || frame->headers.cat != NGHTTP2_HCAT_REQUEST)
        {
            return 0;
        }
        // NOLINTBEGIN(cppcoreguidelines-pro-type-reinterpret-cast)
        if (std::string_view{reinterpret_cast<const char*>(name), namelen} == ":path")
        {
            static_cast<SessionCtx*>(ud)->paths[frame->hd.stream_id].assign(
                reinterpret_cast<const char*>(value), valuelen);
        }
        // NOLINTEND(cppcoreguidelines-pro-type-reinterpret-cast)
        return 0;
    }

    static int OnFrameRecvCb(nghttp2_session* s, const nghttp2_frame* frame, void* ud)
    {
        auto* ctx = static_cast<SessionCtx*>(ud);
        const bool end_stream = (frame->hd.flags & NGHTTP2_FLAG_END_STREAM) != 0U;
        const auto it = ctx->paths.find(frame->hd.stream_id);
        if (!end_stream || it == ctx->paths.end())
        {
            return 0;
        }
        {
            const std::scoped_lock lock{ctx->server->m_mu};
            ctx->server->m_paths.push_back(it->second);
        }
        ctx->paths.erase(it);

        static const std::array<std::uint8_t, 7> kStatusName{':', 's', 't', 'a', 't', 'u', 's'};
        static const std::array<std::uint8_t, 3> kStatusValue{'2', '0', '0'};
        // nghttp2_nv is not const-correct; nghttp2 only reads the two buffers.
        // NOLINTBEGIN(cppcoreguidelines-pro-type-const-cast)
        const nghttp2_nv nv{.name = const_cast<std::uint8_t*>(kStatusName.data()),
                            .value = const_cast<std::uint8_t*>(kStatusValue.data()),
                            .namelen = kStatusName.size(),
                            .valuelen = kStatusValue.size(),
                            .flags = NGHTTP2_NV_FLAG_NONE};
        // NOLINTEND(cppcoreguidelines-pro-type-const-cast)
        ::nghttp2_submit_response(s, frame->hd.stream_id, &nv, 1, nullptr);
        return 0;
    }

    int m_listen_fd = -1;
    std::thread m_thread;
    std::atomic<bool> m_stop{false};
    mutable std::mutex m_mu;
    std::vector<std::string> m_paths;
};

/// Emit one span, one counter point and one log record, then flush all three.
void EmitOneOfEachAndFlush(microtel::Provider& provider)
{
    provider.GetTracer("per-signal")->StartSpan("op")->End();
    provider.GetMeter("per-signal")->CreateCounter<std::int64_t>("ops", "", "1")->Add(1, {});
    microtel::LogRecord record;
    record.body = std::string{"hello"};
    provider.GetLogger("per-signal")->Emit(std::move(record));
    ASSERT_EQ(provider.ForceFlush(kFlushTimeout), microtel::Status::Completed)
        << "last_error: " << provider.GetExporterHealth().last_error_message;
}

[[nodiscard]] std::vector<std::string> Sorted(std::vector<std::string> v)
{
    std::ranges::sort(v);
    return v;
}

}  // namespace

// The Helm / OTel Operator shape: only OTEL_EXPORTER_OTLP_TRACES_ENDPOINT is
// set. It picks the connection's origin, traces go to its path as-is, and
// metrics and logs use their default paths on the same origin.
TEST(PerSignalEndpointIntegrationTest, LoneTracesEndpoint_TracesUseItsPathOthersTheDefaults)
{
    PathRecordingServer server;
    const int port = server.Start();
    ASSERT_GT(port, 0);
    const std::string origin = "http://127.0.0.1:" + std::to_string(port);
    const EnvVar traces{"OTEL_EXPORTER_OTLP_TRACES_ENDPOINT", origin + "/custom/traces"};

    auto result = microtel::SdkBuilder{}.WithServiceName("per-signal-test").Build();
    ASSERT_TRUE(result.has_value()) << result.error().message;
    const auto& provider = *result;
    EmitOneOfEachAndFlush(*provider);

    const auto paths = server.WaitForPaths(kThreeSignals, kFlushTimeout);
    std::ignore = provider->Shutdown(kShutdownTimeout);
    EXPECT_EQ(Sorted(paths),
              (std::vector<std::string>{"/custom/traces", "/v1/logs", "/v1/metrics"}));
}

// A base endpoint with a path prefix plus a metrics endpoint on the same
// origin: traces and logs get the prefix, metrics its own path unchanged.
TEST(PerSignalEndpointIntegrationTest, BaseWithPrefixPlusMetricsEndpoint_OnOneConnection)
{
    PathRecordingServer server;
    const int port = server.Start();
    ASSERT_GT(port, 0);
    const std::string origin = "http://127.0.0.1:" + std::to_string(port);
    const EnvVar base{"OTEL_EXPORTER_OTLP_ENDPOINT", origin + "/gw"};
    const EnvVar metrics{"OTEL_EXPORTER_OTLP_METRICS_ENDPOINT", origin + "/m/ingest"};

    auto result = microtel::SdkBuilder{}.WithServiceName("per-signal-test").Build();
    ASSERT_TRUE(result.has_value()) << result.error().message;
    const auto& provider = *result;
    EmitOneOfEachAndFlush(*provider);

    const auto paths = server.WaitForPaths(kThreeSignals, kFlushTimeout);
    std::ignore = provider->Shutdown(kShutdownTimeout);
    EXPECT_EQ(Sorted(paths),
              (std::vector<std::string>{"/gw/v1/logs", "/gw/v1/traces", "/m/ingest"}));
}
