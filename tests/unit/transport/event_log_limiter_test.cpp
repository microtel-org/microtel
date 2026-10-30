// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0
//
// Unit test: how loudly Http1Transport logs its recurring events.

#include "transport/event_log_limiter.hpp"

#include <gtest/gtest.h>

#include <chrono>

namespace mtt = microtel::transport;

namespace
{

using Kind = mtt::EventLogLimiter::Kind;
using Emit = mtt::EventLogLimiter::Emit;
using std::chrono::seconds;

constexpr auto kT0 = std::chrono::steady_clock::time_point{} + std::chrono::hours(1);

}  // namespace

TEST(EventLogLimiterTest, FirstOfEachKind_IsInfo)
{
    mtt::EventLogLimiter limiter;
    EXPECT_EQ(limiter.OnEvent(Kind::BackgroundReconnect, kT0), Emit::First);
    EXPECT_EQ(limiter.OnEvent(Kind::StaleRetry, kT0 + seconds(1)), Emit::First)
        << "the first stale retry is announced even right after a reconnect";
}

TEST(EventLogLimiterTest, RepeatsWithinTheInterval_AreDebug)
{
    mtt::EventLogLimiter limiter;
    (void)limiter.OnEvent(Kind::BackgroundReconnect, kT0);
    EXPECT_EQ(limiter.OnEvent(Kind::BackgroundReconnect, kT0 + seconds(1)), Emit::Debug);
    EXPECT_EQ(limiter.OnEvent(Kind::BackgroundReconnect, kT0 + seconds(59)), Emit::Debug);
}

TEST(EventLogLimiterTest, AtMostOneSummaryPerInterval)
{
    mtt::EventLogLimiter limiter;
    (void)limiter.OnEvent(Kind::BackgroundReconnect, kT0);
    EXPECT_EQ(limiter.OnEvent(Kind::BackgroundReconnect, kT0 + seconds(60)), Emit::Summary);
    EXPECT_EQ(limiter.OnEvent(Kind::BackgroundReconnect, kT0 + seconds(61)), Emit::Debug);
    EXPECT_EQ(limiter.OnEvent(Kind::BackgroundReconnect, kT0 + seconds(119)), Emit::Debug);
    EXPECT_EQ(limiter.OnEvent(Kind::BackgroundReconnect, kT0 + seconds(120)), Emit::Summary);
}

TEST(EventLogLimiterTest, OneSummaryCoversBothKinds)
{
    mtt::EventLogLimiter limiter;
    (void)limiter.OnEvent(Kind::BackgroundReconnect, kT0);
    (void)limiter.OnEvent(Kind::StaleRetry, kT0 + seconds(10));
    EXPECT_EQ(limiter.OnEvent(Kind::BackgroundReconnect, kT0 + seconds(70)), Emit::Summary);
    EXPECT_EQ(limiter.OnEvent(Kind::StaleRetry, kT0 + seconds(71)), Emit::Debug)
        << "the summary carries both counts, so the other kind waits for the next interval";
}

TEST(EventLogLimiterTest, CustomInterval)
{
    mtt::EventLogLimiter limiter{seconds(5)};
    (void)limiter.OnEvent(Kind::StaleRetry, kT0);
    EXPECT_EQ(limiter.OnEvent(Kind::StaleRetry, kT0 + seconds(4)), Emit::Debug);
    EXPECT_EQ(limiter.OnEvent(Kind::StaleRetry, kT0 + seconds(5)), Emit::Summary);
}
