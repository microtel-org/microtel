# bench/driver: Python benchmark orchestrator

Runs one or more SUT containers against a sink (the blackhole sink by
default), collects `RunResult` samples over the TCP control socket, and
writes `results.json` and `results.md` to the output directory.

## Invocation

```bash
# From the repo root via the shell wrapper:
./bench/bench.sh [options]

# Or directly from bench/:
cd bench
python3 -m driver [options]
```

## Options

```
--profile NAME            Workload profile (default: hot-loop-traces)
--sut NAME[,NAME...]      Run only these SUTs, in that order; default: every SUT
                          in the profile whose registry entry has b0: true
--reps N                  Override sample count from the profile
--out DIR                 Output directory (default: bench/results)
--no-build                Skip image builds; use cached images
--engine ENGINE           Container engine: auto|podman|docker (default: auto)
--seed N                  Random seed placeholder (currently unused)
--sink MODE               auto|blackhole|collector (default: auto, the profile's choice)
--regression-check FILE   Compare against a baseline results.json; exit 2 on regression
--threshold F             Regression threshold as a fraction (default: 0.05)
--allow-smt               Suppress the hyperthreading/SMT env-guard warning
--label TEXT              Shown at the top of results.md, stored as results.json "label"
--verbose                 Show container build and run output
--flamegraph              Also build :perf images and write one SVG flamegraph per SUT
--flamegraph-dir DIR      FlameGraph scripts directory (overrides FLAMEGRAPH_DIR)
--sweep-attr-bytes SIZES  Sweep attribute value sizes, e.g. 0,64,256; one result per size
--sweep-threads N,N,...   Sweep emitter thread counts; one result per count
--sweep-leaves N,N,...    Sweep simulated leaf counts (leaf-fanin); one result per count
--sink-delay-ms MS        Per-request response delay injected into the blackhole sink
```

`python3 -m driver -h` prints the full help text.

`python3 -m driver.rounds --profile P --sut A,B,C --rounds 5 --out DIR` runs
interleaved rounds of one profile, one driver run per SUT per round, behind a
load-average gate, and writes the median and range of the CPU figures to
`DIR/summary.md` (`bench/docs/methodology.md`, "Interleaved rounds").

## Module layout

| File | Purpose |
|------|---------|
| `__main__.py` | CLI, image builds, container lifecycle, run loop |
| `profile.py` | Load `bench/profiles/*.yaml`; stdlib-only YAML parser |
| `registry.py` | Load `bench/sut/registry.yaml`; `Sut` dataclass |
| `env_fingerprint.py` | CPU/governor/HT snapshot for `results.json` |
| `container.py` | `build_image`, `Container`, `wait_tcp`, `wait_http` |
| `control.py` | TCP ndjson client for the emit-app control socket |
| `sink_client.py` | Client for the sink's stats: the blackhole sink's `/stats` and `/reset`, or the collector's receiver metrics |
| `report.py` | Statistics, `build_results()`, `write_json()`, `write_markdown()` |
| `cpu.py` | CPU per SUT container over the measurement window: cgroup `cpu.stat` or `/proc`, split by thread |
| `rounds.py` | `python3 -m driver.rounds`: interleaved CPU rounds, load gate, median and range |
| `regression.py` | `--regression-check`: compares a run against a baseline document |
| `plots.py` | Optional Plotly HTML charts |
| `flamegraph.py` | `--flamegraph`: `perf record` inside the SUT container, rendered to SVG on the host |

## Output

- `results.json`: full structured results (schema version 1.0)
- `results.md`: human-readable summary table
- `plots.html`: interactive charts, written only when Plotly is installed

## Dependencies

A benchmark run needs only the Python 3.11+ standard library. Two
extras are optional: `plotly` for the HTML charts (skipped with a
warning if it's missing), and, for `--flamegraph`, `perl` plus the
FlameGraph scripts on the host. The tests need `pytest`.

## Tests

```bash
cd bench
python3 -m pytest driver/tests/ -q
```
