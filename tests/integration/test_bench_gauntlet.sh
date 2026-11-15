#!/usr/bin/env bash
#
# tests/integration/test_bench_gauntlet.sh - 12-Terminal Gauntlet Integration Benchmark Suite
#
# Tests virmux bench run and virmux bench report subcommands:
#   1. Quick gauntlet run (--quick --output) and JSON schema verification
#   2. Report generation (--input and stdin pipe) and ANSI table column verification
#   3. Single mode execution (--mode managed and --mode baseline) with --json
#   4. 12-task Gauntlet run (--tasks 12 --duration 1) verifying task_count: 12
#   5. Zero process leaks: child worker process cleanup and SIGINT reaping
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

TEMP_DIR="$(mktemp -d /tmp/virmux_bench_gauntlet_XXXXXX)"
BENCH_PID=""
CHILD_PIDS=""

cleanup() {
  if [[ -n "${BENCH_PID:-}" ]] && kill -0 "$BENCH_PID" 2>/dev/null; then
    kill -KILL "$BENCH_PID" 2>/dev/null || true
    wait "$BENCH_PID" 2>/dev/null || true
  fi
  if [[ -n "${CHILD_PIDS:-}" ]]; then
    for cpid in $CHILD_PIDS; do
      kill -KILL "$cpid" 2>/dev/null || true
    done
  fi
  rm -rf "${TEMP_DIR:-}"
}
trap cleanup EXIT

echo -e "${BOLD}Starting 12-terminal gauntlet benchmark integration suite${RESET}"
echo "Binary: ${VIRMUX}"
echo "Build Dir: ${BUILD_DIR}"
echo "Temp Dir: ${TEMP_DIR}"
echo "================================================="

# ---------------------------------------------------------------------------
# Test 1: Quick gauntlet run (--quick --output)
# ---------------------------------------------------------------------------
echo -e "\n${BOLD}Test 1: Quick gauntlet run (--quick --output)${RESET}"

QUICK_JSON="${TEMP_DIR}/quick.json"
"$VIRMUX" bench run --quick --output "$QUICK_JSON"

if [[ ! -f "$QUICK_JSON" ]]; then
  log_error "quick.json was not generated at: ${QUICK_JSON}"
  exit 1
fi

python3 -c "
import json
with open('${QUICK_JSON}') as f:
    data = json.load(f)

assert 'baseline' in data, 'Missing baseline key in JSON'
assert 'managed' in data, 'Missing managed key in JSON'
assert 'summary' in data, 'Missing summary key in JSON'

b = data['baseline']
m = data['managed']
s = data['summary']

assert b['task_count'] > 0, f'Invalid baseline task count: {b[\"task_count\"]}'
assert m['task_count'] > 0, f'Invalid managed task count: {m[\"task_count\"]}'
assert b['wall_time_sec'] > 0.0, 'Baseline wall_time_sec should be positive'
assert m['wall_time_sec'] > 0.0, 'Managed wall_time_sec should be positive'
assert 'peak_rss_bytes' in b, 'Missing peak_rss_bytes in baseline'
assert 'peak_swap_bytes' in b, 'Missing peak_swap_bytes in baseline'
assert 'avg_latency_ms' in b, 'Missing avg_latency_ms in baseline'
assert 'p99_latency_ms' in b, 'Missing p99_latency_ms in baseline'
assert 'rss_reduction_pct' in s, 'Missing rss_reduction_pct in summary'
assert 'latency_improvement_x' in s, 'Missing latency_improvement_x in summary'
"
log_success "Quick gauntlet run succeeded and verified JSON report format"

# ---------------------------------------------------------------------------
# Test 2: Report generation (--input and stdin pipe)
# ---------------------------------------------------------------------------
echo -e "\n${BOLD}Test 2: Report generation (--input and stdin pipe)${RESET}"

# Sub-test 2a: virmux bench report --input <path>
REPORT_FILE_OUT="$("$VIRMUX" bench report --input "$QUICK_JSON")"

if ! echo "$REPORT_FILE_OUT" | grep -qi "Peak.*RAM"; then
  log_error "Report (--input) missing 'Peak RAM' metric in table output"
  exit 1
fi

if ! echo "$REPORT_FILE_OUT" | grep -qi "Input Lag"; then
  log_error "Report (--input) missing 'Input Lag' metric in table output"
  exit 1
fi

if ! echo "$REPORT_FILE_OUT" | grep -qi "Wall-Clock Time"; then
  log_error "Report (--input) missing 'Wall-Clock Time' metric in table output"
  exit 1
fi

log_success "virmux bench report --input verified ANSI table columns (Peak RAM, Input Lag, Wall-Clock Time)"

# Sub-test 2b: stdin pipe
REPORT_PIPE_OUT=$(cat "$QUICK_JSON" | "$VIRMUX" bench report)

if ! echo "$REPORT_PIPE_OUT" | grep -qi "Peak.*RAM"; then
  log_error "Report (stdin) missing 'Peak RAM' metric in table output"
  exit 1
fi

if ! echo "$REPORT_PIPE_OUT" | grep -qi "Input Lag"; then
  log_error "Report (stdin) missing 'Input Lag' metric in table output"
  exit 1
fi

if ! echo "$REPORT_PIPE_OUT" | grep -qi "Wall-Clock Time"; then
  log_error "Report (stdin) missing 'Wall-Clock Time' metric in table output"
  exit 1
fi

log_success "virmux bench report via stdin pipe verified ANSI table columns"

# ---------------------------------------------------------------------------
# Test 3: Single mode execution (--mode managed and --mode baseline)
# ---------------------------------------------------------------------------
echo -e "\n${BOLD}Test 3: Single mode execution (--mode managed and --mode baseline)${RESET}"

MANAGED_JSON="${TEMP_DIR}/managed.json"
"$VIRMUX" bench run --mode managed --quick --json > "$MANAGED_JSON"

python3 -c "
import json
with open('${MANAGED_JSON}') as f:
    data = json.load(f)

assert data['managed']['mode'] == 'managed', f'Unexpected mode: {data[\"managed\"][\"mode\"]}'
assert data['managed']['task_count'] > 0, 'Managed task_count should be > 0'
assert data['managed']['successful_tasks'] > 0, 'Managed successful_tasks should be > 0'
assert data['baseline']['task_count'] == 0, 'Baseline task_count should be 0'
"
log_success "Single mode --mode managed executed cleanly with valid JSON output"

BASELINE_JSON="${TEMP_DIR}/baseline.json"
"$VIRMUX" bench run --mode baseline --quick --json > "$BASELINE_JSON"

python3 -c "
import json
with open('${BASELINE_JSON}') as f:
    data = json.load(f)

assert data['baseline']['mode'] == 'baseline', f'Unexpected mode: {data[\"baseline\"][\"mode\"]}'
assert data['baseline']['task_count'] > 0, 'Baseline task_count should be > 0'
assert data['baseline']['successful_tasks'] > 0, 'Baseline successful_tasks should be > 0'
assert data['managed']['task_count'] == 0, 'Managed task_count should be 0'
"
log_success "Single mode --mode baseline executed cleanly with valid JSON output"

# ---------------------------------------------------------------------------
# Test 4: 12-task Gauntlet run (--tasks 12 --duration 1)
# ---------------------------------------------------------------------------
echo -e "\n${BOLD}Test 4: 12-task Gauntlet run (--tasks 12 --duration 1)${RESET}"

GAUNTLET12_JSON="${TEMP_DIR}/gauntlet12.json"
"$VIRMUX" bench run --tasks 12 --duration 1 --output "$GAUNTLET12_JSON"

if [[ ! -f "$GAUNTLET12_JSON" ]]; then
  log_error "gauntlet12.json was not generated at: ${GAUNTLET12_JSON}"
  exit 1
fi

python3 -c "
import json
with open('${GAUNTLET12_JSON}') as f:
    data = json.load(f)

b = data['baseline']
m = data['managed']

assert b['task_count'] == 12, f'Expected baseline task_count 12, got {b[\"task_count\"]}'
assert m['task_count'] == 12, f'Expected managed task_count 12, got {m[\"task_count\"]}'
assert b['successful_tasks'] == 12, f'Expected 12 successful baseline tasks, got {b[\"successful_tasks\"]}'
assert m['successful_tasks'] == 12, f'Expected 12 successful managed tasks, got {m[\"successful_tasks\"]}'
assert b['failed_tasks'] == 0, f'Expected 0 failed baseline tasks, got {b[\"failed_tasks\"]}'
assert m['failed_tasks'] == 0, f'Expected 0 failed managed tasks, got {m[\"failed_tasks\"]}'
"
log_success "12-task gauntlet run verified task_count: 12 for both baseline and managed"

# ---------------------------------------------------------------------------
# Test 5: Zero process leaks: verify child worker processes are reaped cleanly
# ---------------------------------------------------------------------------
echo -e "\n${BOLD}Test 5: Zero process leaks (verify child worker processes reaped cleanly)${RESET}"

# Launch a longer benchmark run in background
"$VIRMUX" bench run --tasks 12 --duration 10 >/dev/null 2>&1 &
BENCH_PID=$!

# Allow time for tasks to fork
sleep 0.5

# Capture child worker processes
CHILD_PIDS=$(pgrep -P "$BENCH_PID" || true)

# Send SIGINT to benchmark runner to test graceful abort and child reaping
kill -INT "$BENCH_PID" 2>/dev/null || true

# Wait for benchmark runner to finish aborting
wait "$BENCH_PID" 2>/dev/null || true
BENCH_PID=""

# Allow small grace period for kernel process table cleanup
sleep 0.2

# Verify all child workers were cleanly terminated
LEAKED=0
if [[ -n "$CHILD_PIDS" ]]; then
  for cpid in $CHILD_PIDS; do
    if kill -0 "$cpid" 2>/dev/null; then
      log_error "Leaked benchmark child worker process detected: PID ${cpid}"
      kill -KILL "$cpid" 2>/dev/null || true
      LEAKED=1
    fi
  done
fi

if [[ "$LEAKED" -ne 0 ]]; then
  log_error "Process leak test failed: child worker processes were not cleanly reaped"
  exit 1
fi
CHILD_PIDS=""

log_success "Zero process leaks: all child worker processes were cleanly reaped"

# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------
echo "================================================="
echo -e "${GREEN}${BOLD}All 12-terminal gauntlet benchmark integration tests passed successfully! (5/5)${RESET}"
exit 0
