# ICP 0049: Leaf metrics and logs — order, milestone and compile-time signal selection

**Status:** Draft — 2026-11-27. Milestone "Leaf: metrics and logs (after v1.3)"; issues #542, #543.
**Affected interfaces / docs:**
- `docs/leaf-concentrator-design.md` §1.3 (signal order reversed), §1.1 (the one-file-per-backend rule widened), Scope
- `docs/leaf-metrics-logs-design.md` (new; the design pass this ICP and [ICP 0050](0050-leaf-metrics-logs-wire-and-ingest.md) rest on)
- `docs/roadmap.md` (a new unnumbered milestone)
- `leaf/include/microtel/leaf.h` and a new `leaf/include/microtel/leaf_config.h` (**public leaf build contract**)
- `leaf/CMakeLists.txt`, `leaf/README.md` (the file list for non-CMake builds), `ci/scripts/leaf-footprint.sh`, `.github/workflows/ci.yml`

**Affected tracks:** the leaf (C) and CI. The C++ runtime is unaffected by this ICP; its concentrator changes are ICP 0050.

## Summary

Leaf metrics come before leaf logs, in a new unnumbered milestone worked on straight after v1.3, and every leaf signal, and every metric instrument kind, is selected at compile time by plain C preprocessor defines.

## Motivation

`leaf-concentrator-design.md` §1.3 put logs before metrics and gave leaf metrics no milestone. IoT fleets mostly want counters and gauges (readings, error counts, battery, link health), and free text fits poorly in a few hundred bytes of buffer, so metrics come first (#542 question 7). A leaf also has to stay small: the < 15 KB nanopb flash target is for a trace-only leaf, and a firmware that wants only a counter should not carry the trace code.

## Decision

1. **Order.** One design pass covers both signals ([`leaf-metrics-logs-design.md`](../leaf-metrics-logs-design.md)). Leaf metrics are implemented and released first, then leaf logs. This reverses §1.3's "logs first, then metrics".
2. **Milestone.** A new unnumbered milestone, "Leaf: metrics and logs (after v1.3)", is worked on straight after v1.3 and released as the next minor. v1.4 to v1.6 are not renumbered. #542 and #543 move into it, with the implementation issues the design lists.
3. **Compile-time selection.** Three signal switches and four metric-kind switches, each `0` or `1`:

   | Define | Default |
   |---|---|
   | `MICROTEL_LEAF_TRACES` | `1` |
   | `MICROTEL_LEAF_METRICS` | `0` while experimental |
   | `MICROTEL_LEAF_LOGS` | `0` while experimental |
   | `MICROTEL_LEAF_METRIC_COUNTER`, `MICROTEL_LEAF_METRIC_UPDOWN_COUNTER`, `MICROTEL_LEAF_METRIC_GAUGE` | `1` when metrics are on |
   | `MICROTEL_LEAF_METRIC_HISTOGRAM` | `0` (the costliest kind) |

   - **Plain C, not CMake.** The switches are preprocessor defines, set by `-D` flags or by a user config header named by `MICROTEL_LEAF_CONFIG_FILE` (the mbedTLS pattern). `leaf_config.h` reads them, applies the defaults and validates them. The CMake options of the same names only pass `-D` flags through.
   - **A disabled signal or kind has no API.** Its functions are not declared. The header defines each disabled function's name as a function-like macro that expands to an undeclared identifier naming the switch (for example `microtel_leaf_disabled__set_MICROTEL_LEAF_METRIC_COUNTER_to_1`), so a call is a hard compile error in every C compiler, not an implicit declaration that only warns.
   - **Not linked.** A disabled signal's core code, backend code and generated descriptors are not compiled: every leaf source file, generated files included through wrapper files, is guarded by its switch, so "compile every `.c` under `leaf/src/`" is always a correct build.
   - **Invalid selections fail at compile time** with an `#error` naming the fix: no signal selected; metrics on with every kind off; a kind switched on while metrics are off; a switch with a value other than `0` or `1`.
   - **Trace-only users see no change.** With the defaults, the API, the wire bytes (the golden vectors) and the footprint are those of v1.2.6. Struct layouts never depend on a switch.
4. **§1.1 widened.** "Only one source file per backend includes that encoder's headers" becomes "only that backend's own files do": each backend gets a common file and one file per signal, so a signal's backend code can be compiled out.
5. **What this ICP does not decide.** The wire format, the concentrator's ingest of metrics and logs, and the reversal of §6.4 (delta-only leaf metrics) are [ICP 0050](0050-leaf-metrics-logs-wire-and-ingest.md).

## Migration

- **Trace-only leaf users:** nothing. The defaults select today's leaf.
- **Firmware that builds the leaf from a file list:** keep compiling every file under `leaf/src/` and the nanopb wrappers; the new files are empty unless their switch is on. `leaf/README.md` gives the updated list.
- **Contributors:** a test or a CI job that builds the leaf names its profile. CI builds four profiles end to end and compiles every other combination ([design §6](../leaf-metrics-logs-design.md#6-test-plan)).

## Rationale & alternatives

- **CMake options only.** Rejected: most firmware builds do not use CMake.
- **`#ifdef` instead of `0`/`1` values.** Rejected: `-DMICROTEL_LEAF_METRICS=0` would then turn metrics *on*. Values are validated, so a typo fails the build.
- **Leave disabled functions declared and fail at link time.** Rejected: the error appears late, names a missing symbol rather than the switch, and an unused declaration still compiles.
- **`__attribute__((error))` on disabled declarations.** Rejected: GCC-only, and §1.2 bans attributes in the public header.
- **Switches per value type (int64 / double).** Rejected: the kinds already bound the code size; value types are settled per kind in the design.
- **Logs first, as §1.3 had it.** Rejected for the reason in the motivation.
