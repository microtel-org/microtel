# microtel Coding Standards

**Status:** First draft (M0 deliverable)
**Authority:** This document is the source of truth for code style and structural rules. Where CLAUDE.md overlaps with this document, this document is canonical for the *details*; CLAUDE.md and the ICPs are canonical for the *rationale*.

---

## 1. Scope and enforcement

These standards apply to all C++ source under `src/`, `include/`, and `tests/`. C code (the leaf library under `leaf/`, experimental in v1.2 per [ICP 0031](icps/0031-leaf-concentrator-in-v1.3.md) and [ICP 0032](icps/0032-release-reorder-v1.1.1.md)) follows §15, which says which of the C++ rules carry over and what replaces the rest.

**Enforcement is mechanical wherever possible:**

- `clang-format` enforces layout. PRs with formatting drift fail CI.
- `clang-tidy` enforces structural rules. PRs with warnings fail CI.
- A **SonarQube Cloud** scan (free OSS tier) flags issues that escape the above and adds inline PR decoration.
- Code review handles judgment calls and design decisions that tools can't catch.

The complete `.clang-format` and `.clang-tidy` configurations live at the repo root and are the operational form of this document. **If `.clang-tidy` and this document disagree, this document wins** — open a PR fixing `.clang-tidy`.

---

## 2. Style — layout

Rules in this section are enforced by `clang-format`.

### 2.1 Braces and blocks

- **Allman braces.** Opening brace on its own line.
- **Always brace** `if`, `else`, `for`, `while`, `do`. Even single-statement bodies.
- One statement per line.

```cpp
// Correct
if (m_queue.full())
{
    DropRecord(record);
    return;
}

// Wrong — missing braces
if (m_queue.full()) DropRecord(record);
```

### 2.2 Indentation

- Four spaces. No tabs.
- Continuation indents align with the opening delimiter where practical, otherwise four spaces.
- **Maximum three levels of indentation per function.** At four levels, refactor — extract a helper, invert a condition with early return, or split the function.

### 2.3 Line length

- 100 columns. Hard limit.
- Long expressions break at logical operators or argument boundaries; the `clang-format` config decides exact placement.

### 2.4 Spacing

- Space after keywords (`if (`, `for (`, `while (`).
- Space around binary operators (`a + b`, not `a+b`).
- No space after `(` or before `)`.
- One space after a comma.
- No trailing whitespace.

### 2.5 Pointer / reference declarations

- Star and ampersand bind to the *type*: `int* p`, `const std::string& s`.
- One declaration per line.

### 2.6 File layout

- Each file ends with a single newline.
- No more than one blank line between code blocks.
- Two blank lines between top-level definitions in a `.cpp` file.

---

## 3. Naming

| Element | Convention | Example |
|---|---|---|
| Type (class, struct, enum, type alias) | PascalCase | `BatchSpanProcessor`, `OtlpEncoder` |
| Function (free or member) | PascalCase | `StartSpan()`, `ForceFlush()` |
| Variable (local, parameter) | snake_case | `int batch_size`, `Span* current_span` |
| Member variable | `m_` prefix + snake_case | `m_queue`, `m_max_export_batch_size` |
| Static member variable | `s_` prefix + snake_case | `s_singleton_instance` |
| Constant (`constexpr`, `const`) | `kPascalCase` | `kDefaultMaxBatchSize` |
| Macro | `SCREAMING_SNAKE_CASE` | `MICROTEL_LIKELY` |
| Namespace | snake_case | `microtel`, `microtel::wire::grpc` |
| Template type parameter | PascalCase | `template <typename T, typename Allocator>` |
| File | snake_case + extension | `batch_span_processor.cpp`, `otlp_encoder.hpp` |

**Rules of thumb:**
- Function names are verbs or verb phrases. Boolean-returning getters are predicates: `IsRunning()`, `HasPendingFlush()`.
- Avoid abbreviations. `m_request` not `m_req`. Exceptions: well-known ones like `m_id`, `m_ptr`, `m_ctx`.
- Don't encode types in names (`m_count`, not `m_int_count`).

---

## 4. Language rules

### 4.1 C++ version

- C++20 throughout. There is no C++17 fallback.
- Prefer modern idioms: `std::span`, `microtel::Expected`, `std::string_view`, designated initializers, structured bindings, concepts, `consteval` / `constexpr`. `microtel::Expected` is a project-local alias (`include/microtel/expected.hpp`) that resolves to `std::expected` when the floor moves to C++23; on C++20 it aliases the vendored `tl::expected`. See [`docs/icps/0002-vendor-tl-expected.md`](icps/0002-vendor-tl-expected.md).

### 4.2 Banned constructs

These are hard prohibitions. clang-tidy or a custom check rejects each.

- **`goto`.** No exceptions.
- **Raw `new` / `delete`.** Use `std::make_unique` / `std::make_shared`. Custom allocators may use `placement new` inside `src/common/raii/` only.
- **Naked `catch(...)`** without rethrow. If you genuinely need to swallow all exceptions (e.g., destructor of an RAII type), document why with a comment.
- **`using namespace` at file scope in headers.** Function-scope `using` declarations in `.cpp` files are fine.
- **Unnamed namespaces in headers.** Use named namespaces or static.
- **Multiple inheritance** of non-interface classes. Mix-in interfaces (pure abstract) are allowed.
- **`friend` declarations** other than for testing (where `friend class FooTest` is acceptable).
- **`std::auto_ptr`, `boost::scoped_ptr`, homegrown smart pointers.**
- **C-style casts** (`(int)x`). Use `static_cast`, `reinterpret_cast`, `const_cast`, `dynamic_cast`.
- **Unsafe C functions:** `strcpy`, `strcat`, `sprintf`, `gets`, unbounded `scanf` family. Use `snprintf`, `std::format`, or C++ alternatives.
- **`std::endl`** in hot-path logging — flushes unnecessarily. Use `"\n"`.

### 4.3 Required constructs

- **All non-static class members initialized** at declaration or in every constructor. Default initializers in the class definition are preferred.

```cpp
// Correct
class Batch
{
public:
    Batch() = default;

private:
    std::size_t m_size = 0;                    // default-initialized
    std::chrono::steady_clock::time_point m_first_record_time;
    std::vector<Span> m_spans;
};
```

- **Const correctness.** Methods that don't mutate state are `const`. Parameters passed by reference are `const&` unless the function modifies them. Local variables that don't change are `const`.
- **`override`** on every overriding member function. `final` where further overriding is unwanted.
- **`explicit`** on single-argument constructors and conversion operators unless implicit conversion is intentional.
- **`noexcept`** on:
  - Hot-path methods (`StartSpan`, `SetAttribute`, `AddEvent`, `End`).
  - Move constructors and move assignment operators.
  - Destructors (always — by language default — but state it for emphasis on RAII types).
  - Swap functions.
- **`[[nodiscard]]`** on functions returning a status that the caller must check (`Build()`, `ForceFlush()`, `Shutdown()`).

### 4.4 Templates

- Prefer concepts to enable_if where C++20 constraints suffice.
- Avoid SFINAE tricks unless concepts can't express the constraint.
- Don't put template implementations in `.cpp` files unless the type list is closed and explicit instantiation is used.
- Template metaprogramming: keep it simple and commented. If you need >50 lines of `if constexpr` chains, the design likely needs a rethink.

### 4.5 Exceptions

Per CLAUDE.md rules 14–16 and [`error-model.md`](error-model.md) §2:

- **Initialization paths can throw** or, preferred, return `microtel::Expected<T, Error>` (alias — see ICP 0002).
- **Hot-path methods are `noexcept`.** Drop the record, increment diagnostics, return.
- **Destructors are `noexcept`.** They invoke `Shutdown` with a finite timeout if not already shut down.
- Catch by `const&`. Never catch by value.
- Don't throw from across thread boundaries — handle the error in the thread that produced it; surface diagnostics, not exceptions, to the consumer thread.

---

## 5. RAII discipline

The full RAII rules are CLAUDE.md rules 5–11. Recap:

- **Every resource** (heap memory, file descriptors, sockets, mutexes, threads, OpenSSL contexts, nghttp2 sessions, upb arenas, callback registrations) is owned by an RAII type.
- **No raw `new`/`delete`** in production code.
- **`std::unique_ptr`** for unique ownership; **`std::shared_ptr`** only with justification; **`std::weak_ptr`** for cycles.
- **Raw pointers are non-owning by definition.**
- **Move-only by default** for resource-owning types.
- **Rule of zero** where possible; **rule of five** when custom resource handling is required (all five members specified, no compiler-generated mix).

Custom RAII wrappers live in `src/common/raii/`. The authoritative list, and the wrapper contract, is [`memory-model.md`](memory-model.md) §4.2 ([ICP 0040](icps/0040-contract-docs-match-the-code.md)). Each is move-only with a `noexcept` destructor, and has a `Release()` method for explicit ownership transfer where a C API or caller can take ownership of the resource.

`UpbArena` follows the same contract but lives in [`src/wire/encoder/`](../src/wire/encoder/), not `src/common/raii/`: including it means including a upb header, and upb is confined to the encoder directory (rule 13).

---

## 6. Complexity limits

These are SonarQube-aligned limits. clang-tidy enforces a subset on every PR; the SonarQube Cloud scan catches the rest.

| Metric | Limit | Notes |
|---|---|---|
| Cognitive complexity | ≤ 15 per function | the SonarQube definition; counts nesting and break in linear flow |
| Cyclomatic complexity | ≤ 10 per function | control-flow paths through the function |
| Function length | ≤ 75 lines | excluding declaration and braces; refactor at length |
| File length | ≤ 750 lines | refactor at length; split by responsibility |
| Function parameters | ≤ 7 | beyond 7, use a struct |
| Indentation depth | ≤ 3 levels | refactor at 4+; extract helpers, invert conditions |
| Inheritance depth | ≤ 3 levels | including the abstract interface root |
| Template recursion depth | ≤ 5 levels | metaprogramming sanity |

---

## 7. Other anti-patterns

### 7.1 No magic numbers

```cpp
// Wrong
if (queue.size() > 8192) { /* ... */ }

// Correct
constexpr std::size_t kDefaultMaxQueueSize = 8192;
if (queue.size() > kDefaultMaxQueueSize) { /* ... */ }
```

### 7.2 No nested ternaries

```cpp
// Wrong
auto x = a > b ? (c > d ? 1 : 2) : (e > f ? 3 : 4);

// Correct
int x = 0;
if (a > b)
{
    x = c > d ? 1 : 2;
}
else
{
    x = e > f ? 3 : 4;
}
```

### 7.3 No empty if/else/catch blocks

If a branch is intentionally empty, document why with a comment. CI rejects empty branches without comments.

### 7.4 No commented-out code in commits

If code is removed, it's removed from history (or kept in a clearly-marked branch). PRs that include commented-out code blocks fail review.

### 7.5 No `std::endl` in hot paths

It calls `flush()`. Use `"\n"`.

### 7.6 No `auto` for simple types

`auto` is encouraged for complex types (iterators, lambdas, return types of factory functions) but discouraged for simple types where it obscures intent:

```cpp
// Discouraged
auto count = 5;

// Preferred
int count = 5;

// Encouraged
auto it = m_map.find(key);
auto provider = SdkBuilder().Build();
```

---

## 8. Documentation

### 8.1 Public APIs

Every non-trivial public API has a Doxygen comment block explaining:

- What the function does (one sentence).
- Parameter semantics, including ownership and lifetime.
- Return value semantics.
- Pre-conditions and post-conditions.
- Threading guarantees (thread-safe? thread-affine? not safe to call after Shutdown?).
- Error/exception behavior.

```cpp
/// Start a new span attached to the current trace context.
///
/// @param name  Span name. Copied; caller may free immediately on return.
/// @param opts  Optional start options (start time, parent span, attributes).
///              Pass `{}` to use defaults. The contents are copied.
///
/// @return A non-null span handle. The span is open until `End()` is called
///         or the handle goes out of scope (RAII auto-end).
///
/// @threadsafety Thread-safe. May be called from any thread, including
///               concurrently from multiple threads.
///
/// @noexcept Always succeeds. On any internal failure (queue full,
///           span limits exceeded), returns a no-op span; the caller's
///           code path is unchanged.
[[nodiscard]] std::unique_ptr<Span> StartSpan(std::string_view name,
                                               const StartSpanOptions& opts = {}) noexcept;
```

### 8.2 Internal APIs

Internal APIs (anything in `src/` not exposed via `include/microtel/`) have lighter Doxygen:

- One-line description above the function.
- Parameter notes only when non-obvious.
- Threading and error notes when non-obvious.

### 8.3 Comments

- Comments explain **why**, not **what**. The code shows what; the comment explains why.
- TODO comments include an issue reference: `// TODO(#42): handle GOAWAY mid-batch`.
- Don't comment out commits — see 7.4.

### 8.4 File headers

Every source file starts with:

```cpp
// Copyright (c) <year> The microtel Authors.
// SPDX-License-Identifier: Apache-2.0
```

No more than that. No long license preamble in every file.

---

## 9. Includes

### 9.1 Order

Per the LLVM convention, includes are ordered:

1. The header corresponding to this source file (in `.cpp` only).
2. Project headers (`#include "microtel/..."` or `#include "..."` for in-tree).
3. Third-party headers (`<nghttp2/nghttp2.h>`, `<openssl/ssl.h>`).
4. C++ standard library (`<vector>`, `<chrono>`).
5. C standard library (`<cstdint>`).

Each group separated by a blank line. Within each group, alphabetical.

```cpp
// in src/wire/http/http_codec.cpp:

#include "wire/http/http_codec.hpp"

#include "common/logging.hpp"
#include "wire/encoder/otlp_encoder.hpp"

#include <nghttp2/nghttp2.h>

#include <chrono>
#include <string>
#include <vector>

#include <cstdint>
```

### 9.2 Forward declarations vs `#include`

Prefer forward declarations in headers when only a pointer or reference to the type is used. Include the full type when its size or members are needed.

### 9.3 Include guards

Use `#pragma once`. (`clang-tidy` validates.)

---

## 10. Threading

Thread-safety expectations are documented per type. Three categories:

| Category | Meaning | Doxygen tag |
|---|---|---|
| Thread-safe | Any thread may call any method concurrently | `@threadsafety Thread-safe` |
| Thread-affine | Methods must be called from a specific thread (caller / exporter worker / I/O) | `@threadsafety Caller-thread only` |
| Externally synchronized | Caller is responsible for serializing access | `@threadsafety Externally synchronized` |

The threading model document (`docs/threading-model.md`) maps each component to a category.

---

## 11. Testing

Tests follow the same standards as production code with these exceptions:

- Test files live under `tests/`.
- Test functions may be longer (75-line limit relaxes to 200).
- Magic numbers in test data are acceptable (the test expresses what's expected).
- Mock and fake classes may use `friend` declarations into the production type to access internals.
- A `_test_only` suffix on a class member opts it out of some rules where the construct exists for testability.

---

## 12. clang-tidy mapping

The `.clang-tidy` config at the repo root is the operational expression of these rules. Quick summary of which checks enforce which sections:

| Check group | Enforces |
|---|---|
| `bugprone-*` | catches likely bugs (uninit members, dangling refs, sizeof misuse) |
| `cert-*` | C/C++ Coding Standard rules |
| `cppcoreguidelines-*` | most of §4.2, §4.3, §5 (RAII), §6 (complexity) |
| `hicpp-*` | High-Integrity C++ rules — overlaps with cppcoreguidelines |
| `modernize-*` | C++20 idioms (concepts, span, designated init) |
| `misc-*` | misc style and smell checks |
| `performance-*` | performance anti-patterns |
| `readability-*` | naming, complexity, redundancy |
| custom microtel checks | RAII pattern enforcement, banned-function detection |

The config disables checks that conflict with project decisions (e.g., we don't use `gsl::not_null`, so `cppcoreguidelines-pro-bounds-pointer-arithmetic` is tuned).

---

## 13. SonarQube ruleset

The SonarQube Cloud scan runs on the project's OSS tier (free for public/open-source repositories) and uses the default C++ rule set with these adjustments:

- Severity raised to `Critical` for: complexity-over-limit, banned-function-use, raw-new-or-delete-in-production.
- Severity lowered to `Info` for: doxygen-comment-on-private-method, blank-line-after-brace.
- Disabled: rules covering features we don't use (RTTI tags on classes that don't use RTTI, etc.).

The SonarQube config lives at `sonar-project.properties` at the repo root — that location is required for SonarCloud's automatic analysis to discover it. The scan runs on every PR and posts inline findings as PR comments; the quality gate fails the PR on critical or blocker issues.

---

## 14. Review checklist

For non-trivial PRs the reviewer walks this checklist mentally:

- [ ] Tests exist and exercise the new behavior (CI gate also enforces).
- [ ] RAII discipline followed (no raw new/delete, ownership clear).
- [ ] Complexity within limits; long functions split.
- [ ] Public API has Doxygen with threading and error info.
- [ ] No banned constructs.
- [ ] Const correctness applied.
- [ ] Naming follows §3.
- [ ] No magic numbers.
- [ ] No commented-out code.
- [ ] Threading category clearly stated for any new type.

This checklist is a paste-able comment template (lives in `.github/PULL_REQUEST_TEMPLATE.md`).

---

## 15. C code (the leaf library)

This section covers the C source under `leaf/`, the microtel-leaf library of [ICP 0031](icps/0031-leaf-concentrator-in-v1.3.md) Decision 3. Its design is [`docs/leaf-concentrator-design.md`](leaf-concentrator-design.md) §1 and §2, and the section numbers below refer to that document. The C API is experimental in v1.2.

**What carries over from the C++ rules:** layout (§2, enforced by `clang-format` with the repository config), the complexity limits (§6), the anti-patterns in §7 that make sense in C (magic numbers, nested ternaries, empty blocks, commented-out code), public-API documentation (§8.1), `goto` and the unsafe C functions of §4.2, and the review checklist (§14). **What does not:** the rest of §4 (the C++20 language rules), §5 (RAII), and the C++ naming table in §3. §15.1, §15.2 and §15.5 replace them.

### 15.1 Language level

- **C11**, built with `-std=c11 -pedantic-errors` (design §1.2 and §9 decision 11). C11 rather than C99 because `_Static_assert` and `<stdalign.h>` are what make the size guards in §15.3 checkable.
- **No compiler extensions.** No VLAs, no `alloca`, no GNU statement expressions, and no `__attribute__` in the public header.
- **Freestanding-safe libc only:** `memcpy`, `memmove`, `memset` and `memcmp`. No `stdio`, no `malloc`, and no `strlen` on caller input.
- `leaf/include/microtel/leaf.h` is valid C11 **and** valid C++. It carries `extern "C"` guards so the concentrator's tests can include it.

### 15.2 Ownership: caller buffers and init / free pairs

This replaces RAII (§5), which C cannot express.

- **The library never allocates.** Every byte it uses belongs to a buffer or arena the caller owns and passes in with its size: the `microtel_leaf_t` state, the record buffer, the output buffer, and the upb backend's optional scratch arena (design §1.4). The one exception is written down in design §2.2: the upb backend with no scratch buffer uses a heap arena, created and destroyed within one encode call. The nanopb build never touches a heap.
- **Every init has a free.** Each type that is initialised has a paired `_free` function, even when it has nothing to release (`microtel_leaf_free`, design §1.5). `_free` clears the state, so a later call is detected and returns `MICROTEL_LEAF_ERR_STATE` rather than being undefined behaviour. A failed `_init` has no side effects.
- **Pointers are borrowed.** A pointer parameter is valid only for the duration of the call, as in CLAUDE.md rule 7. Data that must outlive the call, such as names, attribute keys and values, is **copied** into the record buffer.
- **Every string comes with its length** (`const char *s, size_t len`). No function scans caller memory for a terminator.
- A backend keeps no state between calls (design §2.2).

### 15.3 Caller-allocated state and `_Static_assert`

Firmware often links a prebuilt `libmicrotel_leaf.a` against a header from a different release. If the library writes more bytes than the caller's header allocated, it silently corrupts a neighbouring object on an MCU with no MMU. So (design §1.4):

- **Opaque storage.** A public state type is a struct with one private array member whose size is a public macro (`MICROTEL_LEAF_STATE_WORDS`). The implementation casts the storage to its real struct and proves at compile time that the struct fits and is aligned:

  ```c
  _Static_assert(sizeof(struct microtel_leaf_internal_state) <= sizeof(microtel_leaf_t),
                 "leaf state outgrew MICROTEL_LEAF_STATE_WORDS");
  _Static_assert(alignof(struct microtel_leaf_internal_state) <= alignof(microtel_leaf_t),
                 "leaf state needs stronger alignment than microtel_leaf_t");
  ```

  Backend-specific state never goes in this storage, so its size is the same for both backends.
- **The `sizeof` guard.** Any caller-allocated struct whose size can change between releases reaches the library together with its size. Either the call takes an explicit size argument (`microtel_leaf_init(leaf, sizeof(microtel_leaf_t), ...)`, `microtel_leaf_get_counters(leaf, &out, sizeof(out))`), or the struct's first field is `struct_size`, set by the caller to `sizeof` the struct (`microtel_leaf_config_t`). The library checks the size **before writing anything**. If it is too small, the call returns `MICROTEL_LEAF_ERR_ARG` and changes nothing. An output struct is written only up to the size given. For a `struct_size` input, fields past `struct_size` read as zero.
- **Handles are fixed-width scalars** (`microtel_leaf_span_t` is a `uint32_t`), not structs. A handle is passed by value, and a change in its size would change the calling convention of every function that takes one, which no runtime check can catch.
- **Frozen layouts are asserted.** When a layout is frozen for 1.x (for example `microtel_leaf_kv_t`), a `_Static_assert` checks that any value type added later still fits it.
- In general, `_Static_assert` checks every compile-time layout assumption the code makes. Use it rather than a runtime check wherever the compiler can decide.

### 15.4 Errors

- Functions return `microtel_leaf_status_t`. No `abort`, no `exit`, and no failure path that leaves the leaf unusable.
- The hot-path rule (CLAUDE.md rule 14) in C form: a failed span, attribute or event call **drops the item, counts it, and returns an error code the caller may ignore** (design §1.7).

### 15.5 Naming and symbols

| Element | Convention | Example |
|---|---|---|
| External function | `microtel_leaf_` + snake_case | `microtel_leaf_encode` |
| Cross-file internal function or type | `microtel_leaf_internal_` + snake_case | `microtel_leaf_internal_encode_upb` |
| File-local function | `static`, snake_case, no prefix required | |
| Public type | `microtel_leaf_` + snake_case + `_t` typedef | `microtel_leaf_config_t` |
| Macro, enumerator | `MICROTEL_LEAF_` + SCREAMING_SNAKE_CASE | `MICROTEL_LEAF_ERR_NO_SPACE` |

- **Every external symbol starts with `microtel_leaf_`** (design §1.9). The leaf's `symbol-scan` pass fails on any other global, apart from vendored code, which ships renamed (`microtel_upb_*` per ICP 0020, `microtel_pb_*` per ICP 0031 Decision 4). The pass also fails on any C++ runtime symbol.
- **Encoder headers are confined.** No `upb_*` or `pb_*` type or symbol appears in the public header. `src/backend_upb.c` is the only file that includes upb headers, and `src/backend_nanopb.c` the only one that includes nanopb headers.

### 15.6 Static analysis

`clang-format` covers `leaf/` with the repository config. `tidy-check.sh` lints `leaf/` with a C profile (design §7.8): the `bugprone-*` checks, the C checks of `cert-*`, `readability-function-cognitive-complexity` (threshold 15), `readability-function-size` (nesting 3) and `hicpp-signed-bitwise`, and none of the C++-only groups. The SonarQube limits of §6 apply unchanged: cognitive complexity 15, cyclomatic complexity 10, nesting 3, and at most 7 parameters. Named constants use `#define` or an `enum` constant, because C11 has no `constexpr`.

---

## 16. Updating this document

This document is versioned with the rest of the project. Changes follow the ICP process when they introduce new banned constructs or change limits — anything that would make existing code non-compliant. Adding new optional guidance or examples is normal-PR territory.
