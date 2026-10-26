# Library sizes: otelcpp-grpc vs microtel (stripped emit_app + loaded shared libraries)

| | |
|---|---|
| **Build** | The bench SUT images `bench-sut-otelcpp-grpc` and `bench-sut-microtel` built 2026-09-27 03:50 UTC from master at about `6c2fe10` (just before the v1.2.0 tag): the same images, and the same `/emit_app` binaries, as the [2026-09-27 benchmark run](../README.md). Measured 2026-10-26. |
| **Packages** | Ubuntu 24.04 (`ubuntu:24.04`) runtime packages in each image; versions [below](#versions). opentelemetry-cpp v1.19.0, built from source. |
| **Method** | Each `/emit_app`, stripped (GNU strip 2.42), split by compile unit with [bloaty](https://github.com/google/bloaty) (upstream `f1a83e6`), using the unstripped binary as `--debug-file`. Plus every shared library `ldd` resolves in the image, sized from the file on disk; two that ship unstripped are counted stripped. [Commands](#commands). |

These figures replace the "benchmark binary size" comparison (43.1 MB against
17.5 MB) that the benchmark README used to report. That compared the
executables' file sizes with debug info (over 92% of each) and without the
shared libraries they load. #445 tracks fixing the benchmark driver.

**Units.** MB = 1,000,000 bytes (the withdrawn 43.1 / 17.5 figures were decimal MB too).
Nothing is rounded except the MB, % and ratio figures; byte columns are exact.

**Footprint definition.** Stripped `/emit_app` (GNU strip 2.42 from ubuntu:24.04, default
`strip`) plus every shared library in `ldd /emit_app` inside the SUT image, each resolved
with `readlink -f`, counted once, size from `stat -c %s` on the real file. The dynamic
loader (`ld-linux-x86-64.so.2`) and `linux-vdso.so.1` are excluded.

## otelcpp-grpc — footprint

| Library | Where | Bytes | MB | % of total |
|---|---|---:|---:|---:|
| gRPC (libgrpc++, libgrpc, libgpr, libupb.so.29) | shared library | 6,683,680 | 6.7 | 26.3% |
| OpenSSL (libssl + libcrypto) | shared library | 6,005,912 | 6.0 | 23.7% |
| C/C++ runtime (libstdc++, libgcc_s, libc, libm) | shared library | 5,857,288 | 5.9 | 23.1% |
| protobuf | shared library | 3,056,272 | 3.1 | 12.0% |
| Abseil (45 libabsl_*.so files) | shared library | 1,378,840 | 1.4 | 5.4% |
| opentelemetry-cpp (static) | in binary | 853,160 | 0.9 | 3.4% |
| libopentelemetry_proto*.so (2 files, stripped copy; 9,165,400 B as shipped) | shared library | 651,096 | 0.7 | 2.6% |
| re2 | shared library | 400,480 | 0.4 | 1.6% |
| c-ares | shared library | 157,792 | 0.2 | 0.6% |
| zlib | shared library | 113,000 | 0.1 | 0.4% |
| emit-app | in binary | 44,946 | 0.0 | 0.2% |
| libaddress_sorting.so.29 (gRPC package, not in the listed gRPC set) | shared library | 14,648 | 0.0 | 0.1% |
| header-inlined third-party (protobuf/gRPC headers) | in binary | 7,812 | 0.0 | 0.0% |
| unattributed (bloaty [section]/[Unmapped]/ELF rows, see below) | in binary | 162,490 | 0.2 | 0.6% |
| **Total, incl. C/C++ runtime** | | **25,387,416** | **25.4** | 100.0% |
| **Total, excl. C/C++ runtime** | | **19,530,128** | **19.5** | |

**`libopentelemetry_proto*.so` are counted stripped.** The Dockerfile copies them unstripped
from the `-g` build (on disk: 5,003,816 + 4,161,584 = 9,165,400 B, with `.debug_*` sections).
They were stripped as a copy, without modifying the image, using the same GNU strip 2.42 (ubuntu:24.04) used for
`/emit_app`; the copy's sha256 matches the image file: 439,400 + 211,696 = **651,096 B**, no
`.debug_*` sections left. Every other library in both images already has no `.debug_*`
sections, so both footprints are now debug-free and comparable.

## microtel — footprint

| Library | Where | Bytes | MB | % of total |
|---|---|---:|---:|---:|
| OpenSSL (libssl + libcrypto) | shared library | 6,005,912 | 6.0 | 45.8% |
| C/C++ runtime (libstdc++, libgcc_s, libc, libm) | shared library | 5,857,288 | 5.9 | 44.6% |
| microtel (static) | in binary | 678,130 | 0.7 | 5.2% |
| nghttp2 | shared library | 170,352 | 0.2 | 1.3% |
| zlib | shared library | 113,000 | 0.1 | 0.9% |
| toml++ (header-only) | in binary | 55,943 | 0.1 | 0.4% |
| upb (vendored, incl. utf8_range) | in binary | 31,012 | 0.0 | 0.2% |
| emit-app | in binary | 29,396 | 0.0 | 0.2% |
| unattributed (bloaty [section]/[Unmapped]/ELF rows, see below) | in binary | 177,447 | 0.2 | 1.4% |
| **Total, incl. C/C++ runtime** | | **13,118,480** | **13.1** | 100.0% |
| **Total, excl. C/C++ runtime** | | **7,261,192** | **7.3** | |

## Subtotals

Computed from exact bytes, not from the rounded rows above. The shared base is the same
files in both images (same Ubuntu packages, same sizes).

| Subtotal | otelcpp-grpc bytes | MB | microtel bytes | MB |
|---|---:|---:|---:|---:|
| Shared base (OpenSSL + zlib + C/C++ runtime) | 11,976,200 | 12.0 | 11,976,200 | 12.0 |
| Telemetry stack (everything else) | 13,411,216 | 13.4 | 1,142,280 | 1.1 |
| **Total** | 25,387,416 | 25.4 | 13,118,480 | 13.1 |

Telemetry-stack ratio, otelcpp-grpc / microtel: 13,411,216 / 1,142,280 = **11.7×** (unrounded 11.7407).

## Inside the stripped /emit_app

Grouping is by compile unit, with three carve-outs by inline-source path (see rules below):
toml++ and upb headers, and protobuf/gRPC/Abseil headers. Inlined libstdc++ template code is
**not** carved out — it stays with the compile unit that instantiated it; the last column
shows how much of each bucket it is.

### otelcpp-grpc (1,068,408 B stripped)

| Bucket | Bytes | % of stripped binary | of which inlined libstdc++ template code |
|---|---:|---:|---:|
| opentelemetry-cpp (static) | 853,160 | 79.9% | 542,110 |
| emit-app | 44,946 | 4.2% | 22,131 |
| header-inlined third-party (protobuf/gRPC headers) | 7,812 | 0.7% | 0 |
| unattributed | 162,490 | 15.2% | 0 |
| **Total = stripped /emit_app** | **1,068,408** | 100.0% | |

bloaty `[section]` / `[Unmapped]` / ELF rows (bytes no compile unit claims), all in "unattributed":

| bloaty row | Bytes |
|---|---:|
| `[section .rela.dyn]` | 43,056 |
| `[section .rodata]` | 40,632 |
| `[section .gcc_except_table]` | 19,318 |
| `[section .dynstr]` | 16,083 |
| `[section .data.rel.ro]` | 14,392 |
| `[Unmapped]` | 9,922 |
| `[section .rela.plt]` | 5,472 |
| `[section .plt]` | 3,664 |
| `[section .data]` | 2,232 |
| `[ELF Section Headers]` | 2,048 |
| `[section .got.plt]` | 1,848 |
| `[section .dynamic]` | 1,472 |
| `[section .gnu.version]` | 614 |
| `[section .gnu.version_r]` | 496 |
| `[section .shstrtab]` | 303 |
| `[section .text]` | 293 |
| `[section .eh_frame]` | 196 |
| `[section .got]` | 152 |
| `[section .init_array]` | 96 |
| `[section .comment]` | 84 |
| `[section .eh_frame_hdr]` | 28 |
| `[section .init]` | 27 |
| `[LOAD #5 [RW]]` | 16 |
| `[LOAD #3 [RX]]` | 13 |
| `[section .fini]` | 13 |
| `[section .fini_array]` | 8 |
| `[section .plt.got]` | 8 |
| `[LOAD #4 [R]]` | 3 |
| `[LOAD #2 [R]]` | 1 |
| **Sum (= unattributed)** | **162,490** |

### microtel (971,928 B stripped)

| Bucket | Bytes | % of stripped binary | of which inlined libstdc++ template code |
|---|---:|---:|---:|
| microtel (static) | 678,130 | 69.8% | 482,215 |
| toml++ (header-only) | 55,943 | 5.8% | 0 |
| upb (vendored, incl. utf8_range) | 31,012 | 3.2% | 0 |
| emit-app | 29,396 | 3.0% | 20,286 |
| unattributed | 177,447 | 18.3% | 0 |
| **Total = stripped /emit_app** | **971,928** | 100.0% | |

bloaty `[section]` / `[Unmapped]` / ELF rows, all in "unattributed". These are net of the
20,374 B of upb/utf8_range symbols recovered from them (see "upb" below):

| bloaty row | Bytes |
|---|---:|
| `[section .rela.dyn]` | 46,224 |
| `[section .rodata]` | 36,886 |
| `[section .gcc_except_table]` | 31,848 |
| `[section .data.rel.ro]` | 17,168 |
| `[section .dynstr]` | 7,410 |
| `[section .text]` | 7,259 |
| `[Unmapped]` | 6,092 |
| `[section .dynsym]` | 6,017 |
| `[section .rela.plt]` | 5,640 |
| `[section .plt]` | 3,776 |
| `[ELF Section Headers]` | 2,048 |
| `[section .got.plt]` | 1,904 |
| `[section .eh_frame]` | 956 |
| `[section .data]` | 928 |
| `[section .gnu.version_r]` | 672 |
| `[section .dynamic]` | 592 |
| `[section .gnu.version]` | 556 |
| `[ELF Program Headers]` | 432 |
| `[section .shstrtab]` | 303 |
| `[section .got]` | 184 |
| `[section .eh_frame_hdr]` | 124 |
| `[section .comment]` | 84 |
| `[section .gnu.hash]` | 80 |
| `[section .note.gnu.build-id]` | 36 |
| `[section .note.ABI-tag]` | 32 |
| `[section .note.gnu.property]` | 32 |
| `[section .interp]` | 28 |
| `[section .init]` | 27 |
| `[section .plt.got]` | 24 |
| `[LOAD #5 [RW]]` | 16 |
| `[section .init_array]` | 16 |
| `[LOAD #2 [R]]` | 15 |
| `[LOAD #3 [RX]]` | 13 |
| `[section .fini]` | 13 |
| `[section .fini_array]` | 8 |
| `[LOAD #4 [R]]` | 4 |
| **Sum (= unattributed)** | **177,447** |

## Reconciliation

- Bucket sums equal the stripped file size exactly for both SUTs (971,928 and 1,068,408 B);
  bloaty's own `compileunits` total equals the file size too. Gap: 0 B.
- These totals do **not** reconcile with the withdrawn 43.1 / 17.5 MB, and are not meant to:
  those were unstripped `/emit_app` file sizes with no shared libraries. Of the withdrawn
  figures, debug sections are 41,496,547 B of 43,128,128 (96.2%) and 16,201,697 B of
  17,496,320 (92.6%).

## Notes on the attribution

- **upb has no DWARF.** Vendored upb/utf8_range are C sources; the bench passes `-g` only via
  `CMAKE_CXX_FLAGS`, so they are compiled without debug info and bloaty files their code under
  `[section .text]` etc. They are recovered from the unstripped symbol table: 108 symbols
  matching `upb|utf8_range`, 20,374 B across sections (15,472 in `.text`). The other
  10,638 B of the upb row is upb header code inlined into microtel's C++ compile units
  (`/src/third_party/upb/...`). upb-generated accessors in `/src/gen/opentelemetry/proto`
  (2,300 B inlined) are left in **microtel**, per ICP 0020's "not upb runtime symbols".
- **toml++** is header-only; all 55,943 B are code inlined from
  `_deps/tomlplusplus-src/include/toml++/impl/*` into `src/common/config/toml_loader.cpp`.
- **"absl" in otelcpp-grpc is not Abseil.** The 28,715 B of inlined code under `.../absl/` paths
  comes from opentelemetry-cpp's own copy (`opentelemetry/nostd/internal/absl`, the build
  has `WITH_ABSEIL=OFF`) and is counted as opentelemetry-cpp. No code is inlined from
  `/usr/include/absl`. The header-inlined row is 7,715 B from `/usr/include/google/protobuf`
  plus 97 B from `/usr/include/grpcpp`.
- **Other third-party headers left inside their compile unit's bucket** (no bucket was asked
  for): libstdc++ (last column above), tl-expected in microtel (4,662 B),
  opentelemetry-cpp's generated `.pb.h` code in otelcpp-grpc (`/build/otelcpp/generated`,
  3,660 B), `/usr/include/x86_64-linux-gnu/c++` (1,459 B).
- **emit-app** rows include microtel / opentelemetry-cpp API header code inlined into the
  emit-app's own compile units (CU rule).
- **libaddress_sorting.so.29** ships in the `libgrpc29t64` package but was not in the listed
  gRPC set, so it has its own row (14,648 B). With it, gRPC would be 6,698,328 B (6.7 MB).
- **OpenSSL in otelcpp-grpc** is loaded transitively (through gRPC); the emit-app does not
  link it directly.

## Protobuf → Abseil

**No, not in this build.** `readelf -d libprotobuf.so.32.0.12` lists NEEDED `libz.so.1`,
`libstdc++.so.6`, `libc.so.6`, `ld-linux-x86-64.so.2`, `libgcc_s.so.1` — no `libabsl_*`.
protobuf 3.21.12 predates protobuf's Abseil dependency (introduced in 22.x). The 45
`libabsl_*` libraries are pulled in by gRPC 1.51 (`libgrpc.so.29`, `libgrpc++.so.1.51`).
The diagram's protobuf → Abseil edge is not true for this build; gRPC → Abseil is.

## Versions

opentelemetry-cpp **v1.19.0** (`OTELCPP_VERSION` in the Dockerfile; `bench/versions.lock`
agrees), built from source with GCC; emit-app built with clang-18. Packages, `dpkg-query -W`
in the runtime images:

| Package | Version | Image |
|---|---|---|
| libprotobuf32t64 | 3.21.12-8.2ubuntu0.3 | otelcpp-grpc |
| libgrpc++1.51t64 | 1.51.1-4.1build5 | otelcpp-grpc |
| libgrpc29t64 | 1.51.1-4.1build5 | otelcpp-grpc |
| libabsl20220623t64 | 20220623.1-3.1ubuntu3.2 | otelcpp-grpc |
| libcares2 | 1.27.0-1.0ubuntu1 | otelcpp-grpc |
| libre2-10 | 20230301-3build1 | otelcpp-grpc |
| libnghttp2-14 | 1.59.0-1ubuntu0.4 | microtel |
| libssl3t64 | 3.0.13-0ubuntu3.15 | both |
| zlib1g | 1:1.3.dfsg-3.1ubuntu2.2 | both |
| libstdc++6 / libgcc-s1 | 14.2.0-4ubuntu2~24.04.1 | both |
| libc6 | 2.39-0ubuntu8.9 | both |

## Grouping rules

Shared libraries (by soname, first match):

| Row | soname prefix |
|---|---|
| libopentelemetry_proto*.so | `libopentelemetry_proto` |
| gRPC | `libgrpc++.so`, `libgrpc.so`, `libgpr.so`, `libupb.so.29` |
| libaddress_sorting.so.29 | `libaddress_sorting.so` |
| Abseil | `libabsl_` |
| protobuf / c-ares / re2 / nghttp2 / zlib | `libprotobuf.so` / `libcares.so` / `libre2.so` / `libnghttp2.so` / `libz.so` |
| OpenSSL | `libssl.so`, `libcrypto.so` |
| C/C++ runtime | `libstdc++.so`, `libgcc_s.so`, `libc.so`, `libm.so` |
| anything else | unattributed (none occurred) |

Stripped binary, per byte of `bloaty -d compileunits,inlines` (first match):

1. inline source contains `tomlplusplus` or `/toml++/` → toml++
2. inline source contains `/third_party/upb/` or `utf8_range` → upb
3. inline source starts `/usr/include/google/protobuf`, `/usr/include/grpc`, `/usr/include/absl` → header-inlined third-party
4. compile unit starts `/src/src/` → microtel; `/src/third_party/upb/` → upb;
   `/src/opentelemetry-cpp/` → opentelemetry-cpp; `/src/bench/emit-app/` or `/src/emit-app/` → emit-app
5. no match → unattributed (no compile unit fell through)

Rows whose compile unit is a bloaty pseudo-row (`[section …]`, `[Unmapped]`, `[ELF …]`, `[LOAD …]`)
are split by `bloaty -d compileunits,symbols`: symbols matching `upb` or `utf8_range` → upb, the
rest → unattributed, itemized above.

## Commands

```bash
# the withdrawn figure: the unstripped file size the driver used to record
podman run --rm --entrypoint /bin/sh bench-sut-<sut> -c 'wc -c < /emit_app'

# extract the shipped (unstripped) binary
c=$(podman create bench-sut-<sut>); podman cp $c:/emit_app <sut>.emit_app; podman rm $c

# strip with the build image's toolchain (GNU strip 2.42)
podman run --rm -v $PWD:/w:Z ubuntu:24.04 bash -c \
  'apt-get update && apt-get install -y binutils && cd /w && strip -o <sut>.emit_app.stripped <sut>.emit_app'

# shared-library closure, real files, sizes, owning package
podman run --rm --entrypoint /bin/sh bench-sut-<sut> -c \
  'ldd /emit_app | awk "/=>/ {print \$1, \$3}" | while read n p; do r=$(readlink -f $p);
   echo "$n $p $r $(stat -c %s $r) $(dpkg -S $r 2>/dev/null | cut -d: -f1 | head -1)"; done'

# bloaty: Ubuntu 24.04 has no package and Fedora's bloaty 1.1 fails on DWARF 5
# ("Data is in new DWARF format we don't understand"), so upstream main was built
# in a throwaway fedora:44 container: google/bloaty f1a83e63f36beb628dcf022e1201a2eb391a22a6
B=/bloaty/build/bloaty
$B -d compileunits         -n 0 --csv --debug-file=<sut>.emit_app <sut>.emit_app.stripped > <sut>-compileunits.csv
$B -d compileunits,inlines -n 0 --csv --debug-file=<sut>.emit_app <sut>.emit_app.stripped > <sut>-cu-inlines.csv
$B -d compileunits,symbols -n 0 --csv --debug-file=<sut>.emit_app <sut>.emit_app.stripped > <sut>-cu-symbols.csv
$B -d inlines              -n 0 --csv --debug-file=<sut>.emit_app <sut>.emit_app.stripped > <sut>-inlines.csv
$B -d sections             -n 0 --csv                             <sut>.emit_app.stripped > <sut>-sections.csv

# protobuf -> Abseil, package versions
readelf -d libprotobuf.so.32.0.12 | grep NEEDED

# libopentelemetry_proto*.so: strip a copy (copied out with podman cp; image untouched)
podman run --rm -v $PWD:/w:Z ubuntu:24.04 bash -c 'apt-get update && apt-get install -y binutils && cd /w &&
  for f in libopentelemetry_proto.so libopentelemetry_proto_grpc.so; do strip -o $f.stripped $f; stat -c "%n %s" $f.stripped; done'
# -> otelcpp-grpc-stripped.txt (real path, stripped bytes), read by group.py
podman run --rm --entrypoint dpkg-query bench-sut-<sut> -W -f '${Package} ${Version}\n'

# grouping (scripts kept next to the CSVs)
python3 -I group.py <csv-dir> <libs-dir> > grouped.json
python3 -I render.py grouped.json
```

Raw data in this directory: the bloaty CSVs above, `<sut>-libs.txt` (ldd closure:
soname, ldd path, real path, bytes as on disk, package), `otelcpp-grpc-stripped.txt`, `grouped.json`, `group.py`, `render.py`.

## Shared-library file list

### otelcpp-grpc

| Group | File (real path basename) | Bytes |
|---|---|---:|
| libopentelemetry_proto*.so | `libopentelemetry_proto_grpc.so` | 211,696 |
| libopentelemetry_proto*.so | `libopentelemetry_proto.so` | 439,400 |
| gRPC | `libgrpc++.so.1.51.1` | 759,088 |
| gRPC | `libgrpc.so.29.0.0` | 5,680,744 |
| gRPC | `libupb.so.29.0.0` | 183,184 |
| gRPC | `libgpr.so.29.0.0` | 60,664 |
| protobuf | `libprotobuf.so.32.0.12` | 3,056,272 |
| zlib | `libz.so.1.3` | 113,000 |
| c-ares | `libcares.so.2.12.0` | 157,792 |
| libaddress_sorting.so.29 | `libaddress_sorting.so.29.0.0` | 14,648 |
| re2 | `libre2.so.10.0.0` | 400,480 |
| Abseil | `libabsl_raw_hash_set.so.20220623.0.0` | 14,568 |
| Abseil | `libabsl_hashtablez_sampler.so.20220623.0.0` | 18,688 |
| Abseil | `libabsl_hash.so.20220623.0.0` | 14,416 |
| Abseil | `libabsl_city.so.20220623.0.0` | 14,128 |
| Abseil | `libabsl_low_level_hash.so.20220623.0.0` | 14,128 |
| Abseil | `libabsl_statusor.so.20220623.0.0` | 18,760 |
| Abseil | `libabsl_bad_variant_access.so.20220623.0.0` | 14,576 |
| Abseil | `libabsl_status.so.20220623.0.0` | 39,176 |
| Abseil | `libabsl_strerror.so.20220623.0.0` | 14,584 |
| Abseil | `libabsl_random_distributions.so.20220623.0.0` | 14,584 |
| Abseil | `libabsl_random_seed_sequences.so.20220623.0.0` | 14,584 |
| Abseil | `libabsl_random_internal_pool_urbg.so.20220623.0.0` | 18,752 |
| Abseil | `libabsl_random_internal_randen.so.20220623.0.0` | 14,512 |
| Abseil | `libabsl_random_internal_randen_hwaes.so.20220623.0.0` | 14,128 |
| Abseil | `libabsl_random_internal_randen_hwaes_impl.so.20220623.0.0` | 14,128 |
| Abseil | `libabsl_random_internal_randen_slow.so.20220623.0.0` | 18,296 |
| Abseil | `libabsl_random_internal_platform.so.20220623.0.0` | 18,216 |
| Abseil | `libabsl_random_internal_seed_material.so.20220623.0.0` | 14,584 |
| Abseil | `libabsl_random_seed_gen_exception.so.20220623.0.0` | 14,576 |
| Abseil | `libabsl_cord.so.20220623.0.0` | 96,504 |
| Abseil | `libabsl_bad_optional_access.so.20220623.0.0` | 14,576 |
| Abseil | `libabsl_cordz_info.so.20220623.0.0` | 26,976 |
| Abseil | `libabsl_cord_internal.so.20220623.0.0` | 108,792 |
| Abseil | `libabsl_cordz_functions.so.20220623.0.0` | 14,568 |
| Abseil | `libabsl_exponential_biased.so.20220623.0.0` | 14,496 |
| Abseil | `libabsl_cordz_handle.so.20220623.0.0` | 18,784 |
| Abseil | `libabsl_str_format_internal.so.20220623.0.0` | 104,696 |
| Abseil | `libabsl_synchronization.so.20220623.0.0` | 59,800 |
| Abseil | `libabsl_stacktrace.so.20220623.0.0` | 14,424 |
| Abseil | `libabsl_symbolize.so.20220623.0.0` | 26,872 |
| Abseil | `libabsl_debugging_internal.so.20220623.0.0` | 22,792 |
| Abseil | `libabsl_demangle_internal.so.20220623.0.0` | 39,240 |
| Abseil | `libabsl_graphcycles_internal.so.20220623.0.0` | 26,872 |
| Abseil | `libabsl_malloc_internal.so.20220623.0.0` | 18,760 |
| Abseil | `libabsl_time.so.20220623.0.0` | 72,248 |
| Abseil | `libabsl_strings.so.20220623.0.0` | 121,080 |
| Abseil | `libabsl_strings_internal.so.20220623.0.0` | 18,672 |
| Abseil | `libabsl_throw_delegate.so.20220623.0.0` | 22,704 |
| Abseil | `libabsl_base.so.20220623.0.0` | 18,696 |
| Abseil | `libabsl_spinlock_wait.so.20220623.0.0` | 14,424 |
| Abseil | `libabsl_int128.so.20220623.0.0` | 22,776 |
| Abseil | `libabsl_raw_logging_internal.so.20220623.0.0` | 14,552 |
| Abseil | `libabsl_log_severity.so.20220623.0.0` | 14,496 |
| Abseil | `libabsl_civil_time.so.20220623.0.0` | 26,872 |
| Abseil | `libabsl_time_zone.so.20220623.0.0` | 104,784 |
| C/C++ runtime | `libm.so.6` | 952,616 |
| C/C++ runtime | `libstdc++.so.6.0.33` | 2,592,224 |
| C/C++ runtime | `libgcc_s.so.1` | 183,024 |
| C/C++ runtime | `libc.so.6` | 2,129,424 |
| OpenSSL | `libssl.so.3` | 696,512 |
| OpenSSL | `libcrypto.so.3` | 5,309,400 |

### microtel

| Group | File (real path basename) | Bytes |
|---|---|---:|
| OpenSSL | `libssl.so.3` | 696,512 |
| OpenSSL | `libcrypto.so.3` | 5,309,400 |
| nghttp2 | `libnghttp2.so.14.26.0` | 170,352 |
| zlib | `libz.so.1.3` | 113,000 |
| C/C++ runtime | `libstdc++.so.6.0.33` | 2,592,224 |
| C/C++ runtime | `libm.so.6` | 952,616 |
| C/C++ runtime | `libgcc_s.so.1` | 183,024 |
| C/C++ runtime | `libc.so.6` | 2,129,424 |
