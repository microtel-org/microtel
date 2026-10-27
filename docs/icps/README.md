# Interface Change Proposals (ICPs)

An ICP is a short markdown document that records a breaking change to an
interface locked in M0, or a material change to an architecture document that
contributors and AI agents treat as a standing rule.

Keep them light (spec §13.2): a few paragraphs that give people a heads-up.
They are not meant to become multi-week reviews.

## Index

The index is append-only (see File naming). Nothing is removed, including
superseded proposals, because the rationale is what the record is for and an
accepted ICP is a decision that still applies. Several are referenced directly
from `CLAUDE.md`, `CMakeLists.txt`, `ci/header_check.cpp` and the headers.

| ICP | Subject | Status | Implemented by |
|---|---|---|---|
| [0001](0001-m0-deliverables-clarification.md) | M0 deliverables clarification | Accepted | — |
| [0002](0002-vendor-tl-expected.md) | Vendor `tl::expected` for `microtel::Expected` | Accepted | — |
| [0003](0003-m0-deferred-decisions.md) | M0 deferred decisions | Accepted | — |
| [0004](0004-vendor-tomlplusplus.md) | Vendor toml++ | Accepted | — |
| [0005](0005-in-repo-benchmarks.md) | In-repo benchmarks | Accepted | — |
| [0006](0006-bench-drop-counter-naming.md) | Bench drop-counter naming | Accepted | — |
| [0007](0007-wire-codec-send-all.md) | `IWireCodec::SendAll` | Accepted | — |
| [0008](0008-metric-drop-reasons.md) | Metric `DropReason` values | Accepted | #75 |
| [0009](0009-transport-concurrent-send.md) | `ITransport::Send` safe for concurrent callers | Accepted (retroactively) | M12; docs + test in #156 |
| [0010](0010-milestone-renumber-views.md) | Milestone renumber (Views → M13) | Accepted | — |
| [0011](0011-log-attribute-limit-drop-reason.md) | `LogAttributeLimit` drop reason | Accepted | — |
| [0012](0012-provider-get-logger.md) | `Provider::GetLogger` | Accepted | — |
| [0013](0013-rescope-defer-python-bindings.md) | Defer Python bindings to M18 | Accepted | #107 |
| [0014](0014-otelcpp-shim-and-rule-13.md) | otel-cpp shim, source-only, rule 13 | Accepted | #107 |
| [0015](0015-unrepresentable-attribute-policy.md) | Unrepresentable attribute values | Accepted | #110 |
| [0016](0016-adapter-drop-accounting.md) | Adapter drop accounting (shim-local) | Accepted | #127 |
| [0017](0017-lazy-transport-connect.md) | Lazy transport connect | Accepted | #129 |
| [0018](0018-reconnect-after-drop.md) | Reconnect after a mid-connection drop | Accepted | #151, #153 |
| [0019](0019-connect-on-the-io-thread.md) | Perform `Connect` on the I/O thread | Draft | — |
| [0020](0020-install-and-package-config.md) | `install()` rules and the exported target set | Accepted | — (M9/M10) |
| [0021](0021-threading-model-reconciliation.md) | Reconcile `threading-model.md`; make LOCKED checkable | Accepted | #231 |
| [0022](0022-tls-peer-verification.md) | Enforce TLS server-certificate verification | Accepted | #164 |
| [0023](0023-span-processor-scope.md) | `ISpanProcessor::OnEnd` carries the `InstrumentationScope` | Accepted | #175 |
| [0024](0024-v1.1-rescope.md) | v1.1 rescope — hot reload without a socket | Accepted | — |
| [0025](0025-propagation-core.md) | Propagation core — baggage, `TraceState` storage, `StartAsCurrentSpan` | Accepted | — (v1.1 packets 2.3a–c) |
| [0026](0026-provider-setters.md) | The four hot-reload `Provider` setters | Accepted | — (v1.1 packets 2.2, 2.4) |
| [0027](0027-multi-profile-threading.md) | Multi-profile — named `Provider`s at full independence | Accepted | — (v1.1 packet 3.1) |
| [0028](0028-sugar-surface.md) | The sugar layer's public surface — `microtel::sugar` | Accepted | — (v1.1 sugar packet) |
| [0029](0029-auth-caller-count-correction.md) | Auth-provider caller-count correction (§4.9 LOCKED sentence) | Accepted | — (docs only) |
| [0030](0030-compile-time-feature-selection.md) | Compile-time feature selection — `MICROTEL_WITH_*` | Draft | — |
| [0031](0031-leaf-concentrator-in-v1.3.md) | Move leaf / concentrator, with upb and nanopb leaf backends, into v1.3 (experimental) | Accepted | — (docs follow-up) |
| [0032](0032-release-reorder-v1.1.1.md) | v1.1.1 patch release; v1.2 Logs + Leaf, v1.3 Metrics, v1.4 Control Plane, later themes +1 | Accepted | — (docs follow-up) |
| [0033](0033-shim-attribute-value-length-limit.md) | otel-cpp shim `ShimOptions::attribute_value_length_limit`; omit oversized byte-span attributes | Accepted | #336 |
| [0034](0034-leaf-receiver-api.md) | `Provider::GetLeafReceiver`, the `LeafReceiver` API, and three leaf `DropReason`s | Accepted | — (v1.2 leaf receiver packet) |
| [0035](0035-leaf-post-shutdown-counter.md) | Leaf payloads after `Shutdown` counted in `LeafReceiverStats::payloads_post_shutdown`, not `post_shutdown` | Accepted | #354 |
| [0036](0036-custom-export-transport.md) | `SdkBuilder::WithExportTransport` for full-SDK nodes; `LeafTimeMode::Unix` for undeclared concentrator payloads | Accepted | #357, #358 |
| [0037](0037-retire-the-v1-spec.md) | Retire `microtel-spec.md`: move the tiers, ABI policy and performance targets to live docs, rewrite references outside the ICPs, delete it (archived at `v1.2.1`) | Accepted | #404, #405, #406, #407 |
| [0038](0038-reject-reserved-request-headers.md) | `Build()` rejects static request headers that HTTP/2 forbids (pseudo-headers, connection-specific, `host`) or that microtel sets for the resolved protocol (#408) | Accepted | #410 |
| [0039](0039-metric-start-timestamps.md) | Metric data-point `time` and `start_time`: one collection time per collect, delta windows chained, cumulative series start at first measurement (observable: the prior collection), gauges time only (#448). Amended: observable sums under delta, first observation, forgotten series, resets (#450) | Accepted | #449, #453 |

[ICP 0024](0024-v1.1-rescope.md) reserved number 0027 for an amendment to
`docs/control-plane-design.md` §9 (the fourth thread role). That amendment is
still unwritten and will now come with the v1.2 socket. 0027 went instead to
the other `docs/threading-model.md` amendment v1.1 needed, for multi-profile.

"Implemented by" is filled in only where a commit explicitly applies the ICP.
A dash means commit messages don't say which change implemented it; it does
not mean the ICP is unimplemented. Several early ones were applied as part of
the milestone that prompted them. Grepping for an ICP number turns up passing
mentions as often as implementations, so the column is left sparse instead of
guessed.

### ICP 0009: a cautionary example

0009 proposed relaxing `ITransport::Send` from "single-caller" to "safe for
concurrent callers" so the M12 metrics pipeline could share one transport.
The relaxation shipped but the ICP did not. Since M12, `SdkBuilder::Build`
has built three codecs over one transport, each driven by its own exporter
worker, while `interfaces.md` §4.1 went on stating, as a LOCKED claim, that concurrent
`Send` was a contract violation. The TSAN test 0009 specified was never
written.

Nothing ever raced, and the implementation was safe for exactly the reasons
0009 gave. But for four milestones the contract said the opposite of what the
code did, and there was no evidence for the safety claim until #156.

It stays here because it shows what issue #134 is about. Documents drift; the
real problem was that nothing checked the document against the code.

## LOCKED claims cite code (ICP 0021)

> **A LOCKED claim that cannot cite code is a claim about intent, and must be
> marked as such.**

Every LOCKED marker carries one of two annotations:

```
(LOCKED — cites `src/sdk/sdk_provider.cpp:Shutdown`)    a claim about code
(LOCKED — cites `src/a.cpp:Foo`, `src/b.hpp:m_bar`)     several, comma-separated
(LOCKED — intent)                                       a claim about intent
```

A citation names a function or member, never a line number. Line numbers go
stale: one added in #149 was already wrong by #144.

[`ci/scripts/citation-check.py`](../../ci/scripts/citation-check.py) (CI job
`citation-check`, a required status check) enforces it in two passes:

- Resolution, over every document under `docs/`: a cited file must exist and
  a cited symbol must appear in it. A stale citation fails the build wherever it
  is written.
- Coverage, only over documents with a `**Citation policy:** complete` line in
  their header: every LOCKED marker there must be annotated with either `cites`
  or `intent`. Documents opt in one at a time because the issue #134 audit of
  all ~90 markers is still open, and the pass that reconciles a document is the
  one that flips it to `complete`.
  [`docs/threading-model.md`](../threading-model.md) was the first.

The check is weak on purpose. It proves a symbol exists, not that the sentence
around it is true. That is still worth having, because it catches a failure
you can't spot by reading: `ShutdownState`, which three normative documents
called the shutdown "single source of truth", did not exist in any commit for
four milestones. Run it locally with `ci/scripts/citation-check.py`, and add
`--self-test` to watch it fail on purpose.

LOCKED has never meant "verified". It means that changing the claim requires
an ICP. Every marker in the repository was written in the M0 commit, before
there was any code to check it against, and ICP 0021 is the cleanup that
followed.

## When an ICP is required

After M0 closes, an ICP is required for:

- Breaking changes to any interface in `include/microtel/internal/*.hpp` or to the public API in `include/microtel/`.
- Changes to a contract documented in `docs/interfaces.md`.
- Changes to architectural commitments in `docs/architecture.md`, `docs/threading-model.md`, `docs/memory-model.md`, or `docs/error-model.md`.
- Changes to durable agent / contributor instructions in `CLAUDE.md`.
- Changes to the deliverable set or scope of a milestone in `microtel-spec.md` §13 or §14.

## When an ICP is *not* required

- Adding a new interface (no existing contract to break).
- Bug fixes that don't alter observable behavior.
- Documentation polish that doesn't change a normative claim.
- Test additions, formatting, comment-only changes.
- New methods on an interface that's still being drafted (M0 in progress).

## Pre-M0-close ICPs

Before M0 closed, ICPs were encouraged but optional. They were used for changes that reshaped the M0 deliverable set or amended CLAUDE.md, the spec or repository-layout.md in ways later contributors would need to find. ICP 0001 is one.

## File naming

`NNNN-short-slug.md`, four digits, zero-padded and increasing. The directory is append-only: a superseded ICP stays, and a new ICP records the supersession.

## Process

1. PR an ICP into `docs/icps/`.
2. Reviewer sign-off (single reviewer for now).
3. Merge the ICP before the implementing PR, so the implementing PR can reference it by number.
4. The implementing PR makes the substantive changes.

## Required sections

Every ICP includes:

- **Status:** Draft | Accepted | Implemented | Superseded.
- **Affected interfaces / docs:** explicit file list.
- **Affected tracks:** which of Tracks A–F (spec §13.1) consume what's changing — or "none / docs only".
- **Summary:** one sentence.
- **Motivation:** why now.
- **Proposed change:** what changes, with file paths.
- **Migration:** what contributors / agents need to do differently.
- **Rationale & alternatives:** what other shapes were considered.
