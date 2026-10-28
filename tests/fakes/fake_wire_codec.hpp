// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "microtel/internal/encoded_payload.hpp"
#include "microtel/internal/wire_codec.hpp"
#include "microtel/internal/wire_result.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

namespace microtel::testing
{

/// @brief Scriptable `IWireCodec` for retry / exporter integration tests.
///
/// Distinct from `MockWireCodec` (which returns one fixed result):
/// `FakeWireCodec` serves scripted `WireResult`s in FIFO order, falling back
/// to `default_result` when the queue is empty. Tests drive scenarios such as
/// retry-then-succeed or retry-until-exhausted by pre-loading the queue.
///
/// It also keeps a copy of every payload it is sent, so a test can decode what
/// would have gone on the wire (`SentPayloads`).
class FakeWireCodec : public internal::IWireCodec
{
public:
    // --- Recording ---
    // Atomic: OtlpExporter's worker thread calls Send() while the test thread
    // reads the count after ForceFlush, so it is accessed cross-thread.
    std::atomic<int> send_call_count{0};

    // --- Configurable ---
    /// @brief FIFO of scripted results; each Send pops the front.
    std::deque<internal::WireResult> scripted_results;
    /// @brief Returned when scripted_results is empty. Default: failure, non-retryable.
    internal::WireResult default_result{};
    /// @brief How long each `Send` takes before answering. Set before the
    ///        codec is handed to an exporter.
    std::chrono::milliseconds send_delay{0};

    [[nodiscard]] internal::WireResult Send(internal::EncodedPayload&& payload,
                                            std::chrono::milliseconds /*deadline*/) override
    {
        std::this_thread::sleep_for(send_delay);
        const auto bytes = payload.Bytes();
        const std::scoped_lock lock{m_mu};
        m_sent.emplace_back(bytes.begin(), bytes.end());
        ++send_call_count;
        if (!scripted_results.empty())
        {
            auto r = std::move(scripted_results.front());
            scripted_results.pop_front();
            return r;
        }
        return default_result;
    }

    /// @brief A copy of every payload sent so far, in send order.
    [[nodiscard]] std::vector<std::vector<std::byte>> SentPayloads() const
    {
        const std::scoped_lock lock{m_mu};
        return m_sent;
    }

private:
    mutable std::mutex m_mu;
    std::vector<std::vector<std::byte>> m_sent;
};

}  // namespace microtel::testing
