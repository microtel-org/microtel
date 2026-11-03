#!/usr/bin/env python3
# Copyright (c) 2026 The microtel Authors.
# SPDX-License-Identifier: Apache-2.0
"""Background-thread breakdown of a perf_profile.py run (bench/parity/README.md).

Usage:  python3 -I bench/parity/perf_threads.py <out-dir>

perf_buckets.py keeps only the emit worker. This reads the same
<out-dir>/perf.script and keeps every other sample: the batch processor's
export thread, the HTTP client's thread and the rest. Each sample is assigned
to the outermost export-path stage it is inside, and separately marked as
allocation when its innermost frame is malloc/free/new/delete. Writes
<out-dir>/threads.json and <out-dir>/background.folded (for flamegraph.pl), and
prints a Markdown table in user-space cycles per span emitted (cycles:u only:
kernel time is not sampled).
"""

import collections
import json
import pathlib
import re
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from perf_buckets import frames_of, short  # noqa: E402

# Outermost match wins, checked in this order on the whole stack.
STAGES = [
    ("serialize request", re.compile(r"SerializeToString|SerializeToArray|SerializePartialTo|"
                                     r"_InternalSerialize|ByteSizeLong|MessageLite::Serialize")),
    ("build request: copy spans (MergeImpl)", re.compile(r"::MergeImpl|::CopyFrom|MergeFrom")),
    ("build request: other", re.compile(r"OtlpRecordableUtils::PopulateRequest")),
    ("destroy request", re.compile(r"ExportTraceServiceRequest::~|ExportTraceServiceRequest::SharedDtor|"
                                   r"Arena::~Arena|ArenaImpl::Free|ThreadSafeArena::~|"
                                   r"ThreadSafeArena::Free|RunCleanup|HttpSessionData|"
                                   r"ResourceSpans::~|ScopeSpans::~|Span::~Span|Span::SharedDtor")),
    ("destroy recordables", re.compile(r"OtlpRecordable::~OtlpRecordable|~unique_ptr<.*Recordable")),
    ("HTTP client (curl)", re.compile(r"curl|ext::http::client")),
    ("batch processor", re.compile(r"BatchSpanProcessor")),
]
ALLOC = re.compile(r"^(malloc|free|cfree|calloc|realloc|__libc_malloc|__libc_free|_int_malloc|"
                   r"_int_free|malloc_consolidate|tcache)|^operator new|^operator delete")


def main():
    out = pathlib.Path(sys.argv[1])
    meta = json.loads(out.joinpath("meta.json").read_text()) if out.joinpath("meta.json").exists() else {}
    spans = meta.get("spans_emitted") or 0
    text = out.joinpath("perf.script").read_text(errors="replace")
    by_thread = collections.Counter()
    stages = collections.Counter()
    stage_alloc = collections.Counter()
    folded = collections.Counter()
    worker_total = 0
    for block in text.split("\n\n"):
        lines = block.strip("\n").split("\n")
        if not lines or not lines[0].strip():
            continue
        head = lines[0].split()
        try:
            period = int(head[-1])
        except (ValueError, IndexError):
            continue
        frames = frames_of(lines)
        names = [short(s) for s, _ in frames]
        if any("WorkerThread" in n for n in names):
            worker_total += period
            continue
        tid = head[1] if len(head) > 1 else "?"
        by_thread[f"{head[0]}/{tid}"] += period
        stage = "other"
        for name, rx in STAGES:
            if any(rx.search(n) for n in names):
                stage = name
                break
        stages[stage] += period
        if names and ALLOC.search(names[0]):
            stage_alloc[stage] += period
        stack = [n if n != "[unknown]" else f"[{d.rsplit('/', 1)[-1] or 'kernel'}]"
                 for n, (_, d) in zip(reversed(names), reversed(frames))]
        folded[";".join(stack)] += period
    bg = sum(stages.values())
    per = (lambda c: c / spans) if spans else (lambda c: float("nan"))
    print(f"Spans emitted: {spans:,}. Background cycles: {bg:,} ({per(bg):,.0f} per span); "
          f"worker cycles: {worker_total:,} ({per(worker_total):,.0f} per span).\n")
    print("| Stage (background threads) | Cycles per span | % of background | of which in malloc/free |")
    print("|---|---:|---:|---:|")
    order = [n for n, _ in STAGES] + ["other"]
    for name in sorted(order, key=lambda n: -stages[n]):
        if stages[name] == 0:
            continue
        print(f"| {name} | {per(stages[name]):,.0f} | {100 * stages[name] / bg:.1f} | "
              f"{100 * stage_alloc[name] / stages[name]:.0f}% |")
    print("\n| Thread (top 5) | Cycles per span |\n|---|---:|")
    for t, c in by_thread.most_common(5):
        print(f"| {t} | {per(c):,.0f} |")
    out.joinpath("background.folded").write_text("".join(f"{k} {v}\n" for k, v in folded.most_common()))
    out.joinpath("threads.json").write_text(json.dumps({
        "spans_emitted": spans, "background_cycles": bg, "worker_cycles": worker_total,
        "stages": dict(stages), "stage_alloc": dict(stage_alloc), "threads": dict(by_thread)},
        indent=2))


if __name__ == "__main__":
    main()
