// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstdint>
#include <string_view>

namespace microtel
{

/// @file version.hpp
/// @brief Version constants for the microtel runtime.
///
/// These constants are hand-set and must be kept equal to the `VERSION` given
/// to `project(microtel …)` in the top-level `CMakeLists.txt`. Generating this
/// header from `PROJECT_VERSION` at configure time is deferred past v1.0: it
/// would make the public API depend on a configured build directory, which the
/// header-only `microtel_headers` target and the M0 header check do not have.
///
/// A release bump therefore edits this header, `CMakeLists.txt`, the gRPC
/// user-agent literal in `src/wire/grpc/grpc_wire_codec.cpp`, and `kVersion` in
/// `tools/preflight/preflight.cpp`. The user-agent carries a `static_assert`
/// against `kVersionString`; the preflight literal does not, and was out of step
/// from M6-D until 1.0.0 shipped. `ci/scripts/version-drift-check.sh` (CI job
/// `version-drift-check`) now compares all of them against `PROJECT_VERSION`.
/// The procedure is in `docs/releasing.md`.

inline constexpr std::uint32_t kVersionMajor = 1;
inline constexpr std::uint32_t kVersionMinor = 2;
inline constexpr std::uint32_t kVersionPatch = 6;

inline constexpr std::string_view kVersionString = "1.2.6";

}  // namespace microtel
