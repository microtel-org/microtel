# Branch Protection and Repository Settings

This document is both a **target** for the GitHub-side configuration that enforces the contribution policy described in [CONTRIBUTING.md](../.github/CONTRIBUTING.md), and a **record** of what is configured today. Where the two differ, each table marks it. Verify the record against the live settings before relying on it — `gh api repos/microtel-org/microtel/branches/master/protection` is the source of truth.

The intent is **layered signaling**, not hard blocking. PRs from outside contributors aren't disabled — that would be hostile to the OSS framing — but several layers prevent unwanted PRs from landing accidentally.

---

## Layer 1: Documentation signals (already in place)

- README.md states the release scope up front — v1.0 is traces; metrics and logs are implemented but experimental.
- CONTRIBUTING.md "Project status" section says external contributions are welcome, with a table separating what to just send from what wants an Issue or an ICP first.
- `.github/PULL_REQUEST_TEMPLATE.md` opens with the pre-submission checklist — tests first, format and tidy locally, ICP for interface changes — that any PR author sees as soon as they hit "Open pull request."

These three signals point most well-meaning contributors the right way before they write code. The remaining layers handle what gets opened anyway.

---

## Layer 2: Branch protection on `master`

The protected branch is **`master`** (the repository's default branch).

Two columns below, deliberately: **Target** is the policy this document
prescribes; **Today** is what is actually configured, read back from
`GET /repos/:owner/:repo/branches/master/protection`. Where they differ, the
target is aspirational and has not been applied. Do not read this table as a
description of current enforcement — read the Today column for that.

| Setting | Target | Today | Notes |
|---|---|---|---|
| Require a pull request before merging | ✅ | ❌ | See "What actually blocks a push" below |
| Require approvals | ✅ 1 | ❌ 0 | Solo maintainer pre-1.0 |
| Dismiss stale approvals when new commits are pushed | ✅ | ❌ | Moot while approvals are 0 |
| Require review from Code Owners | ✅ | ❌ | CODEOWNERS still auto-requests review |
| Require status checks to pass before merging | ✅ | ✅ | **The load-bearing gate** |
| Require branches to be up to date before merging | ✅ | ❌ | `strict: false` |
| Require conversation resolution before merging | ✅ | ❌ | |
| Require signed commits | optional pre-1.0 | ❌ | Tighten before public release |
| Require linear history | ✅ | ❌ | Conflicts with current practice — see note below |
| Require deployments to succeed before merging | n/a | n/a | Not deploying anywhere |
| Lock branch | ❌ | ❌ | Maintainers must be able to merge |
| Do not allow bypassing the above settings | ✅ | ✅ | `enforce_admins: true` |
| Restrict who can push to matching branches | ✅ | ❌ | |

**Required status checks (14, enforced today).** Names must match the job `name:`
in [`.github/workflows/ci.yml`](../.github/workflows/ci.yml) exactly — renaming a
job silently un-requires it:

`clang-format`, `clang-tidy`, `cxx20 / clang`, `cxx20 / gcc`, `asan`, `tsan`,
`ubsan`, `coverage`, `symbol-scan`, `conformance`, `regen-check`,
`version-drift-check`, `citation-check`, `system-deps`

On a docs-only pull request (every changed path is `*.md`, `docs/**` or
`tools/diagrams/**`; see `ci/scripts/classify-change.sh`) the heavy jobs skip
their work but still report: a job skipped by its `if` counts as passing, and
the matrix jobs (`cxx20 / …`, the sanitizers) run with every step skipped.

`regen-check` has been required in the GitHub settings since v1.0 — this
document said otherwise until the drift was caught while reconciling the list
against the live setting. It has no path filter, so it produces a status on
every PR and is safe to require.

### What actually blocks a push

"Require a pull request before merging" is off, so the thing preventing a direct
`git push` to `master` is the combination of **required status checks** and
**`enforce_admins: true`**: a pushed commit has no passing checks, so the push is
rejected — for admins too. That is why milestone work goes through PRs in
practice even though the PR requirement itself is not configured.

The practical consequence: the required-checks list *is* the branch protection
here. Adding or renaming a check changes what can land.

### Open conflict: linear history vs. merge commits

This table targets "Require linear history ✅", and Layer 3 targets "Allow merge
commits ❌". Neither is configured, and current practice is the opposite —
history is a series of `Merge pull request #NNN` commits, and
[CLAUDE.md](../CLAUDE.md) documents the merge-via-PR workflow. Either the
target should change to match practice or practice should change to match the
target; leaving both recorded as ✅ while doing neither is the drift this section
exists to stop.

---

## Layer 3: Repository settings

### Settings → General

| Setting | Value | Reason |
|---|---|---|
| Issues | ✅ enabled | Issues are how we want feedback |
| Discussions | ✅ enabled | For broader architecture conversations |
| Projects | ✅ enabled | For roadmap tracking |
| Wikis | ❌ disabled | Docs live in `/docs` |
| Sponsorships | optional | Decide pre-1.0 |
| Preserve this repository | ✅ | Archive eligibility |

### Settings → Pull Requests

| Setting | Value | Reason |
|---|---|---|
| Allow merge commits | ❌ | Linear history |
| Allow squash merging | ✅ (default) | Squash with PR-title message |
| Allow rebase merging | ✅ | For trivial PRs |
| Always suggest updating pull request branches | ✅ | UX for contributors |
| Allow auto-merge | ✅ | Maintainer convenience |
| Automatically delete head branches | ✅ | Cleanup |

### Settings → Moderation → Code review limits

This is **the closest GitHub gets to "don't let strangers PR me."** Find it under Settings → Moderation → Code review limits.

| Setting | Value | Reason |
|---|---|---|
| Limit who can review pull requests | "Users that have read access" (default) | Don't restrict reviewers |
| Limit who can submit pull requests | **"Users that have explicitly been granted access"** | This blocks random PRs |

**This is the practical lever.** Setting "Limit who can submit pull requests" to "explicitly granted access" means:

- Existing collaborators (you, plus anyone you've added) can submit PRs.
- Anyone else who tries gets blocked at PR submission with a message about repository access.
- It doesn't disable the fork button, but PRs from forks get rejected.

You can lift this restriction when v1.0 ships and you're ready to accept external contributions.

### Settings → Moderation → Interaction limits

GitHub also offers "Interaction limits" under Settings → Moderation → Interaction limits. Options:

- **"Limit to existing users"** (no accounts < 24 hours old)
- **"Limit to prior contributors"** (users who've previously committed)
- **"Limit to repository collaborators"** (only your CODEOWNERS-listed users)

Each lasts up to 6 months (renewable). Use **"Limit to repository collaborators"** as the strongest signal. Combined with the code-review-submission limit above, this is belt-and-suspenders.

---

## Layer 4: CODEOWNERS as the routing layer

[CODEOWNERS](../.github/CODEOWNERS) is the file that GitHub uses to auto-request reviews. It routes everything to `@chanderraja`. Even if a PR is somehow opened, you're auto-requested as the reviewer and the merge is gated on your approval per Layer 2.

When the team grows post-v1.0, replace the default owner with the relevant track owners.

---

## Layer 5: GitHub Actions permissions

Under **Settings → Actions → General → Workflow permissions:**

| Setting | Value | Reason |
|---|---|---|
| Default workflow permissions | `Read repository contents and packages permissions` | Principle of least privilege |
| Allow GitHub Actions to create and approve pull requests | ❌ | Prevents auto-merge bots from approving |

Under **Settings → Actions → General → Fork pull request workflows:**

| Setting | Value | Reason |
|---|---|---|
| Run workflows from fork pull requests | ✅ but require approval for first-time contributors | Saves CI minutes on spam |
| Send write tokens to workflows from fork pull requests | ❌ | Security: forked PRs run with read-only tokens |
| Send secrets and variables to workflows from fork pull requests | ❌ | Security: secrets not exposed to forks |

---

## Layer 6: Issue & PR templates

`.github/ISSUE_TEMPLATE/config.yml` is configured to redirect common questions:

- **Questions about current behaviour** → docs/README.md (the documentation index)
- **Roadmap questions** → docs/roadmap.md
- **Contribution questions** → CONTRIBUTING.md
- **Security issues** → SECURITY.md (private disclosure)

Blank issues are disabled. Anyone opening an issue picks a template, which forces them to think about which kind of feedback they're providing.

---

## What this configuration achieves

- **Strangers can't submit implementation PRs.** Code review submission limit blocks them.
- **Existing contributors (you) can still PR normally.** Layer 5 doesn't apply to you.
- **CI minutes don't get burned by spam PRs.** Fork workflow approval requirement.
- **Secrets stay safe.** Forks don't get write tokens or secrets.
- **You're auto-requested as reviewer for everything.** Via CODEOWNERS.
- **Nothing merges without green CI.** The 14 required status checks are enforced, admins included. (Approval-based gating is a *target*, not configured today — see Layer 2.)
- **External contributors are routed to the right channel** — Issues, Discussions, or an ICP — via templates and config.yml.

This is the right set of layers for a released, publicly developed OSS project.

---

## Now that v1.0 has shipped

The documentation half of opening the gates is done: CONTRIBUTING.md's "Project
status" section and `.github/PULL_REQUEST_TEMPLATE.md` now describe a project
that accepts external PRs, and the README carries no "not accepting PRs"
callout. What's left is on the GitHub settings side:

1. Lift the "code review submission limit" — set back to "anyone with read access."
2. Lift "interaction limits" if set.
3. Keep all other branch-protection settings — those are still good practice post-1.0.
4. Update CODEOWNERS to add other track owners as the team grows.

---

## Settings checklist for first-time setup

When you create the repo:

```
[ ] Make repo public
[ ] Add LICENSE (Apache 2.0)
[ ] Add NOTICE (placeholder)
[ ] Push initial doc bundle (CLAUDE.md, spec, roadmap, etc.)
[ ] Settings → Branches → Branch protection rule on `master` per Layer 2
[ ] Settings → Moderation → Code review limits → "explicitly granted access"
[ ] Settings → Moderation → Interaction limits → "limit to collaborators"
[ ] Settings → Actions → Workflow permissions per Layer 5
[ ] Add SONAR_TOKEN to repo secrets
[ ] Verify CODEOWNERS triggers review request on a test PR (use a draft PR from a branch)
[ ] Set up SonarQube Cloud project in OSS tier
```
