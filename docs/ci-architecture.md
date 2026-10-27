# microtel CI Architecture

This document describes the CI pipeline structure. The actual workflow files live under `.github/workflows/`. The gates it runs are the ones `CLAUDE.md` (hard rules) and [`CONTRIBUTING.md`](../.github/CONTRIBUTING.md) require of every PR — they are not aspirational; they're the contract.

CI runs on **GitHub Actions** for the OSS path. A self-hosted Jenkins line may be added later for users who fork microtel into private CI environments.

---

## Job overview

### As built (check names as they appear in GitHub)

These are the jobs that exist today. The **check name** column is what branch
protection matches on — see [`branch-protection.md`](branch-protection.md) — and
it is the job's `name:` field, which for matrix jobs is expanded per cell.

| Workflow | Job | Check name(s) | Required? |
|---|---|---|---|
| `ci.yml` | `format-check` | `clang-format` | ✅ |
| `ci.yml` | `tidy-check` | `clang-tidy` (PRs lint changed TUs only, full on master; see `ci/scripts/tidy-check.sh`) | ✅ |
| `ci.yml` | `compile` | `cxx20 / gcc`, `cxx20 / clang`, `cxx23 / gcc`, `cxx23 / clang` | ✅ (cxx20 pair only) |
| `ci.yml` | `sanitizers` | `asan`, `tsan`, `ubsan` | ✅ |
| `ci.yml` | `coverage` | `coverage` | ✅ |
| `ci.yml` | `test-presence` | `test-presence` | ❌ (see below) |
| `ci.yml` | `regen-check` | `regen-check` | ✅ |
| `ci.yml` | `symbol-scan` | `symbol-scan` | ✅ |
| `ci.yml` | `leaf-standalone` | `leaf-standalone / nanopb`, `leaf-standalone / upb` | ❌ |
| `ci.yml` | `leaf-footprint` | `leaf-footprint / <cortex-m0plus, cortex-m4, aarch64> / <nanopb, upb>` (six cells) | ❌ |
| `ci.yml` | `leaf-target` | `leaf-target / cortex-m0plus`, `leaf-target / cortex-m4`, `leaf-target / aarch64`, `leaf-target / i686` | ❌ |
| `ci.yml` | `version-drift-check` | `version-drift-check` | ✅ |
| `ci.yml` | `conformance` | `conformance` | ✅ |
| `ci.yml` | `citation-check` | `citation-check` | ✅ |
| `ci.yml` | `system-deps` | `system-deps` | ✅ |
| `ci.yml` | `consumer-smoke` | `consumer-smoke` | ❌ |
| `ci.yml` | `consumer-package` | `consumer-package (fetchcontent)`, `consumer-package (cpm)`, `consumer-package (vcpkg)` | ❌ |
| `ci.yml` | `corpus-check` | `corpus-check` | ❌ |
| `ci.yml` | `otelcpp-shim` | `otelcpp-shim` | ❌ |
| `sonarqube.yml` | — | `scan` | ❌ |
| `fuzz.yml`, `soak.yml`, `interop.yml`, `benchmark.yml` | — | scheduled / on-demand | ❌ |
| `tidy-nightly.yml` | `tidy-full` | `clang-tidy (full)`, nightly / on-demand | ❌ |

Note that `cxx23 / gcc` and `cxx23 / clang` run on every PR but are **not**
required, so a C++23-only regression can currently merge.

### Design intent (not all implemented)

The table below is the originally designed job set. Five rows don't exist
under those names but are covered by as-built jobs: `build-and-test` by
`compile`, `static-analysis` by `format-check` and `tidy-check`, `fuzz-smoke` by
`corpus-check` (which replays committed crash inputs and does not fuzz),
`fuzz-soak` by the weekly `fuzz.yml`, and `release` by `release.yml`, which runs
on `v*` tags. `license-scan` was never built. Treat this as the target, not as
a description of the pipeline.

| Job | When | Blocking? | Approx. duration |
|---|---|---|---|
| `build-and-test` | every PR, every push to main | yes | 8–12 min |
| `static-analysis` | every PR, every push to main | yes | 4–6 min |
| `sanitizers` | every PR, every push to main | yes | 15–20 min |
| `coverage` | every PR | yes (diff coverage gate) | 10–15 min |
| `test-presence` | every PR | yes | < 30 sec |
| `license-scan` | every PR, weekly cron | yes (PR), reports (cron) | 3 min |
| `fuzz-smoke` | every PR (60 sec budget per target) | yes | 5 min |
| `fuzz-soak` | nightly cron | reports only | 8 hours |
| `sonarqube` | every PR | yes (no critical/blocker) | 5–10 min |
| `release` | tagged commits | yes | 30+ min |

---

## Per-job detail

### `compile` (job in `.github/workflows/ci.yml`)

Builds and runs the tests on a 2×2 matrix: C++20 and C++23, each with
`g++-13` and `clang-18`, on `ubuntu-latest` (x86_64) with CMake's default build
type. There is no ARM64 runtime build; the only aarch64 cells are the leaf's
(`leaf-footprint`, `leaf-target`).

Each cell configures with the header check, tests, the glog and log4cxx
bridges and the leaf on (upb encoder under gcc, nanopb under clang). The C++20
cells also build the concentrator, and `cxx20 / clang` builds the examples.
Then `cmake --build` and `ctest --output-on-failure`, unfiltered. Only the
cxx20 pair is required.

The ctest labels that exist are `unit`, `integration` and `conformance` — set
in [`tests/CMakeLists.txt`](../tests/CMakeLists.txt),
[`tests/integration/CMakeLists.txt`](../tests/integration/CMakeLists.txt) and
[`tests/conformance/CMakeLists.txt`](../tests/conformance/CMakeLists.txt)
respectively. There is no `wire` label: the byte-level wire tests live under
`tests/unit/wire/` (and `tests/unit/wire/grpc/`) and carry `unit`. Conformance
against the collector is the separate `conformance` job documented below.

### `format-check` and `tidy-check` (jobs in `.github/workflows/ci.yml`)

`format-check` (check name `clang-format`) installs Ubuntu's `clang-format-18`
(18.1.3) and runs [`ci/scripts/format-check.sh`](../ci/scripts/format-check.sh)
over `include/`, `src/`, `tests/`, `ci/`, `examples/` and `leaf/`. It fails on
any drift.

`tidy-check` (check name `clang-tidy`) configures with `clang-18` and every
optional target on (header check, tests, the otelcpp shim, both log bridges,
the leaf with upb, fuzz harnesses, concentrator) to get a complete
`compile_commands.json`, then runs `clang-tidy-18` through
[`ci/scripts/tidy-check.sh`](../ci/scripts/tidy-check.sh). On a PR it lints
the TUs changed against the base SHA; on a push to `master` it lints all of
them. Any warning fails. There is no cppcheck step.

### `sanitizers` (job in `.github/workflows/ci.yml`)

Three cells, `asan`, `tsan` and `ubsan`, run separately because the sanitizers
don't combine. Each configures C++20 with `clang-18`, the same options as
`compile` plus the leaf (upb) and the concentrator, and
`-DMICROTEL_SANITIZER=<cell>`, then runs the whole ctest suite with
`detect_leaks=1` (asan) and `halt_on_error=1` (tsan, ubsan). All three are
required.

### `coverage` (job in `.github/workflows/ci.yml`)

Two independent coverage measurements, both against the same filtered lcov tracefile: **aggregate** (the whole tree must meet the floors in the table below) and **diff** (the lines this PR touched must be covered).

**Steps:**
1. Checkout at `fetch-depth: 0` — the diff gate needs `origin/master` as a base.
2. [`ci/scripts/coverage.sh build`](../ci/scripts/coverage.sh) configures with `-DMICROTEL_COVERAGE=ON` (clang `-fprofile-instr-generate -fcoverage-mapping`), builds, runs `ctest` with `LLVM_PROFILE_FILE` pointed at a per-process `.profraw`, merges them with `llvm-profdata`, and exports a filtered lcov tracefile with `llvm-cov export -format=lcov`. `llvm-profdata` must be exactly the compiler's major version and `llvm-cov` at least 21; the script checks both and refuses to run otherwise.
3. The same script then enforces the aggregate floors, and **fails the job** below them. It reduces the tracefile to one row per file (`build/coverage.per-file.tsv`, uploaded with the report) and sums those rows into two groups. The group mapping is stated in the script's header; briefly:

   | Group | Paths | Floor |
   |---|---|---|
   | `sdk-encoder` | `include/microtel/**`, `src/api/`, `src/sdk/`, `src/common/`, `src/wire/encoder/` | ≥ 90% line, ≥ 85% branch |
   | `transport-exporter` | `src/transport/`, `src/exporter/`, `src/adapters/`, rest of `src/wire/`, `tools/` | ≥ 80% line |

   Nothing that survives the tracefile filter is exempt. A path matching neither rule is gated as `sdk-encoder` — the stricter floor — and named in the output, so a new directory cannot dodge the gate by going unmentioned.
4. `diff-cover` (pip, PR events only) against the same tracefile with `--compare-branch origin/<base>` and `--fail-under=80`.

**Branch coverage is enforced** (issue #198). It was not, under gcov: gcov records an edge for the unwind path out of every potentially-throwing call, so `include/microtel/meter.hpp` measured 100% line and 50% branch with its "uncovered" branches on lines holding no conditional at all, and whole-tree branch coverage read 57.8% against 91.0% line. Clang's source-based coverage attaches counters to source *regions* the front end knows about — the same `meter.hpp` now reports **0 branch records**, because it contains no conditional — so the 85% branch floor measures program logic and fails the job like the line floors do. The 85% threshold was never lowered while it was unenforceable, and it is not raised now. The coverage gate is enforced for line and branch.

**Why clang-only.** The gate's numbers are the compiler's numbers, so the compiler is part of the gate. `coverage.sh` refuses to run under gcc rather than silently measuring something else, and checks its llvm tools rather than surfacing a version skew as a corrupt-looking file three steps later.

**Why the exporter is newer than the compiler** (issue #236). The build uses Ubuntu's `clang-18`, the same compiler as the `compile` matrix; the export uses `llvm-cov` **21** from `apt.llvm.org`, and `coverage.sh` refuses to run below that. Up to `llvm-cov` 20, `export` emits one set of branch records per *template instantiation* while emitting line records already merged across them — so the gate would compare a merged line percentage against a per-instantiation branch percentage, as if the two were commensurable. `llvm-cov` 21 applies the same merge to both. Bisected on one `clang-19` object and profile, a function template with one `if` instantiated twice: `llvm-cov` 19 and 20 emit 4 `BRDA` records, 21 and 22 emit 2. On this tree that is `sdk-encoder` reading 81.48% (1219/1496) versus 85.59% (1099/1284) branch, from identical line coverage of 4757/5189. Newer llvm tools read older coverage-mapping and profile formats, so the built code is unchanged.

`llvm-profdata`, by contrast, must match the compiler **exactly**: it reads the *raw* profiles the instrumented binaries write, and that format is locked to the compiler's release in both directions — `llvm-profdata-21` rejects `clang-18`'s raw version 9 with "raw profile version mismatch … expected version = 10" and then "no profile can be merged". So the job installs `clang-18`, `llvm-18` and `llvm-21`, and pairs `clang-18` + `llvm-profdata-18` + `llvm-cov-21`. `llvm-cov` reads the *indexed* profile and the coverage mapping, both of which a newer reader accepts.

The LLVM apt repository is added to the two jobs that run `coverage.sh` — `coverage` here and `scan` in `sonarqube.yml` — and deliberately to no others; `clang-format` and `clang-tidy` stay on their pinned Ubuntu 18 packages. The tradeoff is that `apt.llvm.org` becomes a network dependency of a required check: if it flakes, re-run the job.

**Why `--fail-under=80` when CLAUDE.md rule 3 names two diff thresholds.** `diff-cover` takes a single threshold and does not partition by path. 80 is the floor that holds everywhere; the 90 for SDK/encoder is carried by the aggregate gate in step 3, which *is* measured per group. A PR that drags `sdk-encoder` below 90 fails step 3 whatever step 4 reports.

`diff-cover` is a pip package installed in the job and used only at test time. It is not part of microtel's runtime dependency closure (CLAUDE.md rule 12).

**Pass condition:** both enforced aggregate floors met, and diff coverage ≥ 80% on the changed lines. A PR whose diff touches no covered source (a docs- or CI-only PR, for example) reports "no lines with coverage information in this diff" and passes.

### `test-presence` (job in `.github/workflows/ci.yml`)

[`ci/scripts/test-presence.sh`](../ci/scripts/test-presence.sh) fails any PR modifying `src/**/*.{cpp,hpp}` without a corresponding modification to `tests/**/*.{cpp,hpp}`. It is a presence check, not a coverage check — the diff-coverage gate in `coverage` proves the new lines are exercised; this proves a test file moved at all, and says so in seconds rather than after a 10-minute instrumented build.

**Logic:**
```
merge_base   = git merge-base origin/<base> HEAD
src_changed   = git diff --name-only --diff-filter=d merge_base..HEAD -- src   | grep '\.(cpp|hpp)$'
tests_changed = git diff --name-only --diff-filter=d merge_base..HEAD -- tests | grep '\.(cpp|hpp)$'

if src_changed and not tests_changed:
    if '[refactor]' in PR labels:  pass
    else:                          fail
```

Labels reach the script through `MICROTEL_PR_LABELS`, which the workflow fills from `github.event.pull_request.labels.*.name`; the script also falls back to parsing `GITHUB_EVENT_PATH` with `jq`, and honours `MICROTEL_PR_LABELS` directly for local dry-runs.

**Exceptions**, per CLAUDE.md rule 3:
- *Code deletions* — handled by `--diff-filter=d`, which drops deleted files from the changed-source set.
- *Pure refactors* — the `[refactor]` PR label.
- *Comment/formatting-only changes* — **not** detected automatically. Classifying a hunk as semantically empty needs a structured diff, and a wrong answer silently disables the gate. The `[refactor]` label is the manual override for this case too; a label is visible on the PR where a heuristic would not be.

**Not currently a required status check.** It runs on `pull_request` only (it needs a base to diff against), so requiring it would block pushes to `master`, and it is new enough to want a few PRs of observation first.

**Exit codes:** 0 satisfied or waived, 1 rule violated, 2 the gate could not run (base ref unreachable — in CI that means the checkout was too shallow).

### `regen-check` (job in `.github/workflows/ci.yml`)

Verifies that regenerating the proto accessors produces a zero-diff result against the committed generated code under `gen/`. Prevents drift between `proto/`, the pinned upb version, and the committed generated outputs.

**Steps:**
1. Install pinned `protoc` v29.4 and build `protoc-gen-upb` / `protoc-gen-upb_minitable` from the matching protobuf tag.
2. Clone the nanopb generator at the tag `third_party/nanopb/` is pinned to (`nanopb-0.4.9.2`) and install `protobuf==5.29.4` into a venv for it.
3. Run [`ci/scripts/regen-protos.sh`](../ci/scripts/regen-protos.sh) with those binaries and `--nanopb-generator`, which also regenerates the leaf's nanopb descriptors under `gen/nanopb/`.
4. `git diff --exit-code gen/`, and `git status --porcelain gen/` for files the generator newly emits. Fail if either reports anything.

**Pass condition:** generated code matches the result of regenerating from pinned sources.

**When it runs:** every PR and every push to `master`. It carries **no path filter** — an earlier version of this document claimed it was skipped unless `proto/`, `third_party/upb/`, or `gen/` changed; that was never implemented.

**A required status check.** Because it runs unconditionally, requiring it is safe (a path-filtered job could never be required — the check would never report and would block every PR that didn't trigger it). The reason to require it is that a stale `gen/` means the shipped encoder disagrees with the pinned `opentelemetry-proto`, which is a Tier 1 wire-compatibility break.

### `symbol-scan` (job in `.github/workflows/ci.yml`)

Mechanical enforcement of the dependency closure, in several passes (the script header lists them all).

**Pass 1 — forbidden namespaces.** No shipped artifact defines **or references**
a symbol from gRPC, abseil, or the protobuf C++ runtime. This is the test behind
CLAUDE.md rules 12 and 13 — the closure claim is the project's reason to
exist, so it is verified rather than asserted.

Undefined (`U`) references count as violations alongside defined symbols: a
static archive carrying `U absl::…` makes abseil a link requirement for every
consumer even though the archive contains none of abseil's code.

**Pass 2 — the vendored-upb prefix.** No shipped artifact carries a vendored
upb or utf8_range symbol under its *upstream* name. Per ICP 0020 Decision 4 they
ship renamed to `microtel_*`, so a consumer who also links a real upb cannot get
two definitions of `upb_Arena_Init` and — with static libraries — a silent,
undiagnosable selection between them. The pass fails on any unprefixed `upb_*`,
`_upb_*`, `kUpb_*`, `_kUpb_*`, `kWyhashSalt`, `UPB_linkarr*` or `utf8_range_*`
global.

This pass looks at global linkage only (`nm -g`), which is both the collision
surface and the only deterministic choice: `UPB_INLINE` functions are emitted
out-of-line at `-O0` and inlined away at `-O2`, so an all-linkage scan would
fire on optimization level rather than on a real hazard.

A upb pin bump that introduces a new global fails here rather than shipping it
unprefixed; the fix is to regenerate the list using the recipe in
[`third_party/upb/microtel_upb_rename.h`](../third_party/upb/microtel_upb_rename.h),
never to widen the pattern.

**The nanopb passes — nanopb stays in the leaf, renamed.** nanopb is a dependency
of the leaf only (ICP 0031 Decision 4). The script splits artifacts by name:
`libmicrotel_leaf*.a` and `libmicrotel_nanopb*.a` are leaf archives, everything
else is not. One pass fails any non-leaf artifact that defines or references a
nanopb symbol, renamed (`microtel_pb_*`) or not. The other fails any leaf archive
carrying a nanopb global (`pb_*`) or a generated nanopb descriptor
(`opentelemetry_proto_*_msg`, `_field_info`, `_submsg_info`) under its upstream
name; they ship renamed by
[`third_party/nanopb/microtel_pb_rename.h`](../third_party/nanopb/microtel_pb_rename.h),
whose comment carries the regeneration recipe.

The job builds with `-DMICROTEL_BUILD_LEAF=ON` (the default nanopb backend),
so the install tree carries `libmicrotel_leaf.a` and the nanopb archives; it
also passes `--leaf-build build`, which adds the build tree's leaf archives to
the scan and fails if there are none.

**Pass 3 — the leaf** (run when `libmicrotel_leaf.a` is among the artifacts;
[`docs/leaf-concentrator-design.md`](leaf-concentrator-design.md) §1.9, §7.6).
The leaf is C and links into firmware that may have no C++ runtime, so neither
its archive nor the C archives it links (upb runtime, generated upb accessors,
utf8_range, or the nanopb runtime and descriptors) may define or reference a
C++ runtime symbol (`_Z*`, `__cxa_*`, `__gxx_personality*`); every global the
leaf archive defines must start with `microtel_leaf_`; and a nanopb leaf (one
whose archive defines `microtel_leaf_internal_encode_nanopb`) and its nanopb
archives must not reference `malloc`, `calloc`, `realloc`, `free`,
`aligned_alloc` or `posix_memalign` — the mechanical form of "the nanopb leaf
needs no heap" (design §7.6).

The `symbol-scan` job runs this pass on the default nanopb leaf. The
`leaf-standalone` job, a matrix over both backends, builds the leaf the way a
firmware toolchain does — `cmake -S leaf` with only a C compiler, asserting
that no C++ compiler was configured — installs it, and runs this script over
that install tree.

The `leaf-footprint` job runs the same script on cross-compiled leaves, with
the target's `nm`; see its own section below.

**Steps:**
1. Configure with `-DMICROTEL_BUILD_TESTS=OFF` — the gate must see the shipped
   configuration only, never gtest/gmock or other test-only inputs — and
   `-DMICROTEL_BUILD_LEAF=ON`, which builds the nanopb leaf and its archives.
2. Build.
3. `cmake --install build --prefix install-tree`.
4. Run [`ci/scripts/symbol-scan.sh --prefix install-tree --leaf-build build`](../ci/scripts/symbol-scan.sh).

**It scans the install tree, not the build tree** (ICP 0020 Decision 5). Once
`cmake --install` exists, "shipped" means what `cmake --install` produces;
scanning the build directory would verify the closure claim against artifacts
that are not the ones users receive. The narrowing is real and intended —
`libmicrotel_preflight_lib.a` is in the build tree and is deliberately not
installed, so it is no longer scanned. The script keeps its build-directory
form (`symbol-scan.sh [build-dir]`) for the quicker local loop.

**Pass condition:** zero forbidden symbols and zero unprefixed vendored symbols
across every installed `libmicrotel_*.a` and the installed `microtel-preflight`
binary, no nanopb symbol in any of them, and no unprefixed nanopb symbol in the
leaf archives.

### `leaf-footprint` (job in `.github/workflows/ci.yml`)

ICP 0031 gate 4 ([`leaf-concentrator-design.md`](leaf-concentrator-design.md)
§7.6): the leaf's flash, RAM and stack, measured on every PR for the targets it
exists for, with both backends on each so they compare like for like (issue
#351). A matrix of six cells, target × backend, each running
[`ci/scripts/leaf-footprint.sh <target> <backend>`](../ci/scripts/leaf-footprint.sh):

| Target | Toolchain (apt) |
|---|---|
| `cortex-m0plus`, `cortex-m4` | `gcc-arm-none-eabi`, `libnewlib-arm-none-eabi` |
| `aarch64` | `gcc-aarch64-linux-gnu` |

**Steps (in the script):** configure `cmake -S leaf` with the toolchain file in
[`cmake/toolchains/`](../cmake/toolchains/) and `MinSizeRel`; build and install;
link [`examples/leaf/size_probe.c`](../examples/leaf/size_probe.c) with
`--gc-sections` and a linker map; sum the leaf archives' kept input sections
with [`leaf-footprint.py`](../ci/scripts/leaf-footprint.py); compute the
worst-case stack of every public entry point with
[`leaf-stack.py`](../ci/scripts/leaf-stack.py) from the call graph GCC writes
with `-fcallgraph-info=su`; write the tables, the caller-owned RAM and the
whole-image `size` to the job summary; then run
`symbol-scan.sh --prefix` over the install with `NM=<target>-nm`, so the
closure checks (no C++ runtime, prefixed globals, and for nanopb no heap) run on
the target's objects.

**Pass condition:** the build, the link, the stack analysis (every indirect
call resolved, no undeclared recursion) and the closure scan succeed. The
flash figures are compared with ICP 0031's targets (< 15 KB nanopb, < 30 KB upb)
and printed as OVER or UNDER, but do not fail the job: they are targets in
v1.2, and v2.1 makes the nanopb one a gate. Release figures are recorded in
[`bench-results/leaf-footprint.md`](bench-results/leaf-footprint.md).

### `leaf-target` (job in `.github/workflows/ci.yml`)

The leaf's tests on targets other than the x86-64 host (issue #351), one cell
per target, each running
[`ci/scripts/leaf-target.sh <target>`](../ci/scripts/leaf-target.sh) over the
project in [`tests/leaf/target/`](../tests/leaf/target/):

| Cell | Runs on | What runs | Toolchain (apt) |
|---|---|---|---|
| `cortex-m0plus` | `qemu-system-arm -M microbit` (Cortex-M0; QEMU has no M0+, both are ARMv6-M) | the C runner, nanopb, and an alignment control | `gcc-arm-none-eabi`, `libnewlib-arm-none-eabi`, `qemu-system-arm` |
| `cortex-m4` | `qemu-system-arm -M mps2-an386` | the C runner, nanopb and upb | the same |
| `aarch64` | `qemu-aarch64` user mode | the gtest leaf suite and the C runner | `gcc-aarch64-linux-gnu`, `g++-aarch64-linux-gnu`, `qemu-user` |
| `i686` | the runner itself (32-bit, static) | the same | `gcc-i686-linux-gnu`, `g++-i686-linux-gnu` |

The C runner (`tests/leaf/target/leaf_target_test.c`) has no test framework,
no heap and no stdio; on bare metal its output and exit status go over Arm
semihosting, and a fault handler reports the faulting PC. It checks the golden
vectors and a subset of the API tests with buffers at odd byte offsets, and
prints each public entry point's stack, measured by painting, to the job
summary. The alignment control does one unaligned word load and must fault,
which shows the emulated ARMv6-M core traps what silicon would.

**Pass condition:** every test passes on every target, and the control faults.

### `version-drift-check` (job in `.github/workflows/ci.yml`)

[`ci/scripts/version-drift-check.sh`](../ci/scripts/version-drift-check.sh)
compares every hand-written version literal against `project(microtel VERSION …)`
in the top-level `CMakeLists.txt`, which is the authority. The literals are
`kVersionString` and the `kVersionMajor`/`Minor`/`Patch` triple in
`include/microtel/version.hpp`, the gRPC `kUserAgent` in
`src/wire/grpc/grpc_wire_codec.cpp`, and `kVersion` in
`tools/preflight/preflight.cpp`. The last two reach collectors — as the
`user-agent` export header (`grpc-wire-protocol.md` §2.1) and the preflight
span's `microtel.version` attribute — which is why drift is a wire-visible bug and not bookkeeping.

**It is a check, not a generator.** Deriving `version.hpp` from `PROJECT_VERSION`
at configure time would turn a public header into a build artifact that the
header-only `microtel_headers` target, the M0 header check, and the install
surface all read as plain source. [`releasing.md`](releasing.md) records that
decision, the full bump procedure, and why `master` carries no `-dev` suffix.

Zero matches and duplicate matches are both hard failures (exit 2), not passes:
a pattern that stops matching after an unrelated rename would otherwise turn the
gate into a no-op that still reports green — the same principle as `symbol-scan`
failing when it finds no artifacts.

**Steps:**
1. `ci/scripts/version-drift-check.sh` — the gate.
2. `ci/scripts/version-drift-check.sh --self-test` — the gate checking itself
   against synthetic fixture trees: one per drift shape, plus the missing- and
   duplicate-literal cases, asserting the exit code for each.

The job installs no toolchain and builds nothing; it reads source text and
answers in seconds.

**Pass condition:** every literal equals `PROJECT_VERSION`, and the self-test's
nine cases all produce their expected exit codes.

**Deliberate non-violations.** The scan anchors its patterns at the start of the
demangled name, which is what keeps the generated accessors legal: upb emits C
accessors such as `google_protobuf_Timestamp_set_seconds` and
`opentelemetry_proto_trace_v1_Span_set_name`, which are protoc-gen-upb's output
and must not be confused with the `google::protobuf::` C++ runtime. Those
generated names are unchanged by the rename — they are not upb runtime symbols.

Anchoring is also what makes pass 2 self-consistent: a correctly renamed symbol
begins with `microtel_`, so it can never match a pattern anchored on the
upstream name.

**A scan that finds no artifacts fails with exit 2** rather than reporting green,
so a build-layout change cannot silently turn this gate into a no-op.

**Known gap — re-point at the install tree when `install()` lands.** The scan
currently walks the *build* tree. "Shipped" properly means the *install* tree;
the two coincide today only because the project has no `install(TARGETS …
EXPORT …)` rules yet (issue #19). When those land, this job must be re-pointed at
`cmake --install` output — or at minimum extended to cover it — in the same PR.
Otherwise install rules ship and the gate quietly begins checking the wrong set
of artifacts while still reporting green.

### `conformance` (job in `.github/workflows/ci.yml`)

The Tier 1 gate ([`compatibility-matrix.md`](compatibility-matrix.md) §7): a real OpenTelemetry Collector accepts what
microtel emits, and what the collector decodes is what microtel meant. A mock
cannot discharge that claim — it is about a receiver microtel's authors did not
write — so the collector is as much the system under test as microtel is. The
tier itself is documented in
[`tests/conformance/README.md`](../tests/conformance/README.md).

**Steps:**
1. Install clang-18 plus `libssl-dev`, `libnghttp2-dev`, `zlib1g-dev`.
2. Configure with `-DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_CXX_STANDARD=20
   -DMICROTEL_BUILD_TESTS=ON -DMICROTEL_BUILD_LEAF=ON
   -DMICROTEL_WITH_CONCENTRATOR=ON`. The last two build the leaf end-to-end
   gate (`tests/conformance/leaf/`, ICP 0031 gates 3 and 5).
3. Build.
4. Run [`ci/scripts/conformance.sh build`](../ci/scripts/conformance.sh). That
   script generates a throwaway certificate set, starts the collector image
   pinned in [`interop-matrix.md`](interop-matrix.md), waits up to 60 s on its
   `health_check` extension, refuses to continue if no test carries the
   `conformance` label, exports the endpoint / certificate / output-file
   environment contract, and runs `ctest -L conformance`.

Engine selection is the script's: `podman` if present, else `docker` (docker is
what the GitHub-hosted runner has). The job's timeout is 30 minutes because
pulling the collector image dominates; the tests themselves are seconds.

**Pass condition:** every `conformance`-labelled test passes. The script exits
1 when a test fails and 2 when the gate could not run at all — no container
engine, the collector never became healthy, or nothing carried the label — so a
gate that ran nothing cannot report green.

**Why every other job stays green without a collector.** `compile`,
`sanitizers`, `otelcpp-shim` and [`coverage.sh`](../ci/scripts/coverage.sh) all
run `ctest` unfiltered, so the conformance binaries execute there too and skip,
because the `MICROTEL_CONFORMANCE_*` variables are exported only by the runner.
To stop that skip from masquerading as a pass *inside* the gate, the runner also
exports `MICROTEL_CONFORMANCE_REQUIRE`: with it set, an unset variable is a test
failure rather than a skip
([`tests/conformance/support/conformance_env.hpp`](../tests/conformance/support/conformance_env.hpp)).

**A required status check.** The required list is in
[`branch-protection.md`](branch-protection.md) and `conformance` is on it, so a
Tier 1 wire regression cannot merge: Tier 1 is the compatibility promise
[`compatibility-matrix.md`](compatibility-matrix.md) §7 makes testable. The cost
is that every PR depends on pulling a third-party container image.

### `.github/workflows/fuzz.yml`

Runs every harness in [`tests/fuzz/`](../tests/fuzz/README.md) weekly (Wednesday 04:00 UTC) and on manual dispatch; it is not a per-PR gate. Each run first replays the committed `crashes/` inputs, then fuzzes each harness for 120 seconds (adjustable on a dispatch) from its `corpus/` seeds, and uploads any crashing input as an artifact.

The per-PR half is the `corpus-check` job in `ci.yml`, which replays the committed `crashes/` inputs with [`ci/scripts/corpus-check.sh`](../ci/scripts/corpus-check.sh). It is not a required status check.

### `.github/workflows/sonarqube.yml`

Runs **SonarQube Cloud** on the project's OSS tier — free for public/open-source repositories with no LOC cap, full feature set including the C++ analyzer (`cfamily`) and inline PR decoration. Triggered on every PR (not a periodic cron).

**Steps:**
1. Checkout with full history (`fetch-depth: 0` — Sonar uses git blame for issue annotation).
2. Set up build wrapper for the C++ analyzer (`build-wrapper-linux-x86-64`).
3. Configure CMake.
4. Build under the build wrapper to capture compile commands.
5. Upload the lcov coverage report from the `coverage` job's artifacts.
6. Run `sonar-scanner` with the project key and organization configured for SonarQube Cloud.
7. Wait for the quality gate result; fail the PR on critical or blocker issues.

**Pass condition:** SonarQube Cloud quality gate passes — no critical or blocker issues introduced by the PR.

**When `SONAR_TOKEN` is absent the job exits 0 but is no longer silent.** Requiring the secret would block every PR on a maintainer-only setup step, so the job stays green — but a green check that means "nothing ran" is worse than no check, because it reads as "analysed and clean". The skip now emits a `::warning::` annotation and writes a **NO SONAR SCAN RAN** heading to `$GITHUB_STEP_SUMMARY` stating that the SonarQube gate is UNMEASURED, with the maintainer steps to fix it. The scanned path writes its own counterpart heading, so the two outcomes are distinguishable from the summary alone.

The workflow also carries a `workflow_dispatch` trigger, so a scan can be fired from the Actions tab the moment the secret lands, rather than waiting for the next merge to `master`.

`ci/scripts/coverage.sh` runs here with `MICROTEL_COVERAGE_ENFORCE=0`: this job wants the tracefile, not a second opinion on the coverage floors. The `coverage` job owns that gate, and letting a shortfall abort this job too would suppress the Sonar scan exactly when the code most needs looking at.

**Coverage import (issue #211).** The tracefile is *not* handed to the C++ analyzer. It is converted by `ci/scripts/lcov-to-sonar.py` into SonarQube's [generic test coverage XML](https://docs.sonarsource.com/sonarqube-cloud/enriching/test-coverage/generic-test-data/) and imported via `sonar.coverageReportPaths`. The previous configuration passed the lcov `.info` to `sonar.cfamily.llvm-cov.reportPath`, which names an *llvm-cov* report: the sensor parsed the file in ~81 ms, imported nothing, and warned about nothing, so the whole project read 0.0% coverage against `coverage.sh`'s measured ~91% and the default gate's `new_coverage ≥ 80` condition failed on every PR. Compounding it, the tracefile carries absolute `SF:` paths that do not match the repo-relative keys SonarQube indexes files under; the converter rebases them. It emits **lines only**. That used to be because gcov's branch data was an artefact; since issue #198 the `BRDA` records are real, and what remains is that importing conditions moves SonarQube's own `coverage` measure and its new-code gate — a separate decision from the CI gate, left open in the converter's header. `--include src --include include` mirrors `sonar.sources`. The converter exits non-zero rather than writing an empty report, so the silent-zero failure mode cannot recur unnoticed.

**Configuration:**
- `sonar-project.properties` (at the repo root — required by SonarCloud's automatic analysis discovery) — project key, organization, source paths, exclusions, coverage report path.
- `SONAR_TOKEN` GitHub Actions secret — generated in SonarQube Cloud, stored in repo secrets.

**Exclusions (audited).** `sonar.exclusions` covers `gen/**`, `**/third_party/**`, `**/_deps/**`, `proto/**`, `build*/**`, `spike/**`, the generated upb outputs (`**/*.upb.c`, `**/*.upb.h`, `**/*.upb_minitable.*`), and `**/_generated/**`. The build pattern is `build*/**` rather than `build/**` + `build-*/**` so that every build directory the repo's scripts and docs create — `build/`, `build/coverage`, `build-asan/`, `builds/` — is covered; a build tree that escapes it would be scanned as source, and the FetchContent output under `_deps/` is precisely the vendored code these exclusions exist to keep out. `tests/**` is excluded from **coverage** analysis (`sonar.coverage.exclusions`) but deliberately still *analysed* for issues. The most load-bearing patterns are restated inline in the workflow's `args:` as a safety net against a future edit that breaks the `.properties` file.

**Why SonarQube Cloud OSS tier vs self-hosted Community Build:** the cloud OSS tier is free for public projects and includes the C++ analyzer, branch analysis, and PR decoration — all of which the self-hosted Community Build lacks without paid Developer Edition. For a public OSS project, the cloud tier is the strict superset at zero cost.

### `.github/workflows/release.yml`

Triggered on tags matching `v*`, or by `workflow_dispatch` for an existing tag. One job, `source-archive`:

1. Builds `microtel-X.Y.Z.tar.gz` from the tag with [`ci/scripts/source-archive.sh`](../ci/scripts/source-archive.sh), plus its `.sha512`.
2. Checks the SHA-512, extracts the archive, and builds and installs it with tests off.
3. Builds `tests/consumer` against the installed archive ([`ci/scripts/consumer-smoke.sh`](../ci/scripts/consumer-smoke.sh)).
4. Attaches both files to the GitHub Release for the tag, creating the release if it does not exist.

There are no binary packages and no wheels, and the workflow does not run the CI gates itself.

---

## Required secrets

The CI configuration expects these GitHub Actions secrets:

| Secret | Purpose |
|---|---|
| `SONAR_TOKEN` | required for the SonarQube Cloud scan |

That is the only one. `release.yml` publishes with the workflow's own `github.token`.

---

## Status badges

The root `README.md` carries three: CI (`ci.yml`), the latest release, and the
SonarCloud quality gate. There is no coverage badge; coverage is reported by
the `coverage` job and SonarCloud.

---

## Local equivalents

There are no pre-commit hooks. Developers run the CI gates' scripts directly:

```bash
# formatting and clang-tidy (see CLAUDE.md for the clang-format version CI uses)
ci/scripts/format-check.sh
cmake -S . -B build -DCMAKE_EXPORT_COMPILE_COMMANDS=ON -DMICROTEL_BUILD_TESTS=ON
cmake --build build
ci/scripts/tidy-check.sh build

# test-presence, against whatever your branch will be PR'd into
ci/scripts/test-presence.sh origin/master

# ... simulating the [refactor] label
MICROTEL_PR_LABELS='[refactor]' ci/scripts/test-presence.sh origin/master

# coverage build + the aggregate coverage gate (10-15 min). Clang only, and
# llvm-profdata / llvm-cov must be at least the compiler's major version.
CC=clang CXX=clang++ ci/scripts/coverage.sh build/coverage

# ... reporting the numbers without failing on a shortfall
CC=clang CXX=clang++ MICROTEL_COVERAGE_ENFORCE=0 \
    ci/scripts/coverage.sh build/coverage

# diff coverage, the way the coverage job runs it
pip install diff-cover
diff-cover build/coverage/coverage.filtered.info \
    --compare-branch origin/master --src-roots . --fail-under=80
```

The `ci/scripts/` directory holds the scripts the workflow files call, so running one locally reproduces what CI does.

---

## What's NOT in CI

- **GUI / GUI-test runs.** No GUI in microtel.
- **Performance benchmarks.** Live in [`bench/`](../bench/) and run from `benchmark.yml` weekly and on manual dispatch; not part of per-PR CI.
- **macOS / Windows builds.** Out of scope for v1; not in the matrix.
- **End-to-end integration with downstream backends** (Datadog, New Relic). The `tests/conformance/` job runs against an OTel Collector only.
