# Copyright (c) 2026 The microtel Authors.
# SPDX-License-Identifier: Apache-2.0
"""Bench driver entry point — python3 -m driver [options]."""

from __future__ import annotations

import argparse
import dataclasses
import subprocess
import sys
import time
from pathlib import Path
from typing import Any, Optional, Union

from .container import (
    Container,
    _image_id,
    build_image,
    create_network,
    detect_engine,
    remove_network,
    wait_http,
    wait_tcp,
)
from .control import ControlClient
from .env_fingerprint import capture as capture_env
from .env_fingerprint import warnings as env_warnings
from .flamegraph import (
    FlameGraphRecorder,
    check_prerequisites,
    find_flamegraph_dir,
    perf_image_name,
)
from .profile import load as load_profile
from .registry import filter_b0
from .registry import get as get_sut
from .registry import load as load_registry
from .plots import write_plots
from .regression import check as check_regression
from .regression import format_report as format_regression_report
from .report import build_results, write_json, write_markdown
from . import tls_material
from .sink_client import CollectorSinkClient, SinkClient

_BENCH_DIR = Path(__file__).parent.parent
_PROFILES_DIR = _BENCH_DIR / "profiles"
_REGISTRY_PATH = _BENCH_DIR / "sut" / "registry.yaml"
_BLACKHOLE_BUILD_CONTEXT = _BENCH_DIR / "sink" / "blackhole"
_BLACKHOLE_IMAGE = "bench-sink-blackhole"
_COLLECTOR_BUILD_CONTEXT = _BENCH_DIR / "sink" / "collector"
_COLLECTOR_IMAGE = "bench-sink-collector"
_NET_NAME = "bench-net"

AnySinkClient = Union[SinkClient, CollectorSinkClient]


def _parse_args(argv=None) -> argparse.Namespace:
    p = argparse.ArgumentParser(
        prog="python3 -m driver",
        description="microtel benchmark driver",
    )
    p.add_argument("--profile", default="hot-loop-traces",
                   help="Profile name (default: hot-loop-traces)")
    p.add_argument("--sut", default=None,
                   help="Run only this SUT (default: all B0 SUTs in profile)")
    p.add_argument("--reps", type=int, default=None,
                   help="Override number of samples (default: from profile)")
    p.add_argument("--out", default=str(_BENCH_DIR / "results"),
                   help="Output directory (default: bench/results)")
    p.add_argument("--no-build", action="store_true",
                   help="Skip image builds (use cached images)")
    p.add_argument("--engine", default="auto",
                   choices=("auto", "podman", "docker"),
                   help="Container engine (default: auto)")
    p.add_argument("--seed", type=int, default=0,
                   help="Random seed placeholder (unused in B0)")
    p.add_argument("--sink", default="auto",
                   choices=("auto", "blackhole", "collector"),
                   help="Sink mode: auto uses profile default (default: auto)")
    p.add_argument("--regression-check", default=None, metavar="BASELINE",
                   help="Path to baseline results.json; exit 2 on regression")
    p.add_argument("--threshold", type=float, default=0.05,
                   help="Regression threshold as a fraction (default: 0.05 = 5%%)")
    p.add_argument("--allow-smt", action="store_true",
                   help="Suppress hyperthreading/SMT warning")
    p.add_argument("--verbose", action="store_true",
                   help="Show container build and run output")
    p.add_argument("--flamegraph", action="store_true",
                   help=(
                       "Generate SVG flamegraphs per SUT (B3). Builds :perf image "
                       "variants with linux-perf installed; runs one dedicated "
                       "profiling sample after measurements using --cap-add=PERFMON. "
                       "Requires perl on PATH and FlameGraph scripts "
                       "(set FLAMEGRAPH_DIR or pass --flamegraph-dir). "
                       "Normal benchmark runs never need this flag."
                   ))
    p.add_argument("--flamegraph-dir", default=None, metavar="DIR",
                   help="Path to FlameGraph scripts directory (overrides FLAMEGRAPH_DIR "
                        "env var and default search paths)")
    p.add_argument("--sweep-attr-bytes", default=None, metavar="SIZES",
                   help="Comma-separated list of EMIT_ATTRIBUTE_VALUE_BYTES values to sweep "
                        "(e.g. '0,64,256,1024,4096'). Each value produces a separate SUT "
                        "result named <sut>-<N>b so all sizes appear in one results.json.")
    p.add_argument(
        "--sweep-threads", default=None,
        metavar="N,N,...",
        help="Comma-separated thread counts to sweep (e.g. 1,2,4,8,16). "
             "Creates one virtual SUT per count named {sut}-{N}t.",
    )
    p.add_argument(
        "--sweep-leaves", default=None, dest="sweep_leaves",
        metavar="N,N,...",
        help="Comma-separated leaf counts to sweep for the leaf-fanin profile "
             "(e.g. 1,10,100,1000). Creates one virtual SUT per count named "
             "{sut}-{N}l, with EMIT_LEAVES set.",
    )
    p.add_argument(
        "--sink-delay-ms", type=int, default=0, dest="sink_delay_ms",
        metavar="MS",
        help="Artificial per-request response delay injected into the blackhole sink (ms). "
             "Drives the BSP queue toward saturation to measure backpressure behaviour.",
    )
    return p.parse_args(argv)


def _log(msg: str) -> None:
    print(f"[driver] {msg}", file=sys.stderr, flush=True)


# The emit-app's spans_emitted counts workload *iterations* (one latency
# sample each), not spans: EMIT_WORKLOAD=realistic_request emits a parent and
# two children per iteration.  Delivery is spans received over spans sent, so
# the denominator scales by this factor.  For signal=logs the unit is log
# records rather than spans.
_SPANS_PER_ITERATION = {
    "hot_loop":          1,
    "realistic_request": 3,
    "hot_loop_metrics":  0,   # emits metric records, no spans
    "hot_loop_logs":     1,   # one log record per iteration
}
_DEFAULT_SPANS_PER_ITERATION = 1
# leaf_fanin ingests one leaf payload per iteration; its span count is a
# profile setting, with the emit-app's default.
_DEFAULT_LEAF_SPANS_PER_PAYLOAD = 10


def _spans_per_iteration(env: dict) -> int:
    """Spans the emit-app produces per workload iteration, per EMIT_WORKLOAD."""
    workload = str(env.get("EMIT_WORKLOAD", "hot_loop"))
    if workload == "leaf_fanin":
        return int(env.get("EMIT_LEAF_SPANS_PER_PAYLOAD", _DEFAULT_LEAF_SPANS_PER_PAYLOAD))
    return _SPANS_PER_ITERATION.get(workload, _DEFAULT_SPANS_PER_ITERATION)


def _export_requests(sink_snap: dict) -> int:
    """Export requests the sink received, over either protocol."""
    return int(sink_snap.get("http_requests_received") or 0) + int(
        sink_snap.get("grpc_requests_received") or 0)


def _expand_sweep_leaves(suts: list, counts: str, image_ids: dict) -> Optional[list]:
    """One virtual SUT per leaf count, named <sut>-<N>l with EMIT_LEAVES=N.

    Returns None when @p counts is not a comma-separated list of integers. The
    image id of each virtual SUT is its original's.
    """
    try:
        leaf_counts = [int(c.strip()) for c in counts.split(",")]
    except ValueError:
        return None
    expanded = []
    for sut in suts:
        for n in leaf_counts:
            sweep_sut = dataclasses.replace(
                sut, name=f"{sut.name}-{n}l", env={**sut.env, "EMIT_LEAVES": str(n)})
            expanded.append(sweep_sut)
            image_ids[sweep_sut.name] = image_ids.get(sut.name, "")
    return expanded


def _received_count(sink_snap: dict, signal: str) -> int:
    """Items of the profile's signal the sink counted: log records for
    signal=logs, spans otherwise."""
    if signal == "logs":
        return int(sink_snap.get("log_records_received") or 0)
    return int(sink_snap["spans_received"])


def _sink_mode_error(signal: str, sink_mode: str) -> Optional[str]:
    """Reason the sink cannot measure this signal, or None if it can.

    The collector sink runs only a traces pipeline and scrapes span counters.
    """
    if signal == "logs" and sink_mode == "collector":
        return "signal=logs needs --sink blackhole; the collector sink counts spans only"
    return None


def _delivery_rate_pct(spans_received: int, spans_expected: int,
                       signal: str) -> Optional[float]:
    """Percentage of the items sent that reached the sink, or None if undefined.

    Delivery is meaningful for trace and log profiles: the sink decodes and
    counts OTLP spans and log records.  Metric exports land at /v1/metrics and
    are not decoded, so nothing is counted for signal=metrics profiles.
    """
    if signal not in ("traces", "logs") or spans_expected <= 0:
        return None
    return round(spans_received / spans_expected * 100, 2)


# Set by _start_sink when the sink's OTLP/HTTP receiver is TLS-only (collector
# mode).  A stock collector serves only HTTP/1.1 on plaintext, where microtel
# OTLP/HTTP falls back to serial HTTP/1.1, so the HTTP leg uses https:// (ALPN
# h2) to measure HTTP/2.
_SINK_TLS_DIR: Optional[Path] = None


def _otlp_endpoint(protocol: str) -> str:
    if protocol == "grpc":
        return "http://sink:4317"
    if _SINK_TLS_DIR is not None:
        return "https://sink:4318"
    return "http://sink:4318"


def _build_sink_image(engine: str, sink_mode: str, verbose: bool) -> None:
    if sink_mode == "blackhole":
        _log("building blackhole-sink image ...")
        build_image(
            engine,
            dockerfile=str(_BLACKHOLE_BUILD_CONTEXT / "Dockerfile"),
            image_name=_BLACKHOLE_IMAGE,
            build_context=_BLACKHOLE_BUILD_CONTEXT,
            verbose=verbose,
        )
    else:
        _log("building collector-sink image ...")
        build_image(
            engine,
            dockerfile=str(_COLLECTOR_BUILD_CONTEXT / "Dockerfile"),
            image_name=_COLLECTOR_IMAGE,
            build_context=_COLLECTOR_BUILD_CONTEXT,
            verbose=verbose,
        )


def _build_images(
    engine: str,
    sink_mode: str,
    active_suts: list,
    repo_root: Path,
    no_build: bool,
    verbose: bool,
    flamegraph: bool = False,
) -> dict[str, str]:
    """Build (or look up) all container images. Returns sut_name -> image_id.

    When flamegraph=True, additionally builds a :perf variant of each SUT image
    (with linux-perf installed) tagged as <image_name>:perf.  Normal images are
    also built so that measurement samples run against the unmodified binary.
    """
    sut_image_ids: dict[str, str] = {}
    if no_build:
        _log("--no-build: skipping image builds")
        for sut in active_suts:
            tag = perf_image_name(sut.image_name) if flamegraph else sut.image_name
            sut_image_ids[sut.name] = _image_id(engine, tag)
        return sut_image_ids

    _build_sink_image(engine, sink_mode, verbose)
    for sut in active_suts:
        _log(f"building {sut.name} image ...")
        sut_image_ids[sut.name] = build_image(
            engine,
            dockerfile=str(repo_root / sut.dockerfile),
            image_name=sut.image_name,
            build_context=repo_root,
            verbose=verbose,
        )
        if flamegraph:
            _log(f"building {sut.name}:perf image (INSTALL_PERF=true) ...")
            build_image(
                engine,
                dockerfile=str(repo_root / sut.dockerfile),
                image_name=perf_image_name(sut.image_name),
                build_context=repo_root,
                build_args={"INSTALL_PERF": "true"},
                verbose=verbose,
            )
    return sut_image_ids


def _wait_ready_line(cont: Any, needle: str, timeout: float) -> None:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if needle in cont.logs():
            return
        time.sleep(0.5)
    _dump_container_logs(cont, f"container {cont._name}")
    raise TimeoutError(f"{cont._name}: no {needle!r} in logs after {timeout}s")


def _dump_container_logs(cont: Any, label: str) -> None:
    """Print a container's state, exit code and logs (used when it fails)."""
    import subprocess
    state = subprocess.run(
        [cont._engine, "inspect", "--format",
         "{{.State.Status}} exit={{.State.ExitCode}} {{.State.Error}}", cont._name],
        capture_output=True, text=True, check=False,
    )
    print(f"--- {label} state: {(state.stdout or state.stderr).strip()}", file=sys.stderr)
    print(f"--- {label} container logs ---", file=sys.stderr)
    print(cont.logs() or "(no logs)", file=sys.stderr)
    print(f"--- end {label} logs ---", file=sys.stderr, flush=True)


def _start_sink(engine: str, sink_mode: str, verbose: bool, sink_delay_ms: int = 0) -> tuple[Any, AnySinkClient]:
    """Start the sink container and return (Container context, SinkClient)."""
    sink_c = Container(engine, "bench-sink", verbose=verbose)
    if sink_mode == "blackhole":
        sink_env: dict = {}
        if sink_delay_ms > 0:
            sink_env["SINK_RESPONSE_DELAY_MS"] = str(sink_delay_ms)
        sink_c.start(
            image=_BLACKHOLE_IMAGE,
            ports={19080: 19080},
            network=_NET_NAME,
            network_alias="sink",
            env=sink_env if sink_env else None,
        )
        _log("waiting for blackhole-sink /health ...")
        wait_http("http://127.0.0.1:19080/health", timeout=30.0)
        return sink_c, SinkClient("127.0.0.1", 19080)

    global _SINK_TLS_DIR
    _SINK_TLS_DIR = tls_material.generate()
    sink_c.start(
        image=_COLLECTOR_IMAGE,
        ports={8888: 8888, 13133: 13133},
        volumes={str(_SINK_TLS_DIR): "/certs"},
        network=_NET_NAME,
        network_alias="sink",
    )
    _log("waiting for collector-sink health check ...")
    try:
        wait_http("http://127.0.0.1:13133/health/status", timeout=60.0)
    except TimeoutError:
        _dump_container_logs(sink_c, "bench-sink")
        sink_c.stop()
        raise
    return sink_c, CollectorSinkClient("127.0.0.1")


def _run_sut(
    engine: str,
    sut: Any,
    sink_client: AnySinkClient,
    n_samples: int,
    warmup_spans: int,
    spans_per_sample: int,
    profile_env: dict,
    threads: int,
    rate_hz: int,
    signal: str,
    verbose: bool,
    flamegraph_dir: Optional[Path] = None,
    out_dir: Optional[Path] = None,
) -> tuple[list[dict], Optional[Path]]:
    """Start a SUT container, run warmup + N timed samples, return (samples, svg_path).

    When flamegraph_dir is provided the container is started with the :perf image
    and --cap-add=PERFMON.  After all measurement samples, one dedicated profiling
    sample is recorded with perf; the resulting SVG is written to out_dir.
    """
    samples: list[dict] = []
    svg_path: Optional[Path] = None

    image = perf_image_name(sut.image_name) if flamegraph_dir else sut.image_name
    cap_add = ["PERFMON"] if flamegraph_dir else None
    container_name = f"bench-sut-{sut.name}"

    with Container(engine, container_name, verbose=verbose) as c:
        # profile_env is the base; sut.env overrides (sweep values live in sut.env).
        env = {**profile_env, **sut.env}
        env["OTEL_EXPORTER_OTLP_ENDPOINT"] = _otlp_endpoint(sut.protocol)
        volumes = None
        if _SINK_TLS_DIR is not None and sut.protocol == "http":
            env["OTEL_EXPORTER_OTLP_CERTIFICATE"] = "/certs/ca.crt"
            volumes = {str(_SINK_TLS_DIR): "/certs"}
        spans_per_iter = _spans_per_iteration(env)
        try:
            c.start(
                image=image,
                ports={sut.ports.control: sut.ports.control},
                env=env,
                network=_NET_NAME,
                cap_add=cap_add,
                volumes=volumes,
            )
        except subprocess.CalledProcessError as exc:
            stderr = (exc.stderr or "").lower()
            if "address already in use" in stderr or "already in use" in stderr:
                raise RuntimeError(
                    f"SUT container {sut.name!r} failed to start — "
                    f"port {sut.ports.control} is already in use. "
                    f"Check with: ss -tlnp | grep {sut.ports.control}"
                ) from exc
            raise RuntimeError(
                f"SUT container {sut.name!r} failed to start "
                f"(exit {exc.returncode}): "
                f"{exc.stderr.strip() if exc.stderr else '(no stderr)'}"
            ) from exc
        _log(f"  waiting for {sut.name} control port ...")
        wait_tcp("127.0.0.1", sut.ports.control, timeout=30.0)
        # The engine's port proxy accepts before emit_app listens (it connects
        # to the exporter first); wait for emit_app's own ready line.
        _wait_ready_line(c, "ready on control port", timeout=60.0)

        try:
            with ControlClient("127.0.0.1", sut.ports.control) as ctrl:
                _log(f"  warmup ({warmup_spans} spans) ...")
                ctrl.run(warmup_spans, threads=threads, rate_hz=rate_hz)
                ctrl.flush()   # drain both BSP and exporter queues before resetting
                time.sleep(0.1)
                sink_client.reset()

                for i in range(n_samples):
                    sink_client.reset()
                    result = ctrl.run(spans_per_sample, threads=threads, rate_hz=rate_hz)
                    flush_result = ctrl.flush()
                    time.sleep(0.5)
                    sink_snap = sink_client.stats()
                    sink_errors = sink_snap.get("errors", 0)
                    sink_http_req = sink_snap.get("http_requests_received", 0)
                    sink_grpc_req = sink_snap.get("grpc_requests_received", 0)
                    export_requests = _export_requests(sink_snap)
                    _log(
                        f"  sample {i + 1}/{n_samples}: "
                        f"emitted={result['spans_emitted']} "
                        f"sink_received={_received_count(sink_snap, signal)} "
                        f"(http={sink_http_req} grpc={sink_grpc_req}) "
                        f"p50={result['latency_p50_ns']}ns "
                        f"flush={flush_result.get('flush_ns', 0)}ns"
                    )
                    if sink_errors > 0:
                        _log(
                            f"  WARNING: sink errors={sink_errors} "
                            f"last_error={sink_snap.get('last_error', '')!r}"
                        )
                    elif _received_count(sink_snap, signal) == 0 and sink_http_req == 0 and sink_grpc_req == 0:
                        _log(
                            "  WARNING: sink received 0 requests — "
                            "check endpoint URL, port, and container network"
                        )
                    dur_ns = result.get("duration_ns", 0)
                    flush_ns = flush_result.get("flush_ns", 0)
                    bytes_rx = sink_snap.get("bytes_received")
                    total_ns = dur_ns + flush_ns
                    spans_rx = _received_count(sink_snap, signal)
                    # Wire throughput: bytes land at sink during flush, not during emit.
                    # Use flush_ns (the actual HTTP-transfer window) as denominator.
                    throughput_mbps = (
                        bytes_rx * 8000.0 / flush_ns
                        if (bytes_rx is not None and flush_ns > 0)
                        else None
                    )
                    spans_per_sec = (
                        spans_rx / total_ns * 1e9
                        if (spans_rx > 0 and total_ns > 0)
                        else None
                    )
                    spans_emitted_n = result["spans_emitted"]
                    spans_expected = spans_emitted_n * spans_per_iter
                    delivery_rate_pct = _delivery_rate_pct(spans_rx, spans_expected, signal)
                    samples.append({
                        "spans_emitted":  result["spans_emitted"],
                        "spans_expected": spans_expected,
                        "items_received": spans_rx,
                        "spans_dropped":  result["spans_dropped"],
                        "bytes_sent":     result.get("bytes_sent", 0),
                        "duration_ns":     dur_ns,
                        "spans_per_sec":   spans_per_sec,
                        "throughput_mbps": throughput_mbps,
                        "delivery_rate_pct": delivery_rate_pct,
                        "latency_p50_ns":    result["latency_p50_ns"],
                        "latency_p95_ns":    result["latency_p95_ns"],
                        "latency_p99_ns":    result["latency_p99_ns"],
                        "latency_min_ns":    result["latency_min_ns"],
                        "latency_max_ns":    result["latency_max_ns"],
                        "latency_histogram": result.get("latency_histogram", []),
                        "flush_ns":          flush_ns,
                        "export_requests":   export_requests,
                        "sink": {
                            "mode":                   sink_snap["mode"],
                            "spans_received":         sink_snap["spans_received"],
                            "log_records_received":   sink_snap.get("log_records_received", 0),
                            "bytes_received":         sink_snap["bytes_received"],
                            "http_requests_received": sink_http_req,
                            "grpc_requests_received": sink_grpc_req,
                            "errors":                 sink_errors,
                            "last_error":             sink_snap.get("last_error", ""),
                        },
                    })

                if flamegraph_dir and out_dir is not None:
                    _log(f"  flamegraph: recording dedicated profiling sample ...")
                    recorder = FlameGraphRecorder(engine, container_name)
                    recorder.start()
                    ctrl.run(spans_per_sample, threads=threads, rate_hz=rate_hz)
                    svg_path = recorder.finish(sut.name, out_dir, flamegraph_dir)
                    if svg_path:
                        _log(f"  flamegraph: SVG written to {svg_path}")
                    else:
                        _log(f"  flamegraph: SVG generation failed for {sut.name}")

                ctrl.quit()
        except (ConnectionError, OSError, TimeoutError):
            # The SUT usually died (e.g. backend init failed); show why.
            _dump_container_logs(c, f"SUT {sut.name}")
            raise

    return samples, svg_path


def _collect_sut_results(
    engine: str,
    active_suts: list,
    sink_client: AnySinkClient,
    sut_image_ids: dict[str, str],
    n_samples: int,
    profile: Any,
    verbose: bool,
    flamegraph_dir: Optional[Path] = None,
    out_dir: Optional[Path] = None,
) -> list[dict]:
    sut_results = []
    for sut in active_suts:
        _log(f"--- SUT: {sut.name} ---")
        try:
            samples, svg_path = _run_sut(
                engine=engine,
                sut=sut,
                sink_client=sink_client,
                n_samples=n_samples,
                warmup_spans=profile.warmup_spans,
                spans_per_sample=profile.spans_per_sample,
                profile_env=profile.env,
                threads=sut.threads_override if sut.threads_override is not None else profile.threads,
                rate_hz=profile.target_rate_hz,
                signal=profile.signal,
                verbose=verbose,
                flamegraph_dir=flamegraph_dir,
                out_dir=out_dir,
            )
        except Exception as exc:
            _log(f"ERROR running {sut.name}: {exc}")
            samples, svg_path = [], None

        image_tag = (
            perf_image_name(sut.image_name) if flamegraph_dir else sut.image_name
        )
        sut_results.append({
            "name":                sut.name,
            "library":             sut.library,
            "transport":           sut.transport,
            "library_version":     "",
            "library_build_flags": "",
            "image_tag":           image_tag,
            "image_id":            sut_image_ids.get(sut.name, ""),
            "samples":             samples,
            "flamegraph_svg":      str(svg_path) if svg_path else None,
        })
    return sut_results


def main(argv=None) -> int:
    args = _parse_args(argv)
    out_dir = Path(args.out)
    repo_root = _BENCH_DIR.parent

    profile = load_profile(_PROFILES_DIR, args.profile)
    all_suts = load_registry(_REGISTRY_PATH)
    b0_suts = filter_b0(all_suts)

    if args.sut:
        sut = get_sut(all_suts, args.sut)
        if sut is None:
            _log(f"ERROR: SUT {args.sut!r} not found in registry")
            return 1
        active_suts = [sut]
    else:
        active_suts = [s for s in b0_suts if s.name in profile.suts]

    if not active_suts:
        _log("ERROR: no SUTs to run (check profile.suts and registry b0 flags)")
        return 1

    n_samples = args.reps if args.reps is not None else profile.samples
    engine = detect_engine(args.engine)
    sink_mode = args.sink if args.sink != "auto" else profile.sink_mode
    sink_error = _sink_mode_error(profile.signal, sink_mode)
    if sink_error:
        _log(f"ERROR: {sink_error}")
        return 1

    # Resolve flamegraph settings early so we can fail fast before building images.
    flamegraph_dir: Optional[Path] = None
    if args.flamegraph:
        flamegraph_dir = (
            Path(args.flamegraph_dir) if args.flamegraph_dir else find_flamegraph_dir()
        )
        problems = check_prerequisites(flamegraph_dir)
        if problems:
            for p in problems:
                _log(f"ERROR (--flamegraph): {p}")
            return 1
        _log(f"flamegraph: scripts at {flamegraph_dir}")
        _log("flamegraph: NOTE — measurement samples run inside :perf image with "
             "--cap-add=PERFMON; a separate profiling sample is recorded afterward")

    _log(f"engine: {engine}")
    _log(
        f"profile: {profile.name} — "
        f"{profile.spans_per_sample} spans/sample x {n_samples} samples"
    )
    _log(f"SUTs: {[s.name for s in active_suts]}")
    _log(f"sink: {sink_mode}")
    if args.sink_delay_ms > 0:
        _log(f"sink delay: {args.sink_delay_ms} ms per request")

    fp = capture_env(engine)
    warns = env_warnings(fp, args.allow_smt)
    for w in warns:
        _log(f"WARNING: {w}")

    # Build images before sweep expansion (deduplicated by image).
    sut_image_ids = _build_images(
        engine, sink_mode, active_suts, repo_root,
        args.no_build, args.verbose, flamegraph=args.flamegraph,
    )

    # Expand active_suts for --sweep-attr-bytes: one virtual SUT per size,
    # named <sut>-<N>b. Image ID is shared with the original SUT.
    if args.sweep_attr_bytes:
        try:
            sweep_sizes = [int(s.strip()) for s in args.sweep_attr_bytes.split(",")]
        except ValueError:
            _log("ERROR: --sweep-attr-bytes must be comma-separated integers")
            return 1
        expanded = []
        for sut in active_suts:
            for size in sweep_sizes:
                sweep_sut = dataclasses.replace(
                    sut,
                    name=f"{sut.name}-{size}b",
                    env={**sut.env, "EMIT_ATTRIBUTE_VALUE_BYTES": str(size)},
                )
                expanded.append(sweep_sut)
                sut_image_ids[sweep_sut.name] = sut_image_ids.get(sut.name, "")
        active_suts = expanded
        _log(f"sweep: {len(sweep_sizes)} sizes × {len(active_suts)//len(sweep_sizes)} SUT(s) "
             f"= {len(active_suts)} runs")

    if args.sweep_leaves:
        expanded_leaves = _expand_sweep_leaves(active_suts, args.sweep_leaves, sut_image_ids)
        if expanded_leaves is None:
            _log("ERROR: --sweep-leaves must be comma-separated integers")
            return 1
        active_suts = expanded_leaves
        _log(f"leaf sweep: {args.sweep_leaves} — {len(active_suts)} total runs")

    if args.sweep_threads:
        sweep_thread_counts = [int(t.strip()) for t in args.sweep_threads.split(",")]
        expanded = []
        for t in sweep_thread_counts:
            for sut in active_suts:
                expanded.append(dataclasses.replace(sut, name=f"{sut.name}-{t}t",
                                                     threads_override=t))
        active_suts = expanded
        _log(f"thread sweep: {sweep_thread_counts} — {len(active_suts)} total runs")

    create_network(engine, _NET_NAME)
    sut_results = []
    try:
        try:
            sink_c, sink_client = _start_sink(engine, sink_mode, args.verbose, sink_delay_ms=args.sink_delay_ms)
        except subprocess.CalledProcessError as exc:
            stderr = (exc.stderr or "").lower()
            if "address already in use" in stderr or "already in use" in stderr:
                _log(
                    f"ERROR: sink container failed to start — a port required by the "
                    f"bench sink is already in use. Check with: ss -tlnp | grep "
                    f"'4317\\|4318\\|19080'"
                )
            else:
                _log(f"ERROR: sink container failed to start (exit {exc.returncode}): "
                     f"{exc.stderr.strip() if exc.stderr else '(no stderr)'}")
            return 1
        with sink_c:
            sut_results = _collect_sut_results(
                engine, active_suts, sink_client, sut_image_ids,
                n_samples, profile, args.verbose,
                flamegraph_dir=flamegraph_dir,
                out_dir=out_dir,
            )
    finally:
        remove_network(engine, _NET_NAME)

    if sink_mode == "collector":
        warns = list(warns) + [
            "Sink mode 'collector' has higher span-count variance than 'blackhole'. "
            "Wire-bytes metrics are unavailable in collector mode."
        ]

    env_data = dataclasses.asdict(fp)
    profile_data = dataclasses.asdict(profile)
    doc = build_results(profile_data, env_data, sut_results, warns)

    json_path = write_json(doc, out_dir)
    md_path = write_markdown(doc, out_dir)
    plots_path = write_plots(doc, out_dir)
    _log(f"results written to {json_path}")
    _log(f"report written to {md_path}")
    if plots_path:
        _log(f"plots written to {plots_path}")

    for sr in doc["suts"]:
        summary = sr.get("summary", {})
        p50 = summary.get("latency_p50_ns", {})
        delivery = summary.get("delivery_rate_pct")
        delivery_str = f"{delivery:.2f}%" if delivery is not None else "N/A"
        _log(
            f"{sr['name']}: p50={p50.get('median', 0):.0f}ns  "
            f"drop_rate={summary.get('drop_rate_pct', 0)}%  "
            f"delivery={delivery_str}  "
            f"samples={summary.get('reps', 0)}"
        )

    if args.regression_check:
        return _run_regression_check(args.regression_check, doc, args.threshold)

    return 0


def _run_regression_check(baseline_path: str, current: dict, threshold: float) -> int:
    import json
    from pathlib import Path as _Path

    try:
        baseline = json.loads(_Path(baseline_path).read_text(encoding="utf-8"))
    except (OSError, ValueError) as exc:
        _log(f"ERROR: could not load baseline {baseline_path!r}: {exc}")
        return 1

    regressions = check_regression(baseline, current, threshold)
    report = format_regression_report(regressions)
    for line in report.splitlines():
        _log(line)

    if regressions:
        _log(f"FAIL: {len(regressions)} regression(s) detected "
             f"(threshold {threshold * 100:.0f}%)")
        return 2

    _log(f"PASS: no regressions vs {baseline_path}")
    return 0


def _cli() -> int:
    """Entry point wrapper: turn expected failures into a clean message.

    A failing container build is an ordinary outcome of running the harness
    (a missing package, a compiler error in a SUT), not a driver bug. Letting
    CalledProcessError escape printed a Python traceback whose final line was
    just the exit status -- container.py has already echoed the real error by
    the time we get here, so a traceback adds nothing but noise.
    """
    try:
        return main()
    except subprocess.CalledProcessError as exc:
        prog = exc.cmd[0] if exc.cmd else "command"
        _log(f"FAIL: {prog} exited {exc.returncode}; see the error above")
        return 1
    except KeyboardInterrupt:
        _log("interrupted")
        return 130


if __name__ == "__main__":
    sys.exit(_cli())
