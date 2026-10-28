# Copyright (c) 2026 The microtel Authors.
# SPDX-License-Identifier: Apache-2.0
#
# Top-level build: the header-only microtel_headers target, the runtime
# libraries under src/ (exported as microtel::microtel), the vendored upb and
# generated protos, the optional log and opentelemetry-cpp adapters, the leaf
# (MICROTEL_BUILD_LEAF), tools/, tests, benchmarks and examples, and the install
# and package-config rules at the foot of this file. docs/repository-layout.md
# lists every MICROTEL_* option.

cmake_minimum_required(VERSION 3.20)

project(microtel
        VERSION 1.2.5
        LANGUAGES C CXX
        DESCRIPTION "Lightweight OpenTelemetry-compatible runtime for traces, metrics and logs, built on nghttp2")

# TRUE when microtel is the top-level project, FALSE under add_subdirectory()
# or FetchContent. Developer-only parts (tests, the header check, the spdlog
# fetch) default OFF in a parent's build. PROJECT_IS_TOP_LEVEL needs CMake 3.21.
if(CMAKE_SOURCE_DIR STREQUAL PROJECT_SOURCE_DIR)
    set(MICROTEL_IS_TOP_LEVEL ON)
else()
    set(MICROTEL_IS_TOP_LEVEL OFF)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# bindir / libdir / includedir for the install rules at the foot of this file.
# Included up here because the $<INSTALL_INTERFACE:> expressions below need
# CMAKE_INSTALL_INCLUDEDIR.
include(GNUInstallDirs)

# Header-only target — exposes include/ and the vendored tl::expected to
# consumers and to the M0 header check. Per ICP 0002 the public-API
# `microtel::Expected<T, E>` aliases to `tl::expected` on C++20 and to
# `std::expected` on C++23.
#
# In an installed tree the vendored tl::expected moves to
# <includedir>/microtel/vendor/tl/expected.hpp, so `microtel/vendor` — not
# `microtel` — goes on the include path: expected.hpp says
# `#include "tl/expected.hpp"`, and the extra `vendor` level keeps that from
# resolving against a consumer's own tl-expected (or ours from theirs).
add_library(microtel_headers INTERFACE)
target_include_directories(microtel_headers INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/third_party/tl-expected>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/microtel/vendor>)
target_compile_features(microtel_headers INTERFACE cxx_std_20)

# The v1.1 sugar layer (ICP 0028 §4) — header-only, shaped like the spdlog
# bridge in src/adapters/spdlog/CMakeLists.txt but declared here, with three
# deviations from that precedent:
#
#   - it lives in the root file, beside microtel_headers, because it owns no
#     source file and ICP 0020 already centralises the target surface here;
#   - it is not optional — sugar is public API in v1.1, not an opt-in adapter;
#   - it is NOT added to MICROTEL_EXPORTED_TARGETS. ICP 0020 Decision 2 draws
#     the supported line at microtel::microtel, whose interface already
#     carries include/ through microtel_headers, so find_package(microtel) +
#     microtel::microtel + #include <microtel/sugar.hpp> already works. An
#     exported microtel::sugar would be a name with no use case, and
#     Decision 2's own rule is that promotion is available later and
#     demotion is not. In-tree, this target is what sugar's tests name.
#
# No install rule either: install(DIRECTORY include/microtel …) below is
# recursive, so include/microtel/sugar/ ships with everything else.
add_library(microtel_sugar INTERFACE)
target_link_libraries(microtel_sugar INTERFACE microtel_headers)
target_compile_features(microtel_sugar INTERFACE cxx_std_20)

# option(MICROTEL_WARNINGS_AS_ERRORS ... ON) and ${MICROTEL_WERROR}, which
# every target below puts after its -W flags. ON (the default) keeps -Werror;
# packagers turn it OFF. The module, not this file, holds the option so the
# standalone leaf and leaf-target projects share the one definition.
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/MicrotelWarnings.cmake)

# M0 header compile check.
#
# Verifies every public and internal header compiles standalone with -Werror.
# Per ICP 0001 this is the operational form of the rule "Both header trees
# compile under an INTERFACE CMake target with -Werror. No implementations."
#
# Defaults OFF when ci/ is absent: .gitattributes marks it export-ignore, so
# release archives (git archive, GitHub's tag archives) do not carry
# ci/header_check.cpp, and a default configure of one must still succeed.
# Also OFF as a subproject.
if(MICROTEL_IS_TOP_LEVEL AND EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/ci/header_check.cpp)
    set(_microtel_header_check_default ON)
else()
    set(_microtel_header_check_default OFF)
endif()
option(MICROTEL_BUILD_HEADER_CHECK "Build the M0 header compile check"
       ${_microtel_header_check_default})
unset(_microtel_header_check_default)

if(MICROTEL_BUILD_HEADER_CHECK)
    add_executable(microtel_header_check ci/header_check.cpp)
    target_link_libraries(microtel_header_check PRIVATE microtel_headers)
    target_compile_options(microtel_header_check PRIVATE
        -Wall -Wextra -Wpedantic ${MICROTEL_WERROR})
endif()

# Sanitizer build helper. MICROTEL_SANITIZER ∈ { '', asan, tsan, ubsan }.
# Empty (default) means no sanitizer. asan + tsan + ubsan must be clean on the
# full test suite (docs/ci-architecture.md, sanitizers).
set(MICROTEL_SANITIZER "" CACHE STRING
    "Sanitizer to enable: '', asan, tsan, ubsan")
set_property(CACHE MICROTEL_SANITIZER PROPERTY STRINGS "" asan tsan ubsan)

if(NOT MICROTEL_SANITIZER STREQUAL "")
    if(MICROTEL_SANITIZER STREQUAL "asan")
        set(_microtel_san_flags -fsanitize=address -fno-omit-frame-pointer)
    elseif(MICROTEL_SANITIZER STREQUAL "tsan")
        set(_microtel_san_flags -fsanitize=thread -fno-omit-frame-pointer)
    elseif(MICROTEL_SANITIZER STREQUAL "ubsan")
        set(_microtel_san_flags -fsanitize=undefined -fno-omit-frame-pointer)
    else()
        message(FATAL_ERROR
            "MICROTEL_SANITIZER='${MICROTEL_SANITIZER}' invalid; "
            "expected one of: '', asan, tsan, ubsan")
    endif()
    add_compile_options(${_microtel_san_flags})
    add_link_options(${_microtel_san_flags})
    message(STATUS "microtel: ${MICROTEL_SANITIZER} enabled")
endif()

# Coverage build helper. The thresholds are 90% line /
# 85% branch on SDK + encoder code and 80% line on transport + exporter;
# `ci/scripts/coverage.sh` enforces all three.
#
# Clang gets **source-based** coverage (`-fprofile-instr-generate
# -fcoverage-mapping`), which is what the gate measures. Its counters are
# attached to source *regions* the front end knows about, so a branch record
# corresponds to a conditional someone wrote. gcov, by contrast, emits an edge
# for the unwind path out of every potentially-throwing call, which put whole-
# tree branch coverage at 57.8% against 91.0% line and made the branch
# floor unmeasurable (issue #198).
#
# The gcc branch below is a fallback for a local `--coverage` run; it is not
# what CI gates on. `-fprofile-update=atomic` makes gcov's counter increments
# atomic — without it, multi-threaded tests (log_sink_test, the
# BatchSpanProcessor tests) race on shared counters and produce corrupted
# .gcda files, which lcov surfaces as a negative delta.
option(MICROTEL_COVERAGE "Build with coverage instrumentation" OFF)
if(MICROTEL_COVERAGE)
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        # -fprofile-update=atomic covers the same multi-threaded counter race
        # for llvm's instrprof counters that it does for gcov's.
        add_compile_options(
            -fprofile-instr-generate -fcoverage-mapping
            -fprofile-update=atomic -O0 -g)
        add_link_options(-fprofile-instr-generate)
        message(STATUS "microtel: clang source-based coverage enabled")
    else()
        add_compile_options(--coverage -fprofile-update=atomic -O0 -g)
        add_link_options(--coverage)
        message(STATUS "microtel: gcov coverage enabled")
    endif()
endif()

# spdlog for the opt-in log bridge (src/adapters/spdlog); see
# docs/build-options.md. microtel's own diagnostics go to stderr in every
# build, and the public LogSink hook in include/microtel/log_sink.hpp lets
# applications redirect them into their own logger (docs/error-model.md §9.3).
# Default OFF as a subproject: nothing a parent links needs it (the bridge
# header ships in include/ either way), and it would fetch a second spdlog.
option(MICROTEL_USE_SPDLOG "Build the opt-in spdlog log bridge (microtel_spdlog_bridge)"
       ${MICROTEL_IS_TOP_LEVEL})

# Where toml++, spdlog and GoogleTest come from. OFF (the default) downloads
# pinned versions with FetchContent at configure time. ON resolves each with
# find_package(... CONFIG REQUIRED) and never touches the network, which is
# what a package manager (vcpkg, Conan, a distro build) needs: they build
# offline and supply their own copies. Plain find_package / FetchContent
# branches rather than FetchContent's FIND_PACKAGE_ARGS, which needs CMake
# 3.24; microtel's minimum is 3.20.
option(MICROTEL_USE_SYSTEM_DEPS
       "Find toml++, spdlog and GoogleTest with find_package instead of FetchContent" OFF)

if(MICROTEL_USE_SPDLOG AND MICROTEL_USE_SYSTEM_DEPS)
    # Only the header-only bridge (src/adapters/spdlog) and its tests use it;
    # no installed archive references spdlog, whichever way it was found.
    find_package(spdlog CONFIG REQUIRED)
elseif(MICROTEL_USE_SPDLOG)
    include(FetchContent)
    FetchContent_Declare(
        spdlog
        GIT_REPOSITORY https://github.com/gabime/spdlog.git
        GIT_TAG        v1.15.3
        GIT_SHALLOW    TRUE
    )
    # std::format mode — avoids a transitive fmt dependency, which is outside
    # the dependency closure (CLAUDE.md rule 12). Normal variables, not FORCEd
    # cache entries, here and for the other fetched projects below: option()
    # honours them (CMP0077), and they stay in microtel's directory scope
    # instead of overwriting a parent project's cache.
    set(SPDLOG_USE_STD_FORMAT ON)
    set(SPDLOG_INSTALL OFF)
    FetchContent_MakeAvailable(spdlog)
endif()

# toml++ — TOML config file parser (M6-A, ICP 0004).
# Linked PRIVATE into microtel_config only, and always compiled header-only,
# so the runtime closure is unchanged (CLAUDE.md rule 12): the toml++ code
# microtel uses is absorbed into libmicrotel_config.a and no installed archive
# has an undefined toml:: reference. symbol-scan.sh checks that.
if(MICROTEL_USE_SYSTEM_DEPS)
    find_package(tomlplusplus CONFIG REQUIRED)
else()
    include(FetchContent)
    FetchContent_Declare(
        tomlplusplus
        GIT_REPOSITORY https://github.com/marzer/tomlplusplus.git
        GIT_TAG        v3.4.0
        GIT_SHALLOW    TRUE
    )
    set(TOMLPLUSPLUS_INSTALL OFF)
    FetchContent_MakeAvailable(tomlplusplus)
endif()

# microtel takes only toml++'s include directories, never the
# tomlplusplus::tomlplusplus target itself. A packaged toml++ can be a
# compiled library whose target forces TOML_HEADER_ONLY=0 on its users
# (vcpkg's adds -DTOML_HEADER_ONLY=0 to INTERFACE_COMPILE_OPTIONS); linking
# it would leave libmicrotel_config.a with undefined toml:: symbols that the
# installed package does not resolve. Not installed: the only user wraps it
# in $<BUILD_INTERFACE:>. SYSTEM, as both upstream targets already are (an
# imported target's includes, and toml++'s warning guard when it is not the
# top-level project): third-party headers must not trip this project's
# warnings.
get_target_property(_microtel_toml_includes tomlplusplus::tomlplusplus
                    INTERFACE_INCLUDE_DIRECTORIES)
if(NOT _microtel_toml_includes)
    message(FATAL_ERROR
            "tomlplusplus::tomlplusplus has no INTERFACE_INCLUDE_DIRECTORIES; "
            "microtel needs toml++'s headers to compile it header-only.")
endif()
add_library(microtel_tomlplusplus INTERFACE)
target_include_directories(microtel_tomlplusplus SYSTEM INTERFACE ${_microtel_toml_includes})
target_compile_definitions(microtel_tomlplusplus INTERFACE TOML_HEADER_ONLY=1)
unset(_microtel_toml_includes)

# opentelemetry-cpp API-adapter shim (M17, ICP 0014) — opt-in, source-only.
#
# Only the header-only *API* is consumed. otel-cpp's SDK and exporters — and
# their protobuf / libcurl dependencies — are never configured or built:
# SOURCE_SUBDIR points at a directory containing no CMakeLists.txt, so
# FetchContent populates the sources without calling add_subdirectory() on
# otel-cpp's own project.
#
# OPENTELEMETRY_STL_VERSION=2020 is load-bearing, not a style preference. Under
# the upstream default (WITH_STL=OFF) the API resolves nostd:: to a vendored
# abseil snapshot shipped inside its own headers — WITH_ABSEIL=OFF does not
# prevent that; only the STL mode does. Pinned by static_assert in
# tests/unit/adapters/otelcpp_config_test.cpp so a change cannot pass silently.
option(MICROTEL_BUILD_OTELCPP_SHIM
       "Build the opentelemetry-cpp API-adapter shim (source-only, ICP 0014)" OFF)

if(MICROTEL_BUILD_OTELCPP_SHIM)
    include(FetchContent)
    FetchContent_Declare(
        opentelemetry_cpp_api
        GIT_REPOSITORY https://github.com/open-telemetry/opentelemetry-cpp.git
        GIT_TAG        v1.28.0
        GIT_SHALLOW    TRUE
        SOURCE_SUBDIR  api/include
    )
    FetchContent_MakeAvailable(opentelemetry_cpp_api)

    if(TARGET opentelemetry_api)
        message(FATAL_ERROR
                "opentelemetry-cpp's CMake project was added to the build. Only "
                "its header-only API may be consumed. See ICP 0014.")
    endif()

    # SYSTEM: third-party headers must not trip this project's warning settings.
    add_library(microtel_otelcpp_api INTERFACE)
    target_include_directories(microtel_otelcpp_api SYSTEM INTERFACE
        ${opentelemetry_cpp_api_SOURCE_DIR}/api/include)
    target_compile_definitions(microtel_otelcpp_api INTERFACE
        OPENTELEMETRY_STL_VERSION=2020
        OPENTELEMETRY_ABI_VERSION_NO=1)
    target_compile_features(microtel_otelcpp_api INTERFACE cxx_std_20)
endif()

# glog and log4cxx log bridges (issue #304) — opt-in, header-only.
#
# Shaped like the spdlog bridge: an INTERFACE target over one public header in
# include/microtel/adapters/, compiled inside whichever build links it. Neither
# is part of the core, neither is installed as a binary or exported (ICP 0020
# Decision 3), and no libmicrotel_* archive references a glog or log4cxx symbol
# — ci/scripts/symbol-scan.sh asserts that last part. Both libraries are found
# on the host rather than fetched: a consumer who wants a bridge already has
# the library in their build, and the bridge must compile against *that* copy.
option(MICROTEL_BUILD_GLOG_BRIDGE
       "Build the glog log bridge (header-only; needs glog >= 0.6 installed)" OFF)
option(MICROTEL_BUILD_LOG4CXX_BRIDGE
       "Build the log4cxx log bridge (header-only; needs log4cxx >= 1.1 installed)" OFF)

if(MICROTEL_BUILD_GLOG_BRIDGE)
    find_package(glog CONFIG REQUIRED)
    if(glog_VERSION VERSION_LESS 0.6)
        message(FATAL_ERROR
                "MICROTEL_BUILD_GLOG_BRIDGE needs glog 0.6 or newer; found ${glog_VERSION}")
    endif()
endif()

if(MICROTEL_BUILD_LOG4CXX_BRIDGE)
    find_package(log4cxx CONFIG REQUIRED)
    if(log4cxx_VERSION VERSION_LESS 1.1)
        message(FATAL_ERROR
                "MICROTEL_BUILD_LOG4CXX_BRIDGE needs log4cxx 1.1 or newer; found ${log4cxx_VERSION}")
    endif()
endif()

# Compile-time gate: when ON, insecure = true in config causes init failure.
# Default OFF (emit runtime warning only). See docs/configuration.md §3.5.
option(MICROTEL_FORBID_INSECURE_TLS
    "Fail at init if insecure = true is configured" OFF)

# Every vendored upb/utf8_range global is renamed to microtel_* so that a
# consumer who also links a real upb cannot get two definitions and a silent
# static-link selection between them (ICP 0020 Decision 4). The header is
# force-included into every target that compiles a TU reaching a upb header;
# its own comment block carries the rationale and the regeneration recipe.
# `ci/scripts/symbol-scan.sh` fails the build if any global escapes it.
set(MICROTEL_UPB_RENAME_HEADER
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/upb/microtel_upb_rename.h)

# Third-party libraries: upb runtime + utf8_range validator.
# CMake targets: microtel_upb_runtime, microtel_utf8_range.
# These are compiled as C11 with -w (no warnings from vendored code).
add_subdirectory(third_party/utf8_range)
add_subdirectory(third_party/upb)

# Generated upb accessors (protoc-gen-upb output committed under gen/).
# CMake target: microtel_upb_gen. Regen: ci/scripts/regen-protos.sh.
add_subdirectory(gen)

# The leaf (docs/leaf-concentrator-design.md §2.1). Experimental in v1.2 and
# never a dependency of microtel::microtel (ICP 0031 Decision 3).
option(MICROTEL_BUILD_LEAF "Build the microtel leaf C library (experimental)" OFF)
set(MICROTEL_LEAF_ENCODER "nanopb" CACHE STRING
    "Encoder backend of the leaf: upb or nanopb")
set_property(CACHE MICROTEL_LEAF_ENCODER PROPERTY STRINGS upb nanopb)
if(NOT MICROTEL_LEAF_ENCODER MATCHES "^(upb|nanopb)$")
    message(FATAL_ERROR
        "MICROTEL_LEAF_ENCODER must be upb or nanopb, not '${MICROTEL_LEAF_ENCODER}'")
endif()

# Declared here, ahead of the blocks that use them further down, because the
# leaf builds test-only libraries of both backends when either is ON.
option(MICROTEL_BUILD_TESTS "Build the test tree" ${MICROTEL_IS_TOP_LEVEL})
option(MICROTEL_BUILD_FUZZ "Build libFuzzer harnesses (requires clang -fsanitize=fuzzer)" OFF)

# nanopb is linked only into a nanopb leaf (ICP 0031 Decision 4). It is also
# built for a upb leaf when tests or fuzz harnesses are on, because every leaf
# test runs against both backends (docs/leaf-concentrator-design.md §7.1,
# §7.2); those test-only archives are never installed.
# CMake targets: microtel_nanopb (runtime, encoder only), microtel_nanopb_gen.
if(MICROTEL_BUILD_LEAF AND (MICROTEL_LEAF_ENCODER STREQUAL "nanopb"
                            OR MICROTEL_BUILD_TESTS OR MICROTEL_BUILD_FUZZ))
    add_subdirectory(third_party/nanopb)
    add_subdirectory(gen/nanopb)
endif()

# The leaf library itself, `microtel_leaf`, exported as `microtel::leaf`.
if(MICROTEL_BUILD_LEAF)
    add_subdirectory(leaf)
endif()

# The concentrator's leaf receiver (docs/leaf-concentrator-design.md §6.2,
# ICP 0034). Default OFF in v1.2: the receiver parses untrusted bytes, and it
# pulls upb's decoder into the link, which no default build contains. With it
# OFF the receiver and decoder sources are left out, the public types in
# leaf_receiver.hpp stay declared, Provider::GetLeafReceiver returns a no-op
# receiver, and SdkBuilder::WithLeafReceiver fails Build(). Shaped like the
# MICROTEL_WITH_* options of ICP 0030 (draft), and folded into it when it lands.
option(MICROTEL_WITH_CONCENTRATOR
    "Compile the concentrator's leaf receiver (experimental)" OFF)

# System dependencies: OpenSSL (TLS) and nghttp2 (HTTP/2 transport).
# Required by src/common/raii/ and src/transport/; also linked by their tests.
find_package(OpenSSL REQUIRED)
# zlib: request compression (`compression = "gzip"`). Already a member of the
# v1 runtime dependency closure per CLAUDE.md rule 12; this wires it up.
find_package(ZLIB REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(NGHTTP2 REQUIRED IMPORTED_TARGET libnghttp2)

# src/ — production code per the track ownership in
# `docs/development.md`. Subdirectories grow as M3+ implementation
# chunks land. Track A (src/sdk/) leads M3.
add_subdirectory(src/common)
add_subdirectory(src/api)
add_subdirectory(src/sdk)
add_subdirectory(src/wire)
add_subdirectory(src/transport)
add_subdirectory(src/exporter)
if(MICROTEL_USE_SPDLOG)
    add_subdirectory(src/adapters/spdlog)
endif()
if(MICROTEL_BUILD_OTELCPP_SHIM)
    add_subdirectory(src/adapters/otelcpp)
endif()
if(MICROTEL_BUILD_GLOG_BRIDGE)
    add_subdirectory(src/adapters/glog)
endif()
if(MICROTEL_BUILD_LOG4CXX_BRIDGE)
    add_subdirectory(src/adapters/log4cxx)
endif()
add_subdirectory(tools)

# ---------------------------------------------------------------------------
# Install rules and the exported target set — issue #19, ICP 0020.
#
# Centralised here rather than spread across src/*/CMakeLists.txt because the
# exported names are a compatibility surface (ICP 0020 Decision 2): "what
# microtel ships" should be one readable list, not something reconstructed by
# grepping fifteen directories.
# ---------------------------------------------------------------------------

# The aggregate. `microtel::microtel` is the ONLY supported exported name
# (ICP 0020 Decision 2); every other exported target exists so the static link
# closure resolves and may merge, split or disappear without an ICP.
#
# Linking microtel_sdk alone is deliberate: CMake derives the rest of the
# closure — and its link order — from the real dependency graph, which is the
# whole point of Decision 1. A static closure this wide is exactly what a
# consumer should not have to order by hand.
add_library(microtel INTERFACE)
add_library(microtel::microtel ALIAS microtel)
target_link_libraries(microtel INTERFACE microtel_sdk)

# The shipped set: the aggregate, the header carrier, and the fifteen static
# components. `microtel_preflight_lib` is tool-only and is not exported
# (ICP 0020 "What exists to export"); no adapter is exported (Decision 3).
set(MICROTEL_EXPORTED_TARGETS
    microtel
    microtel_headers
    microtel_api
    microtel_common
    microtel_config
    microtel_sdk
    microtel_exporter
    microtel_transport
    microtel_otlp_response
    microtel_gzip
    microtel_http_wire
    microtel_grpc_wire
    microtel_custom_wire
    microtel_encoder
    microtel_upb_gen
    microtel_upb_runtime
    microtel_utf8_range)

# A nanopb leaf carries its nanopb archives in the export set, as a upb leaf
# relies on the upb archives already listed above.
if(TARGET microtel_leaf)
    list(APPEND MICROTEL_EXPORTED_TARGETS microtel_leaf)
    if(MICROTEL_LEAF_ENCODER STREQUAL "nanopb")
        list(APPEND MICROTEL_EXPORTED_TARGETS microtel_nanopb_gen microtel_nanopb)
    endif()
endif()

# `microtel_sdk` exports as `microtel::sdk`, not `microtel::microtel_sdk`.
# The aggregate is already bare `microtel`, so the regex leaves it alone.
foreach(microtel_target IN LISTS MICROTEL_EXPORTED_TARGETS)
    string(REGEX REPLACE "^microtel_" "" microtel_export_name "${microtel_target}")
    set_target_properties(${microtel_target} PROPERTIES
        EXPORT_NAME "${microtel_export_name}")
endforeach()

install(TARGETS ${MICROTEL_EXPORTED_TARGETS}
    EXPORT MicrotelTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})

# The operator CLI (docs/troubleshooting.md). The binary ships; `microtel_preflight_lib`
# does not, and no target for either is exported — installing an executable
# needs no export entry.
install(TARGETS microtel-preflight
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

# Public headers, plus `internal/`.
#
# `include/microtel/internal/` is not public API — but public headers include
# it (provider.hpp reaches internal/processor.hpp, sdk_builder.hpp reaches
# internal/sampler.hpp, and so on), so an install that omitted it would ship a
# tree that does not compile. Shipping the files is not a promise about them:
# ICP 0020 Decision 2 draws the supported line at `include/microtel/*.hpp`.
install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/include/microtel
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    FILES_MATCHING PATTERN "*.hpp")

# The leaf's one public header, a C header beside the C++ ones.
if(TARGET microtel_leaf)
    install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/leaf/include/microtel/leaf.h
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/microtel)
endif()

# The vendored tl::expected, under a `vendor/` level of its own so that
# `#include "tl/expected.hpp"` from expected.hpp keeps resolving without the
# installed tree putting a bare `tl/` on a consumer's include path — where it
# would collide with their own copy. See the microtel_headers include
# directories at the top of this file.
install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/third_party/tl-expected/tl/expected.hpp
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/microtel/vendor/tl)

# License texts (issue #191). Until this rule, `cmake --install` produced
# archives, headers and a binary with no license of any kind — which Apache-2.0
# §4(a)/§4(d) asks for on microtel's own account, and which the vendored upb's
# BSD-3 binary-redistribution clause requires outright, since upb is compiled
# into the archives above.
#
# CMAKE_INSTALL_DATADIR, not DATAROOTDIR: GNUInstallDirs defines DATADIR as the
# read-only architecture-independent data root and defaults it to DATAROOTDIR,
# so the default path is the same `share/` either way — but DATADIR is the
# variable a distro packager retargets, and license text is data, not a
# packaging root. `licenses/microtel/` matches what Fedora, Debian and vcpkg
# already look for.
#
# THIRD_PARTY_NOTICES.md carries the full text of every vendored license, so
# these three files are the whole attribution set; the per-component
# third_party/*/LICENSE files need no separate install rule.
install(FILES
        ${CMAKE_CURRENT_SOURCE_DIR}/LICENSE
        ${CMAKE_CURRENT_SOURCE_DIR}/NOTICE
        ${CMAKE_CURRENT_SOURCE_DIR}/THIRD_PARTY_NOTICES.md
    DESTINATION ${CMAKE_INSTALL_DATADIR}/licenses/microtel)

# opentelemetry-cpp shim headers — ICP 0020 Decision 3: headers install, no
# binary and no exported target, permanently. Installed unconditionally
# (i.e. not gated on MICROTEL_BUILD_OTELCPP_SHIM) because the consumer opts in
# from *their* build, which means the headers have to be in the package they
# installed.
#
# They live under src/adapters/otelcpp/ and include each other as
# "adapters/otelcpp/…", so they need a root of their own that is not
# <includedir>: a consumer adds <includedir>/microtel-shim to their include
# path and the self-includes resolve unchanged.
install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/src/adapters/otelcpp/
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/microtel-shim/adapters/otelcpp
    FILES_MATCHING PATTERN "*.hpp")

install(EXPORT MicrotelTargets
    FILE MicrotelTargets.cmake
    NAMESPACE microtel::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/microtel)

# The package config pair that makes `find_package(microtel)` work.
#
# The installed filenames are `microtelConfig.cmake` /
# `microtelConfigVersion.cmake` — lowercase `microtel`, because find_package
# builds the filename from the package name it is given and the documented
# usage is `find_package(microtel …)`. `MicrotelConfig.cmake` would only be
# found by `find_package(Microtel)`.
#
# The version is PROJECT_VERSION above, and include/microtel/version.hpp
# agrees with it. Both are hand-set — generating the header from PROJECT_VERSION
# would make a public header a build artifact, which docs/releasing.md records as a
# rejected trade. A release bump edits this file, version.hpp, the grpc
# user-agent literal, tools/preflight/preflight.cpp, leaf/include/microtel/leaf.h
# and leaf/CMakeLists.txt; ci/scripts/version-drift-check.sh (CI job
# version-drift-check) fails any PR in which they stop agreeing.
include(CMakePackageConfigHelpers)

configure_package_config_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/cmake/microtelConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/microtelConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/microtel)

# Since 1.0.0, SameMajorVersion is the right mode: semver
# says a 1.x consumer requirement is satisfied by any later 1.y. ExactVersion
# was correct only below 1.0, where minor bumps are breaking by convention;
# keeping it past the 1.0 cut would fail `find_package(microtel 1.0)` against a
# 1.1 install for no reason.
write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/microtelConfigVersion.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion)

install(FILES
        ${CMAKE_CURRENT_BINARY_DIR}/microtelConfig.cmake
        ${CMAKE_CURRENT_BINARY_DIR}/microtelConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/microtel)

# Test tree — gtest-based unit/integration/conformance/wire/grpc-wire/fuzz
# per docs/coding-standards.md §11. Default ON at the top level and OFF as a subproject; OFF skips the whole tree for
# cross-compilation or constrained builds.
option(MICROTEL_BUILD_BENCH "Build the benchmark harness (requires Docker)" OFF)

if(MICROTEL_BUILD_BENCH)
    add_subdirectory(bench)
endif()

# Standalone, runnable demonstrations of the public API. Default OFF; the
# examples link microtel_sdk, so they require the libraries to be configured.
option(MICROTEL_BUILD_EXAMPLES "Build the standalone API examples" OFF)

if(MICROTEL_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

# MICROTEL_BUILD_TESTS is declared with the leaf options above.
if(MICROTEL_BUILD_TESTS)
    enable_testing()

    # GoogleTest via FetchContent — pinned to a stable tag — or, under
    # MICROTEL_USE_SYSTEM_DEPS, the installed package (GTestConfig.cmake
    # provides the same GTest::gtest and GTest::gtest_main targets).
    # Test-only; not part of the runtime closure (CLAUDE.md rule 12).
    if(MICROTEL_USE_SYSTEM_DEPS)
        find_package(GTest CONFIG REQUIRED)
    else()
        include(FetchContent)
        FetchContent_Declare(
            googletest
            GIT_REPOSITORY https://github.com/google/googletest.git
            GIT_TAG        v1.17.0
            GIT_SHALLOW    TRUE
        )
        # Don't install gtest as part of microtel's install target.
        set(INSTALL_GTEST OFF)
        FetchContent_MakeAvailable(googletest)
    endif()

    add_subdirectory(tests)
endif()

# Fuzz harnesses — libFuzzer, clang-only, independent of the gtest tree.
# cmake -S . -B build-fuzz -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
#       -DMICROTEL_BUILD_FUZZ=ON -DMICROTEL_BUILD_LEAF=ON -DMICROTEL_BUILD_TESTS=OFF
# MICROTEL_BUILD_FUZZ is declared with the leaf options above.
if(MICROTEL_BUILD_FUZZ)
    add_subdirectory(tests/fuzz)
endif()
