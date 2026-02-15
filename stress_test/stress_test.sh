#!/usr/bin/env bash
# Starts a fresh FlashKV server, stress tests it, then shuts it down.
#
#   ./stress_test/stress_test.sh [threads] [ops_per_thread] [port]
set -euo pipefail

THREADS="${1:-50}"
OPS="${2:-1000}"
PORT="${3:-6399}"

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# FLASHKV_BIN overrides the location for out-of-tree builds.
SERVER="${FLASHKV_BIN:-$ROOT/build/flashkv}"

if [[ ! -x "$SERVER" ]]; then
    echo "Server binary not found at $SERVER - build it first:" >&2
    echo "  mkdir -p build && cd build && cmake .. && make -j\$(nproc)" >&2
    exit 1
fi

echo "Starting server on port $PORT..."
"$SERVER" --port "$PORT" &
SERVER_PID=$!

cleanup() {
    kill "$SERVER_PID" 2>/dev/null || true
    wait "$SERVER_PID" 2>/dev/null || true
}
trap cleanup EXIT

# Wait for the port to accept connections instead of guessing with sleep.
for _ in $(seq 1 50); do
    if (exec 3<>"/dev/tcp/127.0.0.1/$PORT") 2>/dev/null; then
        exec 3>&-
        break
    fi
    sleep 0.1
done

python3 "$ROOT/stress_test/stress_test.py" \
    --host 127.0.0.1 --port "$PORT" --threads "$THREADS" --ops "$OPS"
