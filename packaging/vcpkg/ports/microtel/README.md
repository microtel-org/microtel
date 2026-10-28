# vcpkg overlay port for microtel

This directory is a vcpkg port that the repository hosts itself. Its twin,
[`ports/microtel/`](../../../../ports/microtel/), is the same port in the form
submitted to the curated registry
([`docs/packaging/PR_VCPKG.md`](../../../../docs/packaging/PR_VCPKG.md)): it
has no README and no `MICROTEL_SOURCE_DIR` hook. Keep the two in step. vcpkg's
curated registry doesn't carry microtel yet, so you point vcpkg at this
directory with `--overlay-ports`. vcpkg then builds microtel from source like
any other port, with nghttp2, OpenSSL, zlib, toml++ and spdlog taken from
vcpkg.

## Install

Use a checkout of `master`. The port there builds the latest release; a
release tag's own copy of this port still names the release before it, because
the port is bumped in a follow-up PR once the tag exists (`docs/releasing.md` §4).

Classic mode, from a microtel checkout:

```bash
vcpkg install microtel --overlay-ports=packaging/vcpkg/ports
vcpkg install "microtel[leaf]" --overlay-ports=packaging/vcpkg/ports
```

Manifest mode: add the dependency to your project's `vcpkg.json`, and give
the overlay path in `vcpkg-configuration.json` beside it:

```json
{
  "dependencies": [
    "microtel"
  ]
}
```

```json
{
  "overlay-ports": [
    "/path/to/microtel/packaging/vcpkg/ports"
  ]
}
```

The overlay path is the `ports` directory, not `ports/microtel`, so other
ports can be added beside it later. A relative path is resolved against the
directory of `vcpkg-configuration.json`.

## Use

```cmake
find_package(microtel CONFIG REQUIRED)
target_link_libraries(my_app PRIVATE microtel::microtel)
```

Configure with vcpkg's toolchain file
(`-DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake`). Link
`microtel::microtel` only. The per-layer targets exist because a static link
needs them, and they can change without notice (ICP 0020).

`find_package(microtel)` finds nghttp2 through pkg-config, so your configure
needs a `pkg-config` executable on `PATH` or in `PKG_CONFIG_EXECUTABLE`. vcpkg's
toolchain puts the vcpkg tree on `CMAKE_PREFIX_PATH`, so vcpkg's own
`libnghttp2.pc` is the one found.

The `microtel-preflight` tool is installed as
`<installed>/<triplet>/tools/microtel/microtel-preflight`.

## Features

| Feature | Default | CMake option | What it adds |
|---|---|---|---|
| `spdlog` | on | `MICROTEL_USE_SPDLOG=ON` | spdlog as a dependency, for the header-only spdlog log bridge (`microtel/adapters/spdlog_sink.hpp`). No installed archive references spdlog; link `spdlog::spdlog` yourself if you use the bridge. Turn it off with `"default-features": false` or `microtel[core]`. |
| `leaf` | off | `MICROTEL_BUILD_LEAF=ON` | The experimental C leaf library (`microtel/leaf.h`), exported as `microtel::leaf`, with its renamed nanopb archives. |
| `concentrator` | off | `MICROTEL_WITH_CONCENTRATOR=ON` | The experimental `LeafReceiver`, so a `Provider` built with `SdkBuilder::WithLeafReceiver` can ingest leaf payloads. |

The leaf's encoder backend is always nanopb, the default. vcpkg features must
be additive, and the two backends exclude each other, so there is no `upb`
backend feature. Build the leaf from source (`MICROTEL_LEAF_ENCODER=upb`) if
you need it. A leaf for a microcontroller is normally cross-compiled on its
own from `leaf/` (see `leaf/README.md`); this feature is for a Linux gateway or
host build.

## Source pinning

`portfile.cmake` downloads the release tag that matches the port's `version`
(`REF "v${VERSION}"`), currently `v1.2.5`, and checks it against `SHA512`.
`v1.2.1` is the first tag with the three build changes the port needs:
`MICROTEL_USE_SYSTEM_DEPS`, `MICROTEL_WARNINGS_AS_ERRORS` and the
`OpenSSL::Crypto` link. `docs/releasing.md` has the step for updating `version`
and `SHA512` at each release.

Other ways to get the source:

- `vcpkg install microtel --head --overlay-ports=...` builds the current tip of
  `master` instead of the release.
- To build a local checkout, for example to test a change to microtel or to
  this port, set `MICROTEL_SOURCE_DIR` in the environment:

  ```bash
  MICROTEL_SOURCE_DIR=$PWD vcpkg install microtel \
      --overlay-ports=packaging/vcpkg/ports --binarysource=clear
  ```

  vcpkg's binary cache keys on the port files, not on that directory, so
  `--binarysource=clear` stops it from reusing an older build.

To compute the `SHA512` of a new release:

```bash
curl -sL https://github.com/microtel-org/microtel/archive/vX.Y.Z.tar.gz | sha512sum
```

GitHub's archive omits the `export-ignore` paths in `.gitattributes`, among
them `ci/`. The port turns off everything that would need them, such as the
header check.

## How the port builds microtel

- `MICROTEL_USE_SYSTEM_DEPS=ON`: toml++ and spdlog come from vcpkg through
  `find_package`, and configure downloads nothing. vcpkg's toml++ is a
  compiled library, but microtel still compiles toml++ header-only into
  `libmicrotel_config.a`, so the installed package doesn't depend on it.
- `MICROTEL_WARNINGS_AS_ERRORS=OFF`: a warning added by a newer compiler
  must not fail the package build.
- Tests, examples, benchmarks, fuzzers, the header check and the
  opentelemetry-cpp shim build are off. The shim's headers are still
  installed, since they are source-only.
- Static libraries only (`vcpkg_check_linkage(ONLY_STATIC_LIBRARY)`); microtel
  has no shared build. A dynamic triplet still gets static archives.
- upb, utf8_range and nanopb stay vendored, with every global symbol renamed
  `microtel_*`. vcpkg's `protobuf` and `utf8-range` ports depend on abseil,
  and its `nanopb` port would drop the rename. See
  `docs/packaging-research.md` §4.

## Limits

- **Linux only** (`"supports": "linux"`). The I/O thread uses `epoll`.
- **GCC 13 or newer, or Clang 18 or newer.** microtel uses C++20 and
  `std::format`, which libstdc++ added in GCC 13.
- **Not in the curated vcpkg registry.** Its maintainer guide asks for a
  release at least six months old or six months of public development, and
  it forbids vendored dependencies. microtel doesn't meet the first rule yet
  and keeps its renamed vendored code on purpose. `docs/packaging-research.md`
  has the details.
- **Experimental features.** `leaf` and `concentrator` are experimental in
  v1.2, as they are in a source build.
