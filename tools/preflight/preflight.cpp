// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#include "preflight/preflight.hpp"

#include "microtel/protocol.hpp"
#include "microtel/provider.hpp"
#include "microtel/sdk_builder.hpp"
#include "microtel/span.hpp"
#include "microtel/status.hpp"
#include "microtel/tracer.hpp"

#include "common/config/config.hpp"
#include "common/config/config_validator.hpp"
#include "common/config/env_resolver.hpp"
#include "common/config/toml_loader.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>
#include <utility>

namespace tools
{

namespace
{

constexpr std::string_view kVersion = "1.2.4";
constexpr std::string_view kPreflightFlag = "--preflight=";

/// The synthetic span's `service.name`, fixed so a collector rule
/// can drop preflight traffic by matching it.
constexpr std::string_view kPreflightServiceName = "microtel-preflight";

/// Spell a protocol the way the `microtel.protocol` attribute reads.
[[nodiscard]] constexpr std::string_view ProtocolName(microtel::Protocol protocol) noexcept
{
    return protocol == microtel::Protocol::Grpc ? "grpc" : "http";
}

enum class Mode
{
    Connect,
    Export,
};

struct PreflightArgs
{
    Mode mode;
    std::optional<std::filesystem::path> config_path;
};

[[nodiscard]] std::optional<PreflightArgs> ParseArgs(int argc, char** argv, std::ostream& err)
{
    if (argc < 2)
    {
        err << "Usage: microtel-preflight --preflight={connect|export} [config.toml]\n";
        return std::nullopt;
    }

    const std::string_view arg1{argv[1]};
    if (arg1.substr(0, kPreflightFlag.size()) != kPreflightFlag)
    {
        err << "error: expected --preflight={connect|export}, got: " << arg1 << "\n";
        return std::nullopt;
    }

    const std::string_view mode_str = arg1.substr(kPreflightFlag.size());
    Mode mode;
    if (mode_str == "connect")
    {
        mode = Mode::Connect;
    }
    else if (mode_str == "export")
    {
        mode = Mode::Export;
    }
    else
    {
        err << "error: unknown preflight mode '" << mode_str
            << "' (expected 'connect' or 'export')\n";
        return std::nullopt;
    }

    std::optional<std::filesystem::path> config_path;
    if (argc >= 3)
    {
        config_path = std::filesystem::path{argv[2]};
    }

    return PreflightArgs{.mode = mode, .config_path = std::move(config_path)};
}

}  // namespace

SpanIdentity ResolveSpanIdentity(std::string_view config_path)
{
    // The service name is preflight's own and is not resolved from anything:
    // it is fixed precisely so that a collector rule can match it, and
    // a value the file under test could change would not be that.
    //
    // The protocol is the opposite — it is whatever the configuration under
    // test resolves to, so it has to be computed. `SdkBuilder::Build()` runs
    // exactly the chain below and then keeps the result to itself, so the only
    // way to report it truthfully today is to run the chain again. preflight
    // applies no exporter-level code override, so the two agree by
    // construction; an override added here without a matching line below is
    // the one way that could stop being true.
    //
    // Every failure below is deliberately discarded. `Build()` runs next and
    // reports the same failure with a field-level message, and a run that
    // cannot be configured never reaches the span.
    microtel::config::Config cfg;

    if (!config_path.empty())
    {
        auto file_cfg = microtel::config::LoadToml(std::filesystem::path{config_path});
        if (file_cfg)
        {
            cfg = std::move(*file_cfg);
        }
    }

    if (microtel::config::OverlayEnv(cfg))
    {
        // `Validate` is what turns a `grpc://` scheme into `Protocol::Grpc`,
        // and it does so before it parses the endpoint URL — so even an
        // unparseable endpoint leaves the protocol resolved behind it.
        (void)microtel::config::Validate(cfg);
    }

    return SpanIdentity{.service_name = std::string{kPreflightServiceName},
                        .protocol = std::string{ProtocolName(cfg.protocol)},
                        .version = std::string{kVersion}};
}

int ReportExport(microtel::Status flush,
                 const microtel::HealthSnapshot& health,
                 std::ostream& out,
                 std::ostream& err)
{
    if (flush != microtel::Status::Completed)
    {
        err << "error: export timed out or failed\n";
        return kExitRuntime;
    }

    // Completed only means the queue drained; a batch the collector rejected
    // still counts as drained. The span was exported only if no batch failed.
    if (health.batches_failed != 0)
    {
        err << "error: export failed: the collector did not accept the batch: "
            << health.last_error_message << "\n";
        return kExitRuntime;
    }

    // OTLP partial success: the collector answered success but rejected some
    // or all of the items. The batch counts as sent, so it never reaches
    // batches_failed; the rejected count is only in this drop counter.
    const std::uint64_t rejected =
        health
            .drop_counters[static_cast<std::size_t>(microtel::DropReason::PartialSuccessRejection)];
    if (rejected != 0)
    {
        err << "error: export failed: the collector rejected " << rejected
            << " span(s) via OTLP partial success\n";
        return kExitRuntime;
    }

    out << "export OK\n";
    return kExitOk;
}

int RunPreflight(int argc, char** argv)
{
    return RunPreflight(argc, argv, std::cout, std::cerr);
}

int RunPreflight(int argc, char** argv, std::ostream& out, std::ostream& err)
{
    const auto args = ParseArgs(argc, argv, err);
    if (!args)
    {
        return kExitUsage;
    }

    const std::string config_path = args->config_path ? args->config_path->string() : std::string{};
    const SpanIdentity identity = ResolveSpanIdentity(config_path);

    // Build Provider from file → env → defaults.
    microtel::SdkBuilder builder;
    if (args->config_path)
    {
        builder.FromFile(*args->config_path);
    }

    // The synthetic span's service name is fixed, and it has to survive
    // a run against an operator's own config: the value exists so a collector
    // rule can drop preflight traffic, which it cannot do if the file under
    // test renames it. A code override is the highest-precedence layer
    // (docs/configuration.md §2), so this wins over file and environment.
    builder.WithServiceName(identity.service_name);

    auto provider_result = builder.Build();
    if (!provider_result)
    {
        err << "error: configuration failed: " << provider_result.error().message << "\n";
        return kExitConfig;
    }

    auto& provider = *provider_result;

    // Establish connection.
    auto connect_result = provider->Connect();
    if (!connect_result)
    {
        err << "error: connect failed: " << connect_result.error().message << "\n";
        (void)provider->Shutdown(std::chrono::seconds(5));
        return kExitRuntime;
    }

    if (args->mode == Mode::Connect)
    {
        out << "connect OK\n";
        (void)provider->Shutdown(std::chrono::seconds(5));
        return kExitOk;
    }

    // Export mode: send one synthetic span tagged so collectors can drop it.
    auto tracer = provider->GetTracer("microtel-preflight", std::string{kVersion});
    {
        auto span = tracer->StartSpan("microtel.preflight");
        span->SetAttribute("microtel.preflight", true);
        span->SetAttribute("microtel.version", std::string{kVersion});
        span->SetAttribute("microtel.protocol", identity.protocol);
        span->End();
    }

    const auto status = provider->ForceFlush(std::chrono::seconds(10));
    const microtel::HealthSnapshot health = provider->GetExporterHealth();
    (void)provider->Shutdown(std::chrono::seconds(5));

    return ReportExport(status, health, out, err);
}

}  // namespace tools
