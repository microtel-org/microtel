#!/usr/bin/env bash
# Copyright (c) 2026 The microtel Authors.
# SPDX-License-Identifier: Apache-2.0
#
# Interleaved hot-loop-traces rounds for the parity study (bench/parity/README.md).
# Each round runs every SUT once (10 samples each, the profile's default),
# starting one SUT later than the round before, so no SUT always runs first or
# last. Before each SUT it waits for the 1-minute load average to fall below
# 0.5, the driver's own warning threshold, and logs the load it started at.
#
# Usage:  bench/parity/run_rounds.sh <out-dir> <rounds> <sut> [<sut> ...]
# Output: <out-dir>/round<r>/<sut>/results.json, plus <out-dir>/rounds.log.
set -euo pipefail

OUT=${1:?usage: run_rounds.sh <out-dir> <rounds> <sut>...}
ROUNDS=${2:?}
shift 2
SUTS=("$@")
N=${#SUTS[@]}
BENCH_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
mkdir -p "$OUT"
LOG=$OUT/rounds.log

load1() { cut -d' ' -f1 /proc/loadavg; }
tctl() { sensors 2>/dev/null | awk '/Tctl/ { print $2; exit }'; }
wait_quiet() {
    local waited=0
    while awk -v l="$(load1)" 'BEGIN { exit !(l >= 0.5) }'; do
        (( waited >= 900 )) && { echo "   load still $(load1) after 15 min; running anyway" | tee -a "$LOG"; return; }
        sleep 15; (( waited += 15 ))
    done
}

for (( r = 1; r <= ROUNDS; r++ )); do
    echo "== round $r  $(date -u +%FT%TZ)  governor=$(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor) boost=$(cat /sys/devices/system/cpu/cpufreq/boost 2>/dev/null || echo ?)" | tee -a "$LOG"
    for (( i = 0; i < N; i++ )); do
        sut=${SUTS[$(( (i + r - 1) % N ))]}
        wait_quiet
        echo "   $(date -u +%T) $sut  load1=$(load1)  tctl=$(tctl)" | tee -a "$LOG"
        dest=$OUT/round$r/$sut
        mkdir -p "$dest"
        if ! (cd "$BENCH_DIR" && python3 -m driver --profile hot-loop-traces --sut "$sut" \
                --no-build --allow-smt --out "$dest") > "$dest/driver.log" 2>&1; then
            echo "   FAILED: $sut (see $dest/driver.log)" | tee -a "$LOG"
            touch "$OUT/round$r/FAILED"
        fi
        echo "   $(date -u +%T) $sut  done  tctl=$(tctl)" | tee -a "$LOG"
    done
done
echo "== done $(date -u +%FT%TZ)" | tee -a "$LOG"
