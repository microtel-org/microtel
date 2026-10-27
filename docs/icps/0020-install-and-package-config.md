# ICP 0020: `install()` rules, the exported target set, and what `microtel::` means

**Status:** Accepted — signed off 2026-08-31. Decision only; implementation is
M9/M10 work. The decision shapes what M10 must carry rather than being M10.
**Affected interfaces / docs:** the **exported CMake target names become public
API** — that is the substance of this ICP. Also `ci/scripts/symbol-scan.sh`
(scope changes from build tree to install tree); `README.md` /
`docs/development.md` (consumer instructions).
**Affected tracks:** packaging / release (M9–M10).

## Summary

microtel has **no `install()` rules at all**. `cmake --install` produces
nothing, so `find_package(microtel)` is impossible and the only ways to consume
the library are vendoring the source or hand-writing a find module (issue #19).

This ICP decides **what gets exported and under what names**, because those
names are a compatibility surface that cannot change after v1.0. It does not
decide when to implement — that is M9/M10 work — but the decision shapes what
M10 has to carry, and install rules block packaging, which blocks vcpkg and
Conan, which is the actual distribution path.

## What exists to export

Thirteen shipped static libraries, with this dependency graph:

```
microtel_sdk ──┬─ microtel_config
               ├─ microtel_exporter
               ├─ microtel_transport ── microtel_common
               ├─ microtel_http_wire ─┐
               ├─ microtel_grpc_wire ─┴─┬─ microtel_otlp_response
               │                        └─ microtel_gzip ── (system zlib)
               └─ microtel_encoder ───── microtel_upb_gen

microtel_upb_runtime ── microtel_utf8_range        (vendored)
microtel_headers (INTERFACE, carries include/)     (linked by all of the above)
```

`microtel_preflight_lib` is tool-only and is **not** exported.

## Decision 1 — export both the components and an aggregate

Export every component with its real `target_link_libraries` graph, and add
`microtel::microtel` as an `INTERFACE` target over the public set. CMake then
derives link order from the graph, so the ordering sensitivity of a
twelve-archive static closure stops being the consumer's problem — which it
would very much be if we shipped a bare list.

```cmake
find_package(microtel REQUIRED)
target_link_libraries(my_app PRIVATE microtel::microtel)
```

**Correction to a motivating argument.** The case for component granularity is
often "a consumer who wants traces and no metrics gets a smaller closure". That
consumer **cannot be served by this decomposition**, and the ICP should not
imply otherwise: `microtel_sdk` contains the metrics and logs implementations
alongside traces (8 metric sources and 3 log sources in one archive). The
libraries are split by **layer** — sdk / transport / wire / encoder — not by
**signal**. Component targets therefore buy layer-level reuse (linking the
encoder without the SDK, say), not signal-level subsetting.

Signal subsetting would require splitting `microtel_sdk`, which is a much
larger change and is not proposed here. Recording the limitation because the
smaller-closure argument is the project's own pitch, and it does not currently
hold at this seam.

## Decision 2 — one target is supported API

The rest are **exported because the static link closure requires them, and
unsupported**: they may merge, split, or disappear without an ICP.

| Target | Status |
|---|---|
| `microtel::microtel` | **Public — the only supported name.** |
| `microtel::sdk`, `::transport`, `::encoder`, `::exporter`, `::config`, `::common`, `::http_wire`, `::grpc_wire`, `::otlp_response`, `::gzip`, `::upb_gen`, `::upb_runtime`, `::utf8_range` | Exported, **not supported**. Present so the link closure resolves. |

**`microtel::sdk` was demoted at review**, and the reason is this section's own
principle: no component exposes public headers directly, so a consumer never
needs to name one. `microtel::sdk` would be a promise with no use case the
aggregate does not already serve, and the counter-argument — that naming a
component shows which layer you depend on — is answered by the aggregate
resolving to the same closure anyway. **Promotion is available later if someone
asks; demotion is not.**

The line has a principled basis rather than being a judgement call: **no
static component library exposes public headers directly.** Every one of them
gets `include/` through the `microtel_headers` INTERFACE target, and
`include/microtel/*.hpp` is the public API while `include/microtel/internal/`
and everything in `src/` is not. A consumer never needs to name a component to
get a public header — so components are link-graph plumbing, and the aggregate
is the only thing to promise.

"Exported but not supported" is a coherent and common position, but only if it
is **written down**. Left ambiguous, every exported name becomes a de-facto
commitment the first time someone depends on it.

## Decision 3 — the adapters are not installed as binaries

Two INTERFACE targets sit outside the twelve static libraries and need an
explicit position, because both reference `include/` directly and would
otherwise fall through the gaps of Decision 2:

- **`microtel_otelcpp_shim`** — **must not be installed as a binary, ever.**
  ICP 0014 makes it source-only for a hard reason: the shim's ABI depends on
  configuration choices the *consumer* makes when building opentelemetry-cpp
  (`OPENTELEMETRY_STL_VERSION`, `ABI_VERSION_NO`), so the same source compiled
  under two configurations produces link-incompatible symbols. A prebuilt
  artifact would be wrong for most consumers and silently so. Install its
  **headers** if anything; never an archive, and never an exported target that
  implies one.
- **`microtel_spdlog_bridge`** — INTERFACE and header-only. Installing the
  headers is harmless and useful; it adds nothing to the link closure.

**Decided: install adapter headers; export no adapter targets.** A consumer
opts in by adding the shim to their own build, exactly as today.

The concern that shipping headers invites the prebuilt-binary expectation was
considered and rejected: **that expectation comes from an exported target, not
from headers in an include directory**, and no adapter target is exported.
Withholding the headers would mean the shim can only be consumed by pointing at
a source tree — worse ergonomics for what is meant to be the project's main
adoption path.

Instead, `src/adapters/otelcpp/`'s public header carries a comment stating why
there is no binary. A one-paragraph explanation where someone is already looking
is a cheaper answer than absence, which explains nothing.

## Decision 4 — prefix the vendored upb symbols, before v1.0

Exporting `microtel::upb_runtime` means shipping a static library, under our
namespace, containing upstream protobuf's code. Two consequences:

1. **Duplicate symbols with different provenance.** A consumer who also links
   real upb gets two definitions of `upb_*`, with no diagnostic beyond a link
   error or — worse, with static libraries — silent selection of one.
2. **The namespace asserts ownership of code we did not write.** `microtel::`
   in front of vendored protobuf code is at best misleading.

Three options:

- **(a) Prefix the vendored symbols** (`microtel_upb_*`) so collision is
  impossible by construction.
- **(b) Document the hazard and do nothing.**
- **(c) Offer `MICROTEL_USE_SYSTEM_UPB`** so consumers can opt out of the
  vendored copy.

**Decided: (a), and in scope for v1.0.** The cost asymmetry settles it —
prefixing before v1.0 is a build-system change; after, it is an ABI break for
every consumer who linked the exported target. And the duplicate-symbol failure
mode with static libraries — silent selection of one definition, no diagnostic
— is undiagnosable in the field, which is the worst category of defect to ship
knowingly.

**The rule-13 scan update ships in the same change, not separately.**
`ci/scripts/symbol-scan.sh` explicitly permits `upb_*` and `utf8_range_*` as
vendored-closure members; prefixed symbols would fail that allowance. Landing
the prefix without the scan update breaks CI, and landing the allowance first
would silently widen what rule 13 permits. They are one change.

## Decision 5 — `symbol-scan` must follow the install tree

`ci/scripts/symbol-scan.sh` currently scans `libmicrotel_*.a` under the **build
directory**. Once `cmake --install` exists, "shipped" means the **install
tree**, and the gate must scan that instead — otherwise the dependency-closure
claim that the whole project rests on (rule 13) is verified against artifacts
that are not the ones users receive.

This is the closing half of a linkage already noted: the scan asserts what
ships, and until now nothing else defined "ships".

## Decision 6 — a consumer test, or the export set breaks silently

**Export-set breakage is invisible to every in-tree test.** Everything builds
because everything is a subdirectory; a missing `install(TARGETS …)`, a
forgotten transitive dependency, or an unexported vendored archive shows up
only in a consumer that is not part of this build.

Required: a separate CMake project under `tests/consumer/` that
`find_package(microtel REQUIRED)`s, links `microtel::microtel`, compiles a
program against the public headers, and runs in CI.

**It must run against a genuinely installed tree, in its own build
directory** — `cmake --install` to a staging prefix, then configure the
consumer with `CMAKE_PREFIX_PATH` pointing at *that*. Not at the build tree.
The weaker version is easier to write, looks identical in CI, and misses
precisely what this test exists to catch: a missing `install(FILES …)` for
headers, or an export set referencing a target it never installed. Both are
invisible when `find_package` resolves against a build directory that still has
every source file in place.

Without this, the export set is asserted rather than tested — the pattern issue
#134 catalogues.

## Migration

- No source or public-header change; this is packaging.
- `README.md` and `docs/development.md` gain the `find_package` usage.
- The bench harness's in-tree `MICROTEL_BUILD_BENCH=ON` workaround (issue #19)
  can stay; it is not blocked on this and there is no reason to churn it.

## Resolved at review

1. **Aggregate only.** `microtel::sdk` demoted — see Decision 2. Fewer
   promises; promotion is available later, demotion is not.
2. **(a) symbol prefixing, in scope for v1.0**, with the rule-13 scan update as
   part of the same change — see Decision 4.
3. **No `microtel::detail::` prefix.** Two reasons. It makes the names ugly for
   plumbing that CMake resolves automatically and nobody types. More
   importantly, a `detail::` marker on some exported targets invites the
   reading that anything *not* so marked is supported — reintroducing the
   ambiguity somewhere new. Decision 2's principle, written down in the
   package's own documentation, is sufficient; a consumer who links
   `microtel::grpc_wire` anyway did it deliberately.
4. **Install the shim's headers** — see Decision 3.

## Open questions

None outstanding.

## Amendment — 2026-08-31: a thirteenth library, and zlib in the install interface

Request compression (`compression = "gzip"`) shipped after this ICP was signed
off, adding `microtel_gzip` and making **zlib a real link dependency** rather
than a permitted-but-unused member of rule 12's closure. Two consequences for
the install work, neither of which changes a decision above:

1. **The component inventory is thirteen, not twelve.** `microtel::gzip` joins
   the exported-but-unsupported row. Decision 2's principle is unchanged — it
   exposes no public header, so it is link-graph plumbing like the rest.
2. **The generated package config must `find_dependency(ZLIB)`, and this is
   what turns Decision 6 from hypothetical into demonstrated.** zlib reaches
   `microtel_gzip` through a PRIVATE link interface, so it is *not* a usage
   requirement that propagates to consumers — but the static archive still
   carries `U deflate` and needs it resolved at final link. That combination is
   exactly the case that builds cleanly in-tree, configures cleanly under
   `find_package(microtel)`, and fails only in a consumer's link step.

   Nothing in this repository can catch it. Every in-tree target links zlib
   because the top-level `find_package(ZLIB REQUIRED)` is still in scope; only
   a project configured against an installed tree sees the omission. **zlib is
   therefore the first concrete thing Decision 6's consumer test would catch**,
   and the first argument that the test must run against a real
   `cmake --install` output rather than the build directory — against a build
   tree the dependency resolves by accident and the test passes while the
   package is broken.

   OpenSSL and nghttp2 reach consumers the same way and need the same
   `find_dependency` treatment; the gzip work is simply what made the class of
   defect concrete.

## Status addendum — Decision 4 implemented

Decision 4 shipped ahead of the rest of this ICP. It is the only part that is a
v1.0 deadline rather than a v1.0 convenience — after v1.0 the rename is an ABI
break for every consumer who linked the exported target — so it landed on its
own rather than waiting on `install(TARGETS … EXPORT …)` (issue #19). Decisions
1, 2, 3, 5 and 6 remain open.

**Mechanism.** [`third_party/upb/microtel_upb_rename.h`](../../third_party/upb/microtel_upb_rename.h)
is a generated block of `#define <name> microtel_<name>` lines — one per
globally-visible symbol of the vendored upb runtime and utf8_range, 217 on the
v29.4 pin. CMake force-includes it with `-include` on every target that
compiles a translation unit reaching a upb header: `microtel_upb_runtime`,
`microtel_utf8_range`, `microtel_upb_gen`, `microtel_encoder`, and the four
upb-touching test targets.

Three properties made this the cheapest mechanism available. upb v29.4 has no
rename hook of its own, so a preprocessor rename was the only option that does
not fork the vendored source. No source file changes, which keeps `gen/`
byte-identical to protoc's output and leaves the `regen-check` gate untouched —
the rename is a compile option, not an edit. And no upb header reaches a
consumer, because the upb include paths and link dependencies are PRIVATE to
those targets, so there is no ODR hazard from two translation units disagreeing
about a name.

**The list covers global linkage only** (`nm -g`: defined, weak, and
undefined). That is exactly the set a linker can collide; a file-local `static`
helper inside upb's `.c` files is never a candidate when resolving a consumer's
reference. It is also the only stable choice — `UPB_INLINE` functions are
emitted out-of-line at `-O0` and inlined away at `-O2`, and the all-linkage
symbol set differs between clang and gcc at the same optimization level, while
the global set is identical across clang-Debug, gcc-Debug and Release.

**The scan update shipped in the same change**, as Decision 4 required.
`ci/scripts/symbol-scan.sh` gained a second pass that fails on any *unprefixed*
`upb_*`, `_upb_*`, `kUpb_*`, `_kUpb_*`, `kWyhashSalt`, `UPB_linkarr*` or
`utf8_range_*` global in a shipped artifact. The old allowance was only a
comment — the forbidden pattern never matched upb names — so this pass is the
first mechanical enforcement of anything in this area. It is what makes the
rename list *enforced* rather than trusted: a upb pin bump that adds a global
fails the build rather than silently shipping it unprefixed. The header carries
the regeneration recipe.

### Known residual — the `linkarr_upb_AllExts` section

One vendored name cannot be renamed. upb's linked-extension registry lives in
an ELF section whose name is composed by stringification in `upb/port/def.inc`
(`section("linkarr_" #name)`), with the array symbol and the `__start_`/
`__stop_` bounds composed by token-pasting from the same operand. `#` and `##`
suppress macro expansion of their operand, so no `#define` can reach the
composed section name `linkarr_upb_AllExts`. The array symbol
`UPB_linkarr_internal_empty_upb_AllExts` is renamed by an explicit entry in the
list; the section name and its linker-synthesized bounds are not.

Consequence: a consumer linking microtel *and* a real upb gets one shared
`linkarr_upb_AllExts` section holding both extension registries, and
`upb_ExtensionRegistry_AddAllLinkedExtensions` on either side would walk the
concatenation.

**Accepted.** microtel contributes nothing to that section but upb's
zero-filled placeholder array, which the walk already skips on its
`upb_MiniTableExtension_Number(p) != 0` guard, and the OTLP encoder never calls
the linked-extension API. Renaming the section would mean rewriting three
platform variants of upb's macro block — a fork of vendored source, which is
the cost the whole mechanism was chosen to avoid. Revisit if microtel ever
gains a proto extension of its own.

## Status addendum — Decisions 1, 2, 3 and 5 implemented

Issue #19 landed the `install()` rules, the `MicrotelTargets` export set and
the generated package config. Decision 6's consumer test remains open and is
the next piece; until it exists the export set is verified by hand against a
real `cmake --install` prefix.

Four points where the implementation had to decide something this ICP did not
settle. None changes a decision above.

**1. `microtel::api` and `microtel::headers` join the exported-but-unsupported
row.** `microtel_api` was created after this ICP was signed off (issue #168 —
it defines `TraceId::ToHex` / `SpanId::ToHex`, declared on public API types, and
is `PUBLIC` into `microtel_sdk`). `microtel_headers` was always in the graph but
never in Decision 2's table; it is `PUBLIC` on every component, so it cannot be
omitted from the export set. Neither is a supported name. The component
inventory is therefore **fourteen static archives** — thirteen per the earlier
amendment, plus `api` — with `microtel::headers` as a fifteenth exported
`INTERFACE` target carrying the include directories, and `microtel::microtel` as
the sixteenth and only supported one.

**2. `include/microtel/internal/` is installed.** It is not public API and
Decision 2's line is unmoved — but public headers include it (`provider.hpp`
reaches `internal/processor.hpp`, `sdk_builder.hpp` reaches
`internal/sampler.hpp`), so an install that omitted it would ship a header tree
that does not compile. Shipping a file is not a promise about it; the install
rule carries that note where someone would look for it.

**3. The vendored `tl::expected` installs to
`<includedir>/microtel/vendor/tl/expected.hpp`.** `expected.hpp` says
`#include "tl/expected.hpp"`, so *some* directory must put `tl/` on the include
path. Installing it at `<includedir>/tl/` would collide with a consumer's own
tl-expected — in whichever direction the include order fell. The extra `vendor`
level keeps the include resolving while confining it to microtel's own subtree.

**4. The package config installs as `microtelConfig.cmake`, not
`MicrotelConfig.cmake`.** `find_package` derives the filename from the package
name it is given, so `MicrotelConfig.cmake` is only found by
`find_package(Microtel)`. Both issue #19 and Decision 1 spell the usage
`find_package(microtel REQUIRED)`, so the lowercase spelling is the one that
works. The export file keeps the name this ICP gave it (`MicrotelTargets.cmake`)
because nothing searches for it — the config includes it by path.

**Two things the install deliberately does not do.** The preflight *binary*
installs to `<bindir>` (it is the shipped operator CLI of spec §6.4, and
dropping it would have narrowed Decision 5's scan); `microtel_preflight_lib`
stays unexported, as this ICP's "What exists to export" says. And the package
config issues **no `find_dependency(spdlog)`** even when built with
`MICROTEL_USE_SPDLOG=ON`: spdlog is FetchContent'd with `SPDLOG_INSTALL=OFF`
so it is not part of the package, and no installed archive references a spdlog
symbol — the option currently selects a compile-time route that has not landed.
The config records the option as `microtel_WITH_SPDLOG` instead. This stops
being true the moment the M3+ logging route calls into spdlog; issue #190
tracks it.

## Status addendum — Decision 6 implemented

**All of this ICP is now implemented.** The consumer test is
[`tests/consumer/`](../../tests/consumer/): an external CMake project —
`find_package(microtel REQUIRED CONFIG)`, one executable, `microtel::microtel`
and nothing else — driven by
[`ci/scripts/consumer-smoke.sh`](../../ci/scripts/consumer-smoke.sh) and run by
the `consumer-smoke` CI job. The script installs the build tree to a scratch
prefix, configures the consumer against *that* prefix, builds it, **runs** it,
and re-runs `symbol-scan.sh --prefix` over the same tree.

It is deliberately not reachable from the main build: `tests/CMakeLists.txt`
does not `add_subdirectory` it, which Decision 6 requires — a consumer resolved
against a build directory that still has every source file in place is the
weaker test that misses the defect.

Three implementation notes.

**The program asserts the package, not the exporter.** It builds a provider,
takes a tracer, starts and ends a span, hex-encodes the resulting ids (which is
what puts `libmicrotel_api.a` on the link line), flushes and shuts down. The
endpoint is a closed port and every timeout is a few hundred milliseconds, so
it passes offline and cannot flake: a failed `Connect()` is the expected
outcome, and `ForceFlush` / `Shutdown` are asserted only to return a live
status. Behaviour is the unit, integration and conformance suites' job.

**libdir is located, never assumed.** `<prefix>/lib64` on Fedora,
`<prefix>/lib` on Debian/Ubuntu, and multiarch elsewhere — the script finds
`microtelConfig.cmake` under the prefix and passes its directory as
`microtel_DIR`. An install that produces no package config fails the gate
rather than confusing it.

**One job, default configuration.** `MICROTEL_USE_SPDLOG=OFF` changes no
exported target — only the self-describing `microtel_WITH_SPDLOG` variable —
so a CI matrix axis for it would re-assert the same thing.

## Amendment — 2026-10-27: the graph as exported at v1.2.4 ([ICP 0040](0040-contract-docs-match-the-code.md))

The dependency graph under "What exists to export" stopped matching the code
after the gzip amendment above. `microtel_api` and `microtel_custom_wire`
(ICP 0036) were added, and several edges were never drawn. No decision above
changes. This is the graph the installed `MicrotelTargets.cmake` exports at
v1.2.4: fifteen static libraries plus the `microtel_headers` INTERFACE
target. `PRIVATE` edges are exported as `LINK_ONLY`.

```
microtel::microtel (INTERFACE) ── microtel_sdk

microtel_sdk ──┬─ microtel_api                 (PUBLIC)
               ├─ microtel_config
               ├─ microtel_exporter ── microtel_encoder
               ├─ microtel_transport ─┬─ microtel_common
               │                      └─ OpenSSL::SSL, OpenSSL::Crypto, nghttp2
               ├─ microtel_http_wire ─┬─ microtel_otlp_response
               │                      └─ microtel_gzip ── zlib
               ├─ microtel_grpc_wire ─┬─ microtel_otlp_response, microtel_gzip
               │                      └─ microtel_common
               ├─ microtel_custom_wire ── microtel_common
               ├─ microtel_encoder ─┬─ microtel_api
               │                    └─ microtel_upb_gen ── microtel_upb_runtime ── microtel_utf8_range
               └─ OpenSSL::SSL, nghttp2     (its sources include the transport header, #473)

microtel_headers (INTERFACE, carries include/)   (linked by every C++ library above)
```

- **Decision 2's "not supported" row** gains `microtel::api`,
  `microtel::custom_wire` and `microtel::headers`.
  `microtel::microtel` remains the only supported name.
- **The leaf targets** (`microtel::leaf`, `microtel::nanopb_gen` and
  `microtel::nanopb`, behind `COMPONENTS leaf`) are as
  [ICP 0031](0031-leaf-concentrator-in-v1.3.md) defines them, and are not
  redrawn here.
- **The Conan recipe** declares one component per library with these edges
  as its `requires`, so Conan derives the same link order (#475).
