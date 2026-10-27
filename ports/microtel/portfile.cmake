vcpkg_check_linkage(ONLY_STATIC_LIBRARY)

vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO microtel-org/microtel
    REF "v${VERSION}"
    SHA512 8a133cf10eef52e09990caef6fc83e877cd38e8d7a944f8748382abd3784fa4650b172239fe4c91ab622fc2635c82c47f375bac0ca501f54018294dd70a5ceaa
    HEAD_REF master
)

vcpkg_check_features(OUT_FEATURE_OPTIONS FEATURE_OPTIONS
    FEATURES
        leaf          MICROTEL_BUILD_LEAF
        concentrator  MICROTEL_WITH_CONCENTRATOR
        spdlog        MICROTEL_USE_SPDLOG
)

# nghttp2 is found through pkg-config, both here and by the installed config.
vcpkg_find_acquire_program(PKGCONFIG)

# upb, utf8_range and nanopb stay vendored: every global symbol is renamed
# microtel_*, and vcpkg's protobuf/utf8-range ports would add abseil.
# The leaf backend is pinned to nanopb (the default): backends are exclusive.
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

file(REMOVE_RECURSE
    "${CURRENT_PACKAGES_DIR}/debug/include"
    "${CURRENT_PACKAGES_DIR}/debug/share"
    "${CURRENT_PACKAGES_DIR}/share/licenses"
)

file(INSTALL "${CMAKE_CURRENT_LIST_DIR}/usage" DESTINATION "${CURRENT_PACKAGES_DIR}/share/${PORT}")
vcpkg_install_copyright(FILE_LIST
    "${SOURCE_PATH}/LICENSE"
    "${SOURCE_PATH}/NOTICE"
    "${SOURCE_PATH}/THIRD_PARTY_NOTICES.md"
)
