# ICP 0037: retire `microtel-spec.md` — move what is still in force, rewrite the references, delete it

**Status:** Accepted — 2026-10-02. Implemented in three follow-up PRs (move, rewrite references, delete).
**Affected interfaces / docs:**
- `microtel-spec.md` (deleted; archived at the `v1.2.1` tag)
- `docs/compatibility-matrix.md` (gains the compatibility tiers, spec §2.2)
- `docs/releasing.md` (gains the versioning and ABI policy, spec §19)
- `docs/bench-spec.md` (gains the performance and footprint targets, spec §10)
- `docs/README.md` (the spec-section → current-home table and the archive link)
- every file outside `docs/icps/` that names the spec: about 100 files across
  `docs/`, `src/`, `include/`, `tests/`, `ci/`, `.github/` (issue templates),
  `CMakeLists.txt`, `CLAUDE.md`, `CONTRIBUTING.md` and the other top-level docs

**Affected tracks:** documentation, and comments in code and CI. No code
behaviour, no public or internal interface.

## Summary

`microtel-spec.md` stops existing in the tree. The three parts of it that
still define rules nothing else defines move to the live documents readers
already use; every reference to it outside the ICPs is rewritten to point at
the current home of that topic; then the file is deleted. The text stays
readable at the `v1.2.1` tag, which is where the ICPs' own citations resolve.

## Motivation

The spec was written in M0 as the contract for the v1.0 release and has not
been re-baselined since. Everything after v1.0 was specified elsewhere: the
metrics, logs and leaf design docs, the ICPs, `docs/interfaces.md`,
`docs/configuration.md`. ICPs patched the spec with short annotations
instead of rewriting it. The result reads as wrong to anyone who starts
there: §1 says "v1 is exporter-first with traces as the only signal", §8's
feature table marks metrics, logs and the sugar layer ❌, and §13 carries a
warning that "the implementation has run ahead of" it. Yet `CLAUDE.md`
calls it "Source of truth for everything in v1.0" and `CONTRIBUTING.md`
calls it "the contract". Reviews this week kept tripping over that gap.

A file that is in the tree gets read as current, banner or not. Removing it
ends that; the archive at `v1.2.1` keeps the original rationale available to
anyone who wants it.

Nothing reads the file mechanically: the build, the tests and the checks only
mention it, in comments, one `symbol-scan.sh` error message and the issue
templates. At the time of writing it is named in about 120 files: 6 Markdown
links, 126 filename mentions and 227 "spec §N" mentions.

## Proposed change

Three steps, each its own PR, in this order.

1. **Move the three parts still in force, and only those.** Each move copies
   the text, adapted only where it names a release as current:

   | Spec | What it defines | New home |
   |---|---|---|
   | §2.2 | The compatibility tiers (Tier 1 OTLP wire, Tier 2 data model, Tier 3 API adapter, Tier 4 full SDK conformance), which the roadmap, the migration guide, `metrics-design.md`, `ci-architecture.md` and `control-plane-design.md` all use | a new last section of `docs/compatibility-matrix.md` (last, so §4, which many docs cite, keeps its number) |
   | §19 | Semantic versioning, the ABI policy (source-compatible within a major; binary ABI best-effort within a minor, not guaranteed across minors), release cadence | a new section of `docs/releasing.md` |
   | §10 | The hot-path, exporter, footprint and cold-start metrics, and the v1 footprint targets | a new section of `docs/bench-spec.md` |

2. **Rewrite every reference outside `docs/icps/`.** Each reference points at
   the current home of its topic, per this table, which also goes into
   `docs/README.md` as the key for the ICPs' citations:

   | Spec | Now lives in |
   |---|---|
   | §1–§4 Summary, goals, non-goals, motivation | README "Why it exists"; roadmap anti-goals |
   | §2.2 Compatibility tiers | `docs/compatibility-matrix.md` (moved in step 1) |
   | §5 Architecture | `docs/architecture.md`, threading, memory and error models |
   | §6 API, §8 SDK features | the public headers' Doxygen, `docs/interfaces.md`, README Status |
   | §7 Wire protocols | `docs/grpc-wire-protocol.md` |
   | §9 Build & dependencies | `docs/build-options.md`; `CLAUDE.md` rules 12–13 |
   | §10 Performance targets | `docs/bench-spec.md` (moved in step 1) |
   | §11 Project structure | `docs/repository-layout.md` |
   | §12 Configuration | `docs/configuration.md` |
   | §13, §16–§18 Roadmap, risks, open questions, future | `docs/roadmap.md` and the design docs |
   | §14 Engineering practices | `CONTRIBUTING.md`, `docs/coding-standards.md`, `docs/ci-architecture.md` |
   | §15 Compatibility & interop | `docs/compatibility-matrix.md`, `docs/interop-matrix.md` |
   | §19 Versioning, ABI, cadence | `docs/releasing.md` (moved in step 1) |
   | §19 DCO, security, CODEOWNERS, CI quality gates | `CONTRIBUTING.md`, `SECURITY.md`, `CODEOWNERS`, `docs/ci-architecture.md` |

   A reference that only records history (a milestone number, "v1 scope")
   is reworded or dropped rather than pointed somewhere. `CLAUDE.md`
   "Authoritative documents" and `CONTRIBUTING.md` step 2 name the live
   documents (the design docs, `docs/interfaces.md`,
   `docs/configuration.md`, the ICPs, the roadmap). The issue templates and
   the `symbol-scan.sh` message point at the live documents too. Split by
   area: docs; comments in `src/`, `include/`, `tests/`; CI, templates and
   agent files. Comment-only changes are exempt from the test-presence check.

3. **Delete `microtel-spec.md`.** `docs/README.md` says that "spec §N"
   citations inside ICPs refer to the v1.0 design specification, archived at
   <https://github.com/microtel-org/microtel/blob/v1.2.1/microtel-spec.md>,
   and points at the table above for where each topic lives now. A grep for
   `microtel-spec` and `spec §` outside `docs/icps/` comes back empty, and
   `citation-check.py` and the link checks pass.

**Future ICPs amend the live document.** There is no spec to amend.

## Migration

- **ICPs are the one exception.** `docs/icps/` is append-only by policy, so
  the 19 ICPs that cite the spec keep their citations; they resolve through
  the archive link and the table in `docs/README.md`.
- **Contributors and agents** read the live documents named in `CLAUDE.md`
  and `CONTRIBUTING.md`; anyone who wants the original v1.0 rationale reads
  the archived copy.
- **No code behaviour changes.** Only comments, messages and docs move.

## Rationale & alternatives

- **Freeze it in place with a banner.** Every citation would keep resolving
  inside the repo, but a file in the tree keeps being read as current; that
  is the failure this ICP exists to end. Deleting costs a reference rewrite,
  which is bounded and checkable by grep.
- **Re-baseline the spec as a living v1.x document.** Rejected: a large
  rewrite that would duplicate the design docs and drift again; the design
  docs plus ICPs are already the working record.
- **Delete it without rewriting references.** Rejected: about 100 files
  outside the ICPs would point at a file that is gone, with no key to where
  each topic went.
- **Move nothing, only delete.** Rejected: the tiers and the ABI policy would
  have no current home.
- **Defer to v2.0.** The roadmap treats v2.0 as a re-cut, but the confusion
  is costing review time now and the change touches no behaviour.
