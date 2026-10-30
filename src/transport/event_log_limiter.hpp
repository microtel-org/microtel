// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <chrono>
#include <cstdint>

namespace microtel::transport
{

/// @brief Decides how loudly to log a transport's recurring events.
///
/// `Http1Transport` reconnects in the background and retries stale
/// connections. Both are normal on a busy keep-alive connection, so logging
/// each at Info would flood the log. The rule:
///
/// - the first event of each kind is logged at Info;
/// - after that, at most one Info summary, carrying the running counts, per
///   `interval`;
/// - every other event is logged at Debug.
///
/// Pure logic: the caller passes the time, so tests need no clock. Not
/// thread-safe; the transport calls it from its I/O thread only.
class EventLogLimiter
{
public:
    /// @brief The kinds of event counted. Each has its own first Info line.
    enum class Kind : std::uint8_t
    {
        BackgroundReconnect = 0,
        StaleRetry = 1,
    };

    /// @brief How to log one event.
    enum class Emit : std::uint8_t
    {
        First,    ///< The first of its kind: log it at Info.
        Summary,  ///< An interval has passed: log the running counts at Info.
        Debug,    ///< Log the event at Debug only.
    };

    /// @brief Default summary interval: one Info line a minute at most.
    static constexpr std::chrono::seconds kDefaultInterval{60};

    explicit EventLogLimiter(
        std::chrono::steady_clock::duration interval = kDefaultInterval) noexcept
        : m_interval(interval)
    {
    }

    /// @brief Record one event of @p kind at @p now and say how to log it.
    [[nodiscard]] Emit OnEvent(Kind kind, std::chrono::steady_clock::time_point now) noexcept
    {
        bool& seen = (kind == Kind::BackgroundReconnect) ? m_seen_reconnect : m_seen_stale_retry;
        if (!seen)
        {
            seen = true;
            m_last_info = now;
            return Emit::First;
        }
        // Reached only after a First, which set m_last_info.
        if (now - m_last_info >= m_interval)
        {
            m_last_info = now;
            return Emit::Summary;
        }
        return Emit::Debug;
    }

private:
    std::chrono::steady_clock::duration m_interval;
    bool m_seen_reconnect = false;
    bool m_seen_stale_retry = false;
    std::chrono::steady_clock::time_point m_last_info{};
};

}  // namespace microtel::transport
