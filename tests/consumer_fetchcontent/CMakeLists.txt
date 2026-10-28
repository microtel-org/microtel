# Copyright (c) 2026 The microtel Authors.
# SPDX-License-Identifier: Apache-2.0
#
# tests/consumer_fetchcontent — microtel as a FetchContent subproject.
#
# Like tests/consumer, this is NOT part of the microtel build. It builds the
# same main.cpp, but with microtel added to this project's build instead of
# found in an install prefix. ci/scripts/consumer-package.sh drives it and
# points FetchContent at the checkout under test:
#
#   cmake -S tests/consumer_fetchcontent -B /tmp/mt-fc \
#         -DFETCHCONTENT_SOURCE_DIR_MICROTEL=$PWD
#
# Without that override it downloads the tag below, as an application would.

cmake_minimum_required(VERSION 3.20)

project(microtel_consumer_fetchcontent LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)
FetchContent_Declare(
    microtel
    GIT_REPOSITORY https://github.com/microtel-org/microtel.git
    GIT_TAG        v1.2.5
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(microtel)

add_executable(microtel_consumer_smoke ../consumer/main.cpp)
target_link_libraries(microtel_consumer_smoke PRIVATE microtel::microtel)

# main.cpp checks kVersionString against the version CMake saw; for a
# subproject that is microtel's own PROJECT_VERSION.
get_directory_property(microtel_version DIRECTORY ${microtel_SOURCE_DIR}
                       DEFINITION PROJECT_VERSION)
target_compile_definitions(microtel_consumer_smoke
    PRIVATE MICROTEL_CONSUMER_PACKAGE_VERSION="${microtel_version}")
