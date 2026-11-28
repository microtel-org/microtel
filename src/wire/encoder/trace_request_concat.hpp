// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "microtel/internal/encoded_payload.hpp"

#include <span>

namespace microtel::wire
{

/// @brief Join several `ExportTraceServiceRequest` encodings into one request.
///
/// `ExportTraceServiceRequest` has exactly one field, `repeated ResourceSpans
/// resource_spans = 1`, and protobuf parses two concatenated encodings of a
/// message as their merge, which appends repeated fields. So the bytes of A
/// followed by the bytes of B are, exactly, a valid request whose
/// `resource_spans` are A's followed by B's
/// (`docs/leaf-concentrator-design.md` §3.6.1). This holds for this message
/// only; it is not a general way to merge OTLP payloads.
///
/// Byte-level: no upb is involved, and nothing is parsed or validated.
///
/// @param parts the encodings, in request order. Consumed: a single part is
///        moved out, and the caller must not read any of them afterwards.
///        Empty parts contribute nothing. The span lets a caller pass a run of
///        a larger buffer without moving it into a vector of its own.
/// @return one payload. A single part is returned as is, with no copy.
/// @throws std::bad_alloc if the joined buffer cannot be allocated.
[[nodiscard]] internal::EncodedPayload ConcatenateTraceRequests(
    std::span<internal::EncodedPayload> parts);

}  // namespace microtel::wire
