#!/usr/bin/env bash
#
# tests/integration/test_memory_pack.sh - Integration tests for virmux stats and pack
#
# Tests system telemetry reporting (table and JSON schema), CLI help flags,
# and proactive memory compaction on live processes and cgroups.
#

set -euo pipefail

# ANSI color codes
if [[ -t 1 ]]; then
  GREEN="\033[0;32m"
  RED="\033[0;31m"
  BOLD="\033[1m"
  RESET="\033[0m"
else
  GREEN=""
  RED=""
  BOLD=""
  RESET=""
fi

log_success() {
  echo -e "  [${GREEN}PASS${RESET}] $1"
}

log_error() {
  echo -e "  [${RED}FAIL${RESET}] $1" >&2
}

# Resolve repository root and build directory
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"

if [[ $# -ge 1 && -n "$1" ]]; then
  if [[ "$1" = /* ]]; then
    BUILD_DIR="$1"
  elif [[ -d "$1" ]]; then
    BUILD_DIR="$(cd "$1" && pwd)"
  elif [[ -d "${REPO_ROOT}/$1" ]]; then
    BUILD_DIR="$(cd "${REPO_ROOT}/$1" && pwd)"
  else
    BUILD_DIR="$1"
  fi
else
  BUILD_DIR="${REPO_ROOT}/build"
fi

VIRMUX="${BUILD_DIR}/virmux"
if [[ ! -x "$VIRMUX" ]]; then
  log_error "virmux executable not found or not executable at: ${VIRMUX}"
  exit 1
fi

INTERCEPT_LIB="${BUILD_DIR}/libvirmux_intercept.so"
if [[ -f "$INTERCEPT_LIB" ]]; then
  export VIRMUX_INTERCEPT_LIB="$INTERCEPT_LIB"
fi

WORKER_PID_FILE="/tmp/virmux_worker_$$.pid"
WORKER_PID=""
WORKER_BG_PID=""

cleanup() {
  if [[ -n "${WORKER_PID:-}" ]]; then
    kill -9 "$WORKER_PID" 2>/dev/null || true
  fi
  if [[ -n "${WORKER_BG_PID:-}" ]]; then
    kill -9 "$WORKER_BG_PID" 2>/dev/null || true
  fi
  if [[ -f "${WORKER_PID_FILE:-}" ]]; then
    rm -f "$WORKER_PID_FILE"
  fi
}
trap cleanup EXIT INT TERM

echo -e "${BOLD}Starting virmux memory compaction and telemetry integration test suite${RESET}"
echo "Binary: ${VIRMUX}"
echo "Build Dir: ${BUILD_DIR}"
echo "================================================="

# ---------------------------------------------------------------------------
# Test 1: virmux stats Table Output
# ---------------------------------------------------------------------------
echo -e "\n${BOLD}Test 1: virmux stats table output${RESET}"

STATS_OUT=$("$VIRMUX" stats)
for expected in "Host Physical & Swap Memory" "Kernel Samepage Merging" "zram In-RAM Compressed Swap"; do
  if ! echo "$STATS_OUT" | grep -q -- "$expected"; then
    log_error "Expected section '${expected}' not found in 'virmux stats' output"
    echo "$STATS_OUT" >&2
    exit 1
  fi
done
log_success "virmux stats table output contains required telemetry sections"

# ---------------------------------------------------------------------------
# Test 2: virmux stats --json Schema Validation
# ---------------------------------------------------------------------------
echo -e "\n${BOLD}Test 2: virmux stats --json schema validation${RESET}"

"$VIRMUX" stats --json | python3 -c "
import json, sys
data = json.load(sys.stdin)
assert 'host_memory' in data, 'Missing host_memory in JSON'
assert 'ksm' in data, 'Missing ksm in JSON'
assert 'zram' in data, 'Missing zram in JSON'
assert 'total_bytes' in data['host_memory'], 'Missing total_bytes in host_memory'
assert 'present' in data['ksm'], 'Missing present in ksm'
"
log_success "virmux stats --json schema verified successfully"

# ---------------------------------------------------------------------------
# Test 3: CLI Help Output
# ---------------------------------------------------------------------------
echo -e "\n${BOLD}Test 3: CLI help output${RESET}"

"$VIRMUX" stats --help >/dev/null
log_success "virmux stats --help returned 0"

"$VIRMUX" pack --help >/dev/null
log_success "virmux pack --help returned 0"

# ---------------------------------------------------------------------------
# Test 4: End-to-End Heap Compaction (virmux pack <pid>)
# ---------------------------------------------------------------------------
echo -e "\n${BOLD}Test 4: End-to-End Heap Compaction (virmux pack <pid>)${RESET}"

rm -f "$WORKER_PID_FILE"
python3 -c "
import time, os
buf = bytearray(20 * 1024 * 1024)  # 20 MB dirty heap
with open('${WORKER_PID_FILE}', 'w') as f:
    f.write(str(os.getpid()))
time.sleep(10)
" &
WORKER_BG_PID=$!

WAIT_COUNT=0
while [[ ! -f "$WORKER_PID_FILE" && $WAIT_COUNT -lt 50 ]]; do
  sleep 0.1
  WAIT_COUNT=$((WAIT_COUNT + 1))
done

if [[ ! -f "$WORKER_PID_FILE" ]]; then
  log_error "Timed out waiting for worker PID file ${WORKER_PID_FILE}"
  exit 1
fi

WORKER_PID=$(cat "$WORKER_PID_FILE")

PACK_OUT=$("$VIRMUX" pack "$WORKER_PID")
if echo "$PACK_OUT" | grep -q "Compacted 1 process(es)"; then
  log_success "virmux pack compacted live target process (${PACK_OUT})"
else
  log_error "virmux pack failed to compact target process: ${PACK_OUT}"
  exit 1
fi

kill -9 "$WORKER_PID" 2>/dev/null || true
wait "$WORKER_BG_PID" 2>/dev/null || true
rm -f "$WORKER_PID_FILE"
WORKER_PID=""

# ---------------------------------------------------------------------------
# Test 5: virmux pack --all
# ---------------------------------------------------------------------------
echo -e "\n${BOLD}Test 5: virmux pack --all${RESET}"

PACK_ALL_OUT=$("$VIRMUX" pack --all)
log_success "virmux pack --all completed cleanly (${PACK_ALL_OUT})"

# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------
echo "================================================="
echo -e "${GREEN}${BOLD}All memory compaction and telemetry integration tests passed! (5/5)${RESET}"
exit 0
