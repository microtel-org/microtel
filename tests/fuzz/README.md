# `tests/fuzz/`

libFuzzer harnesses for adversarial inputs: the gRPC framing and trailer
paths, the TOML parser and the response-size limits.

## Required harnesses

| File | Surface |
|---|---|
| `grpc_codec_fuzz.cpp`         | The gRPC response parser entry point: split prefixes, multiple frames, trailer parsing, `RetryInfo` decoding. |
| `toml_fuzz.cpp`               | The `microtel.toml` parser, fed adversarial TOML. |
| `response_decompression_fuzz.cpp` | Decompression-bomb protection, bounded by `max_decompressed_bytes`. |
| `otlp_response_fuzz.cpp`      | The `ExportTraceServiceResponse` proto parser (the partial-success path). |

## v1.1 harnesses

| File | Surface |
|---|---|
| `baggage_fuzz.cpp` | The W3C `baggage` header parser, required by clause 3 of the v1.1 ships-when gate ([ICP 0024](../../docs/icps/0024-v1.1-rescope.md)). Besides crash-freedom, it asserts the three grammar limits and that `FromHeader`/`ToHeader` round-trips are stable. |
| `provider_setters_fuzz.cpp` | The validation surface of the four hot-reload `Provider` setters ([ICP 0026](../../docs/icps/0026-provider-setters.md)), required by clause 2 of the same gate. The input is a program rather than a value: byte 0 picks the provider's shape, and the rest is a stream of opcodes replayed against one live provider, so interleavings get fuzzed along with ranges. It asserts that `InvalidArgument` comes back exactly when the harness independently judges the value bad, that nothing but `Completed`, `InvalidArgument` or `Unsupported` comes back before `Shutdown`, and that `Unsupported` depends on the pipeline and not on history. |

## v1.2 harnesses

`leaf_ingest_fuzz` and `otlp_trace_decoder_fuzz` are built only with
`MICROTEL_WITH_CONCENTRATOR=ON`, `leaf_backend_diff_fuzz` only with
`MICROTEL_BUILD_LEAF=ON`; the fuzz jobs set both.

| File | Surface |
|---|---|
| `leaf_ingest_fuzz.cpp` | The concentrator's ingest path ([`leaf-concentrator-design.md`](../../docs/leaf-concentrator-design.md) §7.3, ICP 0031 ship gate 2): byte 0 picks the leaf id, the configured time mode, the unknown-leaf policy and whether `default_time_mode` is `unix` (ICP 0036); the rest is the payload, fed to `LeafReceiver::Ingest` on a live provider with the real upb decoder. It asserts that an accepted payload's three counts add up to the spans it decodes to, that a rejected one moves exactly one of `leaf_payload_malformed`, `leaf_payload_too_large` and `leaf_unknown` by one, that no reserved `microtel.leaf.*` key reaches a Resource, and that an accepted payload carried a span or reported leaf drops. Seeds are well-formed leaf payloads, undeclared full-node payloads under `unix`, header-only leaf payloads with and without drops, plus truncations and single-byte corruptions of each. |
| `otlp_trace_decoder_fuzz.cpp` | `IOtlpTraceDecoder` alone, bytes 0-2 choosing random `DecodeLimits`, so decoder bugs are not hidden behind the receiver's validation. |
| `leaf_backend_diff_fuzz.cpp` | Byte identity of the leaf's upb and nanopb encoder backends ([docs/leaf-concentrator-design.md](../../docs/leaf-concentrator-design.md) §7.2). The input is a program of leaf builder calls (`tests/leaf/diff/`), run on two identical leaves linked through `microtel_leaf_dual`; every call must return the same status on both, and every encode the same status, size and bytes, which must decode with upb. Needs `-DMICROTEL_BUILD_LEAF=ON`. |

## Invariants

From `docs/grpc-wire-protocol.md` §7.4, applied to every harness:

- No crashes.
- No ASan, UBSan or TSan findings.
- Memory growth stays bounded by the configured response-size limits,
  whatever the input.
- No infinite loops. The parser always makes progress or terminates.

## Inputs

Each harness has two committed input directories:

- `corpus/<harness>/` holds seed inputs. The fuzzing job starts from
  these.
- `crashes/<harness>/` holds confirmed crashing inputs. Once the bug is
  fixed, the input stays as a regression check. Every `crashes/`
  directory is empty today (apart from `.gitkeep`).

## CI

Fuzzing itself is not a per-PR gate, because it is slow and its
findings are non-deterministic. [`fuzz.yml`](../../.github/workflows/fuzz.yml)
runs every harness weekly (default 120 s each, adjustable on a manual
dispatch) and uploads any crashing input as an artifact. Findings open
issues; they don't block PRs.

Replaying `crashes/` is a hard gate on every PR. The gate is
[`ci/scripts/corpus-check.sh`](../../ci/scripts/corpus-check.sh), run by
the `corpus-check` job in `ci.yml`. To reproduce it locally:

```bash
cmake -S . -B build-fuzz \
      -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
      -DMICROTEL_BUILD_FUZZ=ON -DMICROTEL_BUILD_LEAF=ON -DMICROTEL_BUILD_TESTS=OFF \
      -DMICROTEL_WITH_CONCENTRATOR=ON
cmake --build build-fuzz
ci/scripts/corpus-check.sh build-fuzz
```

The script replays every committed input with `-runs=1` and fails on any
non-zero exit, printing the harness output so the sanitizer report is
visible. A missing harness binary is an error, not a skip, so a renamed
target can't silently stop being checked. Empty `crashes/` directories
pass, and the script says so instead of implying coverage it doesn't
have.
