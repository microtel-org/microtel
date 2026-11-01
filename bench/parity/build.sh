#!/usr/bin/env bash
# Copyright (c) 2026 The microtel Authors.
# SPDX-License-Identifier: Apache-2.0
#
# Builds the parity-study SUT images (bench/parity/README.md) with podman, one
# at a time, from a `git archive` of HEAD, so local build trees stay out of the
# context and every image names the commit it came from.
#
# Usage:  bench/parity/build.sh <log-dir> [variant ...]   (default: all)
#
# With PARITY_DOUBLE=1, each variant is built a second time with --no-cache as
# <tag>:check and compared with compare_builds.sh; any section difference stops
# the run (exit 4), since this host is not memory-tested.
#
# Stops at the first build that shows a compiler or linker crash, so a crash on
# this host is looked at before any result is trusted. A clang-18.1.3 crash in
# filtered_ordered_attribute_map.cpp is the known upstream bug that made the
# published otelcpp-grpc image build opentelemetry-cpp with GCC; it is reported
# as such and also stops the run.
set -euo pipefail

LOG_DIR=${1:?usage: build.sh <log-dir> [variant ...]}
shift
REPO_ROOT=$(git rev-parse --show-toplevel)
COMMIT=$(git -C "$REPO_ROOT" rev-parse --short HEAD)
mkdir -p "$LOG_DIR"

# Ubuntu GCC 13 turns these on by default; the "nohard" variants turn them off.
NOHARD="-fno-stack-protector -fcf-protection=none -fno-stack-clash-protection -U_FORTIFY_SOURCE"

OTEL=bench/sut/parity/otelcpp.Dockerfile
MT=bench/sut/parity/microtel.Dockerfile

# name | dockerfile | build args (space-separated KEY=VALUE; values without spaces,
# except EXTRA_FLAGS, which is passed separately)
declare -A FILE ARGS EXTRA
add() { FILE[$1]=$2; ARGS[$1]=$3; EXTRA[$1]=${4:-}; }
add otelcpp-http-clang        $OTEL "EXPORTER=http SDK_CC=clang-18 SDK_CXX=clang++-18"
add otelcpp-grpc-clang        $OTEL "EXPORTER=grpc SDK_CC=clang-18 SDK_CXX=clang++-18"
add otelcpp-http-gcc-nohard   $OTEL "EXPORTER=http SDK_CC=gcc SDK_CXX=g++" "$NOHARD"
add otelcpp-grpc-gcc-nohard   $OTEL "EXPORTER=grpc SDK_CC=gcc SDK_CXX=g++" "$NOHARD"
add otelcpp-http-clang-pbsrc  $OTEL "EXPORTER=http SDK_CC=clang-18 SDK_CXX=clang++-18 PROTOBUF_TAG=v21.12"
add otelcpp-http-clang-inmem  $OTEL "EXPORTER=http SDK_CC=clang-18 SDK_CXX=clang++-18 INMEMORY=ON"
add otelcpp130-http-clang     $OTEL "OTELCPP_VERSION=v1.30.0 EXPORTER=http SDK_CC=clang-18 SDK_CXX=clang++-18"
add otelcpp130-grpc-clang     $OTEL "OTELCPP_VERSION=v1.30.0 EXPORTER=grpc SDK_CC=clang-18 SDK_CXX=clang++-18"
add microtel-http-clang       $MT   "CC_BIN=clang-18 CXX_BIN=clang++-18 GRPC=OFF"
add microtel-grpc-clang       $MT   "CC_BIN=clang-18 CXX_BIN=clang++-18 GRPC=ON"
add microtel-http-gcc         $MT   "CC_BIN=gcc CXX_BIN=g++ GRPC=OFF"
add microtel-grpc-gcc         $MT   "CC_BIN=gcc CXX_BIN=g++ GRPC=ON"
ORDER=(microtel-http-clang microtel-grpc-clang microtel-http-gcc microtel-grpc-gcc
       otelcpp-http-clang otelcpp-http-clang-inmem otelcpp-http-gcc-nohard
       otelcpp-http-clang-pbsrc otelcpp-grpc-gcc-nohard otelcpp130-http-clang
       otelcpp-grpc-clang otelcpp130-grpc-clang)

# Only these get a :perf image (linux-tools in the runtime stage) for part 2.
PERF_VARIANTS=" otelcpp-http-clang otelcpp-http-clang-inmem microtel-http-clang "

VARIANTS=("$@")
[[ ${#VARIANTS[@]} -eq 0 ]] && VARIANTS=("${ORDER[@]}")

CTX=$(mktemp -d)
trap 'rm -rf "$CTX"' EXIT
git -C "$REPO_ROOT" archive HEAD | tar -x -C "$CTX"

CRASH_RE='internal compiler error|Segmentation fault|PLEASE submit a bug report|Stack dump:|clang(\+\+)?(-18)?: error: unable to execute command|signal terminated program|collect2: fatal error|Bus error|Illegal instruction'

for v in "${VARIANTS[@]}"; do
    [[ -n ${FILE[$v]:-} ]] || { echo "build.sh: unknown variant $v" >&2; exit 2; }
    perfs=(false)
    [[ $PERF_VARIANTS == *" $v "* ]] && perfs+=(true)
    for perf in "${perfs[@]}"; do
        tag=bench-sut-parity-$v; log=$LOG_DIR/$v.log
        if [[ $perf == true ]]; then tag+=":perf"; log=$LOG_DIR/$v.perf.log; fi
        cmd=(podman build -f "${FILE[$v]}" -t "$tag" --label "parity.commit=$COMMIT"
             --build-arg INSTALL_PERF=$perf)
        for kv in ${ARGS[$v]}; do cmd+=(--build-arg "$kv"); done
        [[ -n ${EXTRA[$v]} ]] && cmd+=(--build-arg "EXTRA_FLAGS=${EXTRA[$v]}")
        echo "== $tag  (commit $COMMIT)"
        printf '%q ' "${cmd[@]}" > "$log"; echo >> "$log"
        start=$(date +%s)
        if "${cmd[@]}" "$CTX" >> "$log" 2>&1; then status=ok; else status=FAILED; fi
        echo "   $status in $(( $(date +%s) - start ))s  -> $log"
        if grep -qE "$CRASH_RE" "$log"; then
            echo "build.sh: compiler/linker crash signature in $log:" >&2
            grep -nE "$CRASH_RE" "$log" | head -5 >&2
            exit 3
        fi
        [[ $status == ok ]] || exit 1
        if [[ ${PARITY_DOUBLE:-0} == 1 && $perf == false ]]; then
            clog=$LOG_DIR/$v.check.log
            echo "== $tag:check  (--no-cache rebuild)"
            start=$(date +%s)
            if "${cmd[@]/#$tag/$tag:check}" --no-cache "$CTX" > "$clog" 2>&1; then status=ok; else status=FAILED; fi
            echo "   $status in $(( $(date +%s) - start ))s  -> $clog"
            if grep -qE "$CRASH_RE" "$clog"; then
                echo "build.sh: compiler/linker crash signature in $clog:" >&2
                grep -nE "$CRASH_RE" "$clog" | head -5 >&2
                exit 3
            fi
            [[ $status == ok ]] || exit 1
            if ! "$REPO_ROOT/bench/parity/compare_builds.sh" "$tag" "$tag:check" \
                    "$LOG_DIR/compare/$v" > "$LOG_DIR/$v.compare.log" 2>&1; then
                echo "build.sh: $tag and $tag:check differ:" >&2
                grep DIFF "$LOG_DIR/$v.compare.log" | head -20 >&2
                exit 4
            fi
            echo "   $(tail -1 "$LOG_DIR/$v.compare.log")"
        fi
    done
done
