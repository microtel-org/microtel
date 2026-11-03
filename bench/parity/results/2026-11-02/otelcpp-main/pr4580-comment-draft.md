Independent measurements of this PR on the application thread.

Builds: `main` at 0a5bb0b, and the same commit with this PR's head (5aa9922) merged in. I built both in the repo's dev container (`.devcontainer/Dockerfile.dev`: clang 22.1.8; protobuf 35.1.0 and abseil 20250512.1 from `third_party_release`), Release with `-O2 -g -fno-omit-frame-pointer`. I built each one twice, and the two copies are identical section by section.

The harness is a small app that emits spans in a tight loop on one thread. It uses the OTLP HTTP exporter and a `BatchSpanProcessor` with `max_queue_size` 16384 (no drops), exporting to a local sink that discards everything. I counted allocations per thread with an `LD_PRELOAD` malloc counter and took them from the emitting thread only: 10,000 spans after a 1,000-span warmup. Latency is the time per span in the loop, 10 interleaved rounds of 10 samples each.

| | main | main + #4580 |
|---|---:|---:|
| Allocations per span, no attributes | 9 | **5** |
| Allocations per span, 5 string attributes (12-byte keys, 24-byte values) | 36 | **11** |
| `SetIdentity` allocations, root / child span | 4 / 5 | 1 / 1 |
| Span emit p50, ratio to main (median of per-round ratios, range) | 1 | 0.74 (0.68–0.88) |
| Span emit p95, ratio to main | 1 | 0.66 (0.64–0.80) |

This reproduces the 9 → 5 for a minimal span. The allocation counts were identical across runs. The PR was faster in every one of the 10 rounds, though main's per-sample p50s were much more spread out than the PR's. Nothing that I measured on the emitting thread got worse, including p95. This harness keeps many recordables alive in the queue at once.

I didn't measure memory. From the sizes in this PR, `sizeof(OtlpRecordable)` goes from 216 to 752 bytes, so a full default queue of 2048 recordables holds about 1.1 MB more. Part of that is offset by the heap strings the old version allocated for each recordable, which the arena replaces; I haven't measured how much. I also didn't measure export-thread CPU.

Host caveat: these runs were on a single desktop machine (Ryzen 5 5600G, single-channel DDR4, boost off), so treat the absolute numbers as specific to that machine. That's why the table gives ratios rather than nanoseconds.

I also tried two changes to `SetIdentity` on top of the PR:
- skip `set_trace_state` when `trace_state()->Empty()`;
- fill `trace_id` with `mutable_trace_id()->assign(...)`.

On top of this PR they make no measurable difference: same allocation counts, and p50 0.98 of the PR (range 0.92–1.01, over 5 rounds). Without the PR they save one allocation per span (9 → 8). So I don't think they're worth adding here.

One practical note: the PR currently conflicts with `main`, in `CHANGELOG.md` and in `otlp_recordable.h`. #4734 added `attribute_value_depth_limit` to `SpanLimits`, so the constructor this PR moves into `otlp_recordable.cc` needs that extra initializer.

Harness, method and data: [section 4](https://github.com/microtel-org/microtel/blob/1544051299b08b0d9f4a67098028bbf1fc05f56d/bench/parity/RESULTS.md#4-opentelemetry-cpp-main-and-pr-4580) of a parity study in the microtel repo.

I'm happy to rerun these on later revisions of the PR.
