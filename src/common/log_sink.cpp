// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0
//
// Implementation of microtel::SetLogSink / ResetLogSink (declared in
// include/microtel/log_sink.hpp) and of the internal log entry point and its
// minimum-level filter (declared in src/common/internal_log.hpp).
//
// Stores the active sink behind a mutex so the (rare) SetLogSink call
// races safely with concurrent log emissions on internal threads. The
// internal log entry point (LogImpl below) is what production code calls;
// rate-limiting per docs/error-model.md §9.2 is still unbuilt.
//
// There is no spdlog route here, now or later. Issue #190 asked what happens
// to libmicrotel_common.a's link closure when the internal log route finally
// does something; ICP 0026 §6 takes option (1) — the public LogSink hook only,
// with spdlog staying in the consumer's own build behind
// microtel_spdlog_bridge. So this file's default route is stderr in every
// build configuration, and the archive never carries an undefined spdlog
// reference for a consumer of the installed package to resolve.

#include "microtel/log_sink.hpp"

#include "common/internal_log.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>

namespace microtel
{

namespace
{

/// @brief Active sink + mutex protecting it.
///
/// The mutex is held only briefly: SetLogSink takes it to swap in a new
/// callable; LogImpl takes it to copy the callable out before invoking,
/// so the user's lambda is not invoked under our lock (per
/// docs/error-model.md §9.3 — "the sink is invoked under no microtel-held
/// lock").
struct SinkState
{
    std::mutex mu;
    LogSink sink;  // empty default; meaning "use default fallback"
};

SinkState& State() noexcept
{
    static SinkState s_state;
    return s_state;
}

/// @brief Minimum severity `LogImpl` emits. ICP 0026 §6, issue #190.
///
/// An atomic rather than a field of `SinkState`: `LogImpl` reads it *before*
/// the sink mutex, so a record below the threshold costs one relaxed load and
/// takes no lock at all.
///
/// Wrapped in an accessor, like `State()` above, rather than left as a
/// namespace-scope global. It has to be mutable — it is a runtime knob — and a
/// function-local static keeps that mutability out of file scope while giving
/// the same guarantees, `std::atomic<LogLevel>` having a constexpr constructor.
///
/// `Info` is the shipped default and changes nothing observable: every
/// production `LogImpl` call site emits at `Warn`.
std::atomic<LogLevel>& MinLevel() noexcept
{
    static std::atomic<LogLevel> s_min_level{LogLevel::Info};
    return s_min_level;
}

/// The highest declared enumerator, for validating a cast-in value.
constexpr auto kMaxLogLevel = static_cast<std::uint8_t>(LogLevel::Error);

const char* LevelTag(LogLevel level) noexcept
{
    switch (level)
    {
        case LogLevel::Trace:
            return "trace";
        case LogLevel::Debug:
            return "debug";
        case LogLevel::Info:
            return "info";
        case LogLevel::Warn:
            return "warn";
        case LogLevel::Error:
            return "error";
    }
    return "?";
}

/// Size of the stack buffer the stderr fallback assembles its line in. A
/// longer record goes out in several writes; nothing is truncated.
constexpr std::size_t kFallbackBufferSize = 512;

/// Below this byte value is the C0 control range.
constexpr unsigned char kFirstPrintable = 0x20;

/// DEL, the one control character above the C0 range.
constexpr unsigned char kDelete = 0x7f;

constexpr unsigned kNibbleBits = 4;
constexpr unsigned kNibbleMask = 0x0f;
constexpr std::string_view kHexDigits = "0123456789abcdef";

/// Passes `c` to `put`, rendering LF as `\n`, CR as `\r` and every other
/// control character except tab as `\xNN`. Bytes >= 0x80 (UTF-8) pass
/// through unchanged.
template <typename PutFn>
void PutEscapedChar(char c, PutFn&& put)
{
    const auto byte = static_cast<unsigned char>(c);
    if (c == '\t' || (byte >= kFirstPrintable && byte != kDelete))
    {
        put(c);
        return;
    }
    put('\\');
    if (c == '\n')
    {
        put('n');
        return;
    }
    if (c == '\r')
    {
        put('r');
        return;
    }
    put('x');
    put(kHexDigits.at(byte >> kNibbleBits));
    put(kHexDigits.at(byte & kNibbleMask));
}

/// @brief Builds the stderr fallback line in a stack buffer, escaping control
/// characters in the message so one record can never forge another
/// (SonarCloud cpp:S5145, logging injection).
///
/// No heap: a full buffer is flushed with one `fwrite` and reused. A record
/// that fits is still a single write, as the old single `fprintf` was.
class FallbackLineWriter
{
public:
    /// @param out  Borrowed stream; must outlive the writer.
    explicit FallbackLineWriter(std::FILE* out) noexcept : m_out(out) {}

    void Put(char c) noexcept
    {
        if (m_len == m_buf.size())
        {
            Flush();
        }
        m_buf.at(m_len) = c;
        ++m_len;
    }

    void PutText(std::string_view text) noexcept
    {
        for (const char c : text)
        {
            Put(c);
        }
    }

    /// Writes `text` with control characters escaped, as `PutEscapedChar`.
    void PutSanitized(std::string_view text) noexcept
    {
        for (const char c : text)
        {
            PutEscapedChar(c, [this](char e) noexcept { Put(e); });
        }
    }

    void Flush() noexcept
    {
        if (m_len > 0)
        {
            (void)std::fwrite(m_buf.data(), 1, m_len, m_out);
            m_len = 0;
        }
    }

private:
    std::FILE* m_out;
    std::array<char, kFallbackBufferSize> m_buf{};
    std::size_t m_len{0};
};

}  // namespace

void SetLogSink(LogSink sink) noexcept
{
    auto& st = State();
    const std::scoped_lock lock(st.mu);
    st.sink = std::move(sink);
}

void ResetLogSink() noexcept
{
    auto& st = State();
    const std::scoped_lock lock(st.mu);
    st.sink = {};
}

namespace internal
{

bool SetMinLogLevel(LogLevel level) noexcept
{
    if (static_cast<std::uint8_t>(level) > kMaxLogLevel)
    {
        return false;
    }
    MinLevel().store(level, std::memory_order_relaxed);
    return true;
}

LogLevel MinLogLevel() noexcept
{
    return MinLevel().load(std::memory_order_relaxed);
}

void AppendEscaped(std::string& out, std::string_view text)
{
    for (const char c : text)
    {
        PutEscapedChar(c, [&out](char e) { out.push_back(e); });
    }
}

/// @brief Internal log entry point. Production code calls this; the
/// public API is `microtel::SetLogSink` for redirection, and
/// `microtel::Provider::SetLogLevel` (via `SetMinLogLevel`) for filtering.
///
/// Records below the minimum level are dropped here, before the sink mutex is
/// touched — one relaxed atomic load on the way out.
///
/// If a sink is installed, it's invoked with no microtel-held lock; the
/// callable's thread-safety is the application's responsibility (LOCKED
/// per docs/error-model.md §9.3).
///
/// If no sink is installed the record goes to stderr, in every build
/// configuration. `MICROTEL_USE_SPDLOG` does not change that and never will:
/// see the file header and ICP 0026 §6.
// NOLINTNEXTLINE(misc-use-internal-linkage)
void LogImpl(LogLevel level, std::string_view message) noexcept
{
    if (level < MinLevel().load(std::memory_order_relaxed))
    {
        return;
    }

    LogSink local_copy;
    {
        auto& st = State();
        const std::scoped_lock lock(st.mu);
        local_copy = st.sink;  // copy the std::function out
    }

    if (local_copy)
    {
        // Application sink. Best-effort: if it throws, swallow per the
        // contract in error-model.md §9.3 (a sink must not destabilise
        // microtel by throwing).
        try
        {
            local_copy(level, message);
        }
        // NOLINTNEXTLINE(bugprone-empty-catch)
        catch (...)
        {
            // Intentionally empty — sinks must not throw per error-model.md §9.3.
        }
        return;
    }

    // No sink installed. Default fallback: stderr.
    // Format: "[microtel level] message\n", with control characters in
    // `message` escaped so a record cannot inject extra lines.
    FallbackLineWriter line(stderr);
    line.PutText("[microtel ");
    line.PutText(LevelTag(level));
    line.PutText("] ");
    line.PutSanitized(message);
    line.Put('\n');
    line.Flush();
}

}  // namespace internal

}  // namespace microtel
