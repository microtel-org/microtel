// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0
//
// tls — exporting to a collector over TLS, with a CA the machine does not
// otherwise trust.
//
// Four phases against the opt-in TLS overlay (examples/tls/up-tls.sh):
//
//   1. gRPC, no ca_bundle    system trust only. The overlay's certificate is
//                            signed by a throwaway CA nothing on the machine
//                            trusts, so the handshake is refused. This is the
//                            phase that proves verification is happening —
//                            a client that accepts anything passes every
//                            positive test in this file.
//   2. gRPC, ca_bundle       the same endpoint with the CA pinned. Works.
//   3. HTTP, ca_bundle       OTLP/HTTP over TLS, where ALPN negotiates `h2`.
//                            Plaintext http:// to a collector's HTTP
//                            receiver is HTTP/1.1, one request at a time
//                            (docs/compatibility-matrix.md §4).
//   4. gRPC, mTLS            client certificate as well, against a receiver
//                            configured with a client CA.
//
// microtel's TLS floor is **1.2**: it calls
// SSL_CTX_set_min_proto_version(TLS1_2_VERSION) itself rather than inheriting
// the linked OpenSSL's floor, and there is no option to lower it. A receiver
// limited to TLS 1.0/1.1 fails the handshake on every build alike
// (docs/compatibility-matrix.md §3).
//
// Usage:
//   tls [grpc-endpoint] [http-endpoint] [mtls-endpoint] [cert-dir]
//
// The defaults are the overlay's ports and examples/tls/certs, which resolves
// when the binary is run from the repository root:
//   ./build/examples/microtel_example_tls

#include "microtel/provider.hpp"
#include "microtel/sdk_builder.hpp"
#include "microtel/span.hpp"
#include "microtel/tracer.hpp"

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace
{

constexpr const char* kDefaultGrpcEndpoint{"https://localhost:5327"};
constexpr const char* kDefaultHttpEndpoint{"https://localhost:5328"};
constexpr const char* kDefaultMtlsEndpoint{"https://localhost:5337"};
constexpr const char* kDefaultCertDir{"examples/tls/certs"};

constexpr const char* kServiceName{"microtel-tls-example"};
constexpr const char* kScopeVersion{"1.0.0"};

constexpr std::chrono::seconds kFlushTimeout{10};
constexpr std::chrono::seconds kShutdownTimeout{5};

// Short enough that the phase that is meant to fail fails in seconds rather
// than sitting out a default sixty-second retry budget.
constexpr microtel::TimeoutOptions kTimeouts{.connect = std::chrono::seconds{5},
                                             .tls_handshake = std::chrono::seconds{5},
                                             .per_export = std::chrono::seconds{5},
                                             .retry_budget = std::chrono::seconds{5},
                                             .flush = std::chrono::seconds{10},
                                             .shutdown = std::chrono::seconds{5}};

const char* StatusToString(microtel::Status status) noexcept
{
    switch (status)
    {
        case microtel::Status::Completed:
            return "Completed";
        case microtel::Status::TimedOut:
            return "TimedOut";
        case microtel::Status::AlreadyShutDown:
            return "AlreadyShutDown";
        case microtel::Status::Failed:
            return "Failed";
        case microtel::Status::InvalidArgument:
            return "InvalidArgument";
        case microtel::Status::Unsupported:
            return "Unsupported";
    }
    return "Unknown";
}

const char* ConnectionStateToString(microtel::ConnectionState state) noexcept
{
    switch (state)
    {
        case microtel::ConnectionState::Disconnected:
            return "Disconnected";
        case microtel::ConnectionState::Connecting:
            return "Connecting";
        case microtel::ConnectionState::Connected:
            return "Connected";
        case microtel::ConnectionState::Reconnecting:
            return "Reconnecting";
        case microtel::ConnectionState::Closed:
            return "Closed";
    }
    return "Unknown";
}

std::size_t DropCount(const microtel::HealthSnapshot& health, microtel::DropReason reason)
{
    return health.drop_counters[static_cast<std::size_t>(reason)];
}

/// @brief The health fields this example can move. `health_and_backpressure/`
///        prints the whole snapshot with every drop counter named.
void PrintHealth(const microtel::HealthSnapshot& health)
{
    std::cout << "  health: connection_state=" << ConnectionStateToString(health.connection_state)
              << " batches_sent=" << health.batches_sent
              << " batches_failed=" << health.batches_failed
              << " rejected=" << DropCount(health, microtel::DropReason::PartialSuccessRejection)
              << " ConnectFailure=" << DropCount(health, microtel::DropReason::ConnectFailure)
              << '\n';
    if (!health.last_error_message.empty())
    {
        std::cout << "  last_error: " << health.last_error_message << '\n';
    }
}

struct TlsPhase
{
    const char* label;
    const char* profile;
    std::string endpoint;
    microtel::Protocol protocol;
    microtel::TlsOptions tls;
    bool expect_export;
};

/// @brief Build a provider, emit one span, flush, and report health.
/// @return true when the batch reached the collector and it rejected no span
///         via OTLP partial success.
bool RunPhase(const TlsPhase& phase)
{
    std::cout << "\n=== " << phase.label << " ===\n  endpoint: " << phase.endpoint
              << "\n  ca_bundle: "
              << (phase.tls.ca_bundle.empty() ? "(system trust)" : phase.tls.ca_bundle.string())
              << '\n';

    auto built = microtel::SdkBuilder{}
                     .WithEndpoint(phase.endpoint)
                     .WithProtocol(phase.protocol)
                     .WithServiceName(kServiceName)
                     .WithServiceVersion(kScopeVersion)
                     .WithProfileName(phase.profile)
                     .WithTls(phase.tls)
                     .WithTimeouts(kTimeouts)
                     .Build();
    if (!built)
    {
        // Unreadable TLS material is caught here, before any socket: Build()
        // validates that the files exist and parse (ConfigError::Kind::
        // TlsMaterialUnreadable), and network reachability is never validated.
        std::cerr << "  Build() failed: " << built.error().message << '\n';
        return false;
    }
    const std::shared_ptr<microtel::Provider> provider = std::move(*built);

    if (auto connected = provider->Connect(); !connected)
    {
        std::cout << "  Connect() failed: " << connected.error().message << '\n';
    }
    else
    {
        std::cout << "  Connect(): ok\n";
    }

    const std::shared_ptr<microtel::Tracer> tracer =
        provider->GetTracer("microtel-tls-example", kScopeVersion);
    const auto span = tracer->StartSpan(
        "tls.request",
        {.kind = microtel::SpanKind::Client, .parent = {}, .start_time = {}, .attributes = {}});
    span->SetAttribute("phase", std::string{phase.label});
    span->SetStatus(microtel::StatusCode::Ok);
    const std::string trace_id = span->GetContext().trace_id.ToHex();
    span->End();

    std::cout << "  trace_id: " << trace_id << '\n'
              << "  ForceFlush: " << StatusToString(provider->ForceFlush(kFlushTimeout)) << '\n';

    const microtel::HealthSnapshot health = provider->GetExporterHealth();
    PrintHealth(health);
    std::cout << "  Shutdown: " << StatusToString(provider->Shutdown(kShutdownTimeout)) << '\n';

    return health.batches_sent > 0 && health.batches_failed == 0 &&
           DropCount(health, microtel::DropReason::PartialSuccessRejection) == 0;
}

std::vector<TlsPhase> MakePhases(const std::string& grpc_endpoint,
                                 const std::string& http_endpoint,
                                 const std::string& mtls_endpoint,
                                 const std::filesystem::path& cert_dir)
{
    const std::filesystem::path ca = cert_dir / "ca.crt";
    const std::filesystem::path client_cert = cert_dir / "client.crt";
    const std::filesystem::path client_key = cert_dir / "client.key";

    std::vector<TlsPhase> phases;
    phases.push_back({.label = "phase 1: gRPC over TLS, no ca_bundle (expected to fail)",
                      .profile = "tls-system-trust",
                      .endpoint = grpc_endpoint,
                      .protocol = microtel::Protocol::Grpc,
                      // Empty ca_bundle means the system trust store. The
                      // overlay's CA is not in it, and `insecure` is false, so
                      // the peer is verified and rejected.
                      .tls = {.insecure = false,
                              .ca_bundle = {},
                              .client_cert = {},
                              .client_key = {},
                              .sni_override = {}},
                      .expect_export = false});

    phases.push_back({.label = "phase 2: gRPC over TLS, ca_bundle pinned",
                      .profile = "tls-custom-ca",
                      .endpoint = grpc_endpoint,
                      .protocol = microtel::Protocol::Grpc,
                      .tls = {.insecure = false,
                              .ca_bundle = ca,
                              .client_cert = {},
                              .client_key = {},
                              .sni_override = {}},
                      .expect_export = true});

    phases.push_back({.label = "phase 3: OTLP/HTTP over TLS, ca_bundle pinned",
                      .profile = "tls-http",
                      .endpoint = http_endpoint,
                      .protocol = microtel::Protocol::Http,
                      .tls = {.insecure = false,
                              .ca_bundle = ca,
                              .client_cert = {},
                              .client_key = {},
                              .sni_override = {}},
                      .expect_export = true});

    if (std::filesystem::exists(client_cert) && std::filesystem::exists(client_key))
    {
        phases.push_back({.label = "phase 4: gRPC over mTLS, client certificate presented",
                          .profile = "tls-mtls",
                          .endpoint = mtls_endpoint,
                          .protocol = microtel::Protocol::Grpc,
                          .tls = {.insecure = false,
                                  .ca_bundle = ca,
                                  .client_cert = client_cert,
                                  .client_key = client_key,
                                  .sni_override = {}},
                          .expect_export = true});
    }
    else
    {
        std::cout << "note: no client certificate in " << cert_dir
                  << " — skipping the mTLS phase. Run examples/tls/gen-certs.sh.\n";
    }
    return phases;
}

}  // namespace

int main(int argc, char** argv)
{
    const std::string grpc_endpoint{(argc > 1) ? argv[1] : kDefaultGrpcEndpoint};
    const std::string http_endpoint{(argc > 2) ? argv[2] : kDefaultHttpEndpoint};
    const std::string mtls_endpoint{(argc > 3) ? argv[3] : kDefaultMtlsEndpoint};
    const std::filesystem::path cert_dir{(argc > 4) ? argv[4] : kDefaultCertDir};

    std::cout << "cert dir: " << cert_dir
              << "\nstart the TLS overlay with: examples/tls/up-tls.sh\n";
    if (!std::filesystem::exists(cert_dir / "ca.crt"))
    {
        std::cerr << "error: " << (cert_dir / "ca.crt")
                  << " not found. Run examples/tls/gen-certs.sh, or pass the cert "
                     "directory as the fourth argument.\n";
        return 1;
    }

    bool as_documented = true;
    for (const TlsPhase& phase : MakePhases(grpc_endpoint, http_endpoint, mtls_endpoint, cert_dir))
    {
        const bool exported = RunPhase(phase);
        if (exported != phase.expect_export)
        {
            std::cerr << "  UNEXPECTED: this phase " << (exported ? "exported" : "did not export")
                      << ", which is not what it is written to demonstrate\n";
            as_documented = false;
        }
    }

    std::cout << "\nGrafana: http://localhost:3000 — the successful phases' traces are forwarded\n"
                 "         by the overlay collector into the shared stack's Tempo.\n";

    return as_documented ? 0 : 2;
}
