# microtel documentation

Everything under `docs/`, grouped by who it is for. Start with the root
[`README.md`](../README.md) for what microtel is, how to build it and a quick
start; come here for the detail.

## Using microtel

| Document | What it is for |
|---|---|
| [troubleshooting.md](troubleshooting.md) | If you see an error, warning, counter or exit code: what it means and what to do. |
| [configuration.md](configuration.md) | Every setting: `SdkBuilder` setter, environment variable and `microtel.toml` key, and which wins. |
| [build-options.md](build-options.md) | The CMake options for building and packaging microtel, with defaults. |
| [compatibility-matrix.md](compatibility-matrix.md) | What microtel supports, the test that proves each item, and what does not work. |
| [interop-matrix.md](interop-matrix.md) | The pinned Collector and backend versions the conformance gates test against. |
| [auth-callback-recipes.md](auth-callback-recipes.md) | OAuth2 and AWS SigV4 recipes over `SdkBuilder::WithAuthProvider`. |
| [migration-from-otel-cpp.md](migration-from-otel-cpp.md) | Moving from opentelemetry-cpp, including the API shim. |
| [error-model.md](error-model.md) | How failures surface: init errors, drop counters, retry classification, diagnostics. |
| [threading-model.md](threading-model.md) | Which threads microtel runs, what they own, and what is safe to call from where. |
| [memory-model.md](memory-model.md) | Resource ownership and byte budgets, and what happens when one is exceeded. |
| [vcpkg overlay port](../packaging/vcpkg/ports/microtel/README.md) | Installing microtel with vcpkg from the repository's own overlay port. |
| [bench-results/README.md](bench-results/README.md) | The benchmark summary table, and the committed snapshot behind it and the root README's performance claims ([results](bench-results/results.md), [plots](bench-results/plots.html), [raw JSON](bench-results/results.json)). |

The runnable programs are in [`examples/`](../examples/README.md), and the
public headers in [`include/microtel/`](../include/microtel/).

## Microcontrollers: the leaf

| Document | What it is for |
|---|---|
| [leaf/README.md](../leaf/README.md) | The C leaf library for microcontrollers; start here. |
| [leaf-concentrator-design.md](leaf-concentrator-design.md) | Design of the leaf and of the concentrator that receives its spans. |
| [bench-results/leaf-footprint.md](bench-results/leaf-footprint.md) | Measured flash, RAM and stack of the leaf on each target and backend. |
| [examples/leaf](../examples/leaf/README.md), [examples/leaf_mqtt](../examples/leaf_mqtt/README.md) | A leaf and a concentrator over UDP and over MQTT. |

## Contributing and design

The design documents below, the ICPs and [`interfaces.md`](interfaces.md)
are the working record. The original v1.0 design specification was retired by
[ICP 0037](icps/0037-retire-the-v1-spec.md); [where its sections live
now](#where-the-v10-specifications-sections-live-now) is at the end of this
page.

| Document | What it is for |
|---|---|
| [roadmap.md](roadmap.md) | What ships in which release, v1.0 onward. |
| [releasing.md](releasing.md) | The release procedure, versioning and ABI policy. |
| [architecture.md](architecture.md) | The layered structure of the runtime: what each layer owns and how they connect. |
| [interfaces.md](interfaces.md) | The locked contracts for every internal interface in `include/microtel/internal/`. |
| [grpc-wire-protocol.md](grpc-wire-protocol.md) | How OTLP/gRPC is implemented on nghttp2 without the gRPC library. |
| [metrics-design.md](metrics-design.md) | Design of the metrics signal. |
| [logs-design.md](logs-design.md) | Design of the logs signal. |
| [control-plane-design.md](control-plane-design.md) | Design of the runtime control plane, and which parts are deferred. |
| [coding-standards.md](coding-standards.md) | Code style and structural rules; the clang-tidy / SonarQube ruleset. |
| [development.md](development.md) | Which track owns which source directory. |
| [repository-layout.md](repository-layout.md) | Map of the tree, build options, and where a new file goes. |
| [ci-architecture.md](ci-architecture.md) | The CI jobs and what each one gates. |
| [packaging-research.md](packaging-research.md) | What packaging microtel for vcpkg and Conan takes, and what blocks the public registries. |
| [branch-protection.md](branch-protection.md) | GitHub branch-protection and repository settings. |
| [bench-spec.md](bench-spec.md) | Specification of the benchmark harness in [`bench/`](../bench/README.md). |
| [icps/README.md](icps/README.md) | Interface Change Proposals: the process, and the index of every accepted change. |
| [graph-report.md](graph-report.md) | Committed snapshot of the graphify knowledge-graph report of the codebase. |

### Sequence diagrams

| Document | What it is for |
|---|---|
| [connection-establishment.md](sequences/connection-establishment.md) | The first request after `Build()` returns. |
| [retry-after-failure.md](sequences/retry-after-failure.md) | The retry path on a retryable export failure. |
| [goaway-handling.md](sequences/goaway-handling.md) | HTTP/2 GOAWAY received mid-batch. |
| [shutdown-drain.md](sequences/shutdown-drain.md) | `Provider::Shutdown(timeout)`. |
| [fork-survival.md](sequences/fork-survival.md) | `fork()` in a process running microtel. |
| [backpressure-and-drop.md](sequences/backpressure-and-drop.md) | Queue overflow under sustained load. |
| [partial-success.md](sequences/partial-success.md) | OTLP partial-success responses. |
| [grpc-trailer-only-and-multi-frame.md](sequences/grpc-trailer-only-and-multi-frame.md) | gRPC trailers-only and multi-frame response parsing. |

### Images

The SVGs in [`images/`](images/) are generated by
[`tools/diagrams/`](../tools/diagrams/README.md); edit the scripts, not the
SVGs.

Also see [`CONTRIBUTING.md`](../.github/CONTRIBUTING.md), [`releasing.md`](releasing.md)
and, for AI coding agents, [`CLAUDE.md`](../CLAUDE.md).

## Where the v1.0 specification's sections live now

"spec §N" citations inside the ICPs refer to the v1.0 design specification,
archived at
<https://github.com/microtel-org/microtel/blob/v1.2.1/microtel-spec.md>
([ICP 0037](icps/0037-retire-the-v1-spec.md)). The ICPs are append-only, so
their citations stay as written; this table is the key to where each topic
lives now.

| Spec | Now lives in |
|---|---|
| §1–§4 Summary, goals, non-goals, motivation | [README "Why it exists"](../README.md#why-it-exists); [roadmap](roadmap.md) §8 anti-goals |
| §2.2 Compatibility tiers | [compatibility-matrix.md](compatibility-matrix.md) §7 |
| §5 Architecture | [architecture.md](architecture.md), the [threading](threading-model.md), [memory](memory-model.md) and [error](error-model.md) models |
| §6 API, §8 SDK features | the public headers' Doxygen in [`include/microtel/`](../include/microtel/), [interfaces.md](interfaces.md), [README "Status"](../README.md#status) |
| §7 Wire protocols | [grpc-wire-protocol.md](grpc-wire-protocol.md) |
| §9 Build & dependencies | [build-options.md](build-options.md); [`CLAUDE.md`](../CLAUDE.md) rules 12–13 |
| §10 Performance targets | [bench-spec.md](bench-spec.md) §14 |
| §11 Project structure | [repository-layout.md](repository-layout.md) |
| §12 Configuration | [configuration.md](configuration.md) |
| §13, §16–§18 Roadmap, risks, open questions, future | [roadmap.md](roadmap.md) and the design docs |
| §14 Engineering practices | [`CONTRIBUTING.md`](../.github/CONTRIBUTING.md), [coding-standards.md](coding-standards.md), [ci-architecture.md](ci-architecture.md) |
| §15 Compatibility & interop | [compatibility-matrix.md](compatibility-matrix.md), [interop-matrix.md](interop-matrix.md) |
| §19 Versioning, ABI, cadence | [`releasing.md`](releasing.md) §7 |
| §19 DCO, security, CODEOWNERS, CI quality gates | [`CONTRIBUTING.md`](../.github/CONTRIBUTING.md), [`SECURITY.md`](../.github/SECURITY.md), [`CODEOWNERS`](../.github/CODEOWNERS), [ci-architecture.md](ci-architecture.md) |
