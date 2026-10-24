# microtel — Agent Instructions

Durable rules-of-engagement for Claude Code (and any other AI coding agent) working on this project. Read this once per session before doing anything else.


1. Don't assume. Don't hide confusion. Surface tradeoffs.

2. Minimum code that solves the problem. Nothing speculative.

3. Touch only what you must. Clean up only your own mess.

4. Define success criteria. Loop until verified.


---

## Project at a glance

`microtel` is a lightweight OpenTelemetry-compatible **trace runtime and OTLP exporter** built on nghttp2. It speaks both **OTLP/HTTP-protobuf** and **OTLP/gRPC** on the wire without linking the gRPC library — the gRPC path is implemented as a small unary-RPC protocol layer over the same nghttp2 transport.

**v1 is exporter-first, traces only.** Metrics, logs, control plane, hot reload, sugar APIs, and broader SDK coverage are deferred to v1.1 and beyond.

### Authoritative documents

- **The design docs in `docs/`** — [`docs/architecture.md`](docs/architecture.md), the threading, memory and error models, [`docs/grpc-wire-protocol.md`](docs/grpc-wire-protocol.md), and the metrics, logs and leaf / concentrator design docs. [`docs/README.md`](docs/README.md) indexes them.
- **`docs/interfaces.md`** — internal interface contracts. Created in M0; sign-off required before M1 begins.
- **`docs/configuration.md`** — every setting, its sources and precedence.
- **`docs/icps/`** — the Interface Change Proposals: every decision made since M0, and how each changed the documents above.
- **`docs/roadmap.md`** — multi-year roadmap, v1.0 → v3.0. Source of truth for what's deferred and when.

The original v1.0 design specification was retired by [ICP 0037](docs/icps/0037-retire-the-v1-spec.md); it is archived at the `v1.2.1` tag ([`microtel-spec.md`](https://github.com/microtel-org/microtel/blob/v1.2.1/microtel-spec.md)). "spec §N" citations inside ICPs refer to it; [`docs/README.md`](docs/README.md) maps each section to its current home.

---

## Current phase: check before you act

1. If `docs/interfaces.md` doesn't exist or isn't signed off → you're in **M0 (Architecture & Design)**. No source code yet, only design docs and compilable interface header stubs.
2. If `docs/interfaces.md` is signed off but no `src/` exists → you're in **M1 (Spike)**. Throwaway code that validates M0's architecture decisions.
3. If `src/` exists with mocks under `tests/mocks/` → you're in **M2 (Skeleton & contracts)** or later. TDD applies from M3 onward.

When in doubt, run `git log --oneline -20` and look at recent commits for clues.

---

## Hard rules

These are non-negotiable. Violations produce PRs that fail CI or get rejected in review.

### Phase discipline

1. **No source code in M0.** Only documents in `docs/` and compilable header stubs in `include/microtel/` (public API) and `include/microtel/internal/` (internal interfaces). Public headers carry full Doxygen and method signatures with no bodies; internal interface headers are pure-virtual / abstract with full Doxygen. Both compile under an `INTERFACE` CMake target with `-Werror`. No method implementations. Implementation begins in M3. Captured in [`docs/icps/0001-m0-deliverables-clarification.md`](docs/icps/0001-m0-deliverables-clarification.md).
2. **No interface changes after M0 sign-off without an ICP.** Breaking changes to any locked interface require a markdown ICP PR'd into `docs/icps/` identifying affected components and migration path.

### TDD enforcement (M3 onward)

3. **Tests first, every time.** Write the failing test, then the minimal code to pass it, then refactor. CI gates enforce this mechanically:
   - **Diff coverage:** every line added/modified in a PR must be exercised by a test in the same PR. Threshold: 90% on SDK and encoder; 80% on transport and exporter.
   - **Test-presence:** any change to `src/**/*.{cpp,hpp}` requires a corresponding change to `tests/**/*.{cpp,hpp}`. Documented exceptions: pure refactors with the `[refactor]` PR label, formatting/comment-only changes, code deletions.
4. **Mocks are dumb.** They return what they're configured to return. No logic. If you need logic, write a fake under `tests/fakes/`. "Smart mocks" are a code smell.

### RAII discipline (always)

5. **Every resource is RAII-owned.** Heap memory, file descriptors, sockets, mutexes, threads, OpenSSL contexts, nghttp2 sessions, upb arenas, callback registrations — every one of them lives inside a type whose destructor releases it.
6. **No raw `new`/`delete`** in production code. Use `std::make_unique` / `std::make_shared`.
7. **Raw pointers are non-owning by definition.** A function taking `T*` does not delete; a function returning `T*` returns a borrowed reference whose lifetime is documented in Doxygen.
8. **`std::shared_ptr` requires justification in code review.** Default to `std::unique_ptr`. Use `std::weak_ptr` for breaking cycles.
9. **Custom RAII wrappers** for C resources live in `src/common/raii/`: `UniqueFd`, `SslCtx`, `SslSession`, `Nghttp2Session`. Each is move-only with a `noexcept` destructor and a `Release()` method. **Exception:** `UpbArena` lives in `src/wire/encoder/` — including it means including a upb header, and rule 13 confines upb to that directory.
10. **Resource-owning types are move-only by default.** Add copy semantics only with explicit justification.
11. **Rule of zero or rule of five.** Never the compiler-generated mix.

### SonarQube-aligned static analysis (always)

CI runs clang-tidy with a SonarQube-aligned ruleset; full list in `docs/coding-standards.md`. The most-likely-to-trip rules:

- **No `goto`.**
- **No commented-out code** in commits.
- **Cognitive complexity ≤ 15** per function.
- **Cyclomatic complexity ≤ 10** per function.
- **No magic numbers** — use `constexpr` named constants.
- **No naked `catch(...)`.**
- **No unsafe C functions:** `strcpy`, `strcat`, `sprintf`, `gets`, unbounded `scanf`. Use `snprintf` or C++ alternatives.
- **All non-static class members initialized** at declaration or in every constructor.
- **Const correctness:** mark methods, parameters, locals `const` where applicable.
- **Max 3 indentation levels** per function (refactor at 4+).
- **No nested ternaries.**
- **Max 7 parameters** per function (use a struct beyond that).
- **No empty `if`/`else`/`catch` blocks.**
- **All non-trivial public APIs** documented in Doxygen.
- **No unnamed namespaces in headers.**
- **No `using namespace`** at file scope in headers.
- **Header include order:** own header, project headers, system headers (LLVM convention).

### Dependency discipline

12. **The runtime dependency closure for v1 is fixed:** nghttp2, OpenSSL, upb (vendored), zlib, plus optional spdlog, plus nanopb (vendored, renamed `microtel_pb_*`) linked into the leaf artifact only, per [ICP 0031](docs/icps/0031-leaf-concentrator-in-v1.3.md). **No new runtime dependencies without an ICP.** This is the project's whole reason to exist.
13. **No shipped microtel artifact links or contains symbols from gRPC, abseil, or the protobuf C++ runtime.** Enforced mechanically by [`ci/scripts/symbol-scan.sh`](ci/scripts/symbol-scan.sh) (CI job `symbol-scan`, a required status check), which scans every `libmicrotel_*.a` and the `microtel-preflight` binary and fails on any symbol — **defined or undefined** — whose demangled name begins with `absl::`, `absl_`, `grpc::`, `grpc_`, `GRPC_`, or `google::protobuf::`. Undefined references count: an archive carrying `U absl::…` makes abseil a link requirement for every consumer. Vendored upb and utf8_range are members of the closure and are **not** violations — but they ship **renamed**: every globally-visible vendored symbol carries a `microtel_` prefix (`microtel_upb_*`, `microtel_utf8_range_*`), applied by the force-included [`third_party/upb/microtel_upb_rename.h`](third_party/upb/microtel_upb_rename.h) per [ICP 0020](docs/icps/0020-install-and-package-config.md) Decision 4, so a consumer who also links a real upb cannot get two definitions and a silent static-link selection. A second `symbol-scan` pass fails the build on any *unprefixed* `upb_*` / `utf8_range_*` global. The upb-generated `google_protobuf_*` and `opentelemetry_*` C accessor names under `gen/` are unchanged — they are not upb runtime symbols. The wire encoder is upb; the HTTP/2 transport is nghttp2 directly.

    Optional **source-distributed** adapters may compile against third-party headers already present in the consumer's build, provided they add nothing to the consumer's link closure. See [ICP 0014](docs/icps/0014-otelcpp-shim-and-rule-13.md).

### Public API contracts

14. **Hot-path methods are `noexcept`.** `StartSpan`, `SetAttribute`, `AddEvent`, `End` never throw — on failure they drop the record/field, increment diagnostics, and return.
15. **Destructors are `noexcept`** and must not block indefinitely. They invoke `Shutdown` with a small finite timeout if not already shut down.
16. **Initialization returns `microtel::Expected<T, Error>`**, not exceptions. `microtel::Expected` is a project-local alias defined in [`include/microtel/expected.hpp`](include/microtel/expected.hpp); it resolves to `std::expected` on C++23 and to a vendored `tl::expected` on C++20. See [`docs/icps/0002-vendor-tl-expected.md`](docs/icps/0002-vendor-tl-expected.md).
17. **`ForceFlush(timeout)` and `Shutdown(timeout)` return a structured status** (`Completed` / `TimedOut` / `AlreadyShutDown` / `Failed`).

---

## Style cheatsheet

- Allman braces.
- `m_` prefix on members.
- PascalCase types; snake_case where OTel naming dictates on the public API.
- C++20: prefer `std::span`, `microtel::Expected` (alias — see ICP 0002), designated initializers, concepts for template constraints.
- Doxygen comments on every non-trivial public API.

---

## Build & test

There are no CMake presets. Configure, build and test from a checkout:

```bash
cmake -S . -B build -DMICROTEL_BUILD_TESTS=ON
cmake --build build -j"$(nproc)"
ctest --test-dir build --output-on-failure
```

CI configuration lives in `ci/`. PRs run: build matrix (Linux x86_64, ARM64), all test categories (unit / integration / conformance / wire / fuzz), sanitizers (asan / tsan / ubsan), clang-tidy, clang-format, aggregate coverage, diff coverage, test-presence check, SonarQube Cloud (OSS tier).

---

## File ownership (post-M2)

After M2 lands the project skeleton, each `src/<directory>/` is owned per `CODEOWNERS`. Each directory carries a one-screen `README.md` describing:

- What lives there.
- Which interfaces it implements.
- What it depends on (and where the mocks are).
- Test entry points.
- Style notes specific to the directory.

**Read the directory README before editing files in it.** This is your local map.

---

## What to do FIRST in any session

1. Run `git status` and `git log --oneline -20`.
2. Read this file (CLAUDE.md).
3. Determine the current phase (see "Current phase" above).
4. Read the design doc for the area you're touching (see "Authoritative documents").
5. If working in `src/`, read that directory's `README.md`.
6. If touching an interface, read its entry in `docs/interfaces.md`.

---

## What NOT to do

- **Don't add source code in M0** beyond compilable interface stubs.
- **Don't add a runtime dependency** without an ICP.
- **Don't commit milestone work directly to `master`.** Each milestone (M6, M7, …) or sub-task gets its own branch; merge via PR once CI is green.
- **Don't break the build** intending to fix it later.
- **Don't merge without CI green.**
- **Don't bypass the test-presence check** by mislabeling a real change as `[refactor]`.
- **Don't write multi-paragraph commit messages** — short summary, longer body if needed, reference the issue.
- **Don't use `goto`, raw `new`/`delete`, `using namespace` in headers**, or any banned C function.
- **Don't introduce `std::shared_ptr` without justification** in the PR description.
- **Don't write "smart mocks"** — mocks are dumb, fakes have logic.
- **Don't expose upb symbols** outside `src/wire/encoder/` and the leaf's own directory ([ICP 0031](docs/icps/0031-leaf-concentrator-in-v1.3.md)). In the runtime, the `OtlpEncoder` C++ wrapper is the only file that touches upb directly.

---

## Pre-PR CI checklist

Run these locally before opening or pushing to a PR. CI runs Ubuntu with clang-18/g++-13; the local Fedora host compiles with clang-22 and ships clang-format 18.1.8. Formatting and tidy are version-sensitive — and for clang-format that means the **patch** version, not just the major.

### Format

```bash
CLANG_FORMAT=clang-format ci/scripts/format-check.sh
```

If it reports violations, run `clang-format -i <file>` to fix them. The `.clang-format` config sets `AlignConsecutiveDeclarations: None` and `AlignConsecutiveAssignments: None` — **never use manual column alignment**; the formatter removes it and CI fails.

**Match CI's patch version.** CI installs `clang-format-18` from Ubuntu's apt, which is **18.1.3**; Fedora ships **18.1.8**. They disagree on real code — notably where a `<<` chain breaks between two adjacent string literals — so a clean local run is not proof, and that divergence is what turned #288 red. The check that actually matches CI:

```bash
python3 -m venv /tmp/cf && /tmp/cf/bin/pip install -q clang-format==18.1.3
CLANG_FORMAT=/tmp/cf/bin/clang-format ci/scripts/format-check.sh
```

### clang-tidy

```bash
cmake -S . -B build -DCMAKE_EXPORT_COMPILE_COMMANDS=ON -DMICROTEL_BUILD_TESTS=ON
cmake --build build
ci/scripts/tidy-check.sh build
```

`tidy-check.sh` lints `ci/`, `src/`, and `tests/`. The most frequently violated rules:

- **`cppcoreguidelines-avoid-do-while`** — use a `while` loop with a pre-call instead of `do { } while (EINTR)`.
- **`cppcoreguidelines-special-member-functions`** — any class with a user-defined destructor must explicitly declare (or `= delete`) all five special members. `AllowSoleDefaultDtor: true` is set, but only exempts `= default` destructors, not user-defined ones.
- **`modernize-use-designated-initializers`** — aggregate structs must be initialized with `.field = value` syntax in C++20.
- **`modernize-use-scoped-lock`** — use `std::scoped_lock` instead of `std::lock_guard`.
- **`readability-qualified-auto`** — `const auto p = <ptr>` must be `const auto* const p`.
- **`readability-function-cognitive-complexity`** (threshold 15) and **`readability-function-size`** (threshold 3 nesting levels) — extract helpers to reduce deep nesting in SSL/TLS or nghttp2 loops.
- **`hicpp-signed-bitwise`** — bitwise ops on signed POSIX constants (`O_NONBLOCK`, `SOCK_CLOEXEC`) need `// NOLINTBEGIN/END(hicpp-signed-bitwise)` when the call spans multiple lines, or `// NOLINTNEXTLINE(...)` when it fits on one.
- **`readability-static-definition-in-anonymous-namespace`** — `static constexpr` inside an anon namespace is redundant; omit `static`.

### Threading discipline

The EpollReactor interface documents all methods except `Wake()` as I/O-thread-only. In practice, `Connect()` calls `Register()` from the caller thread — EpollReactor guards `m_callbacks` with `std::mutex m_mu` for exactly this cross-thread case. If you add new caller-thread → reactor calls, protect them the same way. TSAN (`-DMICROTEL_SANITIZER=tsan`) catches data races; always run it when touching the reactor or I/O thread.

### Adding new system dependencies to CI

If a new header or library is needed (e.g., a new `apt` package), add it to **all four** CI jobs that build: `tidy-check`, `compile`, `sanitizers`, and `coverage`. Missing it in one job causes an opaque link or include error only on that matrix entry.

---

## Working with the user

- **Iterate.** Propose, get feedback, revise. Don't try to ship multi-component changes in one shot.
- **Ask clarifying questions** when the design docs are ambiguous, before writing code.
- **Surface decisions, don't make them silently.** If a change requires picking between two reasonable approaches, ask which the user prefers.
- **Use the ICP process** for interface changes — don't argue them in PR comments.
- **Keep ICPs short.** A few paragraphs. They're heads-up documents, not multi-week reviews.

## graphify

This project has a knowledge graph at graphify-out/ with god nodes, community structure, and cross-file relationships.

Rules:
- ALWAYS read graphify-out/GRAPH_REPORT.md before reading any source files, running grep/glob searches, or answering codebase questions. The graph is your primary map of the codebase.
- IF graphify-out/wiki/index.md EXISTS, navigate it instead of reading raw files
- For cross-module "how does X relate to Y" questions, prefer `graphify query "<question>"`, `graphify path "<A>" "<B>"`, or `graphify explain "<concept>"` over grep — these traverse the graph's EXTRACTED + INFERRED edges instead of scanning files
- After modifying code, run `graphify update .` to keep the graph current (AST-only, no API cost).
- Corpus exclusions live in [`.graphifyignore`](.graphifyignore) (gitignore syntax; graphify does **not** read `.gitignore`). It already excludes the `build*/` and `_deps/` trees and the committed snapshot `docs/graph-report.md` (which would otherwise self-index). You no longer need to move build dirs out of the tree before refreshing. If you *change* `.graphifyignore`, delete `graphify-out/graph.json` and `graphify-out/manifest.json` first — `graphify update` is incremental and won't prune already-indexed files that are now ignored.
- After merging a PR that changes code (`src/`, `include/`, `leaf/`, `tests/`), or before a release, refresh the committed snapshot **via its own `docs/...` branch + PR**. Docs-only merges don't need one: `graphify update` rebuilds the code graph only, so they barely move the report; batch them into the next code-driven or release refresh. The refresh goes through a PR because `master` is protected (14 required status checks), so a direct `git commit` + push to `master` is rejected. Steps: (1) `graphify update . --force` (the rebuild has fewer nodes, so `--force` is required); (2) `cp graphify-out/GRAPH_REPORT.md docs/graph-report.md`; (3) commit on a `docs/graph-snapshot-*` branch, open a PR, and merge once CI is green.
