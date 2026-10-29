// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0
//
// microtel consumer smoke test — ICP 0020 Decision 6.
//
// What a consumer of an *installed* microtel looks like: find_package, link
// microtel::microtel, include <microtel/…>, done. It doubles as the canonical
// find_package example (docs/development.md §12), so it stays short enough to
// read in one screen.
//
// What it proves about the install tree, none of which any in-tree test can:
//
//   - the public headers are all installed and still include one another
//     correctly from their installed locations (including internal/ and the
//     vendored tl::expected under microtel/vendor/);
//   - the export set resolves the whole static closure at link time —
//     TraceId::ToHex lives in libmicrotel_api.a, and nothing but the export set
//     puts that archive on a consumer's link line;
//   - microtelConfig.cmake's find_dependency calls resolve the archives'
//     undefined references to zlib, OpenSSL and nghttp2;
//   - the version find_package() matched (microtelConfigVersion.cmake, from
//     PROJECT_VERSION) is the version the installed headers report;
//   - the result runs.
//
// It makes no network assumption. The endpoint is a closed port, the timeouts
// are a few hundred milliseconds, and a failed Connect() is the expected
// outcome — so it passes offline and cannot flake on a slow machine. The
// assertions are deliberately loose about exporter behaviour: this gate is
// about the package, and the unit, integration and conformance suites own
// behaviour.

#include <microtel/baggage.hpp>
#include <microtel/error.hpp>
#include <microtel/provider.hpp>
#include <microtel/resource_detectors.hpp>
#include <microtel/sdk_builder.hpp>
#include <microtel/span.hpp>
#include <microtel/status.hpp>
#include <microtel/sugar.hpp>
#include <microtel/trace.hpp>
#include <microtel/tracer.hpp>
#include <microtel/version.hpp>

#include <chrono>
#include <cstddef>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>

namespace
{

// Nothing here is meant to reach a collector, and the gate should not spend
// seconds proving it.
constexpr std::chrono::milliseconds kFastTimeout{250};
constexpr std::chrono::milliseconds kFlushTimeout{500};
constexpr std::chrono::milliseconds kShutdownTimeout{500};

// Port 1 (tcpmux) needs root to bind, and nothing on a CI runner or a
// developer box does — so Connect() here is a connection refused, not a hang.
constexpr const char* kDeadEndpoint{"http://127.0.0.1:1"};

constexpr std::size_t kTraceIdHexChars{32};
constexpr std::size_t kSpanIdHexChars{16};

const char* StatusToString(const microtel::Status status) noexcept
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
        // v1.1 adds two enumerators for the hot-reload setters (ICP 0026 §2).
        // This switch is exhaustive without a default, so it is exactly the
        // in-tree shape that sees a new -Wswitch warning; external code with
        // the same shape sees the same one.
        case microtel::Status::InvalidArgument:
            return "InvalidArgument";
        case microtel::Status::Unsupported:
            return "Unsupported";
    }
    return "Unknown";
}

bool Check(const bool ok, const char* what)
{
    std::cout << (ok ? "ok   " : "FAIL ") << what << '\n';
    return ok;
}

// --- v1.1 sugar layer (ICP 0028) -------------------------------------------
//
// Four installed headers with no archive behind them:
// <microtel/sugar.hpp> and <microtel/sugar/{span,exception,attr_key}.hpp>.
// They ship only because the package installs include/microtel as a directory
// (ICP 0020 Decision 2), and `microtel::sugar` is deliberately *not* an
// exported CMake target (ICP 0028 §4) — so this is the only gate that can
// prove a consumer linking nothing but microtel::microtel can include and use
// them. Header-only means it must also add nothing to the link line.

constexpr microtel::sugar::AttrKey kSugarTag{"consumer.sugar"};

bool SugarSmoke(microtel::Tracer& tracer)
{
    MICROTEL_TRACE_FUNCTION(tracer);
    const microtel::ScopedSpan child = microtel::sugar::Span(
        tracer, "consumer.sugar.child", {kSugarTag(std::string{"installed-tree"})});
    return child.Get() != nullptr &&
           microtel::sugar::Traced(tracer, "consumer.sugar.traced", [] { return true; });
}

}  // namespace

int main()
{
    const microtel::TimeoutOptions timeouts{.connect = kFastTimeout,
                                            .tls_handshake = kFastTimeout,
                                            .per_export = kFastTimeout,
                                            .retry_budget = kFastTimeout,
                                            .flush = kFlushTimeout,
                                            .shutdown = kShutdownTimeout};

    // The two built-in resource detectors, reached through the installed
    // <microtel/resource_detectors.hpp> and handed to the installed
    // WithResourceDetector. Both are v1.1 public surface, so both are export-set
    // material: an unresolved MakeProcessDetector here is exactly the kind of
    // missing-archive defect this gate exists to catch.
    auto built = microtel::SdkBuilder{}
                     .WithEndpoint(kDeadEndpoint)
                     .WithProtocol(microtel::Protocol::Grpc)
                     .WithServiceName("microtel-consumer-smoke")
                     .WithServiceVersion("0.1.0")
                     .WithResourceDetector(microtel::MakeProcessDetector())
                     .WithResourceDetector(microtel::MakeHostDetector())
                     .WithTimeouts(timeouts)
                     .Build();

    if (!built)
    {
        std::cerr << "FAIL SdkBuilder::Build(): " << built.error().message << '\n';
        return 1;
    }

    const std::shared_ptr<microtel::Provider> provider = std::move(*built);
    bool ok = Check(provider != nullptr, "Build() returned a provider");

    // A `find_package(microtel 1.2)` requirement is satisfied by
    // microtelConfigVersion.cmake, which is written from PROJECT_VERSION; the
    // code then reports kVersionString. Both are hand-set (docs/releasing.md §1),
    // and this is the only place the *installed* pair meets.
    std::cout << "package version=" << MICROTEL_CONSUMER_PACKAGE_VERSION
              << " kVersionString=" << microtel::kVersionString << '\n';
    ok = Check(microtel::kVersionString == std::string_view{MICROTEL_CONSUMER_PACKAGE_VERSION},
               "the installed package version is the installed kVersionString") &&
         ok;

    // A closed port must fail, and fail cleanly rather than hanging or
    // aborting. A *successful* Connect() would mean something is listening on
    // port 1, which makes the rest of this run untrustworthy rather than
    // better — so it is a failure here, not a bonus.
    const auto connected = provider->Connect();
    ok = Check(!connected, "Connect() to a closed port failed cleanly") && ok;

    const std::shared_ptr<microtel::Tracer> tracer =
        provider->GetTracer("microtel-consumer-smoke", "0.1.0");
    ok = Check(tracer != nullptr, "GetTracer() returned a tracer") && ok;

    const microtel::SpanHandle span = tracer->StartSpan(
        "consumer.smoke",
        {.kind = microtel::SpanKind::Internal, .parent = {}, .start_time = {}, .attributes = {}});
    span->SetAttribute("consumer.smoke", std::string{"installed-tree"});

    // ToHex is the reason microtel_api is in the export set. An unresolved
    // symbol here is precisely the defect Decision 6 exists to catch.
    const microtel::SpanContext ctx = span->GetContext();
    const std::string trace_hex = ctx.trace_id.ToHex();
    const std::string span_hex = ctx.span_id.ToHex();
    span->End();

    std::cout << "trace_id=" << trace_hex << " span_id=" << span_hex << '\n';
    ok = Check(trace_hex.size() == kTraceIdHexChars, "TraceId::ToHex is 32 hex chars") && ok;
    ok = Check(span_hex.size() == kSpanIdHexChars, "SpanId::ToHex is 16 hex chars") && ok;

    // <microtel/baggage.hpp> is new public surface in v1.1, and it installs
    // only because the package installs include/microtel as a directory
    // (ICP 0020 Decision 2). Parsing here proves both halves: the header is in
    // the install tree, and Baggage::FromHeader resolves out of
    // libmicrotel_api.a, which nothing but the export set puts on this link
    // line.
    const microtel::Baggage bag = microtel::Baggage::FromHeader("tenant=acme,region=eu%2Dwest");
    ok = Check(bag.Get("tenant") == std::string_view{"acme"}, "Baggage::FromHeader round-trips") &&
         ok;

    // --- v1.1 sugar layer (ICP 0028) ---
    // MICROTEL_TRACE_FUNCTION, sugar::Span with an AttrKey-built attribute,
    // and sugar::Traced, all from the installed headers. See SugarSmoke above.
    ok = Check(SugarSmoke(*tracer), "microtel::sugar works from the installed tree") && ok;

    // --- v1.1 multi-profile (ICP 0027) ---
    //
    // `microtel::GetProvider` is a free function defined in libmicrotel_sdk.a
    // and declared in <microtel/provider.hpp>, so an unresolved symbol here is
    // the export-set defect this gate exists to catch — the registry is an SDK
    // concept, and nothing but the export set puts that archive on this link
    // line. The provider built above never named a profile, so it is the
    // default one; this second Build names its own.
    auto audit_built = microtel::SdkBuilder{}
                           .WithEndpoint(kDeadEndpoint)
                           .WithProtocol(microtel::Protocol::Grpc)
                           .WithServiceName("microtel-consumer-smoke-audit")
                           .WithProfileName("audit")
                           .WithTimeouts(timeouts)
                           .Build();
    if (!audit_built)
    {
        std::cerr << "FAIL SdkBuilder::Build(audit): " << audit_built.error().message << '\n';
        return 1;
    }
    const std::shared_ptr<microtel::Provider> audit = std::move(*audit_built);

    ok = Check(microtel::GetProvider() == provider.get(),
               "GetProvider() finds the default profile") &&
         ok;
    ok = Check(microtel::GetProvider("audit") == audit.get(),
               "GetProvider(\"audit\") finds the named profile") &&
         ok;
    ok = Check(microtel::GetProvider("no-such-profile") == nullptr,
               "GetProvider() of an unknown profile is null") &&
         ok;

    // Never last-wins: a duplicate name fails the build loudly.
    const auto duplicate = microtel::SdkBuilder{}
                               .WithEndpoint(kDeadEndpoint)
                               .WithProfileName("audit")
                               .WithTimeouts(timeouts)
                               .Build();
    ok = Check(!duplicate &&
                   duplicate.error().kind == microtel::ConfigError::Kind::DuplicateProfileName,
               "a duplicate profile name fails Build()") &&
         ok;

    const microtel::Status audit_shutdown = audit->Shutdown(kShutdownTimeout);
    std::cout << "audit Shutdown = " << StatusToString(audit_shutdown) << '\n';

    // The v1.1 hot-reload setters are new pure virtuals on Provider (ICP
    // 0026), so they are ABI *and* export-set material: an unresolved
    // SetBatchOptions here is the same class of defect as an unresolved
    // MakeProcessDetector. Two of them are exercised — one that must apply
    // (the builder always builds a batching span processor) and one that must
    // reject — because the pair proves the validation reached the install
    // tree, not just the symbol.
    const microtel::BatchOptions retuned{.max_queue_size = 2048,
                                         .max_export_batch_size = 128,
                                         .schedule_delay = std::chrono::seconds(1),
                                         .drop_policy = microtel::DropPolicy::DropNewest};
    const microtel::Status retune = provider->SetBatchOptions(retuned);
    std::cout << "SetBatchOptions = " << StatusToString(retune) << '\n';
    ok =
        Check(retune == microtel::Status::Completed, "SetBatchOptions retuned the pipelines") && ok;

    const microtel::Status rejected = provider->SetSamplerRatio(1.5);
    std::cout << "SetSamplerRatio(1.5) = " << StatusToString(rejected) << '\n';
    ok = Check(rejected == microtel::Status::InvalidArgument,
               "SetSamplerRatio rejects an out-of-range ratio") &&
         ok;

    // Nothing can reach a closed port, so TimedOut is the expected outcome and
    // Failed is an honest one. Only AlreadyShutDown is wrong — we have not
    // shut down.
    const microtel::Status flush = provider->ForceFlush(kFlushTimeout);
    std::cout << "ForceFlush = " << StatusToString(flush) << '\n';
    ok = Check(flush != microtel::Status::AlreadyShutDown, "ForceFlush returned a live status") &&
         ok;

    // A flush that finishes clears the failures it reported (ICP 0041), so a
    // second span gives Shutdown a batch of its own. Nothing reaches the closed
    // port, so Shutdown cannot be Completed: the span is either still pending
    // at the deadline or failed its last attempt.
    tracer
        ->StartSpan("consumer.smoke.shutdown",
                    {.kind = microtel::SpanKind::Internal,
                     .parent = {},
                     .start_time = {},
                     .attributes = {}})
        ->End();
    const microtel::Status shutdown = provider->Shutdown(kShutdownTimeout);
    std::cout << "Shutdown = " << StatusToString(shutdown) << '\n';
    ok = Check(shutdown == microtel::Status::Failed || shutdown == microtel::Status::TimedOut,
               "Shutdown failed or timed out") &&
         ok;

    std::cout << (ok ? "PASS" : "FAIL") << '\n';
    return ok ? 0 : 1;
}
