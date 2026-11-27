// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0
//
// One place to configure a Provider aimed at the conformance collector.
//
// The defaults in TimeoutOptions and BatchOptions are tuned for a production
// process: a 5 s schedule delay and 10 s timeouts. In a test that is dead wall
// clock on every run and, worse, turns a genuine hang into a ctest timeout
// with no diagnostic. Everything here is short enough to fail fast and long
// enough that a loaded CI runner does not trip it.

#pragma once

#include "microtel/protocol.hpp"
#include "microtel/sdk_builder.hpp"

#include <chrono>
#include <string>

namespace microtel::testing
{

/// @brief Gives @p builder the test timeouts and batch delay, and no endpoint.
///
/// For a test whose endpoint comes from the environment, which `WithEndpoint`
/// would outrank (`docs/configuration.md` §3.3).
///
/// @param builder the builder to configure; borrowed for the call only.
/// @return @p builder, for chaining.
inline SdkBuilder& ConfigureConformanceTimeouts(SdkBuilder& builder)
{
    return builder
        .WithTimeouts(TimeoutOptions{
            .connect = std::chrono::seconds(5),
            .tls_handshake = std::chrono::seconds(5),
            .per_export = std::chrono::seconds(5),
            // flush and shutdown stay generous: they bound a drain, and cutting
            // them short would report TimedOut for a batch that was merely slow.
            .flush = std::chrono::seconds(30),
            .shutdown = std::chrono::seconds(10),
        })
        .WithBatch(BatchOptions{
            // 100 ms instead of the 5 s default so ForceFlush is not the only
            // thing that ever moves a batch.
            .schedule_delay = std::chrono::milliseconds(100),
        });
}

/// @brief Points @p builder at the conformance collector with test timeouts.
///
/// Takes the builder by reference rather than returning a fresh one: the
/// public `SdkBuilder(SdkBuilder&&)` is defaulted in the header over a
/// `unique_ptr` to an incomplete pimpl, so instantiating it outside
/// src/sdk/sdk_builder.cpp does not compile. A by-value factory is therefore
/// not expressible; the caller owns the object and this configures it.
///
/// Service identity and transport security stay with the caller — chain
/// `WithServiceName`, `WithTls`, `WithHeaders` and so on before `Build()`.
///
/// @param builder  the builder to configure; borrowed for the call only.
/// @param endpoint OTLP endpoint URL, e.g. `http://127.0.0.1:4318`.
/// @param proto    wire protocol to exercise.
/// @return @p builder, for chaining.
inline SdkBuilder& ConfigureConformanceBuilder(SdkBuilder& builder,
                                               const std::string& endpoint,
                                               const Protocol proto)
{
    return ConfigureConformanceTimeouts(builder.WithEndpoint(endpoint).WithProtocol(proto));
}

}  // namespace microtel::testing
