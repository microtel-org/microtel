# Copyright (c) 2026 The microtel Authors.
# SPDX-License-Identifier: Apache-2.0
#
# tests/unit/ — gtest unit tests, mirroring src/. See tests/unit/README.md.

# M2 chunk 2: smoke test that puts the public + internal header tree
# through gtest's translation unit. Catches name collisions between
# microtel and gtest (e.g., `internal::Test` vs gtest's `Test`) early.
microtel_add_test(
    NAME    headers_smoke_test
    SOURCES headers_smoke_test.cpp)

# M2 chunk 3: compile + link verification for the dumb mocks under
# tests/mocks/. Real behavioural tests using these mocks land in M3.
microtel_add_test(
    NAME    mocks_smoke_test
    SOURCES mocks_smoke_test.cpp)

# M2 chunk 4: compile + link verification for the logic-bearing fakes
# under tests/fakes/. Exercises the scripting/recording machinery that
# distinguishes a fake from a mock; real behavioural use lands in M3.
microtel_add_test(
    NAME    fakes_smoke_test
    SOURCES fakes_smoke_test.cpp)

# M2 chunk 6: behavioural test of the SetLogSink / ResetLogSink public
# API + the internal LogImpl entry point. Links the real production code
# from src/common/.
microtel_add_test(
    NAME    log_sink_test
    SOURCES log_sink_test.cpp
    LIBS    microtel_common)
target_include_directories(log_sink_test PRIVATE ${PROJECT_SOURCE_DIR}/src)

# kVersionString against kVersionMajor/Minor/Patch (both in the public
# include/microtel/version.hpp), and against the release named in README.md and
# CONTRIBUTING.md, and the tag the FetchContent and CPM consumer projects pin.
# Header-only.
microtel_add_test(
    NAME    version_test
    SOURCES version_test.cpp)
target_compile_definitions(version_test PRIVATE
    MICROTEL_README_PATH="${PROJECT_SOURCE_DIR}/README.md"
    MICROTEL_CONTRIBUTING_PATH="${PROJECT_SOURCE_DIR}/.github/CONTRIBUTING.md"
    MICROTEL_SECURITY_PATH="${PROJECT_SOURCE_DIR}/.github/SECURITY.md"
    MICROTEL_BENCH_RESULTS_DIR="${PROJECT_SOURCE_DIR}/docs/bench-results"
    MICROTEL_CONSUMER_FETCHCONTENT_PATH="${PROJECT_SOURCE_DIR}/tests/consumer_fetchcontent/CMakeLists.txt"
    MICROTEL_CONSUMER_CPM_PATH="${PROJECT_SOURCE_DIR}/tests/consumer_cpm/CMakeLists.txt")

# M3 — Track A unit tests. Mirrors src/sdk/.
add_subdirectory(sdk)

# M3 — Track F unit tests. Mirrors src/wire/encoder/.
add_subdirectory(wire)

# M3 — Track D unit tests. Mirrors src/transport/.
add_subdirectory(transport)

# M3 — Track D RAII wrappers. Mirrors src/common/raii/.
add_subdirectory(common)

# M3 close — export pipeline. Mirrors src/exporter/.
add_subdirectory(exporter)

# M6-D — preflight CLI logic.
add_subdirectory(preflight)

# M12 — public API contract tests (Meter, instruments via Provider interface).
add_subdirectory(api)

# v1.1 — the header-only sugar layer (ICP 0028). Mirrors no src/ directory.
add_subdirectory(sugar)

# Optional adapters: the spdlog bridge (M14 L6), the glog and log4cxx bridges
# (issue #304) and the opentelemetry-cpp shim. Each test inside is gated on its
# own option.
if(MICROTEL_USE_SPDLOG OR MICROTEL_BUILD_GLOG_BRIDGE OR MICROTEL_BUILD_LOG4CXX_BRIDGE)
    add_subdirectory(adapters)
endif()

# v1.2 — vendored nanopb runtime + generated OTLP trace descriptors, built with
# the leaf (MICROTEL_BUILD_LEAF=ON): for a nanopb leaf, and for the leaf tests,
# which run on both backends.
if(TARGET microtel_nanopb_gen)
    add_subdirectory(nanopb)
endif()

# v1.2 — the leaf C library (docs/leaf-concentrator-design.md §7.1), only when
# the top-level build enables it. Mirrors leaf/.
if(TARGET microtel_leaf)
    add_subdirectory(leaf)
endif()
