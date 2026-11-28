# ICP 0051: A third leaf encoder backend that writes protobuf directly

**Status:** Accepted — 2026-11-28. Phase 2.7 of the 2026-11 refactor and performance round; the retirement of nanopb (Decision 7) is phase 2.8, and both land before #565.
**Affected interfaces / docs:**
- `leaf/CMakeLists.txt`: `MICROTEL_LEAF_ENCODER` accepts `nanopb | upb | wire` (**build interface**; the default stays `nanopb`)
- `leaf/src/leaf_internal.h`: declares `microtel_leaf_internal_encode_wire` with the existing backend signature (§2.2 contract unchanged)
- `leaf/src/backend_wire.c` (new)
- `docs/leaf-concentrator-design.md` §1.9 (file layout), §2.1 (build selection), §2.3 (byte identity), §7 (test plan)
- `docs/build-options.md`, `docs/roadmap.md` (v1.3 leaf row: "two encoder backends"), `leaf/README.md`
- tests and CI: `tests/leaf/dual/`, `tests/unit/leaf/leaf_backend_diff_test.cpp`, `tests/fuzz/leaf_backend_diff_fuzz.cpp`, `ci/scripts/leaf-footprint.sh`, `ci/scripts/leaf-target.sh`, `ci/scripts/leaf-stack.py`, the leaf matrix in `.github/workflows/ci.yml`
- phase 2.8 (Decision 7): `leaf/src/backend_nanopb.c`, `third_party/nanopb/`, the `microtel_nanopb*` targets, the `microtel_pb_*` pass of `ci/scripts/symbol-scan.sh`, CLAUDE.md rule 12, the nanopb option in `packaging/` (vcpkg, Conan, Yocto), and the documents above that name nanopb

**Affected tracks:** the leaf, and its packaging in phase 2.8. `leaf.h` does not change, and the concentrator is unaffected: it sees the same bytes.

## Summary

`MICROTEL_LEAF_ENCODER=wire` builds the leaf with a hand-written C11 encoder that writes `ExportTraceServiceRequest` straight to protobuf wire bytes, with no generated descriptors and no encoder runtime. The bytes are identical to the nanopb and upb backends'.

## Motivation

On a Cortex-M0+, the nanopb leaf is 9,472 bytes of flash, of which the nanopb runtime is 2,206 and the generated descriptors about 1,100 ([`leaf-footprint.md`](../bench-results/leaf-footprint.md)). Its worst-case static stack through `microtel_leaf_encode_to` is 3,552 bytes, and most of that is nanopb's nested submessage encoding. On the smallest parts, stack is the scarcer resource. The leaf encodes one fixed message shape, so a generic encoder buys little: a direct writer can size each submessage arithmetically and emit it in one flat, non-recursive pass.

## Decision

1. **A third backend.** `leaf/src/backend_wire.c` implements the §2.2 contract under the entry point `microtel_leaf_internal_encode_wire`. It includes no third-party header and links nothing beyond libc's `memcpy`. With `wire` selected, the leaf compiles `leaf_core.c` and `backend_wire.c` only.
2. **Byte identity is three-way.** §2.3 extends to `wire`: the dual build and `leaf_backend_diff_fuzz` assert `wire == nanopb == upb` on every input, and the 13 golden vectors hold for all three.
3. **Targets** (Cortex-M0+, `-Os`, the probe of `leaf-footprint.md`):
   - flash below 7,500 bytes;
   - worst-case static stack through `microtel_leaf_encode_to` below 512 bytes. If the core's own frames alone exceed that, the target becomes "below the nanopb figure by at least 2,500 bytes", and the PR says so;
   - no static RAM, no caller RAM beyond the record buffer, no heap symbols, and no unaligned accesses on ARMv6-M.
4. **Buffer-too-small leaves the buffer untouched.** The backend sizes the request before writing, so on `MICROTEL_LEAF_ERR_BUFFER_SMALL` it writes nothing to `out`. `leaf.h` says nothing about `out`'s contents on that error, and design §1.7 says no backend produces a partial payload, so this is within the contract.
5. **The default stays `nanopb` until phase 2.8** (Decision 7).
6. **CI** adds `wire` to the leaf footprint, target (QEMU) and stack jobs, next to `nanopb` and `upb`.

7. **nanopb is retired once `wire` passes, before #565 starts.** Leaf metrics and logs (#565–#570, ICPs 0049 and 0050) need every backend to encode every enabled signal. Three backends would mean three encoders per signal, so the leaf keeps two: `wire` for microcontrollers and `upb` for Linux-class boards, where upb also stays the reference that byte identity is checked against. In order:
   - **Phase 2.7** adds `wire` with nanopb still present. `wire` is verified byte for byte against both nanopb and upb: all 13 golden vectors, `leaf_backend_diff_fuzz`, and the QEMU targets.
   - **Phase 2.8**, once `wire` is green and meets the Cortex-M0+ targets of Decision 3 (flash below 7,500 bytes, stack below 512 bytes): the default becomes `wire`, and nanopb is removed. That means the backend, the vendored `third_party/nanopb`, the `microtel_nanopb*` archives, the `microtel_pb_*` symbol-scan pass, and the nanopb option in packaging. CLAUDE.md rule 12 and the documents naming nanopb are updated in the same change, so the leaf's dependency closure shrinks by one.
   - If `wire` misses a target, phase 2.8 doesn't run, and the choice returns to the user before #565.

## Migration

- **Users:** none. The option gains a value, and the default is unchanged.
- **Contributors:** between phases 2.7 and 2.8, a change to the backend contract or to the trace message shape touches three backend files, and the three-way diff test catches any that is missed. After 2.8 it touches two.
- **Users of `MICROTEL_LEAF_ENCODER=nanopb`:** after phase 2.8 that value is a configure error naming `wire` as its replacement. The bytes on the wire are identical, so nothing else changes.

## Rationale & alternatives

- **Tune nanopb.** Rejected: the stack cost comes from its nested submessage callbacks, which are how it encodes. Options files can shrink descriptors but not the call depth.
- **Use upb on microcontrollers.** Rejected: it is larger (14,632 bytes on M0+) and needs an arena.
- **Replace nanopb outright in one step.** Rejected: nanopb stays through phase 2.7 as a second oracle, so `wire` is checked against two independent encoders before it replaces one.
- **Keep all three backends.** Rejected: every leaf signal would need three encoders and three-way identity tests.
- **`wire` traces-only, nanopb for metrics and logs.** Rejected: the microcontroller build would then need nanopb for exactly the signals that grow the leaf the most.
