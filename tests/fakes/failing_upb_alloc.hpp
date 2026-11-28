// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#pragma once

// upb C headers use flexible array members (C99/C11, not ISO C++).
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#include "upb/mem/alloc.h"
#pragma GCC diagnostic pop

#include <cstddef>
#include <cstdlib>

namespace microtel::testing
{

/// @brief While alive, every allocation through upb's global allocator fails,
/// so `upb_Arena_New()` returns null. Frees still free.
///
/// upb routes every arena it creates through the mutable `upb_alloc_global`,
/// and its header names fault injection as what that hook is for. Swapping
/// its function is the only way to drive the encoder's arena-creation failure
/// path deterministically. Only for TUs that already reach upb headers (and
/// so carry the `microtel_upb_rename.h` force-include).
///
/// @threadsafety Process-wide and unsynchronised: use only in a
/// single-threaded test, and only around the call under test.
class ScopedFailingUpbAlloc
{
public:
    ScopedFailingUpbAlloc() noexcept : m_saved(upb_alloc_global.func)
    {
        upb_alloc_global.func = &Fail;
    }

    ~ScopedFailingUpbAlloc() noexcept
    {
        upb_alloc_global.func = m_saved;
    }

    ScopedFailingUpbAlloc(const ScopedFailingUpbAlloc&) = delete;
    ScopedFailingUpbAlloc& operator=(const ScopedFailingUpbAlloc&) = delete;
    ScopedFailingUpbAlloc(ScopedFailingUpbAlloc&&) = delete;
    ScopedFailingUpbAlloc& operator=(ScopedFailingUpbAlloc&&) = delete;

private:
    // upb frees by calling the same function with size 0. An arena made
    // before the swap and freed during it must still be freed.
    static void* Fail(upb_alloc* /*alloc*/,
                      void* ptr,
                      std::size_t /*oldsize*/,
                      std::size_t size) noexcept
    {
        if (size == 0)
        {
            // NOLINTNEXTLINE(cppcoreguidelines-no-malloc,cppcoreguidelines-owning-memory)
            std::free(ptr);
        }
        return nullptr;
    }

    upb_alloc_func* m_saved;
};

}  // namespace microtel::testing
