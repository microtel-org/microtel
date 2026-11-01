# Leaf footprint

Flash, RAM and stack of the experimental microtel leaf, for both encoder
backends on the same targets, as ICP 0031 gate 4 requires (design:
[`leaf-concentrator-design.md`](../leaf-concentrator-design.md) §7.6) and issue
#351 extends. The `leaf-footprint` CI job produces the size and static-stack
tables on every PR, one cell per target and backend, in its job summary; the
`leaf-target` job runs the leaf's tests on the same targets and reports the
measured stack. This file records them for the release.

**v1.2 figures**, measured 2026-09-26 with `ci/scripts/leaf-footprint.sh` and
`ci/scripts/leaf-target.sh` in an `ubuntu:24.04` container using the same apt
toolchains as CI: `arm-none-eabi-gcc` 13.2.1 (newlib-nano),
`aarch64-linux-gnu-gcc` 13.3.0, and QEMU 8.2.2. The Cortex-M0+ measured
encode figures were re-measured 2026-10-01 with the same toolchains, after the
runner was given enough stack to measure them (see below); every other figure
is unchanged.

**Not re-measured for v1.2.4, v1.2.5 or v1.2.6.** Since v1.2.3 the leaf changed only in its
version literals and one line of `leaf/DEVELOPMENT.md`.

## Side by side

The same minimal trace-only probe on every target, both backends. Flash is
`.text` + `.rodata` + `.data` of the leaf's own archives; caller RAM is what the
application allocates for the leaf; worst-case stack is the deepest public entry
point (`microtel_leaf_encode` / `microtel_leaf_encode_to`), from the static call
graph, with the stack measured under QEMU for the probe beside it.

| Target | Backend | Flash | .bss | Leaf static RAM (.data + .bss) | Caller RAM | Worst-case stack (static) | Stack measured, probe encode |
|---|---|---:|---:|---:|---:|---:|---:|
| Cortex-M0+ | nanopb | **9,472** | 0 | 0 | 512 | 3,552 | 2,524 |
| Cortex-M0+ | upb | **14,632** ¹ | 1 | 65 | 2,560 | 3,264 | — ² |
| Cortex-M4 | nanopb | **9,298** | 0 | 0 | 512 | 3,392 | 2,092 |
| Cortex-M4 | upb | **14,392** | 1 | 65 | 2,560 | 2,944 | 1,900 |
| aarch64 Linux | nanopb | **16,366** | 0 | 808 | 512 | 6,864 | 3,956 |
| aarch64 Linux | upb | **23,966** | 1 | 769 | 2,560 | 5,296 | 3,220 |

All in bytes. Every figure is under its target (< 15 KB nanopb, < 30 KB upb)
except nanopb on aarch64, which is over the 15 KB nanopb target but is not what
that target is for: the targets are ICP 0031's, nanopb on a Cortex-M and upb on
a Cortex-A / Linux-on-ARM target. **They are targets in v1.2, not gates**; v2.1
makes the nanopb one a gate.

¹ upb links on ARMv6-M only with an `__atomic_compare_exchange_4`, which
newlib does not ship: the core has no exclusive load / store, so GCC compiles
upb's C11 atomics (its arena's reference count) to libatomic calls, and the
link otherwise fails with `undefined reference to
'__atomic_compare_exchange_4'`. The probe supplies a plain compare-and-swap
(firmware would mask interrupts around it). Cortex-M4 has `LDREX` / `STREX` and
needs nothing.

² Not run: the QEMU Cortex-M0 board (micro:bit) has 16 KiB of RAM, and the
upb runner's golden vectors encode through the heap. The upb runner does run on
the Cortex-M4 board.

Totals for a firmware are flash as above; RAM = leaf static RAM + caller RAM +
the stack of the deepest entry point on the calling thread.

## What is measured

[`examples/leaf/size_probe.c`](../../examples/leaf/size_probe.c) is a minimal
trace-only firmware: it initialises a leaf in static memory, builds one span
with one attribute, and streams the payload to a sink. The leaf is built
standalone (`cmake -S leaf`, `MinSizeRel`, so `-Os`, with
`-ffunction-sections -fdata-sections`) and the probe is linked with
`--gc-sections`. The size tables count the input sections from the leaf's own
archives that survive the link: the leaf core, its backend, and the vendored
encoder's runtime and generated tables. libc and the probe itself are not
counted. Unwind tables (`.eh_frame`, on by default for Linux targets: 2,876
bytes nanopb and 3,744 upb on aarch64) are not counted either; build with
`-fno-asynchronous-unwind-tables` to drop them.

## Flash by part

| Target | Backend | part | .text | .rodata | .data | .bss |
|---|---|---|---:|---:|---:|---:|
| Cortex-M0+ | nanopb | leaf core | 5,074 | 186 | 0 | 0 |
| | | leaf backend | 1,086 | 0 | 0 | 0 |
| | | nanopb runtime | 2,206 | 0 | 0 | 0 |
| | | nanopb descriptors | 0 | 920 | 0 | 0 |
| Cortex-M0+ | upb | leaf core | 5,074 | 186 | 0 | 0 |
| | | leaf backend | 2,426 | 318 | 4 | 0 |
| | | upb runtime | 5,446 | 110 | 8 | 1 |
| | | upb mini-tables | 0 | 1,008 | 52 | 0 |
| Cortex-M4 | nanopb | leaf core | 4,962 | 186 | 0 | 0 |
| | | leaf backend | 1,100 | 0 | 0 | 0 |
| | | nanopb runtime | 2,130 | 0 | 0 | 0 |
| | | nanopb descriptors | 0 | 920 | 0 | 0 |
| Cortex-M4 | upb | leaf core | 4,962 | 186 | 0 | 0 |
| | | leaf backend | 2,538 | 318 | 4 | 0 |
| | | upb runtime | 5,206 | 110 | 8 | 1 |
| | | upb mini-tables | 0 | 1,008 | 52 | 0 |
| aarch64 | nanopb | leaf core | 8,800 | 186 | 0 | 0 |
| | | leaf backend | 2,112 | 0 | 0 | 0 |
| | | nanopb runtime | 3,992 | 0 | 0 | 0 |
| | | nanopb descriptors | 0 | 468 | 808 | 0 |
| aarch64 | upb | leaf core | 8,800 | 186 | 0 | 0 |
| | | leaf backend | 4,192 | 18 | 8 | 0 |
| | | upb runtime | 9,144 | 150 | 168 | 1 |
| | | upb mini-tables | 0 | 708 | 592 | 0 |

The descriptors and mini-tables land in `.data` on aarch64 because they hold
pointers, which a position-independent Linux executable relocates at load; on
Cortex-M they stay in flash.

Whole probe images (`<target>-size`: text / data / bss, including libc start-up
and the probe's static leaf state, record buffer and scratch): Cortex-M0+
nanopb 11,436 / 12 / 1,012, upb 19,232 / 156 / 3,080; Cortex-M4 nanopb
11,344 / 12 / 1,012, upb 23,012 / 156 / 3,080; aarch64 nanopb
23,392 / 1,556 / 528, upb 33,161 / 1,616 / 2,584.

## RAM

The leaf has no static state of its own on nanopb on a Cortex-M (0 bytes
`.data` + `.bss`); upb's runtime keeps 65 bytes there. Everything else is
memory the caller owns and sizes:

| Caller-owned | Bytes | Notes |
|---|---:|---|
| `microtel_leaf_t` | 256 | fixed, both backends |
| record buffer | 256 | what the probe's Resource, scope and one span with one attribute need (a little over 192) |
| upb encode scratch | 2,048 | upb only; without it the upb backend uses the heap. nanopb needs none |
| output buffer | 0 | the probe streams with `microtel_leaf_encode_to`; a buffer encode needs the payload size (204 bytes here) |

## Stack

Worst-case stack per public entry point, in bytes. **Static** is the deepest
path through the call graph, an upper bound; **measured** is what the probe
used under QEMU, by stack painting.

| Entry point | M0+ nanopb static | M0+ nanopb measured | M4 nanopb static | M4 nanopb measured | M4 upb static | M4 upb measured | aarch64 nanopb static | aarch64 nanopb measured | aarch64 upb static | aarch64 upb measured |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| `init` | 248 | 264 | 208 | 220 | 208 | 220 | 368 | 356 | 368 | 356 |
| `span_start` | 760 | 648 | 752 | 604 | 752 | 604 | 1,040 | 788 | 1,040 | 788 |
| `span_set_attribute` | 640 | 608 | 648 | 580 | 648 | 580 | 864 | 724 | 864 | 724 |
| `span_end` | 528 | 552 | 512 | 516 | 512 | 516 | 672 | 660 | 672 | 660 |
| `encode_to`, probe | 3,552 | 2,524 | 3,392 | 2,092 | 2,944 | 1,900 | 6,864 | 3,956 | 5,296 | 3,220 |
| `encode_to`, span with an event with attributes | | 2,884 | | 2,364 | | 2,036 | | 4,564 | | 3,460 |

The other entry points, static: `span_start_remote` 608 (M0+, M4) and 832
(aarch64), `span_add_event` 568 and 800, `span_set_status` 560–576 and 832,
`clock_sync` 32–48, `encoded_size` 680 and 1,088; the same for both backends.
Upb on Cortex-M0+, static: `encode_to` 3,264. On 32-bit x86 (i686, from the
`leaf-target` job) the measured probe `encode_to` is 2,640 upb and 3,512 nanopb.

The encode is the deepest entry point on every target, and its stack comes from
the backend. nanopb recurses once per nested message (the request, ResourceSpans,
ScopeSpans, Span, Event, KeyValue, AnyValue), each level a `pb_encode` frame
plus the leaf's field callback, which builds the next level's message struct on
the stack; its sizing pass for each submessage runs the same callbacks again,
but after the write, not inside it. upb recurses the same way inside its
encoder, over messages the backend has built in the arena beforehand.

### How the static figure is computed

[`ci/scripts/leaf-stack.py`](../../ci/scripts/leaf-stack.py) reads the call
graph GCC writes with `-fcallgraph-info=su`: each function's frame size (the
`-fstack-usage` figure) and its call edges, for the leaf core, its backend and
the vendored encoder. It walks from every public entry point to the deepest
path. Function-pointer calls are resolved from a table in the script: the
application's callbacks (clock, random source, write sink) are not counted;
nanopb's field callbacks resolve by context, so the callback encoding a Span
reaches only the Span's field callbacks, which bounds nanopb's recursion by the
message nesting; upb's allocator calls reach its allocators. Recursion is
allowed only where the script declares it (upb's `encode_message`, seven
levels; nanopb's `pb_encode`, once per callback message plus once for its
static submessage). An unresolved indirect call or an undeclared cycle fails
the job. libc (`memcpy`, `memset`, ...) has no call-graph data and counts as 0,
which is why a measured figure can exceed the static one by a few bytes
(`init`, `span_end` on Cortex-M).

The static figure takes the largest frame at every level, for any span shape;
the measured one is a real run. Size a thread's stack from the static figure.

### How the measured figure is taken

The target runner (`tests/leaf/target/leaf_target_test.c`, below) paints the
stack below the calling frame with a pattern, calls the entry point with its
arguments already in static memory, and finds the lowest word overwritten. It
can under-read by the few bytes of one helper frame. Run on the probe scenario
(`init`, `span_start`, `span_set_attribute`, `span_end`, `encode_to`,
`encode`) and on a span with an event with attributes, the deepest nesting.

A figure is only a measurement if the call left untouched paint below it. The
runner fails if a measured call, or any of its tests, writes into the bottom
256 bytes of the painted region: the figure would then be the region's size,
not the call's stack, and on bare metal the stack may have run on into
`.bss`. On the Cortex-M0 board (16 KiB of RAM) the runner keeps 11,656 bytes
of `.bss` (output and record buffers, the golden vectors' 4 KiB record buffer
and their long strings) and leaves 4,728 bytes of stack; its linker script
requires at least 4,608. The v1.2 release figures for Cortex-M0+ `encode_to`
(2,496 and 2,500) were that cap, not a measurement: the runner then had
2,688 bytes of stack, and the encode reached the bottom of it.

## Tests on the targets

The `leaf-target` job runs the leaf's tests on every PR, on:

| Target | How | What runs |
|---|---|---|
| Cortex-M0+ | bare metal, `-mcpu=cortex-m0plus`, on QEMU's micro:bit (a Cortex-M0: QEMU 8.2 has no M0+; both are ARMv6-M, with no unaligned access) | the C runner, nanopb |
| Cortex-M4 | bare metal on QEMU's MPS2 AN386 | the C runner, nanopb and upb |
| aarch64 | Linux user mode under `qemu-aarch64` | the gtest leaf suite (both backends, and the backend byte-identity test) and the C runner |
| i686 | 32-bit x86 Linux (`size_t` is 32 bits), static, run natively | the same |

The C runner has no test framework, no heap and no stdio: output and exit go
over Arm semihosting. It checks all 13 golden vectors byte for byte, each
twice at different output offsets, and a subset of the leaf API tests (init
guards, short buffer, streaming against buffer encode, a failing write, record
buffer exhaustion and recovery, stale handles), with record and output buffers
at byte offsets 1 to 3 as well as aligned. On the Cortex-M0 board a control
program does one unaligned word load and must fault, which shows that the
emulated core traps what a real M0 / M0+ would. **Every test passes on every
target; the leaf does no unaligned access.**

## Closure

The `leaf-footprint` job runs `ci/scripts/symbol-scan.sh` over the installed
archives with the target's `nm`: no C++ runtime symbol, every global prefixed
`microtel_`, and for nanopb no reference to `malloc`, `calloc`, `realloc`,
`free`, `aligned_alloc` or `posix_memalign`. All six cells are clean.

## Reproducing

```bash
ci/scripts/leaf-footprint.sh cortex-m0plus [nanopb|upb]   # or cortex-m4, aarch64
ci/scripts/leaf-target.sh cortex-m0plus                  # or cortex-m4, aarch64, i686
```

`leaf-footprint.sh` needs `arm-none-eabi-gcc` with newlib (apt
`gcc-arm-none-eabi libnewlib-arm-none-eabi`) or `aarch64-linux-gnu-gcc` (apt
`gcc-aarch64-linux-gnu`), CMake, Ninja and Python 3. `leaf-target.sh` also
needs `qemu-system-arm` for the Cortex-M targets, `g++-aarch64-linux-gnu` and
`qemu-user` for aarch64, and `gcc-i686-linux-gnu g++-i686-linux-gnu` for i686.
Other compiler versions give somewhat different numbers.
