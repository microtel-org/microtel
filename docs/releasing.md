# Releasing microtel

The release procedure as actually practiced. It is short on purpose: microtel has no release automation, so every
step below is something a person does and something a person can forget.

The mechanical half is [`ci/scripts/version-drift-check.sh`](../ci/scripts/version-drift-check.sh),
run on every PR by the `version-drift-check` job. It catches exactly one class of
mistake — a version bumped in some places and not others — which is the mistake
this project has actually made.

---

## 1. Bump the version

`project(microtel VERSION …)` in the top-level `CMakeLists.txt` is the
authority. Six other literals are hand-written and must be edited in the same
commit:

| Location | Literal | Reaches |
|---|---|---|
| [`CMakeLists.txt`](../CMakeLists.txt) | `project(microtel VERSION …)` | `PROJECT_VERSION`, and through it `microtelConfigVersion.cmake` — what `find_package(microtel …)` matches against |
| [`include/microtel/version.hpp`](../include/microtel/version.hpp) | `kVersionString` | public API |
| [`include/microtel/version.hpp`](../include/microtel/version.hpp) | `kVersionMajor` / `kVersionMinor` / `kVersionPatch` | public API |
| [`src/wire/grpc/grpc_wire_codec.cpp`](../src/wire/grpc/grpc_wire_codec.cpp) | `kUserAgent` (`"microtel-cpp/<version>"`) | **the wire** — the `user-agent` header on every gRPC export ([`grpc-wire-protocol.md`](grpc-wire-protocol.md) §2.1) |
| [`tools/preflight/preflight.cpp`](../tools/preflight/preflight.cpp) | `kVersion` | **the wire** — the `microtel.version` span attribute and the preflight tracer's version |
| [`leaf/include/microtel/leaf.h`](../leaf/include/microtel/leaf.h) | `MICROTEL_LEAF_VERSION_MAJOR` / `_MINOR` / `_PATCH` | the leaf C API; `version-drift-check.sh` checks it (from v1.2.0), and `LeafVersionTest` fails when it disagrees with `version.hpp` |
| [`leaf/CMakeLists.txt`](../leaf/CMakeLists.txt) | `project(microtel_leaf VERSION …)` | the standalone leaf build's `PROJECT_VERSION`; read 1.1.1 through the v1.2.0 release until `version-drift-check.sh` started checking it |

The README names the release in two prose places: the Status section ("The
current release is …") and the `git clone --branch` tag in Getting started.
`CONTRIBUTING.md` names it once ("The current release is v…"). The README's
Install section pins it twice more: the FetchContent snippet's `GIT_TAG` and
the CPM snippet's `gh:microtel-org/microtel@X.Y.Z`. Update all five in the same
commit. `version-drift-check.sh` does not read them, but
`tests/unit/version_test.cpp` does (from v1.2.2), so a missed one fails `ctest`.
The two consumer projects pin the same tag — `GIT_TAG` in
`tests/consumer_fetchcontent/CMakeLists.txt` and `@X.Y.Z` in
`tests/consumer_cpm/CMakeLists.txt` — and `version_test` holds them to it too
(from v1.2.3). CI overrides both pins with the checkout under test, so bumping
them before the tag exists is safe.

Two of those seven have a history worth knowing:

- The gRPC user-agent read `microtel-cpp/0.1.0` for several milestones while
  `version.hpp` said something else. It now carries a `static_assert` against
  `kVersionString`, so that one pair fails to compile if it drifts.
- The preflight literal has **no** compile-time guard. For a long stretch it
  disagreed with the project version and only became correct by coincidence,
  not by being bumped. It was not part of the documented bump until this file
  existed.

### Why generate nothing

Deriving `version.hpp` from `PROJECT_VERSION` at configure time was considered
and rejected. It would turn a public header into a build artifact, and three
consumers read it as a plain source file: the header-only `microtel_headers`
target, the M0 header check, and the install surface (`include/` is installed
verbatim). Changing that to save one hand-edit per release is a bad trade. A
check costs one CI job and changes nothing about what ships.

### Why `master` has no `-dev` suffix

Between releases, `master` stays at the version that was last released — today,
`1.2.3`. It is **not** bumped to `1.3.0-dev` or similar.

The reason is that the version is not only metadata: it goes out on the wire.
`kUserAgent` becomes the gRPC `user-agent` header on every export, and `kVersion`
becomes the `microtel.version` span attribute that preflight writes. A `-dev`
suffix on `master` would mean every span exported from a `master` build — every
developer run, every CI run, every downstream consumer pinning a commit rather
than a tag — arrives at the collector labelled `microtel-cpp/1.3.0-dev`.
Collector-side rules that match on the user-agent would see a version that was
never released, and operators reading `microtel.version` in their traces would
see a version they cannot look up.

The cost of this choice is that a commit on `master` is not distinguishable from
the release tag by version string alone. That is the intended trade: the git SHA
is the right identifier for an unreleased build, and the bug-report template
([`.github/ISSUE_TEMPLATE/bug_report.md`](../.github/ISSUE_TEMPLATE/bug_report.md))
already asks for "microtel version (or commit SHA)" for exactly this reason.

The version therefore moves exactly once per release, in the release commit.

### Verify

```bash
ci/scripts/version-drift-check.sh
```

Every literal must agree with `PROJECT_VERSION`. The same script run with
`--self-test` exercises the gate itself against synthetic fixtures; CI runs both.

---

## 2. Package-config compatibility mode

`write_basic_package_version_file(… COMPATIBILITY …)` at the foot of
`CMakeLists.txt` is `SameMajorVersion`: semver says a
`find_package(microtel 1.1)` requirement is satisfied by any later 1.y.

Revisit this line only at a **major** bump. A minor or patch release does not
touch it.

---

## 3. Update `SECURITY.md`

[`SECURITY.md`](../.github/SECURITY.md) carries a supported-versions matrix. Add or amend
the row for the new release, and demote whatever it supersedes, following the
policy stated below the table (latest minor of the latest major fully supported;
previous major gets security fixes for 18 months per `docs/roadmap.md` §2).

---

## 4. Commit, merge, tag

`master` is protected — a direct push is rejected because a pushed commit has no
passing status checks and `enforce_admins` is on (see
[`docs/branch-protection.md`](branch-protection.md)). So the release commit
goes through a PR like anything else:

```bash
git switch master && git pull
git switch -c release/vX.Y.Z
# edit every literal and pin in §1 + SECURITY.md
ci/scripts/version-drift-check.sh
git commit -am "chore: vX.Y.Z"
# plus the published benchmark snapshot — see §5
gh pr create --title "release: vX.Y.Z" --milestone "…" --body "…"
# merge once CI is green
```

The branch is `release/vX.Y.Z` and the PR title `release: vX.Y.Z`, because a
release PR carries more than a chore and the title is what the milestone
listing shows. The *commit* subject stays `chore: vX.Y.Z`.

### `test-presence` will fail, and `[refactor]` is not the answer

The bump edits `src/wire/grpc/grpc_wire_codec.cpp`, so `test-presence` demands a
matching `tests/**` change and fails without one. **Do not reach for the
`[refactor]` label** — a version bump changes what goes out on the wire, so
labelling it a pure refactor is exactly the mislabelling `CLAUDE.md` forbids.

Write the test instead. There is usually one worth having: until 1.1.0 the
user-agent test only asserted the header was non-empty, so nothing checked that
the header the codec *emits* carries `kVersionString` — the `static_assert`
only constrains the constant. Assert the composed value
(`"microtel-cpp/" + kVersionString`) rather than the release literal, so the
test pins the relationship and does not need editing at the next bump.

Then tag the **merge commit on `master`**, not the branch head:

```bash
git switch master && git pull
git tag -a vX.Y.Z -m "microtel vX.Y.Z"
git push origin vX.Y.Z
# wait for the release workflow (below), then:
gh release edit vX.Y.Z --notes "…"
```

Tag names are `vX.Y.Z`. Early development tags carried a milestone suffix
(`v0.2.0-m2`); new tags don't.

### The source archive

Pushing the tag runs [`.github/workflows/release.yml`](../.github/workflows/release.yml).
It uses [`ci/scripts/source-archive.sh`](../ci/scripts/source-archive.sh) to build
two files from the tag:

- `microtel-X.Y.Z.tar.gz`, a `git archive` of the tag under a
  `microtel-X.Y.Z/` directory. microtel has no submodules: upb, utf8_range,
  nanopb, `tl::expected` and the generated protos are all committed, so the
  archive is complete. The `export-ignore` paths in `.gitattributes` (`ci/`,
  `.github/`, dotfiles) are left out.
- `microtel-X.Y.Z.tar.gz.sha512`, its `sha512sum` line.

The workflow fails if the tag is not `v` plus `project(microtel VERSION …)`.
It then extracts the archive, builds and installs it, and runs
`tests/consumer` against the install (`ci/scripts/consumer-smoke.sh`). Only
then does it attach both files to the GitHub Release for the tag. If that
release doesn't exist yet, the workflow creates it with placeholder notes.
That is why the step above *edits* the release rather than creating it.
If you run `gh release create` before the workflow finishes, the workflow
uploads to your release instead.

To attach an archive to a tag that predates the workflow, run it by hand with
that tag (Actions → release → Run workflow, or
`gh workflow run release.yml -f tag=vX.Y.Z`). `git archive` output is
deterministic for a tag, so a rerun uploads identical bytes.

Check the archive locally from any clone that has the tag:

```bash
ci/scripts/source-archive.sh vX.Y.Z /tmp/dist
```

The archive's SHA-512 is **not** the one vcpkg's port uses.
`vcpkg_from_github` downloads GitHub's own `archive/vX.Y.Z.tar.gz`, which
GitHub generates separately, so hash that URL for the ports (below).

### Update the vcpkg overlay port

Once the tag exists, update
[`packaging/vcpkg/ports/microtel/`](../packaging/vcpkg/ports/microtel/README.md) in
a follow-up PR. Set `version` in `vcpkg.json` to the new release. In
`portfile.cmake`, set `REF` to `"v${VERSION}"` and set `SHA512` to the hash of
the tag's archive:

```bash
curl -sL https://github.com/microtel-org/microtel/archive/vX.Y.Z.tar.gz | sha512sum
```

Then install it once with `vcpkg install microtel --overlay-ports=packaging/vcpkg/ports`
to check that the hash matches and the port builds.

Make the same two edits in [`ports/microtel/`](../ports/microtel/), the copy of
the port submitted to microsoft/vcpkg, in the same PR. Once microtel is in the
curated registry, follow the update steps in
[`docs/packaging/PR_VCPKG.md`](packaging/PR_VCPKG.md).

In the same PR, add the version to the Conan recipe: a `sources` entry in
[`packaging/conan/recipes/microtel/all/conandata.yml`](../packaging/conan/recipes/microtel/all/conandata.yml)
with the URL and its `sha256sum`, and the version in `config.yml` beside it.
Check it with `conan create packaging/conan/recipes/microtel/all --version=X.Y.Z
-s compiler.cppstd=20 --build=missing`
([`docs/packaging/PR_CONAN.md`](packaging/PR_CONAN.md)).

In the same PR, rename the Yocto recipe
[`packaging/yocto/meta-microtel/recipes-support/microtel/`](../packaging/yocto/meta-microtel/recipes-support/microtel/)
to `microtel_X.Y.Z.bb` (`git mv`) and set `SRC_URI[microtel.sha256sum]` to the
sha256 of the same tarball. The `yocto` workflow checks the fetch and an
offline build ([`packaging/yocto/meta-microtel/README.md`](../packaging/yocto/meta-microtel/README.md)).

---

## 5. Refresh the published benchmark snapshot — in the release PR

**This one belongs in the release PR**, unlike the two in §6.

[`docs/bench-results/`](bench-results/) (`results.json`, `results.md`,
`plots.html`, `README.md`) is the committed run that the benchmark summary
table in that directory's `README.md`, and the performance claims in the root
[`README.md`](../README.md), are read off. It is documentation, not a gate — the
gate is the separate `bench/baseline/results.json` in §6 — but the root README
quotes it, so the two must agree, and a release is when they are made to agree.

> **Run it on the host the outgoing snapshot came from — see
> [#277](https://github.com/microtel-org/microtel/issues/277).**
> `benchmark.yml` is hard-wired to `runs-on: ubuntu-24.04`, a shared 4-core
> GitHub-hosted VM, while the committed snapshot comes from a 12-core
> workstation. Refreshing one from the other is not a refresh, it is a machine
> swap: when v1.1.0 first tried it, every SUT lost ~45% throughput **including
> the two otelcpp SUTs, whose code had not changed.** Until #277 settles on a
> reference runner, refresh this from a **local run on the snapshot's host**,
> not from a workflow artifact.

1. `cd bench && ./bench.sh` on the reference host. v1.1.0 ran three profiles —
   `hot-loop-traces`, `realistic-request`, `compression` — because the
   delivery-denominator and compression fixes are only visible in the latter
   two; `docs/bench-results/` still commits only `hot-loop-traces`, the profile
   the root README quotes.
2. **Diff the `environment` block against the outgoing `results.json` before
   anything else.** `cpu_model`, `cpu_physical_cores`, `cpu_governor`, `kernel`,
   `container_engine_version` must all match; load average and timestamps are
   expected to differ. An unchanged SUT that moved is the tell that they did not.
3. Copy `results.json`, `results.md` and `plots.html` over
   `docs/bench-results/`, and update that directory's `README.md` provenance
   block (date, host, profile, warnings) to describe the new run.
4. Update the summary table in `docs/bench-results/README.md` and the
   performance claims in the root `README.md` to match. Every number in
   them must be readable off the snapshot — **and re-derive the ratio claims
   rather than carrying the old ones forward.** v1.1.0's p50 ratio
   went 4.0× → 3.5× purely because percentiles stopped being bucket midpoints on
   both sides; a stale ratio would have read as a regression.
5. Refresh the two size snapshots beside it.
   [`sizes/library-sizes.md`](bench-results/sizes/library-sizes.md): re-run its
   Commands section against the SUT images step 1 built, regenerate the tables
   with `group.py` and `render.py`, and update the **Build** row with the image
   date and the commit they were built from.
   [`leaf-footprint.md`](bench-results/leaf-footprint.md): if `leaf/` changed
   since the last release beyond its version literals
   (`git diff --stat <previous tag> -- leaf/`), re-run
   `ci/scripts/leaf-footprint.sh` and `ci/scripts/leaf-target.sh` for every
   target and backend, as its Reproducing section says, and update the tables
   and the dated paragraph under its title, naming the commit measured.
   Otherwise add a line saying which release it was not re-measured for.

**Sanity-compare against the previous snapshot before committing.** Compare the
`environment` block first (`cpu_model`, `cpu_physical_cores`, governor, load
average); an unchanged SUT that moved is the tell that the host changed, not the
code. Then, if a metric moved because the *harness* changed rather than the
code, say so prominently in the commit body, the snapshot README, and the
release notes — a reader cannot otherwise tell a measurement fix from a
regression. v1.1 landed three at once: delivery/drop denominators
(#215/#230, #229), rank-interpolated percentiles (#261/#262), and gRPC
wire-byte accounting (#228).

---

## 6. Refresh the other two snapshots — their own PRs

Two further committed files are point-in-time snapshots of generated output.
Neither is required for the release to be usable, and neither should be folded
into the release PR — both are noisy diffs that would bury the version change.

### Benchmark baseline

[`bench/baseline/results.json`](../bench/baseline/results.json) is what the weekly
`benchmark.yml` regression check compares against. Refresh it after a release so
the next cycle's 5% gate measures against the released numbers:

1. Trigger `benchmark.yml`. (It runs on `ubuntu-24.04`; there is no reference
   runner to select, which is [#277](https://github.com/microtel-org/microtel/issues/277).)
2. `ci/scripts/baseline-update.sh <sha>` — it downloads the
   `bench-results-<sha>` artifact, validates the JSON, and overwrites the file.
3. Commit on its own branch, PR, merge.

As of v1.1.0 this file is still the placeholder it shipped as: `generated_at`
is literally `"PLACEHOLDER"`, `environment` is `null`, and the latency medians
are `0`, which the regression check skips. Only `drop_rate_pct` is gated, 0.0
against 0.0 — so the gate currently passes everything. Populating it is part of
#277, not something a release should do on its own.

See [`bench/baseline/README.md`](../bench/baseline/README.md) for what is actually
gated today (drop rate; the latency medians are still placeholder zeros).

### graphify snapshot

[`docs/graph-report.md`](graph-report.md) is a committed copy of the
knowledge-graph report. Per `CLAUDE.md`:

```bash
graphify update . --force        # --force: the rebuild has fewer nodes
cp graphify-out/GRAPH_REPORT.md docs/graph-report.md
git switch -c docs/graph-snapshot-vX-Y-Z
git commit -am "docs(graph): refresh snapshot at vX.Y.Z"
gh pr create …                   # merge once green; master is protected
```

`.graphifyignore` already excludes `docs/graph-report.md` so the snapshot does
not self-index.

---

## 7. Versioning, compatibility and release cadence

The policy the steps above carry out.

- **Release cadence:** monthly pre-1.0; quarterly stable after 1.0.
- **Compatibility policy:** semantic versioning; the public C++ API is stable within a major version. Wire compatibility is tracked against a pinned OTel spec version, with changes called out per release.
- **ABI policy:** No stable C++ ABI guarantee before 1.0. After 1.0, public headers follow semver source compatibility. Binary ABI compatibility is best-effort within a minor release, **not** guaranteed across minor releases unless explicitly stated. Users requiring strict binary compatibility should pin to a specific minor version.

The package-config `SameMajorVersion` mode in §2 is the source-compatibility
promise expressed to CMake. Which releases receive fixes is
[`SECURITY.md`](../.github/SECURITY.md)'s supported-versions matrix (§3), and the
long-term-support window for a major line is in
[`roadmap.md`](roadmap.md) §2.

---

## Checklist

```
[ ] CMakeLists.txt project(… VERSION …)
[ ] include/microtel/version.hpp  kVersionString
[ ] include/microtel/version.hpp  kVersionMajor / kVersionMinor / kVersionPatch
[ ] src/wire/grpc/grpc_wire_codec.cpp  kUserAgent
[ ] tools/preflight/preflight.cpp  kVersion
[ ] leaf/include/microtel/leaf.h  MICROTEL_LEAF_VERSION_MAJOR / _MINOR / _PATCH
[ ] leaf/CMakeLists.txt  project(microtel_leaf VERSION …)
[ ] README (Status, git clone, FetchContent GIT_TAG, CPM @version) and CONTRIBUTING.md
[ ] tests/consumer_fetchcontent GIT_TAG and tests/consumer_cpm @version
[ ] ci/scripts/version-drift-check.sh passes locally
[ ] test-presence satisfied by a real test, not the [refactor] label
[ ] COMPATIBILITY mode still right (major bumps only)
[ ] SECURITY.md supported-versions row
[ ] docs/bench-results/ refreshed from a run on the snapshot's own host
[ ] environment block diffed against the outgoing snapshot (see #277)
[ ] bench-results summary table and root README agree, and the ratio claims were re-derived
[ ] bench-results/sizes/library-sizes.md re-measured, commit recorded; leaf-footprint.md re-measured if leaf/ changed
[ ] measurement-vs-performance shifts annotated where a reader will see them
[ ] release PR merged with CI green
[ ] annotated tag vX.Y.Z on the master merge commit, pushed
[ ] release workflow green; .tar.gz and .sha512 attached to the release
[ ] release notes written (gh release edit)
[ ] packaging/vcpkg/ports/microtel and ports/microtel: version and SHA512 updated (own PR)
[ ] packaging/conan/recipes/microtel: conandata.yml and config.yml entries added (same PR)
[ ] packaging/yocto/meta-microtel: recipe renamed to microtel_X.Y.Z.bb, sha256sum updated (same PR)
[ ] bench/baseline/results.json refreshed (own PR)
[ ] docs/graph-report.md refreshed (own docs/ PR)
```
