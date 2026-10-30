// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0
//
// A real reactor whose dispatch a test can pause.
//
// `FakeReactor::HoldDispatch` parks the I/O thread but dispatches no real
// socket events. Some races need both: the I/O thread held while a socket
// event is already waiting, then released so that event is the first thing
// it handles. `GatedReactor` wraps a real `IReactor` (an `EpollReactor`) and
// adds the hold.
//
// Not a fake: it does real I/O through the reactor it wraps, which is why it
// lives here rather than in `tests/fakes/`.

#pragma once

#include "microtel/error.hpp"
#include "microtel/expected.hpp"
#include "microtel/internal/reactor.hpp"

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <memory>
#include <mutex>
#include <utility>

namespace microtel::testing
{

class GatedReactor final : public internal::IReactor
{
public:
    explicit GatedReactor(std::unique_ptr<internal::IReactor> inner) noexcept
        : m_inner(std::move(inner))
    {
    }

    /// @brief Park the I/O thread at its next `WaitAndDispatch`.
    ///
    /// Returns once it is parked, or false after @p timeout. A parked thread
    /// is between turns: nothing it was dispatching is still running.
    bool Hold(std::chrono::milliseconds timeout)
    {
        std::unique_lock lk{m_mu};
        m_held = true;
        return m_cv.wait_for(lk, timeout, [this] { return m_parked; });
    }

    /// @brief Let the parked I/O thread go on to dispatch.
    void Release() noexcept
    {
        {
            const std::scoped_lock lk{m_mu};
            m_held = false;
        }
        m_cv.notify_all();
    }

    [[nodiscard]] microtel::Expected<void, microtel::Error> Register(
        int fd, internal::EventMask mask, internal::EventCallback cb) override
    {
        return m_inner->Register(fd, mask, std::move(cb));
    }

    void Modify(int fd, internal::EventMask mask) override
    {
        m_inner->Modify(fd, mask);
    }

    void Unregister(int fd) noexcept override
    {
        m_inner->Unregister(fd);
    }

    std::size_t WaitAndDispatch(internal::TimePointSteady deadline) override
    {
        {
            std::unique_lock lk{m_mu};
            if (m_held)
            {
                m_parked = true;
                m_cv.notify_all();
                m_cv.wait(lk, [this] { return !m_held; });
                m_parked = false;
            }
        }
        return m_inner->WaitAndDispatch(deadline);
    }

    void Wake() noexcept override
    {
        m_inner->Wake();
    }

private:
    std::unique_ptr<internal::IReactor> m_inner;
    std::mutex m_mu;
    std::condition_variable m_cv;
    bool m_held = false;
    bool m_parked = false;
};

}  // namespace microtel::testing
