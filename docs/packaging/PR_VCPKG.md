# Submitting microtel to the vcpkg curated registry

How to submit [`ports/microtel/`](../../ports/microtel/) to
[microsoft/vcpkg](https://github.com/microsoft/vcpkg), and the PR description
to use. Nothing here has been submitted.

## What gets submitted

`ports/microtel/` holds the three files a curated port needs, in the form
microsoft/vcpkg expects:

| File | Contents |
|---|---|
| `vcpkg.json` | name, version, license, `"supports": "linux"`, dependencies (`nghttp2`, `openssl`, `zlib`, `tomlplusplus`, host `pkgconf`, `vcpkg-cmake`, `vcpkg-cmake-config`), features `spdlog` (default), `leaf`, `concentrator` |
| `portfile.cmake` | `vcpkg_from_github` (tag `v${VERSION}` plus SHA512), `vcpkg_cmake_configure` / `_install` / `_config_fixup`, `vcpkg_copy_tools`, `vcpkg_install_copyright` |
| `usage` | the `find_package` / `target_link_libraries` lines |

It differs from the overlay port in
[`packaging/vcpkg/ports/microtel/`](../../packaging/vcpkg/ports/microtel/README.md)
only in what an upstream port must not have: no `README.md`, and no
`MICROTEL_SOURCE_DIR` hook for building a local checkout. Both build the same
release with the same options. Bump both at each release (`docs/releasing.md` §4).

## Before you submit: three gates

1. **Maturity.** The [maintainer guide](https://learn.microsoft.com/vcpkg/contributing/maintainer-guide#packaged-projects-should-be-mature)
   asks for a release at least six months old, or six months of public
   development. The first commit is 2026-05-04 and `v1.0.0` is 2026-09-13.
   If the repository has been public since May, the gate opens on
   **2026-11-04**. Otherwise it opens on **2027-03-13** (six months after
   `v1.0.0`). Don't open the PR before then: reviewers close early ones.
2. **Port name.** The guide asks that the name be strongly associated with the
   project, and "Microtel" is also a hotel chain. Check
   <https://repology.org/project/microtel/versions> and a web search for
   "microtel C++". If neither leads here, rename the port
   `microtel-org-microtel`: the directory, the `"name"` in `vcpkg.json`, and
   the PR title. The CMake package stays `microtel`.
3. **Vendored code.** The guide says "do not use vendored dependencies".
   microtel compiles in upb, utf8_range and (with `leaf`) nanopb, every global
   symbol renamed `microtel_*`. The PR description below explains why.
   `docs/packaging-research.md` §4 has the long form. Be ready for a request to
   use the `tl-expected` port instead of the vendored header (§5 item 5
   there). That would be an upstream change to microtel first, not a port
   patch.

## Steps

From a fork of microsoft/vcpkg (`<you>/vcpkg`), with the release tag
already pushed and its port files in microtel's `master`:

```bash
git clone https://github.com/<you>/vcpkg.git && cd vcpkg
./bootstrap-vcpkg.sh -disableMetrics
git switch -c microtel-new-port

cp -r /path/to/microtel/ports/microtel ports/
./vcpkg format-manifest ports/microtel/vcpkg.json

# Build every feature once, from the port as it will be merged.
./vcpkg install "microtel[core,spdlog,leaf,concentrator]" --binarysource=clear
./vcpkg install microtel --binarysource=clear      # default features

git add ports/microtel
git commit -m "[microtel] new port"

# The version database, from the committed port tree.
./vcpkg x-add-version microtel
git add versions/
git commit -m "[microtel] add version database entry"
./vcpkg x-ci-verify-versions

git push -u origin microtel-new-port
```

Then open a PR against `microsoft/vcpkg:master` titled
**`[microtel] new port`**, with the description below.

`x-add-version` adds `versions/m-/microtel.json` and a `"microtel"` entry in
`versions/baseline.json`. In a local dry run against vcpkg `bf19794383`, the
`v1.2.2` port produced:

```json
{
  "versions": [
    {
      "git-tree": "d105538ba496644e126f50befc21e76cb1ac9878",
      "version": "1.2.2",
      "port-version": 0
    }
  ]
}
```

The `git-tree` hash depends on the exact port files, so let `x-add-version`
compute it. Don't copy this one.

### Before you push, check that

- `vcpkg.json` `version` is the tag without its `v`, and `SHA512` is the hash
  of GitHub's archive of that tag:
  `curl -sL https://github.com/microtel-org/microtel/archive/vX.Y.Z.tar.gz | sha512sum`.
  That archive is not the `microtel-X.Y.Z.tar.gz` asset the release workflow
  attaches, and the two hashes differ.
- `x64-linux` built with no post-build validation warnings.
- `tests/consumer` from the microtel checkout builds against the result:

  ```bash
  cmake -S /path/to/microtel/tests/consumer -B /tmp/consumer \
        -DCMAKE_TOOLCHAIN_FILE=$PWD/scripts/buildsystems/vcpkg.cmake
  cmake --build /tmp/consumer && /tmp/consumer/microtel_consumer_smoke
  ```

## PR description

Copy from here to the end of the block:

```markdown
New port: microtel, a lightweight OpenTelemetry trace runtime and OTLP exporter
(OTLP/HTTP-protobuf and OTLP/gRPC) built on nghttp2. It speaks gRPC on the wire
without linking gRPC, abseil or the protobuf C++ runtime.

- Upstream: https://github.com/microtel-org/microtel (Apache-2.0)
- Source: the release tag `v1.2.2` through `vcpkg_from_github`.
- Linux only (`"supports": "linux"`): the I/O thread uses epoll. Needs GCC 13+ or
  Clang 18+ (C++20 and `std::format`).
- Static only (`vcpkg_check_linkage(ONLY_STATIC_LIBRARY)`). Upstream has no shared build.
- Usage: `find_package(microtel CONFIG REQUIRED)` and `microtel::microtel`.
  nghttp2 is found through pkg-config, so `pkgconf` is a host dependency.
- Features: `spdlog` (default; dependency for the header-only spdlog log bridge),
  `leaf` (experimental C library, `microtel::leaf`), `concentrator`
  (experimental leaf receiver).

**On vendored code.** upb, utf8_range and nanopb are vendored at pinned versions,
and every global symbol is prefixed `microtel_`. Upstream CI enforces the prefix
(`ci/scripts/symbol-scan.sh`) on every install tree, so they cannot collide with
a consumer's own protobuf/upb/nanopb. The packaged alternatives don't fit:
vcpkg's `protobuf` and `utf8-range` ports pull in abseil, which this library
exists to avoid; upb has no ABI promise between its generated code and a
different runtime version, so using `protobuf` would also mean running protoc
at build time; and the `nanopb` port would drop the prefix. toml++ is not
vendored: it comes from the `tomlplusplus` port and is compiled header-only into
the library, so the installed package does not need libtomlplusplus.

All optional dependencies are controlled: spdlog through the feature; glog, log4cxx
and opentelemetry-cpp are behind upstream options that default OFF and the port
leaves OFF. Tests, examples, benchmarks and fuzzers are off, and configure
downloads nothing (`MICROTEL_USE_SYSTEM_DEPS=ON`).

Tested locally: `x64-linux`, `microtel[core,spdlog,leaf,concentrator]` and
default features; upstream's `tests/consumer` built against the installed
package through the vcpkg toolchain and passed.

- [x] Changes comply with the maintainer guide.
- [x] The packaged project is mature and ready for broad sharing with vcpkg users
    - [ ] Has a release at least 6 months old or 6 months of demonstrated public development  <!-- tick only once true -->
- [x] The packaged project shows strong association with the chosen port name.  <!-- say which criterion -->
- [x] Optional dependencies of the build are all controlled by the port.
- [x] The versioning scheme in `vcpkg.json` matches what upstream says.
- [x] The license declaration in `vcpkg.json` matches what upstream says.
- [x] The installed as the "copyright" file matches what upstream says.
- [x] The source code of the component installed comes from an authoritative source.
- [x] The generated "usage text" is brief and accurate.
- [x] The version database is fixed by rerunning `./vcpkg x-add-version --all` and committing the result.
- [x] Exactly one version is added in each modified versions file.
```

Fill in the two commented checklist lines before you submit. Check the
association box only after the name check in gate 2.

## After merge

At each later release, after the tag exists:

1. Update `version` and `SHA512` in `ports/microtel/` (and in the overlay port)
   here, in microtel.
2. In the vcpkg fork, copy the port over, commit, run
   `./vcpkg x-add-version microtel`, commit, and open
   **`[microtel] update to X.Y.Z`**.

## What was verified locally

Against vcpkg `bf19794383` (tool `2026-09-26`) on Linux with g++ 14, with all
vcpkg state under `/tmp` (`--x-install-root` and friends) so the vcpkg clone
was unchanged:

- `vcpkg install "microtel[core,spdlog,leaf,concentrator]" --overlay-ports=ports`
  built `v1.2.2` and its dependencies from source and passed post-build
  validation. A separate `vcpkg install microtel` (default features,
  `[core,spdlog]`) installed the 15 runtime archives and no leaf.
- `tests/consumer`, configured with vcpkg's toolchain, linked vcpkg's
  OpenSSL, zlib and nghttp2 1.70.0 statically and printed `PASS`.
  `ci/scripts/symbol-scan.sh` was clean on the package tree.
- `x-add-version microtel` and `x-ci-verify-versions` succeeded in a throwaway
  copy of the vcpkg repository.
- **Not verified:** vcpkg's own CI, and other triplets (`arm64-linux`,
  `x64-linux-dynamic`). `find_package(microtel COMPONENTS leaf)` needs
  `v1.2.3` or later, the first release whose package config defines components.
