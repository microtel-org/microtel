# ICP 0051: A third leaf encoder backend that writes protobuf directly

**Status:** Draft — 2026-11-28. Phase 2.7 of the 2026-11 refactor and performance round.
**Affected interfaces / docs:**
- `leaf/CMakeLists.txt`: `MICROTEL_LEAF_ENCODER` accepts `nanopb | upb | wire` (**build interface**; the default stays `nanopb`)
- `leaf/src/leaf_internal.h`: declares `microtel_leaf_internal_encode_wire` with the existing backend signature (§2.2 contract unchanged)
- `leaf/src/backend_wire.c` (new)
- `docs/leaf-concentrator-design.md` §1.9 (file layout), §2.1 (build selection), §2.3 (byte identity), §7 (test plan)
- `docs/build-options.md`, `docs/roadmap.md` (v1.3 leaf row: "two encoder backends"), `leaf/README.md`
- tests and CI: `tests/leaf/dual/`, `tests/unit/leaf/leaf_backend_diff_test.cpp`, `tests/fuzz/leaf_backend_diff_fuzz.cpp`, `ci/scripts/leaf-footprint.sh`, `ci/scripts/leaf-target.sh`, `ci/scripts/leaf-stack.py`, the leaf matrix in `.github/workflows/ci.yml`

**Affected tracks:** the leaf only. `leaf.h` does not change, and the concentrator is unaffected: it sees the same bytes.

## Summary

`MICROTEL_LEAF_ENCODER=wire` builds the leaf with a hand-written C11 encoder that writes `ExportTraceServiceRequest` straight to protobuf wire bytes, with no generated descriptors and no encoder runtime. The bytes are identical to the nanopb and upb backends'.

## Motivation

On a Cortex-M0+, the nanopb leaf is 9,472 bytes of flash, of which the nanopb runtime is 2,206 and the generated descriptors about 1,100 ([`leaf-footprint.md`](../bench-results/leaf-footprint.md)). Its worst-case static stack through `microtel_leaf_encode_to` is 3,552 bytes, and most of that is nanopb's nested submessage encoding. On the smallest parts, stack is the scarcer resource. The leaf encodes one fixed message shape, so a generic encoder buys little: a direct writer can size each submessage arithmetically and emit it in one flat, non-recursive pass.

## Proposed change

1. **A third backend.** `leaf/src/backend_wire.c` implements the §2.2 contract under the entry point `microtel_leaf_internal_encode_wire`. It includes no third-party header and links nothing beyond libc's `memcpy`. With `wire` selected, the leaf compiles `leaf_core.c` and `backend_wire.c` only.
2. **Byte identity is three-way.** §2.3 extends to `wire`: the dual build and `leaf_backend_diff_fuzz` assert `wire == nanopb == upb` on every input, and the 13 golden vectors hold for all three.
3. **Targets** (Cortex-M0+, `-Os`, the probe of `leaf-footprint.md`):
   - flash below 7,500 bytes;
   - worst-case static stack through `microtel_leaf_encode_to` below 512 bytes. If the core's own frames alone exceed that, the target becomes "below the nanopb figure by at least 2,500 bytes", and the PR says so;
   - no static RAM, no caller RAM beyond the record buffer, no heap symbols, and no unaligned accesses on ARMv6-M.
4. **Buffer-too-small leaves the buffer untouched.** The backend sizes the request before writing, so on `MICROTEL_LEAF_ERR_BUFFER_SMALL` it writes nothing to `out`. `leaf.h` says nothing about `out`'s contents on that error, and design §1.7 says no backend produces a partial payload, so this is within the contract.
5. **The default stays `nanopb`.** Making `wire` the default, or retiring a backend, is a separate decision (see below).
6. **CI** adds `wire` to the leaf footprint, target (QEMU) and stack jobs, next to `nanopb` and `upb`.

## Interaction with ICPs 0049 and 0050

Leaf metrics and logs (#565–#570) need every backend to encode every enabled signal. A third backend means a third encoder for metrics and for logs, plus three-way identity tests for each. Before #565 starts, decide one of:
- **keep all three**, with the extra work in each signal's issue;
- **retire nanopb** once `wire` meets its gates: `wire` serves microcontrollers, and `upb` stays as the Linux-class backend and the reference that byte identity is checked against;
- **`wire` stays traces-only**: metrics and logs are built only with `nanopb` or `upb`, and a `wire` build with `MICROTEL_LEAF_METRICS` or `MICROTEL_LEAF_LOGS` on is a configure error.

This ICP doesn't choose. Accepting it adds the backend for traces.

## Migration

- **Users:** none. The option gains a value, and the default is unchanged.
- **Contributors:** a change to the backend contract or to the trace message shape now touches three backend files, and the three-way diff test catches any that is missed.

## Rationale & alternatives

- **Tune nanopb.** Rejected: the stack cost comes from its nested submessage callbacks, which are how it encodes. Options files can shrink descriptors but not the call depth.
- **Use upb on microcontrollers.** Rejected: it is larger (14,632 bytes on M0+) and needs an arena.
- **Replace nanopb outright now.** Rejected until `wire` has passed the fuzz, footprint and QEMU gates. nanopb is the shipped default, and a measured replacement is a safer basis for retiring it.
