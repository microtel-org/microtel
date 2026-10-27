# microtel Development — Track-to-Directory Atlas

**Status:** M0 deliverable. Answers "which track owns this code?" for contributors and AI agents.
**Companion:** `architecture.md` §7 (track ↔ component map), `CODEOWNERS` (review routing).

This document is an index, not a design doc. The substantive content lives in the per-track READMEs under each `src/<dir>/README.md`, which land in M2 (Skeleton & contracts). Until M2, this atlas is the only map.

---

## 1. Purpose

When you start a session — human or agent — and need to do non-trivial work on microtel, this document tells you:

- Which **track** owns the code you're about to touch.
- Which **directories** make up that track.
- Which **interfaces** the track provides and which it consumes (via mocks during track work).
- Which **tests** exercise the track.
- Which **other documents** to read before editing.

If your change spans tracks, this is the document that tells you so — and prompts you to think about whether your change is one PR or two.

After M2, each `src/<dir>/README.md` carries the directory-local detail; this document remains the cross-cutting map.

---

## 2. The six tracks

This table is the definition of the tracks.

| Track | Theme | Foundational? |
|---|---|---|
| **A** | Trace SDK (API + SDK + exporter orchestration) | no |
| **B** | OTLP/HTTP wire codec | no |
| **C** | OTLP/gRPC wire codec | no |
| **D** | Transport (nghttp2 + OpenSSL + reactor) | **yes** |
| **E** | Configuration (TOML + env + auth) | no |
| **F** | OTLP encoder (upb-backed) | **yes** |

**Foundational tracks (D, F) finish first.** They produce the `ITransport` and `IOtlpEncoder` interfaces and the value types (`EncodedPayload`, `WireResult`, `ConnectionState`) that A/B/C consume. Once mocks of the foundational interfaces exist (M2), tracks A/B/C/E proceed concurrently.

---

## 3. Track A — Trace SDK

**Theme:** the `Tracer` / `Span` / `Provider` user-facing surface, the four samplers, the `BatchSpanProcessor`, the exporter orchestration loop.

**Directories owned:**
- `src/api/` — `Tracer`, `Span`, propagator inject/extract.
- `src/sdk/` — `Resource`, samplers, `BatchSpanProcessor`, `SimpleSpanProcessor`, `Provider`, `SdkBuilder` implementation.
- `src/exporter/` — protocol-agnostic export pipeline: drains the processor, calls the encoder, calls the wire codec, classifies retries, accounts for drops.
- `include/microtel/` — public API headers (M0 deliverable, drafted per `interfaces.md`).
- Public-facing examples in `examples/` that demonstrate the API.

**Provides:**
- `ISpanProcessor`, `ISampler`, `IResourceDetector`, `IExporter` (`interfaces.md` §4.4–§4.6, §4.10).
- The public API surface (`microtel::Tracer`, `microtel::Span`, `microtel::Provider`, `microtel::SdkBuilder`).

**Consumes (via mocks during track work):**
- `IOtlpEncoder` (Track F) — mock in `tests/mocks/mock_otlp_encoder.hpp`.
- `IWireCodec` (Tracks B/C) — mock in `tests/mocks/mock_wire_codec.hpp`.
- `IClock`, `ISteadyClock` — fake in `tests/fakes/fake_clock.hpp`.
- `IDiagnosticsSink` — fake in `tests/fakes/fake_diagnostics_sink.hpp`.
- `Config` value (Track E) — built directly in tests; no interface needed at consumption.

**Tests:**
- `tests/unit/api/`, `tests/unit/sdk/`, `tests/unit/exporter/`.
- `tests/integration/` flows that drive the SDK end-to-end against fakes.
- `tests/conformance/` end-to-end against a real OpenTelemetry Collector.

**Read before editing:**
- The public headers' Doxygen under `include/microtel/` (API surface) and the README "Status" section (SDK features).
- `architecture.md` §3.1–§3.3.
- `threading-model.md` §2.1, §2.2 (caller-thread and worker-thread contracts).
- `memory-model.md` §8.1 (zero-allocation unsampled-span rule).
- `interfaces.md` §4.4–§4.6.

---

## 4. Track B — OTLP/HTTP wire codec

**Theme:** the OTLP/HTTP-protobuf codec implementation of `IWireCodec`.

**Directories owned:**
- `src/wire/http/` — HTTP-codec source.

**Provides:**
- The HTTP implementation of `IWireCodec` (`interfaces.md` §4.3).

**Consumes (via mocks during track work):**
- `ITransport` (Track D) — mock or fake in `tests/mocks/mock_transport.hpp` / `tests/fakes/fake_transport.hpp`.
- `IAuthProvider` (Track E) — fake in `tests/fakes/fake_auth_provider.hpp`.
- `IDiagnosticsSink` — fake.
- `EncodedPayload` value type (Track F) — used as input only.

**Tests:**
- `tests/unit/wire/http/`.
- `tests/wire/http/` — byte-level codec validation.
- `tests/conformance/` (shared with Track C).

**Read before editing:**
- `src/wire/http/README.md` (the codec) and `grpc-wire-protocol.md` §8 (what the HTTP and gRPC codecs share).
- `error-model.md` §7.1 (HTTP retry classification matrix).
- `interfaces.md` §4.3.

---

## 5. Track C — OTLP/gRPC wire codec

**Theme:** the OTLP/gRPC codec implementation of `IWireCodec`. Built directly on nghttp2; no gRPC library.

**Directories owned:**
- `src/wire/grpc/` — gRPC-codec source.

**Provides:**
- The gRPC implementation of `IWireCodec`.

**Consumes:** same as Track B, plus the vendored `google.rpc.Status` and `RetryInfo` proto generated code under `gen/google/rpc/`.

**Tests:**
- `tests/unit/wire/grpc/`.
- `tests/grpc-wire/` — byte-level edge-case corpus (see `grpc-wire-protocol.md` §7).
- `tests/conformance/`.
- `tests/fuzz/grpc_codec_fuzz.cpp`.

**Read before editing:**
- `configuration.md` §3.4 (the six timeouts).
- `error-model.md` §7.2 (gRPC retry classification matrix).
- **`grpc-wire-protocol.md` in full.**
- `interfaces.md` §4.3.

---

## 6. Track D — Transport (foundational)

**Theme:** the nghttp2 + OpenSSL + reactor connection. The seam where HTTP/3 could drop in later.

**Directories owned:**
- `src/transport/` — `ITransport` implementation, the I/O loop, per-stream state.
- `src/common/raii/` — `UniqueFd`, `SslCtx`, `SslSession`, `Nghttp2Session`, `BioMethod`, `DeflateStream`, `InflateStream` (the list is `memory-model.md` §4.2). (Shared resource — RAII wrappers serve every track but are implemented and tested under D.)

**Provides:**
- `ITransport` (`interfaces.md` §4.1).
- `IReactor` (`interfaces.md` §4.8).
- The five RAII wrappers in `src/common/raii/`.

**Consumes:**
- OpenSSL (system library).
- nghttp2 (system library, found through pkg-config).
- `IDiagnosticsSink` (fake during track work).

**Tests:**
- `tests/unit/transport/`.
- `tests/unit/common/raii/`.
- `tests/integration/transport_loopback/` — runs against a small in-process server.
- `tests/fuzz/` — response-size and trailer-parser fuzzers (Track C uses these too).

**Read before editing:**
- `threading-model.md` §6, §7 (`Shutdown` lifecycle and fork).
- `architecture.md` §3.6.
- `threading-model.md` §2.3, §3.2, §3.3, §5.
- `memory-model.md` §4.2 (RAII wrapper contract).
- `interfaces.md` §4.1, §4.8.

---

## 7. Track E — Configuration

**Theme:** parse `microtel.toml`, resolve env-var overlays, validate, freeze, hand back to `SdkBuilder`. Plus the auth-provider implementations.

**Directories owned:**
- `src/common/config/` — TOML parser usage, env-var reading, validation, the resolved `Config` value type.
- `src/common/auth/` — `StaticHeadersAuthProvider`, `CallbackAuthProvider` (subdirectory created in M3 when first auth code lands; until then auth code may live alongside config).

**Provides:**
- The frozen, validated `Config` value consumed by every other component.
- `IAuthProvider` and its two concrete implementations (`interfaces.md` §4.9).

**Consumes:**
- A TOML parser (vendored or in-tree; selection at M2).
- `std::getenv` for environment reads.

**Tests:**
- `tests/unit/common/config/`.
- `tests/unit/common/auth/`.
- `tests/fuzz/toml_fuzz.cpp` (M9 hardening).

**Read before editing:**
- **`configuration.md` in full** — every new setting takes a row.
- `error-model.md` §8 (init-failure taxonomy).
- `interfaces.md` §4.9.

---

## 8. Track F — OTLP encoder (foundational)

**Theme:** wrap upb. Produce protobuf bytes from a `BatchHandle`. The single file in the codebase that includes upb headers (LOCKED — `memory-model.md` §3.1).

**Directories owned:**
- `src/wire/encoder/` — `IOtlpEncoder` implementation. Sole consumer of upb headers.
- `gen/` — generated upb C accessors for OpenTelemetry protos and `google.rpc.{Status,RetryInfo}`. Committed; regeneration verified by CI.
- `proto/` — vendored `opentelemetry-proto` and the small subset of `googleapis` (Status, RetryInfo). Pinned tags.
- `third_party/upb/` — vendored upb at a pinned commit.

**Provides:**
- `IOtlpEncoder` (`interfaces.md` §4.2).
- `EncodedPayload` (`memory-model.md` §3.2).

**Consumes:**
- upb (vendored).
- The generated proto code under `gen/`.

**Tests:**
- `tests/unit/wire/encoder/`.
- `tests/wire/encoder/` — encoded-byte fixtures verified against canonical upstream encoders.
- A regen-determinism test in CI: `make regen-protos` against the pinned upb + opentelemetry-proto must produce a zero-diff result (`ci-architecture.md`, `regen-check`).

**Read before editing:**
- `architecture.md` §3.4 (one shared encoder for both wire protocols).
- `memory-model.md` §3 (per-encode arena, `EncodedPayload`, the encoder containment rule).
- `interfaces.md` §4.2.

---

## 9. Cross-track work — when your PR spans tracks

If your PR touches files in two or more track directories, stop and check:

1. **Is this really one change?** A bug fix that happens to require touching two tracks (e.g., a missing field in `WireResult`) is one change. A combined "add a new sampler and update the gRPC codec" is two and should be split.
2. **Does it touch a locked interface?** A breaking change to any `interfaces.md` entry needs an ICP merged first (`docs/icps/README.md`). The implementing PR follows.
3. **Does it touch a contract document?** Changes to `architecture.md` §3, `threading-model.md`, `memory-model.md`, or `error-model.md` require an ICP per `docs/icps/README.md` §"When an ICP is required."
4. **Are the right reviewers tagged?** `CODEOWNERS` routes per directory. A cross-track PR adds two or more reviewers automatically; do not bypass that.

When in doubt: **ask before writing.** Cross-track refactors that look small often grow.

---

## 10. Starting a session — checklist

When you (or an AI agent) start a session intending to do non-trivial work:

1. **Run `git status` and `git log --oneline -20`.** Get the current state.
2. **Read `CLAUDE.md`.** Durable rules.
3. **Determine the milestone phase** from `CLAUDE.md` "Current phase: check before you act."
4. **Identify the track** for the work, using §3–§8 above.
5. **Read the entries flagged "Read before editing"** for that track.
6. **Read the per-directory `README.md`** for any directory you're going to edit. (M2+ only.)
7. **If the work touches an interface**, re-read its entry in `interfaces.md`.
8. **If the work changes a contract**, draft the ICP first.

For trivial work (typo fix, comment update, single-line bug fix in one file), skip directly to the change. The checklist exists for substantive work.

---

## 11. CODEOWNERS

`CODEOWNERS` at the repo root is the authoritative routing table for review requests:

- Each `src/<dir>/` has an owner (one or more GitHub handles, plus organisation teams once a maintainer model is established).
- `docs/` and `docs/roadmap.md` have a separate set of owners.
- `third_party/upb/` and `gen/` have specific owners because vendored-dependency updates carry their own pin and provenance (each directory's README).

A solo project before v1.0 has every owner pointing to the project lead. The structure exists so that as the project grows, ownership can fan out without changing the routing mechanism.

A CI check that fails PRs touching files outside the author's claimed track is **deferred** until collision incidents prove the lighter tools insufficient. For now, CODEOWNERS plus this atlas plus per-directory READMEs are sufficient.

---

## 12. The install tree — what ships, and how a consumer links it

`cmake --install` produces the artifact set that `find_package(microtel)`
consumes. The decisions behind it are
[ICP 0020](icps/0020-install-and-package-config.md); this section is the
operational summary.

```bash
cmake -S . -B build -DMICROTEL_BUILD_TESTS=OFF
cmake --build build
cmake --install build --prefix /opt/microtel
```

```cmake
find_package(microtel REQUIRED)              # add -DCMAKE_PREFIX_PATH=/opt/microtel
target_link_libraries(my_app PRIVATE microtel::microtel)
```

### What the prefix contains

| Path | Contents |
|---|---|
| `<libdir>/libmicrotel_*.a` | The fifteen static components. |
| `<libdir>/cmake/microtel/` | `microtelConfig.cmake`, `microtelConfigVersion.cmake`, `MicrotelTargets*.cmake`. |
| `<includedir>/microtel/` | Public headers, plus `internal/` and `adapters/`. |
| `<includedir>/microtel/vendor/tl/` | The vendored `tl::expected` (ICP 0002). |
| `<includedir>/microtel-shim/adapters/otelcpp/` | The opentelemetry-cpp shim's headers. |
| `<bindir>/microtel-preflight` | The operator CLI ([`troubleshooting.md`](troubleshooting.md), exit codes). |

### Rules worth knowing before you change any of it

**`microtel::microtel` is the only supported name.** The thirteen component
targets plus `microtel::headers` are exported because a static link closure
needs them, and are explicitly unsupported (ICP 0020 Decision 2). The aggregate
also means CMake orders the archives, which is not something a consumer should
be doing by hand.

**The install rules live in the top-level `CMakeLists.txt`, not in each
`src/*/CMakeLists.txt`.** The exported names are a compatibility surface, so
"what microtel ships" is one readable list rather than something reconstructed
from fifteen directories.

**`install(EXPORT …)` is itself a gate.** CMake refuses to generate an export
set that references a target it does not install — including targets that
arrive through a `PRIVATE` link on a static library, which land in the exported
interface as `$<LINK_ONLY:…>`. That is why toml++, spdlog and the vendored upb
include paths are wrapped in `$<BUILD_INTERFACE:…>`: each is genuinely absorbed
at build time and reaches no consumer. **A clean configure is the check.** If
you add a dependency to a shipped target and configure starts failing, the fix
is to export the dependency (if it really does ship) or to correct the linkage —
never to promote something to `PUBLIC` that was not.

**`include/microtel/internal/` ships.** Not because it is public API — it is
not — but because public headers include it, so an install that omitted it
would ship a tree that does not compile.

**Not exported:** `microtel_preflight_lib` (tool-only; the *binary* ships, the
library does not), `microtel_otelcpp_shim` and `microtel_spdlog_bridge` (ICP
0020 Decision 3 — adapter headers install, adapter targets do not). The
otelcpp shim must never ship as a binary: its ABI depends on the consumer's own
opentelemetry-cpp configuration.

**Consumer link dependencies are zlib, OpenSSL and libnghttp2.** All three
arrive through `PRIVATE` link interfaces — no includes or definitions
propagate — but the archives carry undefined references that a consumer's final
link must resolve, so `microtelConfig.cmake` resolves all three. nghttp2 has no
CMake package, so the exported interfaces name `PkgConfig::NGHTTP2` and the
config recreates it with `pkg_check_modules`; pkg-config is therefore a
consumer-side requirement.

**No in-tree target catches an export-set defect.** Every one of them builds
because everything is a subdirectory. The gate that does is
[`tests/consumer/`](../tests/consumer/) — a small external CMake project that
`find_package`s an installed prefix, links `microtel::microtel`, and runs. It
is the canonical consumer example as well as the test: `CMakeLists.txt` there
is the whole of what a consumer writes, and `main.cpp` is a full
build-trace-flush-shutdown cycle against the public API. It is deliberately not
`add_subdirectory`'d into this build — configuring it against a build tree that
still holds every source file would miss precisely what it exists to catch.

Run it with [`ci/scripts/consumer-smoke.sh`](../ci/scripts/consumer-smoke.sh),
which installs to a scratch prefix, configures the consumer against that
prefix, builds it, runs it, and re-runs the closure scan over the same tree:

```bash
ci/scripts/consumer-smoke.sh build            # scratch prefix, cleaned up after
ci/scripts/consumer-smoke.sh build /tmp/mt    # or keep the prefix to poke at
```

CI runs it as the `consumer-smoke` job. Run it locally before changing an
install rule, an exported target, or anything in `cmake/microtelConfig.cmake.in`.

**toml++ is compiled header-only, in both dependency modes.** Under
`MICROTEL_USE_SYSTEM_DEPS=ON` the installed toml++ may be a library whose
target forces `TOML_HEADER_ONLY=0` (vcpkg's does), so `microtel_config` takes
only its include directories and sets `TOML_HEADER_ONLY=1` itself.
`symbol-scan.sh` fails on any undefined `toml::` reference in a shipped
archive. [`tests/consumer_toml/`](../tests/consumer_toml/) links the installed
package next to such a compiled toml++ and parses through both;
`CONSUMER_TOML=1` adds it to the smoke run:

```bash
CONSUMER_TOML=1 CMAKE_PREFIX_PATH=<vcpkg>/installed/x64-linux \
    ci/scripts/consumer-smoke.sh build
```
