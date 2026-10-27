# meta-microtel

A Yocto Project / OpenEmbedded layer for
[microtel](https://github.com/microtel-org/microtel), a C++20 OpenTelemetry
runtime for traces, metrics and logs, with an OTLP/HTTP and OTLP/gRPC exporter
built on nghttp2 and a vendored upb.

| | |
|---|---|
| Recipe | `recipes-support/microtel/microtel_1.2.4.bb` (release v1.2.4) |
| Depends on | `openembedded-core` only (`nghttp2`, `openssl`, `zlib`) |
| Compatible with | scarthgap (5.0 LTS), styhead, walnascar, whinlatch |

## Add the layer

```bash
cd poky   # or your build's top directory, with oe-init-build-env sourced
git clone https://github.com/microtel-org/microtel.git ../microtel
bitbake-layers add-layer ../microtel/packaging/yocto/meta-microtel
```

Then use it from an image or a recipe:

```bitbake
# local.conf or an image recipe: put the CLI on the target
IMAGE_INSTALL:append = " microtel"

# an application recipe that links microtel
DEPENDS += "microtel"
```

and in that application's CMake, `find_package(microtel CONFIG REQUIRED)` and
`target_link_libraries(app PRIVATE microtel::microtel)`.

With kas, add the layer to your project file:

```yaml
repos:
  microtel:
    url: https://github.com/microtel-org/microtel.git
    branch: master
    layers:
      packaging/yocto/meta-microtel:
```

## Options

```bitbake
PACKAGECONFIG:pn-microtel = "preflight leaf concentrator"
```

| PACKAGECONFIG | Default | Effect |
|---|---|---|
| `preflight` | on | Install the `microtel-preflight` operator CLI in `microtel` |
| `leaf` | off | Build the C11 leaf library (`microtel::leaf`, nanopb backend; adds Zlib to LICENSE) |
| `concentrator` | off | Compile the concentrator's leaf receiver (experimental upstream) |

## Packaging notes

- **No network at build time.** upb, utf8_range and nanopb are vendored in the
  release tarball. microtel's CMake would download toml++ with FetchContent,
  so the recipe fetches the pinned toml++ v3.4.0 tarball in `do_fetch` instead
  and passes it in with `FETCHCONTENT_SOURCE_DIR_TOMLPLUSPLUS` and
  `FETCHCONTENT_FULLY_DISCONNECTED=ON`. toml++ is header-only and compiled into
  `libmicrotel_config.a`, so it's neither a runtime nor a link dependency.
- **Static archives only.** microtel builds every library `STATIC`. Linking
  needs `microtel-dev` (headers and the CMake package) and `microtel-staticdev`
  (the archives); `microtel-dev` pulls in `-staticdev` for you. The `microtel`
  package itself holds only the optional CLI and may be empty.
- **Updating to a new release:** rename the recipe to `microtel_<version>.bb`,
  then update `SRC_URI[microtel.sha256sum]` (and `LIC_FILES_CHKSUM` if
  `LICENSE` changed). `docs/releasing.md` lists this step.

## Validation status

See the PR that added this layer, and the `yocto` CI job
(`.github/workflows/yocto.yml`), for what's checked automatically: BitBake
parse with poky scarthgap, `do_fetch` with checksum verification, and an
offline build of the unpacked sources with the recipe's CMake options.
