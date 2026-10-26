# Contributing to microtel

Thanks for your interest in contributing.

This document is intentionally short — the heavy details live in other documents that you'll be pointed at as needed. Read this once; consult [CLAUDE.md](../CLAUDE.md) and [docs/coding-standards.md](../docs/coding-standards.md) every time you write code.

---

## Project status

The current release is v1.2.4. The architecture has settled, the interfaces in [docs/interfaces.md](../docs/interfaces.md) are locked, and the rules are enforced by CI rather than by review taste.

**External contributions are welcome.** Bugs, fixes, tests, docs and features all take the same route: an Issue or an ICP where one is needed, then a PR that passes CI and a maintainer review. The discipline hasn't loosened — TDD gates, RAII, and the fixed dependency closure are still non-negotiable — but nothing is closed off because of who's typing.

Where the project most wants help:

| Especially welcome | Talk first, then code |
|---|---|
| Bug reports with a minimal reproduction | Changes to a locked interface — open an [ICP](../docs/icps/) |
| Fixes that arrive with the failing test that proves them | New runtime dependencies — an ICP, and a high bar: the closure is the project's whole reason to exist |
| Interop coverage for metrics and logs beyond the pinned collector — see [docs/compatibility-matrix.md](../docs/compatibility-matrix.md) §2 | Large features that aren't on the [roadmap](../docs/roadmap.md) — file a feature request so scope is agreed before you write code |
| Platform, compiler and toolchain ports | Vendor-specific exporters — better as your own repo than as a fork of this one |
| Comparison data with `opentelemetry-cpp` for benchmarks | Style / formatting PRs that don't change correctness — `clang-format` owns that, and CI runs it |
| Issues for ambiguities or contradictions in the design docs | |
| Typo fixes and doc improvements | |

The [roadmap](../docs/roadmap.md) says what's coming and roughly when. If you want something that isn't on it, file an Issue with the feature-request template — well-argued requests shape what a release includes.

---

## Before you start

1. **Read [CLAUDE.md](../CLAUDE.md).** It's titled for AI coding agents but it's the durable rules document for everyone — humans and agents alike. The hard rules are the same regardless of who's typing.
2. **Read the design docs for the area you're touching.** They, the [ICPs](../docs/icps/) and the public headers' Doxygen are the contract. Trace SDK code: [architecture](../docs/architecture.md), the [threading](../docs/threading-model.md), [memory](../docs/memory-model.md) and [error](../docs/error-model.md) models, and the [interface contracts](../docs/interfaces.md). Wire codecs: [gRPC wire protocol](../docs/grpc-wire-protocol.md) and the retry matrix in [error-model.md](../docs/error-model.md) §7. Configuration: [configuration.md](../docs/configuration.md). Metrics, logs, leaf: [metrics](../docs/metrics-design.md), [logs](../docs/logs-design.md) and [leaf / concentrator](../docs/leaf-concentrator-design.md) design. What is planned and out of scope: the [roadmap](../docs/roadmap.md).
3. **Read [docs/coding-standards.md](../docs/coding-standards.md).** This is the SonarQube-aligned ruleset CI enforces.

---

## Reporting bugs

Use GitHub Issues with the **bug report template**. Include:

- microtel version (or commit SHA if you're building from `master`).
- Build configuration (compiler, build type, sanitizers if any).
- Platform.
- Minimal reproduction.
- What you expected vs what happened.

For **security vulnerabilities**, see [SECURITY.md](SECURITY.md). Don't open public issues for security issues.

## Suggesting features

Use GitHub Issues with the **feature request template**. Describe the use case before the proposed solution. If your suggestion would change a public interface, the response will be to open an [Interface Change Proposal](../docs/icps/) — this is a lightweight markdown document, not a heavyweight process.

## Submitting code changes

The project uses pull requests on GitHub. The same process applies to everyone, maintainers included.

1. Fork the repo and create a feature branch.
2. Follow the [coding standards](../docs/coding-standards.md) — CI will reject PRs that don't.
3. **Tests first.** TDD compliance is mechanically enforced (CLAUDE.md rule 3; the gates are described in [ci-architecture.md](../docs/ci-architecture.md)):
   - **Diff coverage gate:** every changed source line in your PR must be covered by a test in the same PR. Threshold: 90% on SDK and encoder; 80% on transport and exporter. The per-PR check (`diff-cover`) enforces the 80% floor across the whole diff; the 90% for SDK and encoder is enforced by the aggregate gate in the same job, which measures each area separately.
   - **Test-presence gate:** any change to `src/**/*.{cpp,hpp}` requires a corresponding change to `tests/**/*.{cpp,hpp}` unless your PR has the `[refactor]` label. The label is spelled with the brackets, applied to the PR (not written in a commit message), and is the manual override for comment-only and formatting-only changes as well — the gate does not try to detect those on its own. Deleting code needs no accompanying test and is exempt automatically. Applying `[refactor]` to a change that is not behaviour-preserving is a CLAUDE.md violation, and reviewers are expected to check the diff rather than the label.
4. **RAII discipline.** No raw `new`/`delete`. Every resource is owned by an RAII type.
5. Open a PR using the template. Fill in the review checklist.
6. CI will run the full gauntlet — build matrix, sanitizers, static analysis, coverage, fuzz smoke. Address any failures.
7. A maintainer reviews and either merges or requests changes.

### Commit messages

- Short summary line (≤ 72 characters).
- Optional longer body explaining *why*, not *what*.
- Reference the issue if applicable: `Fixes #42` or `Refs #42`.

### Interface changes

Once an interface is locked in `docs/interfaces.md` (post-M0 sign-off), changing it requires an [Interface Change Proposal](../docs/icps/). ICPs are short markdown documents — typically a few paragraphs identifying:

- The interface and the change you propose.
- Affected components and tracks.
- Migration path for existing call sites.

ICPs are reviewed and either accepted (merged into `docs/icps/`) or rejected with feedback. Lightweight and visible.

---

## What gets reviewed

Reviewers focus on:

- **API design** — clarity, consistency with the rest of the project, correctness of error model.
- **RAII compliance** — no raw `new`/`delete`, ownership clear, move-only where appropriate.
- **Test coverage** — tests exist (CI gate), and they're meaningful (judgment call).
- **Complexity** — function and file sizes within limits, no clever-but-unmaintainable constructs.
- **Threading and lifetime** — clearly stated for any new type.
- **Security implications** — for changes touching crypto, parsing, or network handling.

Reviewers do *not* spend time on style — that's `clang-format`'s job. PRs that haven't been auto-formatted fail CI before reaching review.

---

## Code of conduct

A formal code of conduct will be adopted before the first public release. In the meantime: be respectful, be constructive, and assume good faith.

## License

By contributing, you agree that your contributions are licensed under the project's [Apache 2.0 license](../LICENSE).

There is nothing to sign up front and no agreement to file.

---

## Local knowledge graph

The project ships with a [graphify](https://github.com/safishamsi/graphifyy) knowledge graph that maps every source file, internal interface, and design concept into a queryable graph with community detection. Agents (Claude Code) use it as their primary navigation tool — it reduces per-query token cost by ~7× vs. reading raw files.

The graph is **not committed to git** (it's large and auto-generated). To bootstrap on a fresh clone:

1. Install the `graphifyy` package into a Python virtual environment (the project convention is `~/.venv`):
   ```bash
   python3 -m venv ~/.venv
   ~/.venv/bin/pip install graphifyy
   ```
2. In Claude Code, run `/graphify` from the repo root. This runs the full pipeline (~2 min on first run) and installs git hooks that keep the graph current on subsequent commits — no LLM cost for incremental updates.

After bootstrap, `graphify-out/` appears in the repo root and is gitignored. Claude Code reads `graphify-out/wiki/index.md` as its codebase map in every session.

A static snapshot of the community report is committed at [docs/graph-report.md](../docs/graph-report.md) for reference without running the pipeline.

---

## Questions

For project questions that don't fit an issue, use GitHub Discussions.
