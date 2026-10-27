# Packaging research: vcpkg port and Conan 2 recipe

**Status:** research spike, 2026-10-01, against tag `v1.2.0`. Nothing was
submitted upstream. No build, library or ICP file was changed. Every change
listed below is a recommendation.

**Update:** an overlay port is now available at
[`packaging/vcpkg/ports/microtel/`](../packaging/vcpkg/ports/microtel/README.md),
which answers §7 question 6 for vcpkg. It builds with `MICROTEL_USE_SYSTEM_DEPS`
(§5 item 2, which compiles toml++ header-only and so settles §5 item 1 without
a new dependency), `MICROTEL_WARNINGS_AS_ERRORS=OFF` (§5 item 3), and the
`OpenSSL::Crypto` link (§5 item 4). It offers `leaf` and `concentrator` as
opt-in features, and builds the `v1.2.4` release tarball. The curated-registry
blockers in §3.3 still apply. The port in curated-registry form is
[`ports/microtel/`](../ports/microtel/), and a Conan 2 recipe in
conan-center-index layout is under
[`packaging/conan/`](../packaging/conan/README.md). Their submission steps are
[`packaging/PR_VCPKG.md`](packaging/PR_VCPKG.md) and
[`packaging/PR_CONAN.md`](packaging/PR_CONAN.md).

## 1. Summary and recommendation

Both package managers can carry microtel. Each needs one small upstream fix
before a submission is worth making. Both throwaway packages described in §6
built `v1.2.0` offline. Both passed `ci/scripts/symbol-scan.sh` and ran
`tests/consumer` to `PASS`. The vcpkg consumer needed one manual fix before
it linked (§3.2).

**Recommendation: do the upstream fixes in §5 first (about 2 days), then
vcpkg, then Conan.**

- **vcpkg first.** Its Linux CI runs Ubuntu 24.04 with GCC 13, the same
  compiler microtel tests with. Its default `x64-linux` triplet is static,
  which matches microtel's static-only archives. It already has
  `tl-expected` 1.3.1 and `nanopb` 0.4.9.2, the exact versions vendored here.
  The costs are a strict rule against vendored code and a maturity gate (§3.3).
- **Conan second.** ConanCenter states an exception for vendored code
  "if upstream takes care to rename symbols", which fits microtel. However,
  its only Linux build profile is GCC 11. microtel uses `std::format` in
  `src/sdk/` (`sampler_factories.cpp`, `resource_builder.cpp`,
  `sampler_chains.cpp`), which libstdc++ added in GCC 13. So ConanCenter
  would publish no prebuilt Linux binary, and every user would build from
  source with `--build=missing`.

| | vcpkg | Conan 2 / ConanCenter |
|---|---|---|
| Upstream changes needed | §5 items 1–3 (required), 4–5 (advisable) | §5 items 2–4 (required), 1 (only if toml++ is linked as a library) |
| Port/recipe work | 0.5 day (sketch already builds) | 1 day (component model, test_package) |
| Review rounds | 1–2 days of effort, spread over weeks | 1–2 days of effort, spread over weeks |
| **Total effort** | **~3–4 days**, plus the shared §5 work | **~3 days**, plus the shared §5 work |
| Hard blockers | maturity gate (§3.3); vendoring policy argument (§4) | none found; GCC 11 CI means no prebuilt binaries |

## 2. What the package has to contain

Ground truth comes from the top-level `CMakeLists.txt`, `cmake/microtelConfig.cmake.in`,
[ICP 0020](icps/0020-install-and-package-config.md) and
[`THIRD_PARTY_NOTICES.md`](../THIRD_PARTY_NOTICES.md).

- **Artifacts.** Fifteen static archives (`libmicrotel_*.a`, all hard-coded
  `STATIC`), the `microtel-preflight` binary, `include/microtel/**` (with
  `internal/`), the vendored `tl/expected.hpp` under
  `include/microtel/vendor/`, the otel-cpp shim headers under
  `include/microtel-shim/` (source-only, ICP 0014), and
  `lib/cmake/microtel/{microtelConfig,microtelConfigVersion,MicrotelTargets}.cmake`.
  Licenses install to `share/licenses/microtel/`.
- **Supported target.** Only `microtel::microtel` is supported (ICP 0020
  Decision 2). Version compatibility is `SameMajorVersion`.
- **Link dependencies.** OpenSSL (`OpenSSL::SSL`), zlib (`ZLIB::ZLIB`), and
  nghttp2 through **pkg-config** (`PkgConfig::NGHTTP2`). That last one is
  baked into the exported link interface, so the installed config re-runs
  `pkg_check_modules`.
- **Configure-time FetchContent.**
  - toml++ v3.4.0 is always fetched. It is header-only and absorbed into
    `libmicrotel_config.a` behind `$<BUILD_INTERFACE:>`.
  - spdlog v1.15.3 is fetched when `MICROTEL_USE_SPDLOG=ON` (the default).
    It feeds only the bridge adapter and its tests. No installed archive
    references spdlog (issue #190).
  - GoogleTest v1.17.0 is fetched when `MICROTEL_BUILD_TESTS=ON` (the default).
  - The otel-cpp API is fetched when `MICROTEL_BUILD_OTELCPP_SHIM=ON`.
- **Vendored code.** upb and utf8_range (protobuf v29.4) are compiled in,
  and every global symbol is renamed `microtel_*` (ICP 0020 Decision 4). The
  `tl::expected` 1.3.1 header is installed. nanopb 0.4.9.2 goes into the leaf
  only, renamed `microtel_pb_*` (ICP 0031).
- **Generated code.** `gen/` (upb accessors) and `gen/nanopb/` are
  committed. **A package build does not need protoc.** Neither experiment
  ran it. `ci/scripts/regen-protos.sh` is a CI-only check.
- **Leaf.** `MICROTEL_BUILD_LEAF=ON` adds `libmicrotel_leaf.a`,
  `libmicrotel_nanopb{,_gen}.a` and `microtel/leaf.h`. It built offline
  against the vcpkg toolchain (§6).

## 3. vcpkg

### 3.1 Sketch of the port

This is the overlay port that built here. It assumes §5 items 1–3 have landed.

```json
{
  "name": "microtel",
  "version": "1.2.0",
  "description": "Lightweight OpenTelemetry-compatible trace runtime and OTLP exporter built on nghttp2",
  "homepage": "https://github.com/microtel-org/microtel",
  "license": "Apache-2.0 AND BSD-3-Clause AND MIT AND CC0-1.0",
  "supports": "linux",
  "dependencies": [
    "nghttp2", "openssl", "tomlplusplus", "zlib",
    { "name": "pkgconf", "host": true },
    { "name": "vcpkg-cmake", "host": true },
    { "name": "vcpkg-cmake-config", "host": true }
  ],
  "features": {
    "leaf": { "description": "Experimental leaf C library (adds Zlib-licensed nanopb)" }
  }
}
```

```cmake
vcpkg_check_linkage(ONLY_STATIC_LIBRARY)
vcpkg_from_github(OUT_SOURCE_PATH SOURCE_PATH REPO microtel-org/microtel
    REF "v${VERSION}" SHA512 a21085ee…ca679 HEAD_REF master)
vcpkg_find_acquire_program(PKGCONFIG)
vcpkg_check_features(OUT_FEATURE_OPTIONS FEATURE_OPTIONS FEATURES leaf MICROTEL_BUILD_LEAF)
vcpkg_cmake_configure(SOURCE_PATH "${SOURCE_PATH}" OPTIONS ${FEATURE_OPTIONS}
    -DMICROTEL_BUILD_TESTS=OFF -DMICROTEL_BUILD_HEADER_CHECK=OFF
    -DMICROTEL_USE_SPDLOG=OFF -DMICROTEL_WERROR=OFF            # §5 item 3
    -DFETCHCONTENT_TRY_FIND_PACKAGE_MODE=ALWAYS
    "-DPKG_CONFIG_EXECUTABLE=${PKGCONFIG}")
vcpkg_cmake_install()
vcpkg_cmake_config_fixup(CONFIG_PATH lib/cmake/microtel)
vcpkg_copy_tools(TOOL_NAMES microtel-preflight AUTO_CLEAN)
file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include" "${CURRENT_PACKAGES_DIR}/debug/share"
                    "${CURRENT_PACKAGES_DIR}/share/licenses")
vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE" "${SOURCE_PATH}/NOTICE"
                                  "${SOURCE_PATH}/THIRD_PARTY_NOTICES.md")
```

Notes on the sketch:

- **No shared build.** `ONLY_STATIC_LIBRARY` makes the
  `x64-linux-dynamic` community triplet build static archives anyway.
- **spdlog left out.** `MICROTEL_USE_SPDLOG=OFF` loses nothing in an
  installed package, because the bridge header `adapters/spdlog_sink.hpp`
  installs regardless.
- **The shim needs no dependency.** Its headers are installed, and the
  consumer supplies the otel-cpp API.

### 3.2 Findings from the local build

1. **Already offline.** vcpkg forces `-DFETCHCONTENT_FULLY_DISCONNECTED=ON`
   into every CMake port ([vcpkg#28386](https://github.com/microsoft/vcpkg/issues/28386)).
   It showed up in the configure line here. `FETCHCONTENT_TRY_FIND_PACKAGE_MODE=ALWAYS`
   then turns the toml++ `FetchContent_Declare` into `find_package(tomlplusplus)`
   **without any upstream change**. That needs CMake 3.24 or later, and vcpkg
   ships 4.x.
2. **Blocker: vcpkg's toml++ is a compiled library.** The port builds it with
   meson, and its config adds `-DTOML_HEADER_ONLY=0` (see
   `ports/tomlplusplus/portfile.cmake`). So `libmicrotel_config.a` gets four
   `U toml::v3::…` references. But `$<BUILD_INTERFACE:>` strips toml++ from
   the export: `MicrotelTargets.cmake` carries a literal empty
   `$<LINK_ONLY:>`, and `microtelConfig.cmake` has no
   `find_dependency(tomlplusplus)`. As a result `tests/consumer` **failed to
   link** (`undefined reference to toml::v3::ex::parse_file…`). The consumer
   cannot fix this: adding `tomlplusplus::tomlplusplus` itself puts the
   archive before `libmicrotel_config.a` on the link line, and it still
   fails. With both lines hand-patched into the installed config files, the
   consumer linked and printed `PASS`. This is §5 item 1.
3. **nghttp2 works through pkg-config.** It resolved to vcpkg's 1.70.0, not
   the host's 1.68.0, both at build time and in the consumer.
4. **Clean scans and install tree.** `symbol-scan.sh --prefix
   <vcpkg package dir>` was clean on all four passes. Post-build validation
   raised no warnings. Nothing collides with another port's files: the
   vendored header lives at `include/microtel/vendor/tl/expected.hpp`, while
   the `tl-expected` port installs `include/tl/expected.hpp`.

### 3.3 Blockers (from the [Maintainer Guide](https://learn.microsoft.com/en-us/vcpkg/contributing/maintainer-guide))

- **"Do not use vendored dependencies."** This is the main review risk.
  §4 argues that upb, utf8_range and nanopb should stay vendored. vcpkg's
  own rule against ports that "install symbols and definitions owned by
  another package" actually supports keeping the *renamed* copies.
  `tl::expected` is different: it is the same version (1.3.1) as the
  `tl-expected` port, keeps the `tl::` namespace, and reviewers will
  reasonably ask for the port. That is §5 item 5.
- **Maturity.** The guide requires "a release that is at least six months
  old" or "at least six months of active public development". The first
  commit is 2026-05-04 and `v1.0.0` is 2026-09-13. The gate opens around
  **2026-11-04** if the repository has been public since May. Otherwise it
  opens 2027-03-13.
- **Port name.** The guide asks that a search for the name lead to the
  project. "Microtel" is also a hotel brand, so reviewers may ask for
  `microtel-org-microtel`.
- **Hard-coded `-Werror`.** It is set in 13 library and tool `CMakeLists.txt` files under `src/`, `leaf/` and `tools/`. The
  host GCC 15 already gives false positives on the metric `std::variant`
  files. A port must build with whatever compiler vcpkg CI uses, both now
  and after its next image update. This is §5 item 3.

## 4. Vendored, renamed code: keep it vendored

Both registries have a "use the packaged dependency" rule. For upb, utf8_range
and nanopb, the rename is not an obstacle to that rule; it is the reason
for keeping the copies:

- **Abseil.** vcpkg no longer has a standalone `upb` port; upb is built
  inside `protobuf`. Both `protobuf` and `utf8-range` declare `abseil` as a
  dependency (`ports/protobuf/vcpkg.json`, `ports/utf8-range/vcpkg.json`).
  Using them would add abseil to microtel's dependency graph, which is the
  thing CLAUDE.md rule 13 forbids. ConanCenter has no `upb` or `utf8_range`
  recipe at all.
- **Version lock.** `gen/` is generated by protoc-gen-upb v29.4 for the
  v29.4 runtime. The registries ship protobuf 6.33.x (vcpkg) and
  5.29.6 / 6.33.5 / 7.35.0 (Conan). upb gives no compatibility promise
  between generated code and a different runtime. So unvendoring would also
  mean regenerating `gen/` at port build time, which adds protoc and the
  upb plugins as host tools. Today a port needs neither.
- **The rename prevents conflicts.** Renamed symbols cannot collide with a
  consumer's own upb or nanopb, which is exactly the clash both policies
  exist to prevent.
  [ConanCenter says](https://github.com/conan-io/conan-center-index/blob/master/docs/adding_packages/dependencies.md#handling-internal-dependencies)
  that vendored code "should be removed (in a best effort basis) to avoid
  potential ODR violations. If upstream takes care to rename symbols, it
  may be acceptable."
- **nanopb.** Swapping in the vcpkg port, even at the same 0.4.9.2, would
  drop the `microtel_pb_*` rename that ICP 0031 Decision 4 requires for
  firmware that links its own nanopb.

Suggested wording for both PRs: *"upb/utf8_range/nanopb are vendored at
pinned versions with every global symbol prefixed `microtel_`, enforced by
CI (`ci/scripts/symbol-scan.sh`). The packaged alternatives pull abseil or
drop the prefix."*

## 5. Upstream changes this spike identifies (not made)

These are listed in priority order. Items 1 and 5 touch ICP 0020's package
config surface, and item 1 arguably touches rule 12's dependency closure,
so they deserve a short ICP.

1. **toml++ found as a compiled library.** When toml++ comes from
   `find_package` and is not header-only, add it to `microtel_config`'s
   install interface as `$<LINK_ONLY:tomlplusplus::tomlplusplus>` and emit
   `find_dependency(tomlplusplus)` in `microtelConfig.cmake.in`. Gate both
   on how it was found. Note this makes libtomlplusplus a link-time
   dependency of the installed package.

   *Alternative that keeps rule 12 untouched:* compile `microtel_config`
   with `TOML_HEADER_ONLY=1` against only the include directories of the
   found package. toml++ would then be absorbed as it is today. Whether that
   is safe next to a consumer who also links `libtomlplusplus.a` (ODR) has
   **not** been verified. The maintainer should choose (§7).
2. **FetchContent with a package fallback.** Add `FIND_PACKAGE_ARGS` to the
   toml++ and spdlog `FetchContent_Declare` calls (CMake 3.24 or later; the
   current minimum is 3.20). Alternatively, add a
   `MICROTEL_USE_SYSTEM_DEPS` option that calls `find_package` first. Today
   the port relies on the caller setting
   `FETCHCONTENT_TRY_FIND_PACKAGE_MODE=ALWAYS`, which does nothing below
   CMake 3.24. Conan's ConanCenter CI assumes 3.15 unless the recipe adds a
   `tool_requires`. GoogleTest needs no change, because packages build with
   `MICROTEL_BUILD_TESTS=OFF`.
3. **`MICROTEL_WERROR` option** (default ON). It would wrap the 13
   `-Werror` uses so packagers can turn it off. An alternative is to honour
   CMake's own `CMAKE_COMPILE_WARNING_AS_ERROR`.
4. **Link `OpenSSL::Crypto` explicitly in `src/transport/CMakeLists.txt`.**
   `libmicrotel_transport.a` has 37 undefined OpenSSL references,
   including libcrypto ones such as `BIO_meth_free`. It links only
   `OpenSSL::SSL`. That works with CMake's `FindOpenSSL` module, where SSL
   chains to Crypto. It fails with OpenSSL's own `OpenSSLConfig.cmake`
   (Fedora ships `/usr/lib64/cmake/OpenSSL`), which Conan's toolchain picks
   because it sets `CMAKE_FIND_PACKAGE_PREFER_CONFIG=ON`. The Conan build of
   `microtel-preflight` failed with `DSO missing from command line` until
   the recipe patched this line.
5. **Optional system `tl-expected`.** Add
   `find_package(tl-expected)` → `tl::expected` on `microtel_headers`,
   instead of the `microtel/vendor` include path. vcpkg has 1.3.1.
   ConanCenter has only up to 1.2.0, so the Conan recipe would keep the
   vendored copy or bump the recipe first.

Not needed:

- **nghttp2.** pkg-config worked in both managers. Conan's `PkgConfigDeps`
  generates `libnghttp2.pc`, and vcpkg's port keeps its `.pc` file.
- **Generated code.** No change, because a package build never runs protoc.
- **Licenses.** Already installed (issue #191).
- **Version tags.** `v1.2.0` tags map directly to
  `REF "v${VERSION}"` and to the `conandata.yml` URL.

## 6. What was run locally and what came from documentation

**Run locally.** Everything was done on Fedora 44 with clang 22, under the
session scratchpad, and against the GitHub `v1.2.0` tarball.

- **vcpkg** (master tarball, tool 2026-09-26): `vcpkg install microtel
  --overlay-ports=overlay`.
  - The first run failed in vcpkg's own OpenSSL build, because the host perl
    lacks `Time::Piece`. That is unrelated to microtel. An empty overlay
    `openssl` port then used the system OpenSSL 3.5.8.
  - microtel itself built in 39 s.
  - `tests/consumer` built against the toolchain **failed to link**
    (§3.2 item 2). After hand-patching the two lines into the installed
    config files, it linked and printed `PASS`.
  - A separate offline configure of the same source with
    `MICROTEL_BUILD_LEAF=ON` built and installed the leaf and nanopb archives.
- **Conan 2.33.0** (pip venv): `conan create` of a recipe that uses
  `CMakeToolchain`, `CMakeDeps` and `PkgConfigDeps`. Dependencies were
  `libnghttp2/1.68.1`, `zlib`, `tomlplusplus/3.4.0` (header-library) and
  `tool_requires` `cmake`/`pkgconf`. OpenSSL came via `[platform_requires]`.
  - The package built offline (`FETCHCONTENT_FULLY_DISCONNECTED=ON`).
    `microtel-preflight` needed the §5 item 4 patch.
  - `test_package` (a copy of `tests/consumer/main.cpp`) printed `PASS`.
  - Conan's header-only toml++ needs no §5 item 1 change.
  - Two lines in the recipe exist only because of `platform_requires` and
    would be removed for a real recipe: `system_libs = ["ssl", "crypto"]`
    and the OpenSSL config workaround.
- **symbol-scan.** Clean on both install trees.

**From documentation and registry snapshots** (fetched 2026-10-01):
[vcpkg Maintainer Guide](https://learn.microsoft.com/en-us/vcpkg/contributing/maintainer-guide),
[vcpkg#28386](https://github.com/microsoft/vcpkg/issues/28386),
[ConanCenter dependencies](https://github.com/conan-io/conan-center-index/blob/master/docs/adding_packages/dependencies.md),
[supported platforms](https://github.com/conan-io/conan-center-index/blob/master/docs/supported_platforms_and_configurations.md),
[test_package rules](https://github.com/conan-io/conan-center-index/blob/master/docs/adding_packages/folders_and_files.md),
[ConanInvalidConfiguration FAQ](https://github.com/conan-io/conan-center-index/blob/master/docs/faqs.md).
vcpkg's Linux CI image is `ubuntu:noble` (`scripts/azure-pipelines/linux/Dockerfile`).
GCC 11 being unable to build microtel is inferred from the `std::format`
use, **not built**.

Registry availability:

| Dependency | vcpkg | ConanCenter |
|---|---|---|
| nghttp2 | `nghttp2` 1.70.0 | `libnghttp2` 1.66.0, 1.68.1 |
| OpenSSL | 3.6.5 | 3.6.5, 4.0.3, … |
| zlib | 1.3.2 | 1.3.2 |
| spdlog | 1.17.0 | 1.13–1.17.0 |
| toml++ | `tomlplusplus` 3.4.0 (**compiled**, meson) | 3.4.0 (header-library) |
| tl-expected | 1.3.1 | up to 1.2.0 |
| nanopb | 0.4.9.2 | not present |
| upb / utf8_range | inside `protobuf` / `utf8-range` (both need abseil) | not present |

## 7. Open questions for the maintainer

1. **toml++ under vcpkg.** Should it become a declared link dependency of
   the installed package (§5 item 1, which needs an ICP against rule 12 and
   ICP 0020)? Or should `TOML_HEADER_ONLY=1` absorption be kept, at the
   cost of the unverified ODR question?
2. **Is supporting GCC 11 a goal?** If not, a ConanCenter listing means a
   recipe with no prebuilt Linux binaries. Is that worth it compared with
   only documenting `conan create` from this repo?
3. **Public since when?** Has the GitHub repository been public since
   2026-05-04? That date decides whether vcpkg's maturity gate opens in
   November 2026 or March 2027.
4. **Port name.** Is `microtel-org-microtel` acceptable if `microtel` is
   judged ambiguous?
5. **Leaf in packages.** Should the experimental leaf ship as a package
   feature/option now (it builds offline), or wait for it to leave
   experimental status?
6. **Interim channel.** While the gates are closed, should the repository
   host an overlay port and/or recipe, for example under `packaging/`? The
   sketches here are close to ready, and that path has no maturity gate.
