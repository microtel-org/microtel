# ICP 0044: clang-format 18.1.3 pin, and CLAUDE.md edits that sync with CI

**Status:** Draft — 2026-11-17. Retroactive for #539 (merged as `af68f9b`).
**Affected interfaces / docs:**
- `CLAUDE.md`, Pre-PR CI checklist → Format: the "Required version" paragraph, already changed by #539
- `docs/icps/README.md`, "When an ICP is *not* required": a new exemption
- recorded here, no change in this ICP: `.github/workflows/*.yml` (`runs-on: ubuntu-24.04`, and the `clang-format` job's version check) and `docs/development.md` §10, both from #539

**Affected tracks:** none. CI and contributor docs only.

## Summary

Record the clang-format 18.1.3 pin that #539 made, and exempt from the ICP process any CLAUDE.md edit that only brings CLAUDE.md in line with what CI already enforces.

## Motivation

GitHub's `ubuntu-latest` image moved to Ubuntu 26.04, whose apt `clang-format-18` is 18.1.8. The format gate then failed on code no PR had touched (#537, run 44111297427). #539 pinned every job to `ubuntu-24.04`, made the `clang-format` job fail unless it finds 18.1.3, and stated that version in CLAUDE.md and `docs/development.md`.

`docs/icps/README.md` requires an ICP for "changes to durable agent / contributor instructions in `CLAUDE.md`". #539's CLAUDE.md edit had none. The edit made no new rule: it described what the workflow it shipped with enforces. An ICP for that kind of edit records nothing a reviewer can't already see in the diff, and the requirement made a one-line sync look like a process change.

## Proposed change

1. **The pin, recorded.**
   - The format gate requires clang-format **18.1.3** exactly. The `clang-format` job checks it (`REQUIRED_CLANG_FORMAT` in `.github/workflows/ci.yml`) and fails with a clear error on any other version.
   - Every workflow job runs on `ubuntu-24.04`.
   - Moving off that image means choosing a new pinned version and doing one deliberate reformat (#540).
2. **A new exemption** in `docs/icps/README.md`, "When an ICP is *not* required":

   > - Edits to `CLAUDE.md` that only bring it in line with what CI already enforces: a tool version, a check's name, a script's invocation. The PR description must say so and link the CI change. A CLAUDE.md edit that adds, removes or relaxes a rule still needs an ICP.

## Migration

Contributors and agents run the format check with clang-format 18.1.3, as CLAUDE.md's pre-PR checklist already says. A PR that only syncs CLAUDE.md with CI needs no ICP; it adds a note to its PR description instead.

## Rationale & alternatives

- **Exempt nothing; write an ICP for every CLAUDE.md edit.** Rejected: this ICP would then be the template for every tool bump, with no decision in it.
- **Exempt all CLAUDE.md edits.** Rejected: CLAUDE.md holds rules (phase discipline, TDD gates, dependency closure) whose changes are decisions and need the record.
- **Track `ubuntu-latest` and reformat whenever the formatter changes.** Rejected: the formatter would change without anyone deciding it, and every open PR would break at once, as happened on #537.
