# Copyright (c) 2026 The microtel Authors.
# SPDX-License-Identifier: Apache-2.0
#
# libFuzzer harnesses — built only when -DMICROTEL_BUILD_FUZZ=ON.
# Requires clang with -fsanitize=fuzzer (libFuzzer is clang-only).
#
# These targets are NOT registered with ctest. Fuzzing runs under the
# periodic fuzz.yml CI job and locally. The crashes/ corpus replay IS a
# hard PR gate (see ci/scripts/corpus-check.sh).
#
# Usage:
#   cmake -S . -B build-fuzz \
#         -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
#         -DMICROTEL_BUILD_FUZZ=ON -DMICROTEL_BUILD_LEAF=ON -DMICROTEL_BUILD_TESTS=OFF \
#         -DMICROTEL_WITH_CONCENTRATOR=ON
#   cmake --build build-fuzz
#   ./build-fuzz/tests/fuzz/toml_fuzz corpus/toml_fuzz/ -max_total_time=60

if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    message(WARNING
        "MICROTEL_BUILD_FUZZ=ON requires clang "
        "(CMAKE_CXX_COMPILER_ID=${CMAKE_CXX_COMPILER_ID}). "
        "Fuzz targets will not be built.")
    return()
endif()

function(microtel_add_fuzz_target)
    cmake_parse_arguments(MF "" "NAME" "SOURCES;LIBS" ${ARGN})

    if(NOT MF_NAME OR NOT MF_SOURCES)
        message(FATAL_ERROR "microtel_add_fuzz_target: NAME and SOURCES are required")
    endif()

    add_executable(${MF_NAME} ${MF_SOURCES})

    target_compile_options(${MF_NAME} PRIVATE
        -fsanitize=fuzzer,address,undefined
        -fno-omit-frame-pointer
        -Wall -Wextra -Wpedantic ${MICROTEL_WERROR})

    target_link_options(${MF_NAME} PRIVATE
        -fsanitize=fuzzer,address,undefined)

    target_link_libraries(${MF_NAME} PRIVATE
        microtel_headers
        ${MF_LIBS})

    target_include_directories(${MF_NAME} PRIVATE
        ${PROJECT_SOURCE_DIR}/src
        ${PROJECT_SOURCE_DIR}/tests)
endfunction()

microtel_add_fuzz_target(
    NAME    grpc_codec_fuzz
    SOURCES grpc_codec_fuzz.cpp
    LIBS    microtel_grpc_wire)

microtel_add_fuzz_target(
    NAME    toml_fuzz
    SOURCES toml_fuzz.cpp
    LIBS    microtel_config)

microtel_add_fuzz_target(
    NAME    otlp_response_fuzz
    SOURCES otlp_response_fuzz.cpp
    LIBS    microtel_otlp_response)

microtel_add_fuzz_target(
    NAME    response_decompression_fuzz
    SOURCES response_decompression_fuzz.cpp
    LIBS    microtel_gzip microtel_grpc_wire)

microtel_add_fuzz_target(
    NAME    baggage_fuzz
    SOURCES baggage_fuzz.cpp
    LIBS    microtel_api)

# ICP 0026 — the four hot-reload Provider setters, driven against a live
# provider. Needs src/ on the include path for sdk/sdk_provider.hpp and
# common/internal_log.hpp; tests/ is already there for the mocks.
microtel_add_fuzz_target(
    NAME    provider_setters_fuzz
    SOURCES provider_setters_fuzz.cpp
    LIBS    microtel_sdk microtel_common)

target_include_directories(provider_setters_fuzz PRIVATE ${PROJECT_SOURCE_DIR}/src)

# SdkProviderArgs bundles every pipeline; this harness names only the ones it
# varies and lets the rest default, the same waiver the sdk_provider tests take.
target_compile_options(provider_setters_fuzz PRIVATE
    -Wno-missing-field-initializers)

# v1.2 — byte identity of the leaf's upb and nanopb backends
# (docs/leaf-concentrator-design.md §7.2). Needs MICROTEL_BUILD_LEAF=ON, which
# builds microtel_leaf_dual_fuzz: the leaf core with both backends, instrumented
# for libFuzzer. The program interpreter is shared with the deterministic
# microtel_leaf_backend_diff_test (tests/unit/leaf/).
if(TARGET microtel_leaf_dual_fuzz)
    set(MICROTEL_LEAF_DIFF_SOURCES
        leaf_backend_diff_fuzz.cpp
        ${PROJECT_SOURCE_DIR}/tests/leaf/diff/leaf_diff_program.cpp)
    microtel_add_fuzz_target(
        NAME    leaf_backend_diff_fuzz
        SOURCES ${MICROTEL_LEAF_DIFF_SOURCES}
                ${PROJECT_SOURCE_DIR}/tests/leaf/vectors/leaf_vectors.c
        LIBS    microtel_leaf_dual_fuzz microtel_upb_gen)

    # The interpreter decodes each payload with upb: gen/ and the upb runtime
    # headers, with the rename header forced in as for every upb consumer.
    target_include_directories(leaf_backend_diff_fuzz PRIVATE
        ${PROJECT_SOURCE_DIR}/gen
        ${PROJECT_SOURCE_DIR}/third_party/upb)
    set_source_files_properties(${MICROTEL_LEAF_DIFF_SOURCES} PROPERTIES
        COMPILE_OPTIONS "-include;${MICROTEL_UPB_RENAME_HEADER}")
elseif(MICROTEL_BUILD_LEAF)
    message(WARNING "leaf_backend_diff_fuzz not built: microtel_leaf_dual_fuzz is missing")
endif()

# v1.2 — the concentrator ingest path (docs/leaf-concentrator-design.md §7.3,
# ICP 0031 ship gate 2): the receiver end to end, and the upb decoder alone.
# Only in a concentrator build, where the receiver and decoder are compiled;
# the fuzz jobs configure with MICROTEL_WITH_CONCENTRATOR=ON.
if(MICROTEL_WITH_CONCENTRATOR)
    microtel_add_fuzz_target(
        NAME    leaf_ingest_fuzz
        SOURCES leaf_ingest_fuzz.cpp
        LIBS    microtel_sdk microtel_encoder microtel_common)
    target_compile_options(leaf_ingest_fuzz PRIVATE
        -Wno-missing-field-initializers)

    microtel_add_fuzz_target(
        NAME    otlp_trace_decoder_fuzz
        SOURCES otlp_trace_decoder_fuzz.cpp
        LIBS    microtel_encoder)
endif()
