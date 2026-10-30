#!/usr/bin/env bash
# Copyright (c) 2026 The microtel Authors.
# SPDX-License-Identifier: Apache-2.0
#
# Conformance gate. Starts a pinned OpenTelemetry Collector container, waits
# for it to report healthy, and runs every ctest carrying the `conformance`
# label against it.
#
# This is the Tier 1 conformance gate (docs/compatibility-matrix.md §7):
# "OTLP/HTTP trace export passes
# integration tests against the pinned OpenTelemetry Collector matrix", and the
# same for OTLP/gRPC. A mock cannot discharge it — the claim is about a real
# receiver accepting real bytes — so the collector is the system under test as
# much as microtel is.
#
# The collector exposes five receiver pairs (plain / TLS / mTLS / bearer auth /
# leaf)
# on distinct ports; see tests/conformance/collector/config.yaml. Endpoints,
# cert paths and the output file reach the tests through the environment
# contract exported below. Tests skip when a variable is absent, so
# MICROTEL_CONFORMANCE_REQUIRE is exported too: with it set, an unset variable
# fails the test rather than silently skipping it, which is the only way a
# broken export here cannot masquerade as a green gate.
#
# The pinned image tag is duplicated in docs/interop-matrix.md (with its
# digest), in bench/sink/collector/Dockerfile, and in
# examples/stack/compose.yaml. Bump all four together — the examples stack
# reuses this tag on purpose, so that an example talks to the same receiver
# this gate validates against.
#
# Usage:  ci/scripts/conformance.sh [build-dir]     (default: build)
#
# Environment overrides:
#   MICROTEL_CONTAINER_ENGINE  podman | docker  (default: podman if present)
#   MICROTEL_COLLECTOR_IMAGE   override the pinned collector image
#
# Exit codes:
#   0  every conformance test passed
#   1  at least one conformance test failed
#   2  the gate could not run (no container engine, collector never healthy,
#      no conformance tests registered in the build)

set -euo pipefail

BUILD_DIR="${1:-build}"
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"

COLLECTOR_IMAGE="${MICROTEL_COLLECTOR_IMAGE:-otel/opentelemetry-collector-contrib:0.160.0}"
CONTAINER_NAME="microtel-conformance-$$"

# Certs are generated per run and thrown away, so a short life is right: it
# keeps a stale cert from being silently reused if the workdir survives.
readonly CERT_DAYS=2
readonly HEALTH_TIMEOUT_SECS=60
readonly HEALTH_URL="http://127.0.0.1:13133"
readonly METRICS_URL="http://127.0.0.1:8888/metrics"

# ---------------------------------------------------------------------------
# Container engine
# ---------------------------------------------------------------------------

if [[ -n "${MICROTEL_CONTAINER_ENGINE:-}" ]]; then
    ENGINE="$MICROTEL_CONTAINER_ENGINE"
elif command -v podman >/dev/null 2>&1; then
    ENGINE="podman"
elif command -v docker >/dev/null 2>&1; then
    ENGINE="docker"
else
    echo "conformance: no container engine found (looked for podman, then docker)" >&2
    echo "conformance: install one, or set MICROTEL_CONTAINER_ENGINE" >&2
    exit 2
fi

if ! command -v "$ENGINE" >/dev/null 2>&1; then
    echo "conformance: container engine '$ENGINE' not found on PATH" >&2
    exit 2
fi

echo "conformance: engine   = $ENGINE"
echo "conformance: image    = $COLLECTOR_IMAGE"
echo "conformance: build    = $BUILD_DIR"

# ---------------------------------------------------------------------------
# Workdir
# ---------------------------------------------------------------------------

WORK_DIR="${BUILD_DIR}/conformance"
CERT_DIR="${WORK_DIR}/certs"
OUT_DIR="${WORK_DIR}/out"

rm -rf "$WORK_DIR"
mkdir -p "$CERT_DIR" "$OUT_DIR"

# The collector image runs as uid 10001, which owns nothing on the host. 0777
# is the portable way to let it append to the bind-mounted output file under
# both rootful docker and rootless podman.
#
# If a hardened rootless-podman setup still refuses the write, add `--user 0`
# to the `run` below: it maps the container's root to the invoking user, which
# owns the directory. Not the default because it diverges from CI.
chmod 0777 "$OUT_DIR"

CONFIG_FILE="$(realpath "${REPO_ROOT}/tests/conformance/collector/config.yaml")"
CERT_DIR_ABS="$(realpath "$CERT_DIR")"
OUT_DIR_ABS="$(realpath "$OUT_DIR")"

# ---------------------------------------------------------------------------
# Certificates
#
# ECDSA P-256 rather than RSA: generation is instant, and it keeps the gate
# from spending seconds on key material nobody is measuring. The SAN matters —
# microtel verifies the hostname, so the TLS endpoints use `localhost` while
# the plaintext ones use 127.0.0.1.
#
# wrong-ca.crt is a second, unrelated self-signed CA. It is the negative
# control for CA-pinning tests: a client trusting only it must reject the
# server, proving verification is actually happening.
# ---------------------------------------------------------------------------

echo "conformance: generating certificates in $CERT_DIR"

openssl ecparam -name prime256v1 -genkey -noout -out "${CERT_DIR}/ca.key" 2>/dev/null
openssl req -x509 -new -key "${CERT_DIR}/ca.key" -sha256 -days "$CERT_DAYS" \
    -subj "/CN=microtel-conformance-ca" -out "${CERT_DIR}/ca.crt" 2>/dev/null

openssl ecparam -name prime256v1 -genkey -noout -out "${CERT_DIR}/server.key" 2>/dev/null
openssl req -new -key "${CERT_DIR}/server.key" \
    -subj "/CN=localhost" -out "${CERT_DIR}/server.csr" 2>/dev/null
openssl x509 -req -in "${CERT_DIR}/server.csr" \
    -CA "${CERT_DIR}/ca.crt" -CAkey "${CERT_DIR}/ca.key" -CAcreateserial \
    -days "$CERT_DAYS" -sha256 \
    -extfile <(printf 'subjectAltName=DNS:localhost,IP:127.0.0.1\n') \
    -out "${CERT_DIR}/server.crt" 2>/dev/null

openssl ecparam -name prime256v1 -genkey -noout -out "${CERT_DIR}/client.key" 2>/dev/null
openssl req -new -key "${CERT_DIR}/client.key" \
    -subj "/CN=microtel-conformance-client" -out "${CERT_DIR}/client.csr" 2>/dev/null
openssl x509 -req -in "${CERT_DIR}/client.csr" \
    -CA "${CERT_DIR}/ca.crt" -CAkey "${CERT_DIR}/ca.key" -CAcreateserial \
    -days "$CERT_DAYS" -sha256 \
    -out "${CERT_DIR}/client.crt" 2>/dev/null

openssl ecparam -name prime256v1 -genkey -noout -out "${CERT_DIR}/wrong-ca.key" 2>/dev/null
openssl req -x509 -new -key "${CERT_DIR}/wrong-ca.key" -sha256 -days "$CERT_DAYS" \
    -subj "/CN=microtel-conformance-wrong-ca" -out "${CERT_DIR}/wrong-ca.crt" 2>/dev/null

# The collector reads these as uid 10001; the tests read them as the invoking
# user. Both need them, and none of it is secret beyond this run.
chmod 0644 "${CERT_DIR}"/*

# ---------------------------------------------------------------------------
# Teardown
# ---------------------------------------------------------------------------

dump_diagnostics()
{
    echo "conformance: ---- collector logs ----" >&2
    "$ENGINE" logs "$CONTAINER_NAME" 2>&1 | sed 's/^/    /' >&2 || true
    echo "conformance: ---- collector metrics ----" >&2
    curl -s --max-time 5 "$METRICS_URL" | sed 's/^/    /' >&2 || true
}

cleanup()
{
    local status=$?
    if [[ $status -ne 0 ]]; then
        dump_diagnostics
    fi
    "$ENGINE" rm -f "$CONTAINER_NAME" >/dev/null 2>&1 || true
    return $status
}
trap cleanup EXIT

# ---------------------------------------------------------------------------
# Start the collector
#
# Every port is published to 127.0.0.1 only: this opens a receiver that accepts
# unauthenticated telemetry, and on a CI runner or a laptop that has no
# business being reachable off-host.
#
# `:Z` on each mount relabels it for SELinux, which rootless podman on Fedora
# requires and which docker on Ubuntu ignores.
# ---------------------------------------------------------------------------

echo "conformance: starting collector as $CONTAINER_NAME"

"$ENGINE" run -d --name "$CONTAINER_NAME" \
    -p 127.0.0.1:4317:4317 \
    -p 127.0.0.1:4318:4318 \
    -p 127.0.0.1:4327:4327 \
    -p 127.0.0.1:4328:4328 \
    -p 127.0.0.1:4337:4337 \
    -p 127.0.0.1:4338:4338 \
    -p 127.0.0.1:4347:4347 \
    -p 127.0.0.1:4348:4348 \
    -p 127.0.0.1:4357:4357 \
    -p 127.0.0.1:4358:4358 \
    -p 127.0.0.1:13133:13133 \
    -p 127.0.0.1:8888:8888 \
    -v "${CONFIG_FILE}:/etc/otelcol-contrib/config.yaml:ro,Z" \
    -v "${CERT_DIR_ABS}:/certs:ro,Z" \
    -v "${OUT_DIR_ABS}:/out:Z" \
    "$COLLECTOR_IMAGE" >/dev/null

# ---------------------------------------------------------------------------
# Health wait
# ---------------------------------------------------------------------------

echo -n "conformance: waiting for health"
healthy=0
for _ in $(seq "$HEALTH_TIMEOUT_SECS"); do
    if curl -sf --max-time 2 "$HEALTH_URL" >/dev/null 2>&1; then
        healthy=1
        break
    fi
    echo -n "."
    sleep 1
done
echo

if [[ $healthy -ne 1 ]]; then
    echo "conformance: collector did not become healthy within ${HEALTH_TIMEOUT_SECS}s" >&2
    echo "conformance: a config the pinned image rejects fails exactly like this" >&2
    exit 2
fi

echo "conformance: collector healthy"

# ---------------------------------------------------------------------------
# Guard: refuse to report success for a gate that ran nothing
# ---------------------------------------------------------------------------

registered="$(ctest --test-dir "$BUILD_DIR" -N -L conformance 2>/dev/null |
    sed -n 's/^Total Tests: \([0-9]*\)$/\1/p')"

if [[ "${registered:-0}" -lt 1 ]]; then
    echo "conformance: no tests carry the 'conformance' label in '$BUILD_DIR'" >&2
    echo "conformance: configure with -DMICROTEL_BUILD_TESTS=ON and build first" >&2
    exit 2
fi

echo "conformance: $registered labelled test binary/binaries registered"

# ---------------------------------------------------------------------------
# Environment contract (tests/conformance/support/conformance_env.hpp)
# ---------------------------------------------------------------------------

export MICROTEL_CONFORMANCE_REQUIRE=1
export MICROTEL_CONFORMANCE_HTTP_ENDPOINT="http://127.0.0.1:4318"
export MICROTEL_CONFORMANCE_GRPC_ENDPOINT="http://127.0.0.1:4317"
export MICROTEL_CONFORMANCE_HTTP_TLS_ENDPOINT="https://localhost:4328"
export MICROTEL_CONFORMANCE_GRPC_TLS_ENDPOINT="https://localhost:4327"
export MICROTEL_CONFORMANCE_HTTP_MTLS_ENDPOINT="https://localhost:4338"
export MICROTEL_CONFORMANCE_GRPC_MTLS_ENDPOINT="https://localhost:4337"
# https, not http: the auth receiver's HTTP port serves TLS because, until
# v1.2.6, microtel could not reach a plaintext collector HTTP receiver at all
# (issue #166). The gRPC auth port stays plaintext.
export MICROTEL_CONFORMANCE_HTTP_AUTH_ENDPOINT="https://localhost:4348"
export MICROTEL_CONFORMANCE_GRPC_AUTH_ENDPOINT="http://127.0.0.1:4347"
export MICROTEL_CONFORMANCE_CA="${CERT_DIR_ABS}/ca.crt"
export MICROTEL_CONFORMANCE_WRONG_CA="${CERT_DIR_ABS}/wrong-ca.crt"
export MICROTEL_CONFORMANCE_CLIENT_CERT="${CERT_DIR_ABS}/client.crt"
export MICROTEL_CONFORMANCE_CLIENT_KEY="${CERT_DIR_ABS}/client.key"
export MICROTEL_CONFORMANCE_AUTH_TOKEN="microtel-conformance-token"
export MICROTEL_CONFORMANCE_OUTPUT_FILE="${OUT_DIR_ABS}/traces.jsonl"
export MICROTEL_CONFORMANCE_LOGS_OUTPUT_FILE="${OUT_DIR_ABS}/logs.jsonl"
export MICROTEL_CONFORMANCE_METRICS_OUTPUT_FILE="${OUT_DIR_ABS}/metrics.jsonl"
# The leaf / concentrator gate's receiver: no batch processor, its own file, so
# one line is one request (tests/conformance/leaf/). HTTP is TLS, as above.
export MICROTEL_CONFORMANCE_LEAF_HTTP_ENDPOINT="https://localhost:4358"
export MICROTEL_CONFORMANCE_LEAF_GRPC_ENDPOINT="http://127.0.0.1:4357"
export MICROTEL_CONFORMANCE_LEAF_OUTPUT_FILE="${OUT_DIR_ABS}/leaf-traces.jsonl"

# ---------------------------------------------------------------------------
# Run
# ---------------------------------------------------------------------------

echo "conformance: running ctest -L conformance"

set +e
ctest --test-dir "$BUILD_DIR" -L conformance --output-on-failure
rc=$?
set -e

if [[ $rc -ne 0 ]]; then
    echo "conformance: FAIL — ctest exited $rc" >&2
    exit 1
fi

echo "conformance: clean"
