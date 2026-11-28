// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#include "trace_request_concat.hpp"

#include "microtel/internal/encoded_payload.hpp"

#include <cstddef>
#include <cstring>
#include <memory>
#include <span>
#include <utility>

namespace microtel::wire
{

internal::EncodedPayload ConcatenateTraceRequests(std::span<internal::EncodedPayload> parts)
{
    if (parts.size() == 1)
    {
        return std::move(parts.front());
    }
    std::size_t total = 0;
    for (const auto& part : parts)
    {
        total += part.Size();
    }
    if (total == 0)
    {
        return {};
    }
    auto bytes = std::make_unique<std::byte[]>(total);
    std::size_t offset = 0;
    for (const auto& part : parts)
    {
        if (part.Size() > 0)
        {
            std::memcpy(bytes.get() + offset, part.Bytes().data(), part.Size());
            offset += part.Size();
        }
    }
    return internal::EncodedPayload{std::move(bytes), total};
}

}  // namespace microtel::wire
