import os

from conan import ConanFile
from conan.errors import ConanInvalidConfiguration
from conan.tools.build import check_min_cppstd
from conan.tools.cmake import CMake, CMakeDeps, CMakeToolchain, cmake_layout
from conan.tools.files import copy, get, rmdir
from conan.tools.gnu import PkgConfigDeps
from conan.tools.scm import Version

required_conan_version = ">=2.1"


class MicrotelConan(ConanFile):
    name = "microtel"
    description = ("Lightweight OpenTelemetry runtime and OTLP exporter for traces, metrics and "
                   "logs (OTLP/HTTP and OTLP/gRPC) built on nghttp2, without the gRPC or "
                   "protobuf C++ libraries")
    # microtel, then vendored upb (BSD-3-Clause), utf8_range (MIT) and
    # tl::expected (CC0-1.0). The leaf option adds nanopb (Zlib).
    license = ("Apache-2.0", "BSD-3-Clause", "MIT", "CC0-1.0")
    url = "https://github.com/conan-io/conan-center-index"
    homepage = "https://github.com/microtel-org/microtel"
    topics = ("opentelemetry", "otlp", "tracing", "grpc", "nghttp2")
    package_type = "static-library"
    settings = "os", "arch", "compiler", "build_type"
    options = {
        "fPIC": [True, False],
        "leaf": [True, False],
        "concentrator": [True, False],
    }
    default_options = {
        "fPIC": True,
        "leaf": False,
        "concentrator": False,
    }

    @property
    def _min_cppstd(self):
        return 20

    @property
    def _compilers_minimum_version(self):
        # std::format: libstdc++ 13; Clang 18 is the oldest upstream CI builds.
        return {"gcc": "13", "clang": "18"}

    def layout(self):
        cmake_layout(self, src_folder="src")

    def requirements(self):
        self.requires("openssl/[>=3 <4]")
        self.requires("libnghttp2/1.68.1")
        self.requires("zlib/[>=1.2.11 <2]")
        # Compiled header-only into libmicrotel_config.a; consumers never see it.
        self.requires("tomlplusplus/3.4.0", visible=False)

    def build_requirements(self):
        self.tool_requires("cmake/[>=3.20 <5]")
        # nghttp2 is found through pkg-config (PkgConfigDeps writes libnghttp2.pc).
        self.tool_requires("pkgconf/[>=2.1 <3]")

    def validate(self):
        if self.settings.os != "Linux":
            raise ConanInvalidConfiguration(f"{self.ref} supports Linux only (its I/O thread uses epoll)")
        check_min_cppstd(self, self._min_cppstd)
        minimum_version = self._compilers_minimum_version.get(str(self.settings.compiler))
        if minimum_version is None or Version(self.settings.compiler.version) < minimum_version:
            raise ConanInvalidConfiguration(
                f"{self.ref} requires C++{self._min_cppstd} with std::format: GCC 13 or Clang 18 or newer")

    def source(self):
        get(self, **self.conan_data["sources"][self.version], strip_root=True)

    def generate(self):
        tc = CMakeToolchain(self)
        # find_package for toml++ instead of FetchContent: nothing is downloaded.
        tc.cache_variables["MICROTEL_USE_SYSTEM_DEPS"] = True
        tc.cache_variables["MICROTEL_WARNINGS_AS_ERRORS"] = False
        tc.cache_variables["MICROTEL_USE_SPDLOG"] = False
        tc.cache_variables["MICROTEL_BUILD_TESTS"] = False
        tc.cache_variables["MICROTEL_BUILD_EXAMPLES"] = False
        tc.cache_variables["MICROTEL_BUILD_BENCH"] = False
        tc.cache_variables["MICROTEL_BUILD_FUZZ"] = False
        tc.cache_variables["MICROTEL_BUILD_HEADER_CHECK"] = False
        tc.cache_variables["MICROTEL_BUILD_OTELCPP_SHIM"] = False
        tc.cache_variables["MICROTEL_BUILD_LEAF"] = bool(self.options.leaf)
        tc.cache_variables["MICROTEL_LEAF_ENCODER"] = "nanopb"
        tc.cache_variables["MICROTEL_WITH_CONCENTRATOR"] = bool(self.options.concentrator)
        tc.generate()
        CMakeDeps(self).generate()
        PkgConfigDeps(self).generate()

    def build(self):
        cmake = CMake(self)
        cmake.configure()
        cmake.build()

    def package(self):
        for license_file in ("LICENSE", "NOTICE", "THIRD_PARTY_NOTICES.md"):
            copy(self, license_file, self.source_folder, os.path.join(self.package_folder, "licenses"))
        cmake = CMake(self)
        cmake.install()
        rmdir(self, os.path.join(self.package_folder, "lib", "cmake"))
        rmdir(self, os.path.join(self.package_folder, "share"))

    def package_info(self):
        self.cpp_info.set_property("cmake_file_name", "microtel")

        # The supported target. The archives are listed in link order: each
        # one before the archives it depends on.
        core = self.cpp_info.components["core"]
        core.set_property("cmake_target_name", "microtel::microtel")
        # microtel_api comes after microtel_encoder: the encoder calls
        # TraceState (#473).
        core.libs = [
            "microtel_sdk", "microtel_config", "microtel_exporter",
            "microtel_transport", "microtel_http_wire", "microtel_grpc_wire",
            "microtel_otlp_response", "microtel_gzip", "microtel_custom_wire",
            "microtel_common", "microtel_encoder", "microtel_api", "microtel_upb_gen",
            "microtel_upb_runtime", "microtel_utf8_range",
        ]
        # tl::expected is installed under include/microtel/vendor so that it
        # cannot clash with a consumer's own copy.
        core.includedirs = ["include", os.path.join("include", "microtel", "vendor")]
        core.requires = ["openssl::ssl", "openssl::crypto", "libnghttp2::nghttp2", "zlib::zlib"]
        core.system_libs = ["pthread"]

        if self.options.leaf:
            leaf = self.cpp_info.components["leaf"]
            leaf.set_property("cmake_target_name", "microtel::leaf")
            leaf.libs = ["microtel_leaf", "microtel_nanopb_gen", "microtel_nanopb"]
