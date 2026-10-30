// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "microtel/error.hpp"
#include "microtel/expected.hpp"
#include "microtel/internal/transport.hpp"
#include "microtel/internal/wire_result.hpp"

#include "wire/auth_failure.hpp"

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace microtel::wire
{

/// @brief The first request field that neither HTTP/1.1 nor HTTP/2 can carry.
///
/// Rejects a CR, LF or NUL anywhere in a field's name or value (ICP 0042 §3).
/// Both codecs' `BuildHeaders` call it last, after `authorization` is
/// appended, so it covers every field on every transport. `Build()` already
/// checks static headers (ICP 0038, issue #412); this is the check that also
/// sees what an `IAuthProvider` returns at send time, where a CR or LF is
/// header injection on HTTP/1.1. The serialisers keep no check of their own.
///
/// @return A `Malformed` error naming the field, never its value (it may be a
///         secret), or `std::nullopt` if every field is valid. A bad name is
///         not echoed either: it would put the CR or LF into the log line.
[[nodiscard]] inline std::optional<microtel::Error> ValidateRequestHeaders(
    std::span<const internal::HeaderField> headers)
{
    constexpr std::string_view kForbidden{"\r\n\0", 3};
    for (const auto& field : headers)
    {
        if (field.name.find_first_of(kForbidden) != std::string::npos)
        {
            return microtel::Error{.kind = microtel::Error::Kind::Malformed,
                                   .message = "a request header name contains CR, LF or NUL",
                                   .os_errno = 0};
        }
        if (field.value.find_first_of(kForbidden) != std::string::npos)
        {
            return microtel::Error{.kind = microtel::Error::Kind::Malformed,
                                   .message = "request header \"" + field.name +
                                              "\" has a value containing CR, LF or NUL",
                                   .os_errno = 0};
        }
    }
    return std::nullopt;
}

/// @brief The `WireResult` a `ValidateRequestHeaders` failure produces.
///
/// Terminal, like `AuthFailure`: the batch is dropped before
/// `ITransport::Send`, because the same header would be rejected on every
/// retry. Unlike `AuthFailure` the message is not prefixed — the bad field
/// need not be `authorization`.
[[nodiscard]] inline internal::WireResult InvalidHeaderFailure(microtel::Error err)
{
    return internal::WireResult{
        .success = false,
        .retryable = false,
        .retry_after = {},
        .partial_success_rejected = 0,
        .error = std::move(err),
        .response_excerpt = {},
    };
}

/// @brief The last step of both codecs' `BuildHeaders`.
///
/// @param headers    the assembled block, `authorization` included when the
///                   provider gave one.
/// @param auth_error what appending `authorization` failed with, if it did.
/// @return @p headers, or the terminal `WireResult` to return instead: the
///         `AuthFailure` for @p auth_error, or the `InvalidHeaderFailure` for
///         the first field `ValidateRequestHeaders` rejects.
[[nodiscard]] inline microtel::Expected<std::vector<internal::HeaderField>, internal::WireResult>
FinishRequestHeaders(std::vector<internal::HeaderField> headers,
                     const std::optional<microtel::Error>& auth_error)
{
    if (auth_error)
    {
        return microtel::make_unexpected(AuthFailure(*auth_error));
    }
    if (auto err = ValidateRequestHeaders(headers))
    {
        return microtel::make_unexpected(InvalidHeaderFailure(std::move(*err)));
    }
    return headers;
}

}  // namespace microtel::wire
