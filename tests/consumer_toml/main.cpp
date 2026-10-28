// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0
//
// microtel + a compiled toml++ in one process. See CMakeLists.txt for why.
//
// The application parses TOML through its own toml++ library; microtel parses
// microtel.toml through the header-only copy inside libmicrotel_config.a.
// Each side also throws and catches toml::parse_error, which only works if
// both agree on that type's identity. The application parses again after
// microtel has, so a corrupted shared definition cannot hide behind ordering.
//
// No network: the endpoint is a closed port and every timeout is short.

#include <microtel/error.hpp>
#include <microtel/provider.hpp>
#include <microtel/sdk_builder.hpp>
#include <microtel/span.hpp>
#include <microtel/status.hpp>
#include <microtel/tracer.hpp>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>

#include <toml++/toml.hpp>

// The application's own toml++ is the compiled library: its CMake target sets
// TOML_HEADER_ONLY=0, so every toml++ function this file calls is declared
// here and defined in libtomlplusplus.
static_assert(TOML_HEADER_ONLY == 0, "consumer_toml must use toml++ as a compiled library");

namespace
{

constexpr std::chrono::milliseconds kFastTimeout{250};
constexpr std::chrono::milliseconds kShutdownTimeout{500};
constexpr std::int64_t kExpectedPort{8080};

constexpr std::string_view kAppToml = R"(
[app]
name = "greenhouse"
port = 8080
)";

constexpr std::string_view kMicrotelToml = R"(
[config]
unknown_keys = "error"

[exporter]
endpoint = "http://127.0.0.1:1"
protocol = "grpc"

[service]
name = "microtel-consumer-toml"
version = "0.1.0"
)";

bool Check(const bool ok, const char* what)
{
    std::cout << (ok ? "ok   " : "FAIL ") << what << '\n';
    return ok;
}

// The application's toml++: parse, read two values, and catch a syntax error.
bool AppParses(const char* when)
{
    const toml::table tbl = toml::parse(kAppToml);
    const std::string name = tbl["app"]["name"].value_or(std::string{});
    const std::int64_t port = tbl["app"]["port"].value_or(std::int64_t{0});
    std::cout << "app (" << when << "): app.name=" << name << " app.port=" << port << '\n';

    bool caught = false;
    try
    {
        static_cast<void>(toml::parse("broken = [1, 2"));
    }
    catch (const toml::parse_error& e)
    {
        std::cout << "app (" << when << "): parse_error \"" << e.description() << "\" at line "
                  << e.source().begin.line << '\n';
        caught = true;
    }
    return name == "greenhouse" && port == kExpectedPort && caught;
}

std::filesystem::path WriteFile(const std::string& name, const std::string_view content)
{
    const std::filesystem::path path = std::filesystem::temp_directory_path() / name;
    std::ofstream out{path};
    out << content;
    return path;
}

microtel::Expected<std::shared_ptr<microtel::Provider>, microtel::ConfigError> BuildFrom(
    const std::filesystem::path& path)
{
    const microtel::TimeoutOptions timeouts{.connect = kFastTimeout,
                                            .tls_handshake = kFastTimeout,
                                            .per_export = kFastTimeout,
                                            .retry_budget = kFastTimeout,
                                            .flush = kShutdownTimeout,
                                            .shutdown = kShutdownTimeout};
    return microtel::SdkBuilder{}.FromFile(path).WithTimeouts(timeouts).Build();
}

// microtel's header-only toml++: a valid file builds a provider that traces.
bool MicrotelParsesGoodFile(const std::filesystem::path& path)
{
    auto built = BuildFrom(path);
    if (!built)
    {
        std::cout << "microtel: Build() failed: " << built.error().message << '\n';
        return false;
    }
    const std::shared_ptr<microtel::Provider> provider = std::move(*built);
    const std::shared_ptr<microtel::Tracer> tracer =
        provider->GetTracer("microtel-consumer-toml", "0.1.0");
    const microtel::SpanHandle span = tracer->StartSpan(
        "consumer.toml",
        {.kind = microtel::SpanKind::Internal, .parent = {}, .start_time = {}, .attributes = {}});
    const std::string trace_hex = span->GetContext().trace_id.ToHex();
    span->End();
    std::cout << "microtel: built from " << path.filename().string() << ", trace_id=" << trace_hex
              << '\n';
    // The endpoint is a closed port, so the span cannot be exported: it is
    // either still pending at the deadline or failed its last attempt.
    const microtel::Status shutdown = provider->Shutdown(kShutdownTimeout);
    return shutdown == microtel::Status::Failed || shutdown == microtel::Status::TimedOut;
}

// microtel's header-only toml++ throws parse_error and microtel catches it.
bool MicrotelRejects(const std::filesystem::path& path, const microtel::ConfigError::Kind kind)
{
    const auto built = BuildFrom(path);
    if (built)
    {
        return false;
    }
    std::cout << "microtel: " << path.filename().string()
              << " rejected: kind=" << static_cast<int>(built.error().kind) << " message=\""
              << built.error().message << "\"\n";
    return built.error().kind == kind;
}

}  // namespace

int main()
{
    std::cout << "toml++ " << TOML_LIB_MAJOR << '.' << TOML_LIB_MINOR << '.' << TOML_LIB_PATCH
              << ", application TOML_HEADER_ONLY=" << TOML_HEADER_ONLY << '\n';

    bool ok = Check(AppParses("before microtel"), "application toml++ parses and throws");

    const std::filesystem::path good = WriteFile("microtel-consumer-toml-good.toml", kMicrotelToml);
    const std::filesystem::path broken =
        WriteFile("microtel-consumer-toml-broken.toml", "[exporter\nendpoint = 1\n");
    const std::filesystem::path unknown = WriteFile("microtel-consumer-toml-unknown.toml",
                                                    "[config]\nunknown_keys = \"error\"\n[nope]\n");

    ok = Check(MicrotelParsesGoodFile(good), "microtel FromFile() builds and traces") && ok;
    ok = Check(MicrotelRejects(broken, microtel::ConfigError::Kind::FileParseFailure),
               "microtel reports a TOML syntax error") &&
         ok;
    ok = Check(MicrotelRejects(unknown, microtel::ConfigError::Kind::UnknownKey),
               "microtel reports an unknown key") &&
         ok;

    ok = Check(AppParses("after microtel"), "application toml++ still parses and throws") && ok;

    std::error_code ec;
    std::filesystem::remove(good, ec);
    std::filesystem::remove(broken, ec);
    std::filesystem::remove(unknown, ec);

    std::cout << (ok ? "PASS" : "FAIL") << '\n';
    return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
