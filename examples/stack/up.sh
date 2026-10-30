#!/usr/bin/env bash
# Copyright (c) 2026 The microtel Authors.
# SPDX-License-Identifier: Apache-2.0
#
# Start the shared observability stack for examples/: OTel Collector → Tempo →
# Grafana. Idempotent — running it against an already-running stack just
# re-checks readiness and reprints the endpoints.
#
# Usage:  examples/stack/up.sh
#
# Environment overrides:
#   MICROTEL_CONTAINER_ENGINE   podman | docker   (default: podman if present)
#   MICROTEL_STACK_TIMEOUT      readiness timeout in seconds (default: 180)
#
# Exit codes:
#   0  all three services answered their health endpoints
#   1  the stack could not be started, or something never came up

set -euo pipefail

STACK_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
readonly STACK_DIR
readonly PROJECT_NAME="microtel-stack"
readonly COMPOSE_FILE="${STACK_DIR}/compose.yaml"

# First boot pulls three images, which on a cold cache is the slow part.
READY_TIMEOUT_SECS="${MICROTEL_STACK_TIMEOUT:-180}"
readonly READY_TIMEOUT_SECS

readonly COLLECTOR_HEALTH_URL="http://localhost:13133"
readonly TEMPO_READY_URL="http://localhost:3200/ready"
readonly GRAFANA_HEALTH_URL="http://localhost:3000/api/health"

# shellcheck source=./compose-engine.sh
# shellcheck disable=SC1091
source "${STACK_DIR}/compose-engine.sh"

if ! command -v curl >/dev/null 2>&1; then
    echo "stack: curl is required to wait for the services to come up" >&2
    exit 1
fi

microtel_stack_resolve_engine

echo "stack: engine  = ${MICROTEL_ENGINE}"
echo "stack: compose = ${MICROTEL_COMPOSE[*]}"
echo

# Poll one HTTP endpoint until it answers 2xx, or give up.
wait_for()
{
    local name="$1"
    local url="$2"
    local deadline=$(( SECONDS + READY_TIMEOUT_SECS ))

    while (( SECONDS < deadline )); do
        # -s without -S: a not-yet-ready service answers 503, and printing a
        # curl diagnostic once a second while it starts is pure noise.
        if curl -fs --max-time 2 -o /dev/null "$url"; then
            echo "stack: ${name} is ready"
            return 0
        fi
        sleep 1
    done

    echo "stack: ${name} never became ready (${READY_TIMEOUT_SECS}s, ${url})" >&2
    echo "stack: logs — ${MICROTEL_COMPOSE[*]} -f ${COMPOSE_FILE} -p ${PROJECT_NAME} logs" >&2
    return 1
}

"${MICROTEL_COMPOSE[@]}" -f "$COMPOSE_FILE" -p "$PROJECT_NAME" up -d

echo
echo "stack: waiting for the services to answer (timeout ${READY_TIMEOUT_SECS}s each)"
wait_for "tempo" "$TEMPO_READY_URL"
wait_for "otel-collector" "$COLLECTOR_HEALTH_URL"
wait_for "grafana" "$GRAFANA_HEALTH_URL"

cat <<'BANNER'

  Stack is up.

  Grafana:   http://localhost:3000  —  Explore → Tempo  (no login; anonymous
             admin). The "microtel — recent traces" dashboard is the home page
             and lists every trace Tempo has; click a row for the flame graph.

  Examples export to:
    OTLP/gRPC   http://localhost:4317    <- the default; use this
    OTLP/HTTP   http://localhost:4318    <- HTTP/1.1, one request at a time;
                                            see docs/compatibility-matrix.md §4

  Tempo query API:  http://localhost:3200
    curl -s "http://localhost:3200/api/traces/<trace-id>" | head -c 400

  Tear down with:  examples/stack/down.sh

BANNER
