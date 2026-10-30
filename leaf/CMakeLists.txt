# Copyright (c) 2026 The microtel Authors.
# SPDX-License-Identifier: Apache-2.0
#
# microtel-leaf: the C span builder and OTLP encoder for constrained devices
# (docs/leaf-concentrator-design.md §1, §2.1).
#
# Works two ways:
#
#   - standalone:   cmake -S leaf -B build-leaf [-DMICROTEL_LEAF_ENCODER=upb]
#     Needs only a C compiler. Never enables CXX and never looks for nghttp2,
#     OpenSSL or zlib, so a cross toolchain with no C++ runtime can build it.
#   - in-tree:      cmake -S . -DMICROTEL_BUILD_LEAF=ON [-DMICROTEL_LEAF_ENCODER=upb]
#     The top-level project adds this directory. `microtel_leaf` is exported
#     as `microtel::leaf` and is never a dependency of `microtel::microtel`.
#
# The upb backend links the project's own microtel_upb_gen / microtel_upb_runtime
# / microtel_utf8_range archives rather than compiling a second copy, so a process
# that links both the runtime and a upb leaf gets one upb. The nanopb backend
# (the default) links microtel_nanopb_gen / microtel_nanopb, which are built
# for the leaf alone (ICP 0031 Decision 4).

cmake_minimum_required(VERSION 3.20)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(microtel_leaf
            VERSION 1.2.6
            LANGUAGES C
            DESCRIPTION "microtel-leaf: OTLP span encoder for constrained devices")
    set(MICROTEL_LEAF_STANDALONE ON)
    include(GNUInstallDirs)
    # MICROTEL_WARNINGS_AS_ERRORS / ${MICROTEL_WERROR}; in-tree the top level
    # has already included it.
    include(${CMAKE_CURRENT_SOURCE_DIR}/../cmake/MicrotelWarnings.cmake)
else()
    set(MICROTEL_LEAF_STANDALONE OFF)
endif()

# In-tree the top-level CMakeLists defines (and validates) this option; the
# definition here only takes effect standalone. Same default: nanopb (§6.1).
set(MICROTEL_LEAF_ENCODER "nanopb" CACHE STRING "Leaf encoder backend: upb or nanopb")
set_property(CACHE MICROTEL_LEAF_ENCODER PROPERTY STRINGS upb nanopb)

if(NOT MICROTEL_LEAF_ENCODER MATCHES "^(upb|nanopb)$")
    message(FATAL_ERROR
        "MICROTEL_LEAF_ENCODER='${MICROTEL_LEAF_ENCODER}' invalid; expected upb or nanopb")
endif()

# In-tree, the test-only libraries below are built when the test tree or the
# fuzz harnesses are: every leaf test runs against both backends (§7.1), and
# the differential fuzz target links both into one binary (§7.2).
set(MICROTEL_LEAF_TEST_LIBS OFF)
if(NOT MICROTEL_LEAF_STANDALONE AND (MICROTEL_BUILD_TESTS OR MICROTEL_BUILD_FUZZ))
    set(MICROTEL_LEAF_TEST_LIBS ON)
endif()

# Standalone: build the vendored archives the chosen backend needs from the
# same sources the main project uses. In-tree they already exist.
if(MICROTEL_LEAF_ENCODER STREQUAL "upb" AND NOT TARGET microtel_upb_gen)
    set(MICROTEL_UPB_RENAME_HEADER
        ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/upb/microtel_upb_rename.h)
    add_subdirectory(../third_party/utf8_range ${CMAKE_CURRENT_BINARY_DIR}/utf8_range)
    add_subdirectory(../third_party/upb ${CMAKE_CURRENT_BINARY_DIR}/upb)
    add_subdirectory(../gen ${CMAKE_CURRENT_BINARY_DIR}/gen)
endif()
if(MICROTEL_LEAF_ENCODER STREQUAL "nanopb" AND NOT TARGET microtel_nanopb_gen)
    add_subdirectory(../third_party/nanopb ${CMAKE_CURRENT_BINARY_DIR}/nanopb)
    add_subdirectory(../gen/nanopb ${CMAKE_CURRENT_BINARY_DIR}/gen_nanopb)
endif()

# Only backend_upb.c includes upb headers; it needs the rename header forced in
# so it calls the renamed runtime (ICP 0020 Decision 4). The nanopb rename
# header reaches backend_nanopb.c through microtel_nanopb's PUBLIC options.
if(TARGET microtel_upb_gen)
    set_source_files_properties(src/backend_upb.c PROPERTIES
        COMPILE_OPTIONS "-include;${MICROTEL_UPB_RENAME_HEADER}")
endif()

# microtel_leaf_add_library(<name> <upb|nanopb|dual> [extra sources...])
#
# One leaf archive: the core plus one backend, named to leaf_core.c through
# MICROTEL_LEAF_BACKEND_ENCODE (§2.1). `dual` is the test-only form that links
# both backends behind a runtime switch (tests/leaf/dual/), for the byte-
# identity comparison of §7.2; the extra sources supply its entry point.
function(microtel_leaf_add_library name backend)
    set(sources src/leaf_core.c ${ARGN})
    if(backend STREQUAL "upb" OR backend STREQUAL "dual")
        list(APPEND sources src/backend_upb.c)
    endif()
    if(backend STREQUAL "nanopb" OR backend STREQUAL "dual")
        list(APPEND sources src/backend_nanopb.c)
    endif()
    add_library(${name} STATIC ${sources})

    target_include_directories(${name}
        PUBLIC
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
            $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/src)

    # The backend entry point leaf_core.c calls (§2.1).
    target_compile_definitions(${name} PRIVATE
        MICROTEL_LEAF_BACKEND_ENCODE=microtel_leaf_internal_encode_${backend})

    # C11 with no extensions (§1.2).
    set_target_properties(${name} PROPERTIES
        C_STANDARD 11
        C_STANDARD_REQUIRED ON
        C_EXTENSIONS OFF)

    if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(${name} PRIVATE
            -pedantic-errors -Wall -Wextra -Wvla ${MICROTEL_WERROR})
    endif()

    # PRIVATE: the backend's include paths never reach a consumer. A static
    # archive still carries the link dependency, so the closure resolves.
    if(backend STREQUAL "upb" OR backend STREQUAL "dual")
        target_link_libraries(${name} PRIVATE microtel_upb_gen)
    endif()
    if(backend STREQUAL "nanopb" OR backend STREQUAL "dual")
        target_link_libraries(${name} PRIVATE microtel_nanopb_gen)
    endif()
endfunction()

microtel_leaf_add_library(microtel_leaf ${MICROTEL_LEAF_ENCODER})

# Test-only, never installed or exported: the backend that microtel_leaf was
# not built with, so each backend gets its own test binary from one test source
# (tests/unit/leaf/), and microtel_leaf_dual for the differential comparison.
if(MICROTEL_LEAF_TEST_LIBS)
    foreach(backend upb nanopb)
        if(NOT backend STREQUAL MICROTEL_LEAF_ENCODER)
            microtel_leaf_add_library(microtel_leaf_${backend} ${backend})
        endif()
    endforeach()
    microtel_leaf_add_library(microtel_leaf_dual dual
        ${CMAKE_CURRENT_SOURCE_DIR}/../tests/leaf/dual/leaf_dual.c)
    target_include_directories(microtel_leaf_dual PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/../tests/leaf/dual)
endif()

# The same, instrumented for libFuzzer, so leaf_backend_diff_fuzz gets coverage
# feedback from the core and both backends (tests/fuzz/).
if(NOT MICROTEL_LEAF_STANDALONE AND MICROTEL_BUILD_FUZZ AND CMAKE_C_COMPILER_ID MATCHES "Clang")
    microtel_leaf_add_library(microtel_leaf_dual_fuzz dual
        ${CMAKE_CURRENT_SOURCE_DIR}/../tests/leaf/dual/leaf_dual.c)
    target_include_directories(microtel_leaf_dual_fuzz PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/../tests/leaf/dual)
    target_compile_options(microtel_leaf_dual_fuzz PRIVATE
        -fsanitize=fuzzer-no-link,address,undefined -fno-omit-frame-pointer)
endif()

set_target_properties(microtel_leaf PROPERTIES EXPORT_NAME leaf)
add_library(microtel::leaf ALIAS microtel_leaf)

if(MICROTEL_LEAF_STANDALONE)
    if(MICROTEL_LEAF_ENCODER STREQUAL "upb")
        install(TARGETS microtel_leaf microtel_upb_gen microtel_upb_runtime microtel_utf8_range
            ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
    else()
        install(TARGETS microtel_leaf microtel_nanopb_gen microtel_nanopb
            ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
    endif()
    install(FILES include/microtel/leaf.h
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/microtel)
endif()
