# vcpkg overlay port for microtel. See README.md beside this file.
#
# Not a curated-registry port: microtel does not yet meet the registry's
# six-month maturity rule, and it vendors upb, utf8_range and nanopb under
# renamed symbols (docs/packaging-research.md §3.3 and §4).

# microtel installs static archives only (every library is hard-coded STATIC),
# so a dynamic triplet still gets static archives.
vcpkg_check_linkage(ONLY_STATIC_LIBRARY)

# Source: the release tag matching the port's version. Update version and
# SHA512 together at each release (docs/releasing.md). `vcpkg install --head`
# builds the tip of master instead.
#
# MICROTEL_SOURCE_DIR in the environment builds a local checkout instead, for
# testing changes to microtel or to this port before they are released. vcpkg's
# binary cache keys on the port files, not on that directory, so pass
# --binarysource=clear when using it, or a stale cached build is reused.
if(DEFINED ENV{MICROTEL_SOURCE_DIR})
    set(SOURCE_PATH "$ENV{MICROTEL_SOURCE_DIR}")
    message(STATUS "microtel: building the local checkout ${SOURCE_PATH}")
else()
    vcpkg_from_github(
        OUT_SOURCE_PATH SOURCE_PATH
        REPO microtel-org/microtel
        REF "v${VERSION}"
        SHA512 76b47f31364f279bf18b88de9e90f098c84179477f90191e7a160705021ee8b96ad65125dd7f7a9b7a01b157b6f974a6592300716a14c6309abe44fbb9ff3f35
        HEAD_REF master
    )
endif()

vcpkg_check_features(OUT_FEATURE_OPTIONS FEATURE_OPTIONS
    FEATURES
        leaf          MICROTEL_BUILD_LEAF
        concentrator  MICROTEL_WITH_CONCENTRATOR
        spdlog        MICROTEL_USE_SPDLOG
)

# nghttp2 has no CMake package; microtel finds it, and its installed config
# finds it again, through pkg-config.
vcpkg_find_acquire_program(PKGCONFIG)

# MICROTEL_USE_SYSTEM_DEPS: toml++ and spdlog come from vcpkg through
# find_package, nothing is downloaded at configure time. toml++ is compiled
# header-only into libmicrotel_config.a even though vcpkg's toml++ is a
# library, so the installed package does not depend on it.
# MICROTEL_WARNINGS_AS_ERRORS=OFF: a package build must not fail on a warning
# that a newer compiler introduces.
# The leaf's encoder backend is pinned to nanopb, its default; vcpkg features
# must be additive, and the backends are mutually exclusive.
vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS
        ${FEATURE_OPTIONS}
        -DMICROTEL_USE_SYSTEM_DEPS=ON
        -DMICROTEL_WARNINGS_AS_ERRORS=OFF
        -DMICROTEL_BUILD_TESTS=OFF
        -DMICROTEL_BUILD_EXAMPLES=OFF
        -DMICROTEL_BUILD_BENCH=OFF
        -DMICROTEL_BUILD_FUZZ=OFF
        -DMICROTEL_BUILD_HEADER_CHECK=OFF
        -DMICROTEL_BUILD_OTELCPP_SHIM=OFF
        -DMICROTEL_LEAF_ENCODER=nanopb
        "-DPKG_CONFIG_EXECUTABLE=${PKGCONFIG}"
    MAYBE_UNUSED_VARIABLES
        MICROTEL_LEAF_ENCODER
)

vcpkg_cmake_install()
vcpkg_cmake_config_fixup(CONFIG_PATH lib/cmake/microtel)
vcpkg_copy_tools(TOOL_NAMES microtel-preflight AUTO_CLEAN)

# Licenses are installed by vcpkg_install_copyright below, into
# share/microtel/copyright.
file(REMOVE_RECURSE
    "${CURRENT_PACKAGES_DIR}/debug/include"
    "${CURRENT_PACKAGES_DIR}/debug/share"
    "${CURRENT_PACKAGES_DIR}/share/licenses")

file(INSTALL "${CMAKE_CURRENT_LIST_DIR}/usage"
     DESTINATION "${CURRENT_PACKAGES_DIR}/share/${PORT}")
vcpkg_install_copyright(FILE_LIST
    "${SOURCE_PATH}/LICENSE"
    "${SOURCE_PATH}/NOTICE"
    "${SOURCE_PATH}/THIRD_PARTY_NOTICES.md")
