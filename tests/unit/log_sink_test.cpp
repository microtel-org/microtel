// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0
//
// Tests for microtel::SetLogSink / ResetLogSink and the internal
// LogImpl entry point. Behavioural — exercises real production code in
// src/common/log_sink.cpp, not a mock.

#include "microtel/log_sink.hpp"

// Implementation header from src/common/. Only test code reaches into
// the internal log entry point this way; production code uses it via
// the same header.
#include "common/internal_log.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <cstddef>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace mt = microtel;

namespace
{

struct CapturedLog
{
    mt::LogLevel level;
    std::string message;
};

class LogSinkTest : public ::testing::Test
{
protected:
    void TearDown() override
    {
        // Each test installs a fresh sink; restore the default afterward
        // so subsequent tests / suites are not affected. The minimum level is
        // process-global (ICP 0026 §6), so it needs the same treatment — and
        // restoring the *shipped* default, not a convenient one, is what lets
        // `DefaultMinimumLevelIsInfo` below mean something.
        mt::ResetLogSink();
        EXPECT_TRUE(mt::internal::SetMinLogLevel(mt::LogLevel::Info));
    }
};

TEST_F(LogSinkTest, InstalledSinkReceivesLogCalls)
{
    std::vector<CapturedLog> captured;
    mt::SetLogSink([&](mt::LogLevel lvl, std::string_view msg)
                   { captured.push_back({lvl, std::string{msg}}); });

    mt::internal::LogImpl(mt::LogLevel::Info, "hello world");
    mt::internal::LogImpl(mt::LogLevel::Warn, "second");

    ASSERT_EQ(captured.size(), std::size_t{2});
    EXPECT_EQ(captured[0].level, mt::LogLevel::Info);
    EXPECT_EQ(captured[0].message, "hello world");
    EXPECT_EQ(captured[1].level, mt::LogLevel::Warn);
    EXPECT_EQ(captured[1].message, "second");
}

TEST_F(LogSinkTest, ResetRestoresDefault)
{
    int count = 0;
    mt::SetLogSink([&](mt::LogLevel, std::string_view) { ++count; });
    mt::internal::LogImpl(mt::LogLevel::Info, "captured");
    EXPECT_EQ(count, 1);

    mt::ResetLogSink();
    // After reset, LogImpl falls back to stderr; the count must not change.
    mt::internal::LogImpl(mt::LogLevel::Info, "ignored-by-test-but-stderr-emits");
    EXPECT_EQ(count, 1);
}

TEST_F(LogSinkTest, ThrowingSinkIsSwallowed)
{
    mt::SetLogSink([](mt::LogLevel, std::string_view) -> void
                   { throw std::runtime_error{"boom"}; });
    // Per docs/error-model.md §9.3, microtel must not propagate the
    // application's exception. The call below should return normally.
    EXPECT_NO_THROW(mt::internal::LogImpl(mt::LogLevel::Error, "expect-swallowed"));
}

// Helpers for the concurrent stress tests. Extracted to keep each TEST_F
// body small enough for `readability-function-size` and to flatten
// nesting per `coding-standards.md` §2.2 (max 3 levels per function).
void EmitLoop(int iters) noexcept
{
    for (int j = 0; j < iters; ++j)
    {
        // Warn, not Debug: the shipped minimum level is Info, and a loop that
        // emitted below it would be filtered out before the sink-copy path
        // these stress tests exist to exercise (ICP 0026 §6).
        mt::internal::LogImpl(mt::LogLevel::Warn, "x");
    }
}

void SpawnLoggers(int threads, int iters)
{
    std::vector<std::thread> ts;
    ts.reserve(static_cast<std::size_t>(threads));
    for (int i = 0; i < threads; ++i)
    {
        ts.emplace_back(EmitLoop, iters);
    }
    for (auto& t : ts)
    {
        t.join();
    }
}

TEST_F(LogSinkTest, ConcurrentEmitWithStableSink)
{
    // Sanitizer test: many threads call LogImpl with one fixed sink. No
    // assertion on output content; the test's value is TSan/ASan
    // cleanliness through the sink-copy path in LogImpl.
    std::atomic<int> emitted{0};
    mt::SetLogSink([&](mt::LogLevel, std::string_view) { ++emitted; });
    SpawnLoggers(/*threads=*/4, /*iters=*/200);
    EXPECT_GE(emitted.load(), 0);
}

TEST_F(LogSinkTest, ConcurrentSinkSwap)
{
    // Sanitizer test: cycle SetLogSink / ResetLogSink while threads emit.
    std::atomic<int> emitted{0};
    mt::SetLogSink([&](mt::LogLevel, std::string_view) { ++emitted; });
    std::thread loggers([&] { SpawnLoggers(2, 200); });
    for (int j = 0; j < 50; ++j)
    {
        mt::SetLogSink([&](mt::LogLevel, std::string_view) { ++emitted; });
        mt::ResetLogSink();
    }
    loggers.join();
    EXPECT_GE(emitted.load(), 0);
}

// ---------------------------------------------------------------------------
// The minimum-level filter — ICP 0026 §6, issue #190
// ---------------------------------------------------------------------------

/// Emits one record at each declared level and returns what survived.
std::vector<mt::LogLevel> EmitEveryLevel()
{
    std::vector<mt::LogLevel> seen;
    mt::SetLogSink([&seen](mt::LogLevel lvl, std::string_view) { seen.push_back(lvl); });
    for (const mt::LogLevel level : {mt::LogLevel::Trace,
                                     mt::LogLevel::Debug,
                                     mt::LogLevel::Info,
                                     mt::LogLevel::Warn,
                                     mt::LogLevel::Error})
    {
        mt::internal::LogImpl(level, "probe");
    }
    return seen;
}

TEST_F(LogSinkTest, DefaultMinimumLevelIsInfo)
{
    // The shipped default: Trace and Debug are dropped, everything from Info
    // up is emitted. Every production LogImpl call site emits at Warn, so the
    // default changes nothing observable (ICP 0026 §6).
    EXPECT_EQ(mt::internal::MinLogLevel(), mt::LogLevel::Info);
    const std::vector<mt::LogLevel> seen = EmitEveryLevel();
    ASSERT_EQ(seen.size(), std::size_t{3});
    EXPECT_EQ(seen[0], mt::LogLevel::Info);
    EXPECT_EQ(seen[2], mt::LogLevel::Error);
}

TEST_F(LogSinkTest, BelowMinimumLevelNeverReachesTheSink)
{
    ASSERT_TRUE(mt::internal::SetMinLogLevel(mt::LogLevel::Warn));
    const std::vector<mt::LogLevel> seen = EmitEveryLevel();
    ASSERT_EQ(seen.size(), std::size_t{2});
    EXPECT_EQ(seen[0], mt::LogLevel::Warn);
    EXPECT_EQ(seen[1], mt::LogLevel::Error);
}

TEST_F(LogSinkTest, TraceMinimumLevelAdmitsEverything)
{
    ASSERT_TRUE(mt::internal::SetMinLogLevel(mt::LogLevel::Trace));
    EXPECT_EQ(EmitEveryLevel().size(), std::size_t{5});
}

TEST_F(LogSinkTest, ErrorMinimumLevelAdmitsOnlyError)
{
    ASSERT_TRUE(mt::internal::SetMinLogLevel(mt::LogLevel::Error));
    const std::vector<mt::LogLevel> seen = EmitEveryLevel();
    ASSERT_EQ(seen.size(), std::size_t{1});
    EXPECT_EQ(seen[0], mt::LogLevel::Error);
}

TEST_F(LogSinkTest, SetMinLogLevelRejectsAValueOutsideTheEnumerators)
{
    ASSERT_TRUE(mt::internal::SetMinLogLevel(mt::LogLevel::Warn));
    // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)
    EXPECT_FALSE(mt::internal::SetMinLogLevel(static_cast<mt::LogLevel>(9)));
    EXPECT_EQ(mt::internal::MinLogLevel(), mt::LogLevel::Warn);
}

TEST_F(LogSinkTest, ConcurrentLevelChangeWhileEmitting)
{
    // The filter is one relaxed atomic load ahead of the sink mutex; changing
    // it under load must stay race-free (TSAN target).
    std::atomic<int> emitted{0};
    mt::SetLogSink([&](mt::LogLevel, std::string_view) { ++emitted; });
    std::thread loggers([&] { SpawnLoggers(2, 200); });
    for (int j = 0; j < 50; ++j)
    {
        EXPECT_TRUE(mt::internal::SetMinLogLevel(mt::LogLevel::Error));
        EXPECT_TRUE(mt::internal::SetMinLogLevel(mt::LogLevel::Trace));
    }
    loggers.join();
    EXPECT_GE(emitted.load(), 0);
}


// ---- stderr fallback: control characters are escaped (cpp:S5145) ----------

std::string FallbackLineFor(std::string_view message)
{
    mt::ResetLogSink();
    ::testing::internal::CaptureStderr();
    mt::internal::LogImpl(mt::LogLevel::Warn, message);
    return ::testing::internal::GetCapturedStderr();
}

TEST_F(LogSinkTest, FallbackWritesPlainMessageUnchanged)
{
    EXPECT_EQ(FallbackLineFor("plain text"), "[microtel warn] plain text\n");
}

TEST_F(LogSinkTest, FallbackEscapesLineBreaksSoARecordCannotForgeAnother)
{
    EXPECT_EQ(FallbackLineFor("a\nb\r\n[microtel error] forged"),
              "[microtel warn] a\\nb\\r\\n[microtel error] forged\n");
}

TEST_F(LogSinkTest, FallbackHexEscapesOtherControlCharacters)
{
    using namespace std::string_view_literals;
    EXPECT_EQ(FallbackLineFor("x\0y\x1b[31m\x7fz"sv), "[microtel warn] x\\x00y\\x1b[31m\\x7fz\n");
}

TEST_F(LogSinkTest, FallbackKeepsTabAndUtf8)
{
    EXPECT_EQ(FallbackLineFor("k\tv \xc3\xa9"), "[microtel warn] k\tv \xc3\xa9\n");
}

TEST_F(LogSinkTest, FallbackWritesALongRecordWhole)
{
    // Longer than the fallback's stack buffer, so it goes out in chunks.
    const std::string message(2000, 'm');
    EXPECT_EQ(FallbackLineFor(message), "[microtel warn] " + message + "\n");
}

// ---- AppendEscaped: the same escaping, for lines built before LogImpl ------

TEST(AppendEscapedTest, EscapesControlCharactersAfterWhatIsAlreadyThere)
{
    using namespace std::string_view_literals;
    std::string line{"prefix: "};
    mt::internal::AppendEscaped(line, "a\r\n[microtel error] x\0\x1b\x7f\t\xc3\xa9"sv);
    EXPECT_EQ(line, "prefix: a\\r\\n[microtel error] x\\x00\\x1b\\x7f\t\xc3\xa9");
}

TEST(AppendEscapedTest, EmptyTextAppendsNothing)
{
    std::string line{"prefix"};
    mt::internal::AppendEscaped(line, {});
    EXPECT_EQ(line, "prefix");
}

}  // namespace
