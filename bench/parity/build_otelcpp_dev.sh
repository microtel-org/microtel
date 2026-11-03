#!/usr/bin/env bash
# Copyright (c) 2026 The microtel Authors.
# SPDX-License-Identifier: Apache-2.0
#
# Builds bench-sut-parity-otelcpp-main-<name> images from commits of a local
# opentelemetry-cpp clone, with bench/sut/parity/otelcpp-dev.Dockerfile, twice
# each (the second with --no-cache), and compares the two builds section by
# section with compare_builds.sh, as build.sh does with PARITY_DOUBLE=1.
#
# Usage:  bench/parity/build_otelcpp_dev.sh <log-dir> <dev-image> <otelcpp-clone> <name>=<commit> ...
set -euo pipefail

LOG_DIR=${1:?usage: build_otelcpp_dev.sh <log-dir> <dev-image> <otelcpp-clone> <name>=<commit>...}
DEV_IMAGE=${2:?}
CLONE=${3:?}
shift 3
REPO_ROOT=$(git rev-parse --show-toplevel)
COMMIT=$(git -C "$REPO_ROOT" rev-parse --short HEAD)
mkdir -p "$LOG_DIR"
CRASH_RE='internal compiler error|Segmentation fault|PLEASE submit a bug report|Stack dump:|signal terminated program|collect2: fatal error|Bus error|Illegal instruction'

CTX=$(mktemp -d)
trap 'rm -rf "$CTX"' EXIT
git -C "$REPO_ROOT" archive HEAD | tar -x -C "$CTX/"

check_log() {
    if grep -qE "$CRASH_RE" "$1"; then
        echo "build_otelcpp_dev.sh: compiler/linker crash signature in $1:" >&2
        grep -nE "$CRASH_RE" "$1" | head -5 >&2
        exit 3
    fi
}

for spec in "$@"; do
    name=${spec%%=*}; rev=${spec#*=}
    full=$(git -C "$CLONE" rev-parse "$rev")
    src=$(mktemp -d)
    git -C "$CLONE" archive "$full" | tar -x -C "$src"
    # The submodules (third_party/*) are not in the archive; the build uses the
    # dev container's installed third-party packages instead, except
    # opentelemetry-proto, which the build reads from third_party/.
    git -C "$CLONE/third_party/opentelemetry-proto" archive HEAD \
        | tar -x -C "$src/third_party/opentelemetry-proto"
    tag=bench-sut-parity-otelcpp-main-$name
    cmd=(podman build -f bench/sut/parity/otelcpp-dev.Dockerfile -t "$tag"
         --label "parity.commit=$COMMIT" --label "otelcpp.commit=$full"
         --build-context "otelsrc=$src" --build-arg "DEV_IMAGE=$DEV_IMAGE")
    for pass in build check; do
        log=$LOG_DIR/$name.$pass.txt
        extra=(); t=$tag
        if [[ $pass == check ]]; then extra=(--no-cache); t=$tag:check; fi
        echo "== $t  (otelcpp $full, commit $COMMIT)"
        printf '%q ' "${cmd[@]/#$tag/$t}" "${extra[@]}" > "$log"; echo >> "$log"
        start=$(date +%s)
        if "${cmd[@]/#$tag/$t}" "${extra[@]}" "$CTX" >> "$log" 2>&1; then status=ok; else status=FAILED; fi
        echo "   $status in $(( $(date +%s) - start ))s  -> $log"
        check_log "$log"
        [[ $status == ok ]] || exit 1
    done
    rm -rf "$src"
    if ! "$REPO_ROOT/bench/parity/compare_builds.sh" "$tag" "$tag:check" \
            "$LOG_DIR/compare/$name" > "$LOG_DIR/$name.compare.txt" 2>&1; then
        echo "build_otelcpp_dev.sh: $tag and $tag:check differ:" >&2
        grep DIFF "$LOG_DIR/$name.compare.txt" | head -20 >&2
        exit 4
    fi
    echo "   $(tail -1 "$LOG_DIR/$name.compare.txt")"
done
