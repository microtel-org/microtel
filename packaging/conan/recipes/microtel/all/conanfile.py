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

        # One component per installed target, with the requires of its
        # INTERFACE_LINK_LIBRARIES in the exported MicrotelTargets.cmake.
        # Conan derives the link order from them (#473).
        headers = self.cpp_info.components["headers"]
        headers.set_property("cmake_target_name", "microtel::headers")
        # tl::expected is installed under include/microtel/vendor so that it
        # cannot clash with a consumer's own copy.
        headers.includedirs = ["include", os.path.join("include", "microtel", "vendor")]
        headers.libdirs = []

        libraries = {
            "api": ["headers"],
            "common": ["headers"],
            "config": ["headers"],
            "sdk": ["headers", "api", "config", "exporter", "transport", "http_wire", "grpc_wire",
                    "custom_wire", "encoder", "openssl::ssl", "libnghttp2::nghttp2"],
            "exporter": ["headers", "encoder"],
            "transport": ["headers", "common", "openssl::ssl", "openssl::crypto",
                          "libnghttp2::nghttp2"],
            "otlp_response": ["headers"],
            "gzip": ["headers", "zlib::zlib"],
            "http_wire": ["headers", "otlp_response", "gzip"],
            "grpc_wire": ["headers", "otlp_response", "gzip", "common"],
            "custom_wire": ["headers", "common"],
            "encoder": ["headers", "api", "upb_gen"],
            "upb_gen": ["upb_runtime"],
            "upb_runtime": ["utf8_range"],
            "utf8_range": [],
        }
        if self.options.leaf:
            libraries.update({
                "leaf": ["nanopb_gen"],
                "nanopb_gen": ["nanopb"],
                "nanopb": [],
            })
        for name, requires in libraries.items():
            component = self.cpp_info.components[name]
            component.set_property("cmake_target_name", f"microtel::{name}")
            component.libs = [f"microtel_{name}"]
            component.requires = requires
            component.includedirs = []
        self.cpp_info.components["common"].system_libs = ["pthread"]
        if self.options.leaf:
            self.cpp_info.components["leaf"].includedirs = ["include"]

        # The supported target.
        core = self.cpp_info.components["microtel"]
        core.set_property("cmake_target_name", "microtel::microtel")
        core.requires = ["sdk"]
        core.includedirs = []
        core.libdirs = []
