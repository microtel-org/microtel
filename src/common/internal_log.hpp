// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "microtel/log_sink.hpp"

#include <string>
#include <string_view>

namespace microtel::internal
{

/// @brief Internal log entry point used by every microtel component.
///
/// Drops the record when `level` is below `MinLogLevel()`, before it touches
/// the sink mutex. Otherwise routes through the active `microtel::LogSink` if
/// one is installed, and to the stderr fallback if none is. There is no route
/// into spdlog and there will not be one: option (1) of issue #190, recorded
/// in ICP 0026 §6 — an application that wants spdlog installs
/// `microtel_spdlog_bridge` as its `LogSink`, inside its own build, so
/// `libmicrotel_common.a` never acquires an undefined spdlog reference.
///
/// `noexcept`. The implementation never throws and never propagates an
/// application sink's exception (per `docs/error-model.md` §9.3).
///
/// Rate-limiting per `(level, reason)` pair is still unbuilt.
///
/// @threadsafety Thread-safe. May be called from any internal thread.
void LogImpl(microtel::LogLevel level, std::string_view message) noexcept;

/// @brief Set the minimum severity `LogImpl` will emit.
///
/// Process-wide, not per-provider: `LogImpl` is a free function called from
/// code that has no provider in scope, so v1.1's multi-profile providers share
/// this knob and the last writer wins (ICP 0026 §6). Seeded at `Build()` from
/// the `logging.level` TOML key and `MICROTEL_LOG_LEVEL`; the operator-facing
/// surface is `microtel::Provider::SetLogLevel`, which forwards here.
///
/// Backed by one atomic, so it is race-free by construction and adds no lock
/// to the internal log path.
///
/// @param level one of the declared `microtel::LogLevel` enumerators.
/// @return `false` if `level` is not a declared enumerator; the minimum is
///         then unchanged.
///
/// @threadsafety Thread-safe.
[[nodiscard]] bool SetMinLogLevel(microtel::LogLevel level) noexcept;

/// @brief The minimum severity `LogImpl` will emit. Defaults to `Info`.
///
/// @threadsafety Thread-safe.
[[nodiscard]] microtel::LogLevel MinLogLevel() noexcept;

/// @brief Append @p text to @p out with control characters escaped, so a
///        value from the network cannot forge a log record
///        (SonarCloud cpp:S5145, logging injection).
///
/// LF becomes `\n`, CR `\r`, and every other control character except tab
/// (below 0x20, and 0x7F) `\xNN`. Bytes >= 0x80 (UTF-8) pass through. The
/// same escaping the stderr fallback applies to every record.
///
/// @throws std::bad_alloc if @p out cannot grow.
void AppendEscaped(std::string& out, std::string_view text);

}  // namespace microtel::internal
