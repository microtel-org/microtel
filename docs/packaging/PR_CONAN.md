# Submitting microtel to Conan Center

How to submit [`packaging/conan/recipes/microtel/`](../../packaging/conan/recipes/microtel/)
to [conan-center-index](https://github.com/conan-io/conan-center-index) (CCI),
and the PR description to use. Nothing here has been submitted.

## What gets submitted

The directory mirrors CCI's `recipes/<name>/` layout, so it copies across
unchanged:

```
recipes/microtel/
├── config.yml                  # version → folder ("1.2.5": all)
└── all/
    ├── conandata.yml           # source URL and sha256 per version
    ├── conanfile.py            # the recipe (Conan 2)
    └── test_package/
        ├── conanfile.py
        ├── CMakeLists.txt      # find_package(microtel) + microtel::microtel
        └── test_package.cpp    # builds a Provider, shuts it down
```

The recipe in brief:

- **Dependencies from Conan Center:** `openssl/[>=3 <4]`, `libnghttp2/1.68.1`,
  `zlib/[>=1.2.11 <2]`, and `tomlplusplus/3.4.0` (`visible=False`: it is
  compiled header-only into `libmicrotel_config.a`). The tool requirements are
  `cmake/[>=3.20 <5]` and `pkgconf`. nghttp2 is found through pkg-config, and
  `PkgConfigDeps` writes `libnghttp2.pc`.
- **Build:** `MICROTEL_USE_SYSTEM_DEPS=ON`, so configure downloads nothing.
  `MICROTEL_WARNINGS_AS_ERRORS=OFF`. Tests, examples, benchmarks, fuzzers, the
  header check, the spdlog bridge and the otel-cpp shim build are all off.
- **Options:** `leaf` (default `False`) adds the experimental C leaf as
  component `microtel::leaf`. `concentrator` (default `False`) compiles in the
  leaf receiver. `fPIC`.
- **package_info:** the CMake file name is `microtel`. Component `core` is
  `microtel::microtel`, with the 15 archives in link order.
  `include/microtel/vendor` is on its include path for the vendored
  `tl/expected.hpp`. Upstream's own CMake config is removed, as CCI requires;
  Conan generates the config.
- **validate():** Linux only (the I/O thread uses epoll), C++20, and GCC 13+ or
  Clang 18+ (`std::format`).

## Before you submit

1. **No prebuilt Linux binaries.** CCI's Linux build profile is GCC 11, which
   `validate()` rejects, so CCI will mark Linux configurations invalid and
   publish no binaries. Users then build from source with
   `--build=missing`. Say so in the PR (`docs/packaging-research.md` §1).
2. **Vendored code.** CCI asks for vendored dependencies to be unvendored
   "on a best effort basis", but says that when "upstream takes care to rename
   symbols, it may be acceptable". upb, utf8_range and nanopb are renamed
   `microtel_*` and checked by upstream CI. CCI has no `upb` or `utf8_range`
   recipe at all. CCI's `tl-expected` stops at 1.2.0 and microtel vendors
   1.3.1, so the vendored header stays.
3. **Source URL.** `conandata.yml` uses GitHub's tag archive
   (`archive/v1.2.5.tar.gz`). Releases tagged after the release workflow
   landed also carry `microtel-X.Y.Z.tar.gz` as a release asset
   (`docs/releasing.md` §4). Either URL is acceptable to CCI. The asset is
   produced by `git archive` and is reproducible, but its sha256 differs from
   the tag archive's.

## Steps

From a fork of conan-io/conan-center-index (`<you>/conan-center-index`), with
Conan 2:

```bash
git clone https://github.com/<you>/conan-center-index.git && cd conan-center-index
git switch -c microtel-1.2.5

cp -r /path/to/microtel/packaging/conan/recipes/microtel recipes/

# Build and test from the recipe as it will be merged.
cd recipes/microtel/all
conan create . --version=1.2.5 -s compiler.cppstd=20 --build=missing
conan create . --version=1.2.5 -s compiler.cppstd=20 --build=missing \
      -o "microtel/*:leaf=True" -o "microtel/*:concentrator=True"
rm -rf test_package/build test_package/CMakeUserPresets.json
cd ../../..

git add recipes/microtel
git commit -m "microtel: add recipe"
git push -u origin microtel-1.2.5
```

Then open a PR against `conan-io/conan-center-index:master` titled
**`microtel: add recipe`**, with the description below. CCI accepts a PR only
after its author has signed the
[Contributor License Agreement](https://cla-assistant.io/conan-io/conan-center-index)
([contributing guide](https://github.com/conan-io/conan-center-index/blob/master/.github/CONTRIBUTING.md)).

For a new version later, add its `sources` entry (URL plus
`sha256sum` of the download) to `all/conandata.yml`, add the version to
`config.yml`, and open **`microtel: add version X.Y.Z`**.

## PR description

```markdown
### Summary
Changes to recipe:  **microtel/1.2.5**

#### Motivation
New recipe. microtel is a lightweight OpenTelemetry trace runtime and OTLP
exporter (OTLP/HTTP-protobuf and OTLP/gRPC) built on nghttp2. It speaks gRPC on
the wire without linking gRPC, abseil or the protobuf C++ runtime, so it suits
applications that want OTLP export with a small dependency closure.

#### Details
- Upstream: https://github.com/microtel-org/microtel (Apache-2.0)
- Static library only; Linux only (the I/O thread uses epoll).
- Needs C++20 with `std::format`: GCC 13+ or Clang 18+. CCI's GCC 11 Linux
  profile is rejected in `validate()`, so no Linux binaries will be published;
  `--build=missing` works on supported compilers.
- Dependencies from Conan Center: openssl, libnghttp2 (found through
  pkg-config via `PkgConfigDeps`), zlib, tomlplusplus (header-only, compiled
  into the library; not visible to consumers).
- Options: `leaf` (experimental C library, `microtel::leaf`), `concentrator`.
- Vendored code: upb and utf8_range (protobuf v29.4) and, with `leaf`, nanopb,
  each with every global symbol renamed `microtel_*`. Upstream CI enforces the
  prefix on every install tree (`ci/scripts/symbol-scan.sh`), so it cannot
  clash with a consumer's protobuf/upb/nanopb. Conan Center has no upb or
  utf8_range recipe, and upb's generated code is tied to its runtime version.

Tested locally with Conan 2.33.0, GCC 14, `compiler.cppstd=20`: default options
and `leaf=True, concentrator=True`; test_package passed for both.

---

- [x] Read the [contributing guidelines](https://github.com/conan-io/conan-center-index/blob/master/.github/CONTRIBUTING.md)
- [x] Checked that this PR is not a duplicate: [list of PRs by recipe](https://github.com/conan-io/conan-center-index/discussions/24240)
- [x] Tested locally with at least one configuration using a recent version of Conan
```

## What was verified locally

Conan 2.33.0, a detected default profile (GCC 14, x86_64, Release),
`-s compiler.cppstd=20`, dependencies from `conancenter` built with
`--build=missing`:

- `conan create` with default options: built offline from the `v1.2.5` tag
  archive, and `test_package` ran (`microtel 1.2.5`).
- `conan create` with `leaf=True, concentrator=True`: built, and
  `test_package` ran. The leaf and nanopb archives were packaged.
- `ci/scripts/symbol-scan.sh --prefix <package folder>` was clean for both
  packages. `licenses/` holds `LICENSE`, `NOTICE` and `THIRD_PARTY_NOTICES.md`.
- `conan graph info` with `compiler.version=11` reports the configuration
  invalid with the `validate()` message.
- **Not verified:** CCI's own CI and hooks, Clang, Debug builds, and
  `test_package` linking `microtel::leaf`. `test_package` covers only
  `microtel::microtel`, as CCI asks.
