# Copyright (c) 2026 The microtel Authors.
# SPDX-License-Identifier: Apache-2.0
#
# tests/consumer — the external consumer smoke project (ICP 0020 Decision 6).
#
# This is NOT part of the microtel build. tests/CMakeLists.txt does not
# add_subdirectory() this directory and must not: the point is to configure
# against a genuinely *installed* prefix, where a missing install(FILES …) or
# an export set naming a target it never installed is visible. In-tree
# everything builds because everything is a subdirectory.
#
# It doubles as the canonical find_package example — see docs/development.md
# §12 — so it stays the size of a README snippet. Everything below the
# cmake_minimum_required line is what a consumer actually writes.
#
# Driven by ci/scripts/consumer-smoke.sh. By hand:
#
#   cmake --install <build-dir> --prefix /tmp/mt-prefix
#   cmake -S tests/consumer -B /tmp/mt-consumer \
#         -Dmicrotel_DIR=/tmp/mt-prefix/lib64/cmake/microtel
#   cmake --build /tmp/mt-consumer
#   ctest --test-dir /tmp/mt-consumer --output-on-failure
#
# (libdir is lib64 on Fedora and lib on Debian/Ubuntu; the script resolves it
# rather than guessing. CMAKE_PREFIX_PATH=/tmp/mt-prefix works equally well.)

cmake_minimum_required(VERSION 3.20)

project(microtel_consumer_smoke LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(microtel REQUIRED CONFIG)

add_executable(microtel_consumer_smoke main.cpp)

# microtel::microtel is the only supported exported name (ICP 0020 Decision 2).
# It is an INTERFACE aggregate: CMake derives the rest of the static closure,
# and its link order, from the real dependency graph.
target_link_libraries(microtel_consumer_smoke PRIVATE microtel::microtel)

# The version find_package() matched against (microtelConfigVersion.cmake),
# handed to main.cpp so it can be checked against the installed version.hpp.
target_compile_definitions(microtel_consumer_smoke
    PRIVATE MICROTEL_CONSUMER_PACKAGE_VERSION="${microtel_VERSION}")

# So that ctest runs the smoke binary instead of finding no tests.
enable_testing()
add_test(NAME microtel_consumer_smoke COMMAND microtel_consumer_smoke)

# The installed package describes how it was configured. Printing it makes a CI
# log say which prefix was consumed rather than leaving it to be inferred.
message(STATUS "microtel_DIR=${microtel_DIR} version=${microtel_VERSION} "
               "WITH_SPDLOG=${microtel_WITH_SPDLOG}")
