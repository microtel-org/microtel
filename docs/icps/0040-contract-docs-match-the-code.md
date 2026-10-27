# ICP 0040: Contract docs match the code (interfaces, RAII, upb, install graph)

**Status:** Draft — 2026-10-27.
**Affected interfaces / docs:**
- `docs/interfaces.md` §3.1, §4.2, §4.3, §4.4
- `docs/memory-model.md` §3.2, §4.2 (both LOCKED)
- `docs/architecture.md` §3 (encoder containment rule, transport "Consumes", `src/common/raii/` entry, glossary)
- `docs/development.md` (the `src/common/raii/` line)
- `docs/icps/0020-install-and-package-config.md` (an amendment: the dependency graph and the Decision 2 table)

**Affected tracks:** docs only. No header, source or behaviour changes.

## Summary

Several LOCKED or normative statements describe names, shapes or a dependency
graph that the code has not had for several releases. This ICP changes the
documents to match the code. Every change below records what was already
built.

## Motivation

The pre-launch doc audit (items 8, 9, 10 and 21) found these. Each was checked
against `master` at `0d76827`. `interfaces.md` §1 says "if a header and this
document disagree, this document wins and the header is the bug". Read that
way, the headers below are bugs. They are not: the documents are stale. ICP
0009 (README) is the earlier example of the same failure.

## Proposed change

**1. `interfaces.md` §4.2–4.4 (item 8).**
- **§4.3 Contract** gains `SendAll`, as accepted in ICP 0007 and declared in
  `include/microtel/internal/wire_codec.hpp`:
  - results are returned in payload order;
  - the default implementation loops over `Send`;
  - `HttpWireCodec` overrides it to submit every payload as a concurrent
    stream before waiting.
- **§4.3 Lifetime** changes from "paired 1:1 with the transport" to "one codec
  per signal (traces, metrics, logs), all three bound to the Provider's one
  transport" (`SdkBuilder`'s `BuildWireCodec` calls). The per-codec
  single-caller rule is unchanged.
- **§4.4 Invariants** changes from "One exporter per `Provider`" to "one
  `IExporter` (the trace exporter) per `Provider`". Metrics and logs have
  their own exporters (`IMetricExporter`, `ILogExporter`), each with its own
  worker thread (`SdkProvider::m_metric_exporter`, `m_log_exporter`).
- **§4.2** drops `tests/fakes/fake_otlp_encoder.hpp`, which has never existed.
  Tests that need encoded bytes use the real encoder.

**2. `memory-model.md` §4.2 and §3.2 (item 9, both LOCKED).**
- **§4.2 naming.** The wrappers expose `Release()`, not `release()`. The
  descriptor wrapper is `UniqueFd`, not `Socket`. `UpbArena` lives in
  `src/wire/encoder/`, not `src/common/raii/`, as CLAUDE.md rule 9 already
  says.
- **§4.2 missing wrappers.** The table gains the three wrappers it leaves out:
  `BioMethod` (`BIO_METHOD*`), `DeflateStream` and `InflateStream` (zlib
  `z_stream`).
- **§3.2 `EncodedPayload`.** The shape is `Bytes()`, `Size()` and
  `Release() &&`, plus a `noexcept` default constructor that makes the empty
  state, as in `include/microtel/internal/encoded_payload.hpp`.
  `interfaces.md` §3.1's recap follows.
- **Same names elsewhere.** `architecture.md` and `development.md` get the
  same names: `UniqueFd` replaces `Socket`, and the wrapper lists include the
  three missing ones.

**Decision needed.** `DeflateStream` and `InflateStream` have no `Release()`,
so they break point 4 of the LOCKED wrapper contract. The proposal is to
restate point 4 as applying to *handle* wrappers (one C handle with a
sentinel empty state). The two stream wrappers are exempt: each owns an
initialised `z_stream` whose only valid disposal is `deflateEnd` /
`inflateEnd`, so handing it out raw has no safe consumer. The alternative is
to add `Release()` to both, which is code with no caller.

**3. upb containment (item 10).** Three statements are reworded:
- `architecture.md` §3: "the **only** place in the codebase that includes upb
  headers";
- `interfaces.md` §4.2: "The only place in the codebase that includes upb
  headers";
- `architecture.md` glossary: "All in `src/common/raii/`", for RAII
  wrappers.

The first two now say "the only *runtime* directory", as
`repository-layout.md` does. They also name the two other places upb is
used:
- `leaf/src/backend_upb.c`, the leaf's upb backend (ICP 0031,
  `coding-standards.md` §15);
- the vendored and generated code under `third_party/upb` and `gen/`.

The rule itself is unchanged. No upb type appears in a runtime header outside
`src/wire/encoder/` (`memory-model.md` §3.1), and none appears in the leaf's
public header.

**4. ICP 0020's graph (item 21).** An amendment section is appended. ICP 0020
stays Accepted, and the README's append-only rule is kept. The amendment
gives the graph as the installed `MicrotelTargets.cmake` exports it at
v1.2.4:
- **Fifteen libraries, not thirteen.** It adds:
  - `microtel_api`, which `microtel_sdk` links PUBLIC and `microtel_encoder`
    links PRIVATE;
  - `microtel_custom_wire` (ICP 0036), which sdk uses and which uses common.
- **Missing edges added:**
  - `grpc_wire → common`;
  - `exporter → encoder`;
  - `upb_gen → upb_runtime`;
  - `transport → OpenSSL, nghttp2`, and sdk's own links to them (#473).
- **The Decision 2 "not supported" row** gains `::api`, `::custom_wire` and
  `::headers`. The leaf targets stay as ICP 0031 defines them.

The Conan recipe's components mirror this graph since #475.

## Migration

None for code. Contributors and agents reading these sections now see the
names the code uses.

CLAUDE.md rule 9 lists the same four wrappers. It omits `BioMethod`,
`DeflateStream` and `InflateStream` but is otherwise correct. Changing
CLAUDE.md is the maintainer's call and is not part of this ICP.

## Rationale & alternatives

- **One ICP or four.** The four items are the same kind of change, normative
  text catching up with built code, and none changes behaviour. Four ICPs
  would be four reviews of one-line renames.
- **Fixing the code instead of the docs.** Renaming `Release()` to
  `release()`, or `UniqueFd` to `Socket`, would break the naming convention
  (PascalCase methods, CLAUDE.md) and every caller, to match text written
  before the code existed.
- **Implementation.** It follows this ICP in its own docs PR, per the process
  in the README. The amended LOCKED markers in `memory-model.md` gain `cites`
  annotations (ICP 0021), for example ``cites `src/common/raii/unique_fd.hpp:Release` ``,
  so `citation-check` keeps them honest.
