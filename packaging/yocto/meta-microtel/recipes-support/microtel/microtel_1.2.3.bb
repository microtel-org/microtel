SUMMARY = "Lightweight OpenTelemetry trace runtime and OTLP exporter"
DESCRIPTION = "microtel is a C++20 OpenTelemetry-compatible runtime with an \
OTLP/HTTP and OTLP/gRPC exporter built on nghttp2 and a vendored upb, without \
the gRPC or protobuf C++ libraries. Optional: the C11 'leaf' library for \
microcontrollers and the concentrator that ingests leaf payloads."
HOMEPAGE = "https://github.com/microtel-org/microtel"
BUGTRACKER = "https://github.com/microtel-org/microtel/issues"
SECTION = "libs"

# microtel itself is Apache-2.0. Vendored and absorbed into the static
# archives: upb (BSD-3-Clause), utf8_range (MIT), tl-expected (CC0-1.0),
# toml++ (MIT, header-only, compiled into libmicrotel_config.a). The nanopb
# leaf backend (Zlib) is added by PACKAGECONFIG[leaf].
LICENSE = "Apache-2.0 & BSD-3-Clause & MIT & CC0-1.0"
LIC_FILES_CHKSUM = "file://LICENSE;md5=86d3f3a95c324c9479bd8986968f4327"

# Release tarball, plus toml++ for the config parser. microtel's CMake would
# FetchContent toml++ from GitHub at configure time; BitBake forbids network
# access outside do_fetch, so it is fetched here and handed to FetchContent
# through FETCHCONTENT_SOURCE_DIR_TOMLPLUSPLUS with FetchContent fully
# disconnected. upb, utf8_range and nanopb are vendored in the tarball.
SRC_URI = " \
    https://github.com/microtel-org/microtel/archive/refs/tags/v${PV}.tar.gz;downloadfilename=${BP}.tar.gz;name=microtel \
    https://github.com/marzer/tomlplusplus/archive/refs/tags/v${TOMLPP_PV}.tar.gz;downloadfilename=tomlplusplus-${TOMLPP_PV}.tar.gz;name=tomlplusplus;subdir=${BP}/_deps \
"
TOMLPP_PV = "3.4.0"
SRC_URI[microtel.sha256sum] = "918aef3b5549107a43f331ac1d1c551e0eca8204402707b7401ed11c06f18b75"
SRC_URI[tomlplusplus.sha256sum] = "8517f65938a4faae9ccf8ebb36631a38c1cadfb5efa85d9a72e15b9e97d25155"


inherit cmake pkgconfig

DEPENDS = "nghttp2 openssl zlib"

# leaf          - the C11 leaf library (microtel::leaf, nanopb backend)
# concentrator  - the leaf receiver in the C++ runtime (parses untrusted bytes;
#                 experimental, off by default upstream)
# preflight     - the microtel-preflight operator CLI in ${PN}
PACKAGECONFIG ??= "preflight"
PACKAGECONFIG[leaf] = "-DMICROTEL_BUILD_LEAF=ON -DMICROTEL_LEAF_ENCODER=nanopb,-DMICROTEL_BUILD_LEAF=OFF"
PACKAGECONFIG[concentrator] = "-DMICROTEL_WITH_CONCENTRATOR=ON,-DMICROTEL_WITH_CONCENTRATOR=OFF"
PACKAGECONFIG[preflight] = ",,"
LICENSE:append = "${@bb.utils.contains('PACKAGECONFIG', 'leaf', ' & Zlib', '', d)}"

EXTRA_OECMAKE = " \
    -DMICROTEL_BUILD_TESTS=OFF \
    -DMICROTEL_BUILD_EXAMPLES=OFF \
    -DMICROTEL_BUILD_BENCH=OFF \
    -DMICROTEL_BUILD_FUZZ=OFF \
    -DMICROTEL_BUILD_HEADER_CHECK=OFF \
    -DMICROTEL_BUILD_OTELCPP_SHIM=OFF \
    -DMICROTEL_USE_SPDLOG=OFF \
    -DMICROTEL_WARNINGS_AS_ERRORS=OFF \
    -DFETCHCONTENT_FULLY_DISCONNECTED=ON \
    -DFETCHCONTENT_SOURCE_DIR_TOMLPLUSPLUS=${S}/_deps/tomlplusplus-${TOMLPP_PV} \
"

do_install:append() {
    if ${@bb.utils.contains('PACKAGECONFIG', 'preflight', 'false', 'true', d)}; then
        rm -f ${D}${bindir}/microtel-preflight
        rmdir --ignore-fail-on-non-empty ${D}${bindir}
    fi
}

# microtel installs static archives only (every library is STATIC), so the
# runtime package holds just the optional CLI, and anyone linking needs the
# -staticdev package next to -dev.
ALLOW_EMPTY:${PN} = "1"
RDEPENDS:${PN}-dev += "${PN}-staticdev"
FILES:${PN}-dev += "${libdir}/cmake/microtel"
FILES:${PN}-doc += "${datadir}/licenses ${datadir}/doc"

BBCLASSEXTEND = "native nativesdk"
