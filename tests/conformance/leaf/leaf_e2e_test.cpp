// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0
//
// The leaf / concentrator ship gate, end to end (ICP 0031 gates 3 and 5,
// docs/leaf-concentrator-design.md §7.5): C leaves built with the backend this
// binary links → an in-memory link standing in for the application's
// transport → LeafReceiver::Ingest in a concentrator Provider → a real
// OpenTelemetry Collector, over OTLP/HTTP and OTLP/gRPC.
//
// One source, two binaries: conformance_leaf_nanopb_test and
// conformance_leaf_upb_test link the two encoder backends
// (tests/conformance/leaf/CMakeLists.txt). Each runs every test over both
// protocols, so the four runs of §7.5 are {nanopb, upb} x {HTTP, gRPC}.
//
// Only public headers: microtel/leaf.h on the leaf side, the C++ API on the
// concentrator side. The assertions read the collector's output file.
//
// Why a separate collector receiver
// ---------------------------------
// §7.5 requires that spans from many leaves leave the concentrator as ONE
// export request with one ResourceSpans per leaf (§3.6.1). The main traces
// pipeline runs the collector's batch processor, which can merge requests, so
// a line there proves nothing about how many requests microtel sent. The
// `otlp/leaf` receiver (tests/conformance/collector/config.yaml) feeds a
// pipeline with no processor into its own file exporter, which writes one line
// per request it receives. One line holding all the leaves is one request.

#include "microtel/export_transport.hpp"
#include "microtel/leaf.h"
#include "microtel/leaf_receiver.hpp"
#include "microtel/protocol.hpp"
#include "microtel/provider.hpp"
#include "microtel/sdk_builder.hpp"
#include "microtel/span.hpp"
#include "microtel/status.hpp"
#include "microtel/tracer.hpp"

#include "conformance/support/collector_output.hpp"
#include "conformance/support/conformance_env.hpp"
#include "conformance/support/provider_builder.hpp"

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace
{

constexpr const char* kHttpEndpointEnv = "MICROTEL_CONFORMANCE_LEAF_HTTP_ENDPOINT";
constexpr const char* kGrpcEndpointEnv = "MICROTEL_CONFORMANCE_LEAF_GRPC_ENDPOINT";
constexpr const char* kCaEnv = "MICROTEL_CONFORMANCE_CA";
constexpr const char* kOutputFileEnv = "MICROTEL_CONFORMANCE_LEAF_OUTPUT_FILE";

constexpr const char* kSkipReason =
    "no conformance collector configured — run ci/scripts/conformance.sh";

// ConformanceEnabled records a failure and returns false when the runner set
// MICROTEL_CONFORMANCE_REQUIRE but not a variable; gtest then runs the body
// after SetUp's skip, so each body checks that SetUp got as far as a receiver.
constexpr const char* kNoConcentrator = "SetUp did not build a concentrator";

#ifdef MICROTEL_LEAF_E2E_BACKEND_UPB
constexpr std::string_view kBackend = "upb";
#else
constexpr std::string_view kBackend = "nanopb";
#endif

constexpr auto kFlushTimeout = std::chrono::seconds(30);
constexpr auto kCollectorPollTimeout = std::chrono::seconds(15);
constexpr auto kRereadInterval = std::chrono::milliseconds(100);
// Long enough that only ForceFlush moves a batch, so every payload a test
// ingests is in the queue when the flush drains it into one request.
constexpr auto kScheduleDelay = std::chrono::seconds(60);

// §7.5: at least 8 leaves, with distinct ids, into one batch.
constexpr std::size_t kLeafCount = 8;
constexpr std::size_t kRecordBufferBytes = 2048;

// Leaf clock readings (nanoseconds in the leaf's own clock) for the scripted
// span every test builds: start, event, end, encode.
constexpr std::uint64_t kSpanStart = 1'000;
constexpr std::uint64_t kSpanEvent = 2'000;
constexpr std::uint64_t kSpanEnd = 3'000;
constexpr std::uint64_t kEncodeAt = 10'000;

// The receive time every payload is stamped with, so each corrected timestamp
// is an exact number: 2026-09-26T00:00:00Z.
constexpr std::int64_t kReceivedAtNs = 1'790'380'800'000'000'000;

constexpr std::int64_t kInt64Attr = 42;
constexpr double kDoubleAttr = 2.5;

/// The leaf's scripted clock: returns whatever the test last set.
struct LeafClock
{
    std::uint64_t now_ns = 0;
};

std::uint64_t ReadLeafClock(void* ctx)
{
    return static_cast<const LeafClock*>(ctx)->now_ns;
}

/// Random bytes for ids, from a per-leaf engine seeded by std::random_device,
/// so ids differ between runs (the collector's file outlives a run).
void FillRandom(void* ctx, std::uint8_t* out, std::size_t len)
{
    auto* const engine = static_cast<std::mt19937_64*>(ctx);
    for (std::size_t i = 0; i < len; ++i)
    {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic) — C callback contract
        out[i] = static_cast<std::uint8_t>((*engine)());
    }
}

microtel_leaf_kv_t StringKv(const std::string& key, const std::string& value)
{
    microtel_leaf_kv_t kv{};
    kv.key = key.data();
    kv.key_len = key.size();
    kv.type = MICROTEL_LEAF_VALUE_STRING;
    kv.value.s.ptr = value.data();
    kv.value.s.len = value.size();
    return kv;
}

microtel_leaf_kv_t IntKv(const std::string& key, std::int64_t value)
{
    microtel_leaf_kv_t kv{};
    kv.key = key.data();
    kv.key_len = key.size();
    kv.type = MICROTEL_LEAF_VALUE_INT64;
    kv.value.i = value;
    return kv;
}

/// @brief One C leaf in caller-owned memory, as a device would hold it.
///
/// Neither copyable nor movable: the leaf keeps a pointer to its record buffer.
class TestLeaf
{
public:
    TestLeaf(microtel_leaf_time_mode_t mode, const std::string& service, std::int64_t index)
        : m_engine(std::random_device{}())
    {
        const std::string service_key = "service.name";
        const std::string index_key = "sensor.index";
        const std::array<microtel_leaf_kv_t, 2> resource{StringKv(service_key, service),
                                                         IntKv(index_key, index)};
        const std::string scope = "leaf.e2e";
        microtel_leaf_config_t config{};
        config.struct_size = sizeof(config);
        config.time_mode = mode;
        config.now_ns = &ReadLeafClock;
        config.clock_ctx = &m_clock;
        config.random_bytes = &FillRandom;
        config.random_ctx = &m_engine;
        config.resource = resource.data();
        config.resource_count = resource.size();
        config.scope_name = scope.data();
        config.scope_name_len = scope.size();
        config.boot_id = static_cast<std::uint32_t>(m_engine()) | 1U;
        m_status = microtel_leaf_init(
            &m_leaf, sizeof(m_leaf), &config, m_records.data(), m_records.size());
    }

    ~TestLeaf()
    {
        microtel_leaf_free(&m_leaf);
    }

    TestLeaf(const TestLeaf&) = delete;
    TestLeaf& operator=(const TestLeaf&) = delete;
    TestLeaf(TestLeaf&&) = delete;
    TestLeaf& operator=(TestLeaf&&) = delete;

    [[nodiscard]] microtel_leaf_status_t InitStatus() const
    {
        return m_status;
    }
    microtel_leaf_t* Leaf()
    {
        return &m_leaf;
    }
    void SetClock(std::uint64_t now_ns)
    {
        m_clock.now_ns = now_ns;
    }

private:
    microtel_leaf_t m_leaf{};
    std::array<std::uint8_t, kRecordBufferBytes> m_records{};
    LeafClock m_clock{};
    std::mt19937_64 m_engine;
    microtel_leaf_status_t m_status = MICROTEL_LEAF_ERR_STATE;
};

microtel_leaf_kv_t DoubleKv(const std::string& key, double value)
{
    microtel_leaf_kv_t kv{};
    kv.key = key.data();
    kv.key_len = key.size();
    kv.type = MICROTEL_LEAF_VALUE_DOUBLE;
    kv.value.d = value;
    return kv;
}

microtel_leaf_kv_t BoolKv(const std::string& key, bool value)
{
    microtel_leaf_kv_t kv{};
    kv.key = key.data();
    kv.key_len = key.size();
    kv.type = MICROTEL_LEAF_VALUE_BOOL;
    kv.value.b = value ? 1 : 0;
    return kv;
}

/// One attribute of each type the leaf can emit.
microtel_leaf_status_t SetEveryAttributeType(TestLeaf& leaf, microtel_leaf_span_t span)
{
    const std::string s_key = "leaf.attr.string";
    const std::string s_val = "string-value";
    const std::string i_key = "leaf.attr.int64";
    const std::string d_key = "leaf.attr.double";
    const std::string b_key = "leaf.attr.bool";
    const std::array<microtel_leaf_kv_t, 4> attrs{StringKv(s_key, s_val),
                                                  IntKv(i_key, kInt64Attr),
                                                  DoubleKv(d_key, kDoubleAttr),
                                                  BoolKv(b_key, true)};
    microtel_leaf_status_t st = MICROTEL_LEAF_OK;
    for (const microtel_leaf_kv_t& kv : attrs)
    {
        st = st == MICROTEL_LEAF_OK ? microtel_leaf_span_set_attribute(leaf.Leaf(), span, &kv) : st;
    }
    return st;
}

/// One event with one attribute, then an error status.
microtel_leaf_status_t AddEventAndStatus(TestLeaf& leaf, microtel_leaf_span_t span)
{
    const std::string e_name = "leaf.event";
    const std::string e_key = "leaf.event.attr";
    const std::string e_val = "event-value";
    const microtel_leaf_kv_t e_kv = StringKv(e_key, e_val);
    const microtel_leaf_status_t st =
        microtel_leaf_span_add_event(leaf.Leaf(), span, e_name.data(), e_name.size(), &e_kv, 1);
    const std::string message = "leaf-error";
    return st == MICROTEL_LEAF_OK
               ? microtel_leaf_span_set_status(
                     leaf.Leaf(), span, MICROTEL_LEAF_STATUS_ERROR, message.data(), message.size())
               : st;
}

/// @brief Builds the scripted span: every attribute type, one event, an error
///        status, kind CLIENT, at the kSpan* leaf-clock times plus @p offset.
/// @return MICROTEL_LEAF_OK, or the first failing call's status.
microtel_leaf_status_t BuildSpan(TestLeaf& leaf, const std::string& name, std::uint64_t offset)
{
    leaf.SetClock(kSpanStart + offset);
    microtel_leaf_span_t span = 0;
    microtel_leaf_status_t st = microtel_leaf_span_start(
        leaf.Leaf(), &span, name.data(), name.size(), MICROTEL_LEAF_SPAN_KIND_CLIENT, nullptr);
    st = st == MICROTEL_LEAF_OK ? SetEveryAttributeType(leaf, span) : st;
    leaf.SetClock(kSpanEvent + offset);
    st = st == MICROTEL_LEAF_OK ? AddEventAndStatus(leaf, span) : st;
    leaf.SetClock(kSpanEnd + offset);
    return st == MICROTEL_LEAF_OK ? microtel_leaf_span_end(leaf.Leaf(), span) : st;
}

/// One payload in flight on the link, with the id the transport gives it.
struct Frame
{
    std::string leaf_id;
    std::vector<std::byte> bytes;
    std::size_t write_calls = 0;
};

int AppendToFrame(void* ctx, const std::uint8_t* bytes, std::size_t len)
{
    auto* const frame = static_cast<Frame*>(ctx);
    const auto* const first = reinterpret_cast<const std::byte*>(bytes);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic) — C callback contract
    frame->bytes.insert(frame->bytes.end(), first, first + len);
    ++frame->write_calls;
    return 0;
}

/// @brief Encodes every ended span of @p leaf into one frame, through the
///        streaming encoder, the way a device writes to its link.
Frame Encode(TestLeaf& leaf, std::string leaf_id, std::uint64_t encode_at)
{
    Frame frame{.leaf_id = std::move(leaf_id), .bytes = {}, .write_calls = 0};
    leaf.SetClock(encode_at);
    std::size_t written = 0;
    const microtel_leaf_status_t st =
        microtel_leaf_encode_to(leaf.Leaf(), &AppendToFrame, &frame, &written);
    EXPECT_EQ(st, MICROTEL_LEAF_OK);
    EXPECT_EQ(written, frame.bytes.size());
    return frame;
}

microtel::IngestResult Ingest(microtel::LeafReceiver& receiver,
                              const Frame& frame,
                              std::int64_t received_at_ns)
{
    const microtel::IngestRequest request{
        .leaf_id = frame.leaf_id,
        .payload = std::span<const std::byte>(frame.bytes),
        .received_at =
            std::chrono::system_clock::time_point{
                std::chrono::duration_cast<std::chrono::system_clock::duration>(
                    std::chrono::nanoseconds{received_at_ns})},
    };
    return receiver.Ingest(request);
}

std::string TimeJson(const char* field, std::int64_t ns)
{
    return std::string{"\""} + field + "\":\"" + std::to_string(ns) + "\"";
}

std::string DeviceIdJson(const std::string& leaf_id)
{
    return R"({"key":"device.id","value":{"stringValue":")" + leaf_id + R"("}})";
}

void ExpectContains(const std::string& text, const std::string& fragment)
{
    EXPECT_NE(text.find(fragment), std::string::npos)
        << "collector output is missing " << fragment << "\n  in: " << text;
}

// ---------------------------------------------------------------------------
// A full C++ node on the link (ICP 0036)
// ---------------------------------------------------------------------------

/// The full node's leaf id is the UDP source address:port the concentrator
/// reads it from, and the concentrator's resolver trusts every such id as
/// holding Unix time; the C leaves' ids above never look like this.
constexpr std::string_view kNodeIdPrefix = "127.0.0.1:";

// The node's span, at fixed Unix times that must reach the collector as they
// are: a trusted node's clock is its own (t' = t).
constexpr std::int64_t kNodeStartNs = kReceivedAtNs - 2'000'000'000 + 123;
constexpr std::int64_t kNodeEventNs = kNodeStartNs + 1'000;
constexpr std::int64_t kNodeEndNs = kNodeStartNs + 5'000;

constexpr auto kDatagramWait = std::chrono::seconds(5);
constexpr std::size_t kMaxDatagram = 65507;
/// Under one datagram and under the concentrator's max_payload_bytes.
constexpr std::uint32_t kNodeMaxRequestBytes = 60U * 1024U;

/// Move-only owner of a socket.
class Socket
{
public:
    explicit Socket(int fd) noexcept : m_fd(fd) {}
    ~Socket() noexcept
    {
        if (m_fd >= 0)
        {
            ::close(m_fd);
        }
    }
    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;
    Socket(Socket&& other) noexcept : m_fd(std::exchange(other.m_fd, -1)) {}
    Socket& operator=(Socket&&) = delete;

    [[nodiscard]] int Get() const noexcept
    {
        return m_fd;
    }

private:
    int m_fd = -1;
};

sockaddr_in LoopbackAddr(std::uint16_t port)
{
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    return addr;
}

/// A UDP socket bound to 127.0.0.1 on an ephemeral port, and that port.
std::pair<Socket, std::uint16_t> BindEphemeral()
{
    Socket fd{::socket(AF_INET, SOCK_DGRAM, 0)};
    const sockaddr_in addr = LoopbackAddr(0);
    sockaddr_in bound{};
    socklen_t len = sizeof(bound);
    // NOLINTBEGIN(cppcoreguidelines-pro-type-reinterpret-cast) — the sockets API
    if (fd.Get() < 0 ||
        ::bind(fd.Get(), reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0 ||
        ::getsockname(fd.Get(), reinterpret_cast<sockaddr*>(&bound), &len) != 0)
    // NOLINTEND(cppcoreguidelines-pro-type-reinterpret-cast)
    {
        return {Socket{-1}, 0};
    }
    return {std::move(fd), ntohs(bound.sin_port)};
}

/// The node's transport: each request as one datagram on a connected socket,
/// as examples/leaf/udp_full_node.cpp sends it. A send on loopback does not
/// block, so Cancel only has to stop later sends.
class UdpExportTransport final : public microtel::ExportTransport
{
public:
    explicit UdpExportTransport(Socket socket) noexcept : m_socket(std::move(socket)) {}

    [[nodiscard]] microtel::SendResult Send(const microtel::ExportRequest& request) override
    {
        const bool sent =
            !m_cancelled.load() && request.bytes.size() <= kMaxDatagram &&
            ::send(m_socket.Get(), request.bytes.data(), request.bytes.size(), 0) >= 0;
        return microtel::SendResult{
            .outcome = sent ? microtel::SendOutcome::Success : microtel::SendOutcome::NonRetryable,
            .retry_after = std::nullopt,
            .rejected = 0,
            .message = sent ? std::string{} : std::string{"datagram not sent"},
        };
    }

    void Cancel() noexcept override
    {
        m_cancelled.store(true);
    }

private:
    Socket m_socket;
    std::atomic<bool> m_cancelled{false};
};

/// One link between a node and the test's concentrator side.
struct NodeLink
{
    Socket rx{-1};        ///< the concentrator's end
    Socket tx{-1};        ///< the node's end, connected to rx
    std::string node_id;  ///< "127.0.0.1:<tx port>", what rx sees
};

NodeLink OpenNodeLink()
{
    auto [rx, rx_port] = BindEphemeral();
    auto [tx, tx_port] = BindEphemeral();
    const sockaddr_in peer = LoopbackAddr(rx_port);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast) — the sockets API
    if (tx.Get() < 0 ||
        ::connect(tx.Get(), reinterpret_cast<const sockaddr*>(&peer), sizeof(peer)) != 0)
    {
        return NodeLink{};
    }
    return NodeLink{.rx = std::move(rx),
                    .tx = std::move(tx),
                    .node_id = std::string{kNodeIdPrefix} + std::to_string(tx_port)};
}

/// Receives one datagram on @p rx and ingests it under its source
/// address:port, as the example concentrator does.
microtel::IngestResult ReceiveAndIngest(const Socket& rx,
                                        microtel::LeafReceiver& receiver,
                                        std::string& from_id)
{
    pollfd pfd{.fd = rx.Get(), .events = POLLIN, .revents = 0};
    const auto wait_ms = static_cast<int>(
        std::chrono::duration_cast<std::chrono::milliseconds>(kDatagramWait).count());
    std::vector<std::byte> buffer(kMaxDatagram);
    sockaddr_in from{};
    socklen_t from_len = sizeof(from);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast) — the sockets API
    const ssize_t n = ::poll(&pfd, 1, wait_ms) > 0 ? ::recvfrom(rx.Get(),
                                                                buffer.data(),
                                                                buffer.size(),
                                                                0,
                                                                reinterpret_cast<sockaddr*>(&from),
                                                                &from_len)
                                                   : -1;
    if (n < 0)
    {
        ADD_FAILURE() << "no datagram from the full node";
        return microtel::IngestResult{};
    }
    std::array<char, INET_ADDRSTRLEN> host{};
    ::inet_ntop(AF_INET, &from.sin_addr, host.data(), host.size());
    from_id = std::string{host.data()} + ":" + std::to_string(ntohs(from.sin_port));
    return receiver.Ingest(microtel::IngestRequest{
        .leaf_id = from_id,
        .payload = std::span<const std::byte>(buffer.data(), static_cast<std::size_t>(n)),
        .received_at = std::nullopt,
    });
}

std::chrono::system_clock::time_point UnixNs(std::int64_t ns)
{
    return std::chrono::system_clock::time_point{
        std::chrono::duration_cast<std::chrono::system_clock::duration>(
            std::chrono::nanoseconds{ns})};
}

/// A full node: a C++ Provider exporting over @p link with its own
/// ExportTransport, under its own profile name beside the concentrator.
std::shared_ptr<microtel::Provider> BuildNode(NodeLink& link,
                                              const std::string& profile,
                                              std::uint32_t max_request_bytes)
{
    auto result = microtel::SdkBuilder{}
                      .WithProfileName(profile)
                      .WithServiceName("leaf-e2e-full-node")
                      .WithBatch(microtel::BatchOptions{
                          .max_queue_size = microtel::BatchOptions{}.max_queue_size,
                          .max_export_batch_size = microtel::BatchOptions{}.max_export_batch_size,
                          .schedule_delay = kScheduleDelay,
                          .drop_policy = microtel::BatchOptions{}.drop_policy,
                      })
                      .WithExportTransport(std::make_unique<UdpExportTransport>(std::move(link.tx)),
                                           microtel::ExportTransportOptions{
                                               .traces = true,
                                               .metrics = false,
                                               .logs = false,
                                               .max_request_bytes = max_request_bytes,
                                           })
                      .Build();
    EXPECT_TRUE(result.has_value()) << (result ? "" : result.error().message);
    return result ? *result : nullptr;
}

/// The node's one span, at the kNode* Unix times.
void TraceNodeSpan(microtel::Provider& node, const std::string& name)
{
    microtel::StartSpanOptions opts;
    opts.kind = microtel::SpanKind::Server;
    opts.start_time = UnixNs(kNodeStartNs);
    auto span = node.GetTracer("node.e2e", "1.0")->StartSpan(name, opts);
    span->SetAttribute("node.attr", std::string{"node-value"});
    span->AddEvent("node.event", {}, UnixNs(kNodeEventNs));
    span->End(UnixNs(kNodeEndNs));
}

std::size_t CountIn(const std::string& text, const std::string& needle)
{
    std::size_t count = 0;
    for (std::size_t pos = text.find(needle); pos != std::string::npos;
         pos = text.find(needle, pos + needle.size()))
    {
        ++count;
    }
    return count;
}

/// The receiver's options: defaults, plus a resolver that names every
/// "127.0.0.1:<port>" sender, i.e. the full node, as trusted Unix time.
microtel::LeafReceiverOptions NodeTrustingOptions()
{
    microtel::LeafReceiverOptions opts;
    opts.resolver = [](std::string_view leaf_id) -> std::optional<microtel::LeafConfig>
    {
        if (!leaf_id.starts_with(kNodeIdPrefix))
        {
            return std::nullopt;
        }
        return microtel::LeafConfig{.time_mode = microtel::LeafTimeMode::Unix, .resource = {}};
    };
    return opts;
}

/// A Provider in the concentrator role, aimed at the collector's leaf receiver.
class LeafE2e : public ::testing::TestWithParam<microtel::Protocol>
{
protected:
    void SetUp() override
    {
        const bool http = GetParam() == microtel::Protocol::Http;
        std::string endpoint;
        if (!microtel::testing::ConformanceEnabled(http ? kHttpEndpointEnv : kGrpcEndpointEnv,
                                                   endpoint) ||
            !microtel::testing::ConformanceEnabled(kOutputFileEnv, m_output_file))
        {
            GTEST_SKIP() << kSkipReason;
        }

        microtel::SdkBuilder builder;
        microtel::testing::ConfigureConformanceBuilder(builder, endpoint, GetParam())
            .WithServiceName("microtel-leaf-concentrator")
            .WithBatch(microtel::BatchOptions{
                .max_queue_size = microtel::BatchOptions{}.max_queue_size,
                .max_export_batch_size = microtel::BatchOptions{}.max_export_batch_size,
                .schedule_delay = kScheduleDelay,
                .drop_policy = microtel::BatchOptions{}.drop_policy,
            })
            .WithLeafReceiver(NodeTrustingOptions());
        if (http)
        {
            // The HTTP leaf port serves TLS, so this stays on HTTP/2: the
            // collector's plaintext OTLP/HTTP receiver is HTTP/1.1-only (#166).
            builder.WithTls(microtel::TlsOptions{
                .ca_bundle = microtel::testing::GetEnv(kCaEnv).value_or(""),
                .client_cert = {},
                .client_key = {},
                .sni_override = {},
            });
        }
        auto result = builder.Build();
        ASSERT_TRUE(result.has_value()) << result.error().message;
        m_provider = std::move(*result);
        m_receiver = m_provider->GetLeafReceiver();
        ASSERT_NE(m_receiver, nullptr);
    }

    void TearDown() override
    {
        if (m_provider)
        {
            EXPECT_EQ(m_provider->Shutdown(kFlushTimeout), microtel::Status::Completed);
        }
    }

    /// @brief Flushes, then waits for the collector's line holding @p needle
    ///        to be complete: the file exporter may still be writing it when
    ///        the needle first appears, so a line is taken only once it is one
    ///        whole JSON object.
    std::string FlushAndRead(const std::string& needle)
    {
        EXPECT_EQ(m_provider->ForceFlush(kFlushTimeout), microtel::Status::Completed);
        const microtel::HealthSnapshot health = m_provider->GetExporterHealth();
        EXPECT_EQ(health.batches_failed, 0U) << health.last_error_message;
        const auto deadline = std::chrono::steady_clock::now() + kCollectorPollTimeout;
        std::string line;
        while (std::chrono::steady_clock::now() < deadline)
        {
            line = microtel::testing::PollForLineContaining(
                       m_output_file, needle, kCollectorPollTimeout)
                       .value_or(std::string{});
            if (!line.empty() && microtel::testing::detail::ObjectEnd(line, 0) == line.size())
            {
                return line;
            }
            std::this_thread::sleep_for(kRereadInterval);
        }
        ADD_FAILURE() << "collector never wrote a whole line containing '" << needle << "' to "
                      << m_output_file << "; last read: " << line;
        return {};
    }

    std::string m_output_file;
    std::shared_ptr<microtel::Provider> m_provider;
    std::shared_ptr<microtel::LeafReceiver> m_receiver;
};

// Eight leaves, one span each, flushed together: the collector must receive
// ONE request holding eight ResourceSpans, each carrying its leaf's transport
// id as device.id and its own Resource, with every span field intact and the
// timestamps corrected (concentrator-stamped: t' = t + R - E).
TEST_P(LeafE2e, ManyLeavesShareOneRequest)
{
    ASSERT_NE(m_receiver, nullptr) << kNoConcentrator;
    const std::string marker = microtel::testing::UniqueMarker();
    std::vector<Frame> frames;
    for (std::size_t i = 0; i < kLeafCount; ++i)
    {
        TestLeaf leaf(MICROTEL_LEAF_TIME_CONCENTRATOR_STAMPED,
                      "leaf-e2e-sensor",
                      static_cast<std::int64_t>(i));
        ASSERT_EQ(leaf.InitStatus(), MICROTEL_LEAF_OK);
        ASSERT_EQ(BuildSpan(leaf, marker + ".span-" + std::to_string(i), 0), MICROTEL_LEAF_OK);
        frames.push_back(Encode(leaf, marker + ".leaf-" + std::to_string(i), kEncodeAt));
    }

    // The backend under test is the one linked: nanopb streams the payload in
    // pieces, upb hands it over in one write (leaf.h, encode_to).
    if (kBackend == "upb")
    {
        EXPECT_EQ(frames.front().write_calls, 1U);
    }
    else
    {
        EXPECT_GT(frames.front().write_calls, 1U);
    }

    for (const Frame& frame : frames)
    {
        const microtel::IngestResult r = Ingest(*m_receiver, frame, kReceivedAtNs);
        EXPECT_EQ(r.status, microtel::IngestStatus::Accepted) << frame.leaf_id;
        EXPECT_EQ(r.spans_accepted, 1U) << frame.leaf_id;
    }

    const std::string line = FlushAndRead(marker + ".span-0");
    ASSERT_FALSE(line.empty());

    // One line is one request (see the file comment); it holds every leaf's
    // span, each exactly once in the whole file, one ResourceSpans per leaf.
    EXPECT_EQ(CountIn(line, R"("scopeSpans":)"), kLeafCount) << line;
    EXPECT_EQ(line.find("microtel.leaf."), std::string::npos)
        << "reserved leaf attributes reached the collector: " << line;

    const std::int64_t offset = kReceivedAtNs - static_cast<std::int64_t>(kEncodeAt);
    for (std::size_t i = 0; i < kLeafCount; ++i)
    {
        const std::string name = marker + ".span-" + std::to_string(i);
        const std::string name_json = R"("name":")" + name + R"(")";
        EXPECT_EQ(microtel::testing::CountOccurrences(m_output_file, name_json), 1U) << name;

        const std::string span =
            microtel::testing::EnclosingObject(line, name_json, 1).value_or("");
        const std::string resource_spans =
            microtel::testing::EnclosingObject(line, name_json, 3).value_or("");
        ASSERT_FALSE(resource_spans.empty()) << name;

        ExpectContains(resource_spans, DeviceIdJson(marker + ".leaf-" + std::to_string(i)));
        ExpectContains(resource_spans,
                       R"({"key":"service.name","value":{"stringValue":"leaf-e2e-sensor"}})");
        ExpectContains(resource_spans,
                       R"({"key":"sensor.index","value":{"intValue":")" + std::to_string(i) +
                           R"("}})");
        ExpectContains(resource_spans, R"("scope":{"name":"leaf.e2e"})");

        ExpectContains(span, R"("kind":3)");
        ExpectContains(span, TimeJson("startTimeUnixNano", offset + kSpanStart));
        ExpectContains(span, TimeJson("endTimeUnixNano", offset + kSpanEnd));
        ExpectContains(span, TimeJson("timeUnixNano", offset + kSpanEvent));
        ExpectContains(span,
                       R"({"key":"leaf.attr.string","value":{"stringValue":"string-value"}})");
        ExpectContains(span, R"({"key":"leaf.attr.int64","value":{"intValue":"42"}})");
        ExpectContains(span, R"({"key":"leaf.attr.double","value":{"doubleValue":2.5}})");
        ExpectContains(span, R"({"key":"leaf.attr.bool","value":{"boolValue":true}})");
        ExpectContains(span, R"("name":"leaf.event")");
        ExpectContains(span, R"({"key":"leaf.event.attr","value":{"stringValue":"event-value"}})");
        ExpectContains(span, R"("status":{"message":"leaf-error","code":2})");
    }
}

// One leaf per time mode (§5), each timestamp checked to the nanosecond.
//
// - concentrator-stamped: t' = t + (R - E).
// - sync-relative: the leaf converts to Unix time with its last clock_sync, and
//   the concentrator trusts it (fresh sync, small skew): t' = t.
// - boot-relative: t' = t + B, where B is the second-smallest of the leaf's
//   recent samples R - E. A second payload arriving 1 s "early" (a low
//   outlier) must not move B, which a concentrator-stamped correction would.
TEST_P(LeafE2e, TimeModesCorrectTimestamps)
{
    ASSERT_NE(m_receiver, nullptr) << kNoConcentrator;
    const std::string marker = microtel::testing::UniqueMarker();

    TestLeaf stamped(MICROTEL_LEAF_TIME_CONCENTRATOR_STAMPED, "leaf-e2e-stamped", 0);
    TestLeaf synced(MICROTEL_LEAF_TIME_SYNC_RELATIVE, "leaf-e2e-sync", 1);
    TestLeaf boot(MICROTEL_LEAF_TIME_BOOT_RELATIVE, "leaf-e2e-boot", 2);
    ASSERT_EQ(stamped.InitStatus(), MICROTEL_LEAF_OK);
    ASSERT_EQ(synced.InitStatus(), MICROTEL_LEAF_OK);
    ASSERT_EQ(boot.InitStatus(), MICROTEL_LEAF_OK);

    // The sync: the leaf clock read 500 when Unix time was R - 5 s.
    constexpr std::int64_t kSyncLeafNow = 500;
    constexpr std::int64_t kSyncUnix = kReceivedAtNs - 5'000'000'000;
    synced.SetClock(kSyncLeafNow);
    ASSERT_EQ(microtel_leaf_clock_sync(synced.Leaf(), kSyncUnix, kSyncLeafNow), MICROTEL_LEAF_OK);

    ASSERT_EQ(BuildSpan(stamped, marker + ".stamped", 0), MICROTEL_LEAF_OK);
    ASSERT_EQ(BuildSpan(synced, marker + ".sync", 0), MICROTEL_LEAF_OK);
    ASSERT_EQ(BuildSpan(boot, marker + ".boot-1", 0), MICROTEL_LEAF_OK);
    const Frame f_stamped = Encode(stamped, marker + ".stamped-leaf", kEncodeAt);
    const Frame f_synced = Encode(synced, marker + ".sync-leaf", kEncodeAt);
    const Frame f_boot1 = Encode(boot, marker + ".boot-leaf", kEncodeAt);

    // The boot leaf's second payload, 20 us later on its clock, but received
    // as if it had arrived 1 s before its own encode latency allows.
    constexpr std::uint64_t kSecondOffset = 20'000;
    constexpr std::int64_t kOutlierNs = 1'000'000'000;
    ASSERT_EQ(BuildSpan(boot, marker + ".boot-2", kSecondOffset), MICROTEL_LEAF_OK);
    const Frame f_boot2 = Encode(boot, marker + ".boot-leaf", kEncodeAt + kSecondOffset);

    EXPECT_EQ(Ingest(*m_receiver, f_stamped, kReceivedAtNs).status,
              microtel::IngestStatus::Accepted);
    EXPECT_EQ(Ingest(*m_receiver, f_synced, kReceivedAtNs).status,
              microtel::IngestStatus::Accepted);
    EXPECT_EQ(Ingest(*m_receiver, f_boot1, kReceivedAtNs).status, microtel::IngestStatus::Accepted);
    EXPECT_EQ(Ingest(*m_receiver,
                     f_boot2,
                     kReceivedAtNs + static_cast<std::int64_t>(kSecondOffset) - kOutlierNs)
                  .status,
              microtel::IngestStatus::Accepted);
    EXPECT_EQ(m_receiver->Stats().time_fallbacks, 0U);

    const std::string line = FlushAndRead(marker + ".stamped");
    ASSERT_FALSE(line.empty());

    const auto span_of = [&line](const std::string& name) {
        return microtel::testing::EnclosingObject(line, R"("name":")" + name + R"(")", 1)
            .value_or("");
    };

    const std::int64_t stamped_offset = kReceivedAtNs - static_cast<std::int64_t>(kEncodeAt);
    const std::string s_stamped = span_of(marker + ".stamped");
    ExpectContains(s_stamped, TimeJson("startTimeUnixNano", stamped_offset + kSpanStart));
    ExpectContains(s_stamped, TimeJson("endTimeUnixNano", stamped_offset + kSpanEnd));
    ExpectContains(s_stamped, TimeJson("timeUnixNano", stamped_offset + kSpanEvent));

    const std::int64_t sync_offset = kSyncUnix - kSyncLeafNow;
    const std::string s_synced = span_of(marker + ".sync");
    ExpectContains(s_synced, TimeJson("startTimeUnixNano", sync_offset + kSpanStart));
    ExpectContains(s_synced, TimeJson("endTimeUnixNano", sync_offset + kSpanEnd));
    ExpectContains(s_synced, TimeJson("timeUnixNano", sync_offset + kSpanEvent));

    // Both boot payloads use the first sample's anchor: the outlier is alone
    // below it, so the second-smallest sample is still the first one.
    const std::int64_t anchor = kReceivedAtNs - static_cast<std::int64_t>(kEncodeAt);
    const std::string s_boot1 = span_of(marker + ".boot-1");
    ExpectContains(s_boot1, TimeJson("startTimeUnixNano", anchor + kSpanStart));
    ExpectContains(s_boot1, TimeJson("endTimeUnixNano", anchor + kSpanEnd));
    const std::string s_boot2 = span_of(marker + ".boot-2");
    ExpectContains(s_boot2, TimeJson("startTimeUnixNano", anchor + kSpanStart + kSecondOffset));
    ExpectContains(s_boot2, TimeJson("endTimeUnixNano", anchor + kSpanEnd + kSecondOffset));
}

// A payload cut short on the link is rejected as Malformed, counted in the
// receiver's stats and the leaf_payload_malformed drop counter, and none of
// it reaches the collector; the same leaf's next, intact payload does.
TEST_P(LeafE2e, MalformedPayloadRejectedAndCounted)
{
    ASSERT_NE(m_receiver, nullptr) << kNoConcentrator;
    const std::string marker = microtel::testing::UniqueMarker();
    TestLeaf leaf(MICROTEL_LEAF_TIME_CONCENTRATOR_STAMPED, "leaf-e2e-malformed", 0);
    ASSERT_EQ(leaf.InitStatus(), MICROTEL_LEAF_OK);

    ASSERT_EQ(BuildSpan(leaf, marker + ".truncated", 0), MICROTEL_LEAF_OK);
    Frame truncated = Encode(leaf, marker + ".leaf", kEncodeAt);
    ASSERT_GT(truncated.bytes.size(), 1U);
    truncated.bytes.pop_back();

    ASSERT_EQ(BuildSpan(leaf, marker + ".intact", 0), MICROTEL_LEAF_OK);
    const Frame intact = Encode(leaf, marker + ".leaf", kEncodeAt);

    const microtel::IngestResult bad = Ingest(*m_receiver, truncated, kReceivedAtNs);
    EXPECT_EQ(bad.status, microtel::IngestStatus::Malformed);
    EXPECT_EQ(bad.spans_accepted, 0U);
    EXPECT_EQ(Ingest(*m_receiver, intact, kReceivedAtNs).status, microtel::IngestStatus::Accepted);

    const microtel::LeafReceiverStats stats = m_receiver->Stats();
    EXPECT_EQ(stats.payloads_rejected, 1U);
    EXPECT_EQ(stats.payloads_accepted, 1U);

    const std::string line = FlushAndRead(marker + ".intact");
    ASSERT_FALSE(line.empty());
    EXPECT_EQ(microtel::testing::CountOccurrences(m_output_file, marker + ".truncated"), 0U);

    const microtel::HealthSnapshot health = m_provider->GetExporterHealth();
    EXPECT_EQ(health.drop_counters.at(
                  static_cast<std::size_t>(microtel::DropReason::LeafPayloadMalformed)),
              1U);
}

// A full C++ node (ICP 0036) sends its spans over UDP with its own
// ExportTransport. The concentrator, which trusts it as Unix time, takes the
// datagram under its source address:port. Its span shares the export request
// with a C leaf's, keeps its own service.name and exact timestamps (t' = t),
// and carries the transport-derived device.id.
TEST_P(LeafE2e, FullNodeReachesTheCollectorThroughTheConcentrator)
{
    ASSERT_NE(m_receiver, nullptr) << kNoConcentrator;
    const std::string marker = microtel::testing::UniqueMarker();
    NodeLink link = OpenNodeLink();
    ASSERT_GE(link.rx.Get(), 0);
    const std::string node_id = link.node_id;
    const auto node = BuildNode(link, marker + ".node", kNodeMaxRequestBytes);
    ASSERT_NE(node, nullptr);

    TestLeaf leaf(MICROTEL_LEAF_TIME_CONCENTRATOR_STAMPED, "leaf-e2e-sensor", 0);
    ASSERT_EQ(leaf.InitStatus(), MICROTEL_LEAF_OK);
    ASSERT_EQ(BuildSpan(leaf, marker + ".leaf-span", 0), MICROTEL_LEAF_OK);
    EXPECT_EQ(Ingest(*m_receiver, Encode(leaf, marker + ".leaf", kEncodeAt), kReceivedAtNs).status,
              microtel::IngestStatus::Accepted);

    TraceNodeSpan(*node, marker + ".node-span");
    ASSERT_EQ(node->ForceFlush(kFlushTimeout), microtel::Status::Completed);
    EXPECT_EQ(node->GetExporterHealth().batches_sent, 1U);
    std::string from_id;
    const microtel::IngestResult r = ReceiveAndIngest(link.rx, *m_receiver, from_id);
    EXPECT_EQ(from_id, node_id);
    EXPECT_EQ(r.status, microtel::IngestStatus::Accepted);
    EXPECT_EQ(r.spans_accepted, 1U);
    EXPECT_EQ(node->Shutdown(kFlushTimeout), microtel::Status::Completed);

    const std::string line = FlushAndRead(marker + ".node-span");
    ASSERT_FALSE(line.empty());
    ExpectContains(line, marker + ".leaf-span");  // one request, leaf and node
    EXPECT_EQ(line.find("microtel.leaf."), std::string::npos) << line;

    const std::string name_json = R"("name":")" + marker + R"(.node-span")";
    const std::string span = microtel::testing::EnclosingObject(line, name_json, 1).value_or("");
    const std::string resource_spans =
        microtel::testing::EnclosingObject(line, name_json, 3).value_or("");
    ASSERT_FALSE(resource_spans.empty());
    ExpectContains(resource_spans, DeviceIdJson(node_id));
    ExpectContains(resource_spans,
                   R"({"key":"service.name","value":{"stringValue":"leaf-e2e-full-node"}})");
    ExpectContains(resource_spans, R"("scope":{"name":"node.e2e","version":"1.0"})");
    ExpectContains(span, R"("kind":2)");
    ExpectContains(span, TimeJson("startTimeUnixNano", kNodeStartNs));
    ExpectContains(span, TimeJson("endTimeUnixNano", kNodeEndNs));
    ExpectContains(span, TimeJson("timeUnixNano", kNodeEventNs));
    ExpectContains(span, R"({"key":"node.attr","value":{"stringValue":"node-value"}})");
}

// The three limits of ICP 0036 disagreeing: a node whose max_request_bytes
// is above the concentrator's max_payload_bytes sends a request between the
// two. The concentrator refuses it as TooLarge and counts
// leaf_payload_too_large, and nothing of it reaches the collector.
TEST_P(LeafE2e, FullNodeRequestOverThePayloadLimitIsDropped)
{
    ASSERT_NE(m_receiver, nullptr) << kNoConcentrator;
    const std::string marker = microtel::testing::UniqueMarker();
    // A second concentrator with a small payload limit, beside the fixture's.
    constexpr std::uint32_t kSmallPayloadLimit = 512;
    microtel::LeafReceiverOptions small = NodeTrustingOptions();
    small.max_payload_bytes = kSmallPayloadLimit;
    microtel::SdkBuilder builder;
    const std::string endpoint =
        microtel::testing::GetEnv(kGrpcEndpointEnv).value_or(std::string{});
    microtel::testing::ConfigureConformanceBuilder(builder, endpoint, microtel::Protocol::Grpc)
        .WithProfileName(marker + ".small-concentrator")
        .WithServiceName("microtel-leaf-concentrator")
        .WithLeafReceiver(small);
    auto built = builder.Build();
    ASSERT_TRUE(built.has_value()) << built.error().message;
    const std::shared_ptr<microtel::Provider> concentrator = std::move(*built);
    const auto receiver = concentrator->GetLeafReceiver();

    NodeLink link = OpenNodeLink();
    ASSERT_GE(link.rx.Get(), 0);
    const auto node = BuildNode(link, marker + ".node", kNodeMaxRequestBytes);
    ASSERT_NE(node, nullptr);
    constexpr int kSpans = 8;
    for (int i = 0; i < kSpans; ++i)
    {
        TraceNodeSpan(*node, marker + ".oversized");
    }
    ASSERT_EQ(node->ForceFlush(kFlushTimeout), microtel::Status::Completed);
    std::string from_id;
    const microtel::IngestResult r = ReceiveAndIngest(link.rx, *receiver, from_id);
    EXPECT_EQ(r.status, microtel::IngestStatus::TooLarge);
    EXPECT_EQ(node->Shutdown(kFlushTimeout), microtel::Status::Completed);

    EXPECT_EQ(concentrator->ForceFlush(kFlushTimeout), microtel::Status::Completed);
    const microtel::HealthSnapshot health = concentrator->GetExporterHealth();
    EXPECT_EQ(health.drop_counters.at(
                  static_cast<std::size_t>(microtel::DropReason::LeafPayloadTooLarge)),
              1U);
    EXPECT_EQ(concentrator->Shutdown(kFlushTimeout), microtel::Status::Completed);
    EXPECT_EQ(microtel::testing::CountOccurrences(m_output_file, marker + ".oversized"), 0U);
}

std::string ProtocolName(const ::testing::TestParamInfo<microtel::Protocol>& info)
{
    return info.param == microtel::Protocol::Http ? "Http" : "Grpc";
}

INSTANTIATE_TEST_SUITE_P(Protocols,
                         LeafE2e,
                         ::testing::Values(microtel::Protocol::Http, microtel::Protocol::Grpc),
                         ProtocolName);

}  // namespace
