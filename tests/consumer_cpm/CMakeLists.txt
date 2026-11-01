# Copyright (c) 2026 The microtel Authors.
# SPDX-License-Identifier: Apache-2.0
#
# tests/consumer_cpm — microtel added with CPM.cmake.
#
# Like tests/consumer, this is NOT part of the microtel build. It builds the
# same main.cpp, with microtel added through CPMAddPackage. CPM.cmake itself
# is downloaded at a pinned release and checked against its SHA-256.
# ci/scripts/consumer-package.sh drives it and points CPM at the checkout
# under test:
#
#   cmake -S tests/consumer_cpm -B /tmp/mt-cpm -DCPM_microtel_SOURCE=$PWD
#
# Without that override it downloads the tag below, as an application would.

cmake_minimum_required(VERSION 3.20)

project(microtel_consumer_cpm LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CPM_VERSION 0.43.2)
set(CPM_SHA256 49a3bef91ceb65bb66d57255e12d1ffd22abc2f6408fa9fe4534c544a2f232aa)
file(DOWNLOAD
     https://github.com/cpm-cmake/CPM.cmake/releases/download/v${CPM_VERSION}/CPM.cmake
     ${CMAKE_CURRENT_BINARY_DIR}/cmake/CPM.cmake
     EXPECTED_HASH SHA256=${CPM_SHA256})
include(${CMAKE_CURRENT_BINARY_DIR}/cmake/CPM.cmake)

CPMAddPackage("gh:microtel-org/microtel@1.2.6")

add_executable(microtel_consumer_smoke ../consumer/main.cpp)
target_link_libraries(microtel_consumer_smoke PRIVATE microtel::microtel)

# main.cpp checks kVersionString against microtel's own PROJECT_VERSION.
get_directory_property(microtel_version DIRECTORY ${microtel_SOURCE_DIR}
                       DEFINITION PROJECT_VERSION)
target_compile_definitions(microtel_consumer_smoke
    PRIVATE MICROTEL_CONSUMER_PACKAGE_VERSION="${microtel_version}")
