// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#include "backend.hpp"
#include "control_socket.hpp"
#include "workload.hpp"

#include <cstdlib>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>

namespace
{

constexpr int kDefaultControlPort = 19090;

std::string EnvOr(const char* name, const char* fallback)
{
    const char* val = std::getenv(name);  // NOLINT(concurrency-mt-unsafe)
    return (val != nullptr && val[0] != '\0') ? std::string(val) : std::string(fallback);
}

}  // namespace

int main()
{
    // Configuration via environment variables — injected by the SUT Dockerfile
    // or overridden on the developer machine.
    // OTEL_EXPORTER_OTLP_ENDPOINT is the canonical env var set by the bench driver.
    // Fall back to EMIT_ENDPOINT for developer convenience.
    const char* otel_ep =
        std::getenv("OTEL_EXPORTER_OTLP_ENDPOINT");  // NOLINT(concurrency-mt-unsafe)
    const std::string endpoint = (otel_ep != nullptr && otel_ep[0] != '\0')
                                     ? std::string(otel_ep)
                                     : EnvOr("EMIT_ENDPOINT", "http://sink:4318");
    const std::string svc_name = EnvOr("EMIT_SERVICE_NAME", "bench");
    const std::string svc_version = EnvOr("EMIT_SERVICE_VER", "0.0.0");
    const int attrs_per_span = std::stoi(EnvOr("EMIT_ATTRIBUTES_PER_SPAN", "0"));
    const int attr_value_bytes = std::stoi(EnvOr("EMIT_ATTRIBUTE_VALUE_BYTES", "24"));
    const std::string workload_env = EnvOr("EMIT_WORKLOAD", "hot_loop");

    const bench::WorkloadMode mode = bench::ParseWorkloadMode(workload_env);

    const bool compression_gzip = EnvOr("EMIT_COMPRESSION_GZIP", "0") == "1";

    const bench::BackendOptions opts{
        .endpoint = endpoint,
        .service_name = svc_name,
        .service_version = svc_version,
        .compression_gzip = compression_gzip,
        .attributes_per_span = attrs_per_span,
        .attribute_value_bytes = attr_value_bytes,
        .metric_interval_ms = (mode == bench::WorkloadMode::HotLoopMetrics) ? 100 : 0,
        .logs_enabled = (mode == bench::WorkloadMode::HotLoopLogs),
        .leaf_count =
            (mode == bench::WorkloadMode::LeafFanin) ? std::stoi(EnvOr("EMIT_LEAVES", "100")) : 0,
        .leaf_spans_per_payload = std::stoi(EnvOr("EMIT_LEAF_SPANS_PER_PAYLOAD", "10")),
        // CA for an https:// endpoint.  The interop harness needs this: stock
        // collector/Jaeger OTLP/HTTP receivers only speak HTTP/2 over TLS (ALPN
        // h2); on plaintext they serve HTTP/1.1, which microtel sends serially.
        .tls_ca_bundle = EnvOr("OTEL_EXPORTER_OTLP_CERTIFICATE", ""),
    };

    std::unique_ptr<bench::IBackend> backend{bench::CreateBackend()};

    try
    {
        backend->Init(opts);
    }
    catch (const std::exception& ex)
    {
        std::cerr << "emit_app: backend init failed: " << ex.what() << '\n';
        return 1;
    }

    std::cerr << "emit_app: ready on control port " << kDefaultControlPort << '\n';

    try
    {
        bench::RunControlLoop(kDefaultControlPort, *backend, mode);
    }
    catch (const std::exception& ex)
    {
        std::cerr << "emit_app: control loop error: " << ex.what() << '\n';
        backend->Shutdown();
        return 1;
    }

    backend->Shutdown();
    return 0;
}
