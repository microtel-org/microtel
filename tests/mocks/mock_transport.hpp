// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "microtel/internal/transport.hpp"

#include <chrono>
#include <cstdint>
#include <future>
#include <utility>

namespace microtel::testing
{

/// @brief Dumb mock for `microtel::internal::ITransport`.
///
/// Returns configured values; records call counts. No conditional behaviour
/// on argument value or call sequence — if a test needs that, it wants the
/// `FakeTransport` from `tests/fakes/` instead.
class MockTransport : public internal::ITransport
{
public:
    // --- Configurable returns ---
    microtel::Expected<void, microtel::Error> connect_result{};  // default: success
    microtel::ConnectionState state = microtel::ConnectionState::Connected;
    microtel::Status close_result = microtel::Status::Completed;
    /// Result yielded by the future returned from each Send. Defaults to
    /// success with no headers/body.
    internal::TransportResult send_result{};

    // --- Recording ---
    int connect_call_count = 0;
    int send_call_count = 0;
    int cancel_call_count = 0;
    int close_call_count = 0;
    /// The timeout `Close` was handed; -1 until the first call.
    std::chrono::milliseconds last_close_timeout{-1};

    // --- ITransport ---

    [[nodiscard]] microtel::Expected<void, microtel::Error> Connect(
        const internal::ConnectOptions& /*opts*/) override
    {
        ++connect_call_count;
        return connect_result;
    }

    [[nodiscard]] internal::RequestHandle Send(internal::RequestSpec /*spec*/) noexcept override
    {
        ++send_call_count;
        std::promise<internal::TransportResult> p;
        std::future<internal::TransportResult> f = p.get_future();
        // Resolve immediately with the configured result.
        p.set_value(send_result);
        return internal::RequestHandle{static_cast<std::uint64_t>(send_call_count), std::move(f)};
    }

    void Cancel(const internal::RequestHandle& /*handle*/) noexcept override
    {
        ++cancel_call_count;
    }

    [[nodiscard]] microtel::ConnectionState GetState() const noexcept override
    {
        return state;
    }

    [[nodiscard]] microtel::Status Close(std::chrono::milliseconds timeout) noexcept override
    {
        ++close_call_count;
        last_close_timeout = timeout;
        return close_result;
    }
};

}  // namespace microtel::testing
