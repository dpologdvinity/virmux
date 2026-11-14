#!/usr/bin/env bash
#
# tests/integration/test_virmux_run.sh - End-to-end integration tests for virmux run
#
# Tests core runtime virtualization, hardware spoofing, synthetic meminfo,
# headless browser argument injection, admission gate concurrency, and exit codes.
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

# Ensure interceptor library is exported if present in build directory
INTERCEPT_LIB="${BUILD_DIR}/libvirmux_intercept.so"
if [[ -f "$INTERCEPT_LIB" ]]; then
  export VIRMUX_INTERCEPT_LIB="$INTERCEPT_LIB"
fi

echo -e "${BOLD}Starting virmux run integration test suite${RESET}"
echo "Binary: ${VIRMUX}"
echo "Build Dir: ${BUILD_DIR}"
echo "================================================="

# ---------------------------------------------------------------------------
# Test 1: Python CPU spoofing
# ---------------------------------------------------------------------------
echo -e "\n${BOLD}Test 1: Python CPU core spoofing${RESET}"

"$VIRMUX" run --cores 2 -- python3 -c "import os; count = os.cpu_count(); assert count == 2, f'Expected 2, got {count}'"
log_success "virmux run --cores 2 correctly spoofed os.cpu_count() == 2"

"$VIRMUX" run --cores 4 -- python3 -c "import os; count = os.cpu_count(); assert count == 4, f'Expected 4, got {count}'"
log_success "virmux run --cores 4 correctly spoofed os.cpu_count() == 4"

# ---------------------------------------------------------------------------
# Test 2: Virtual /proc/meminfo reporting
# ---------------------------------------------------------------------------
echo -e "\n${BOLD}Test 2: Virtual /proc/meminfo reporting${RESET}"

MEM_OUT=$("$VIRMUX" run --mem 1G -- grep MemTotal /proc/meminfo)
if echo "$MEM_OUT" | grep -q "1048576 kB"; then
  log_success "virmux run --mem 1G reported synthetic MemTotal: 1048576 kB (1GB)"
else
  log_error "MemTotal mismatch for 1G: expected '1048576 kB', got '${MEM_OUT}'"
  exit 1
fi

MEM_OUT_2G=$("$VIRMUX" run --mem 2G -- grep MemTotal /proc/meminfo)
if echo "$MEM_OUT_2G" | grep -q "2097152 kB"; then
  log_success "virmux run --mem 2G reported synthetic MemTotal: 2097152 kB (2GB)"
else
  log_error "MemTotal mismatch for 2G: expected '2097152 kB', got '${MEM_OUT_2G}'"
  exit 1
fi

# ---------------------------------------------------------------------------
# Test 3: Headless browser flag injection
# ---------------------------------------------------------------------------
echo -e "\n${BOLD}Test 3: Headless browser flag injection${RESET}"

TMP_MOCK_DIR="$(mktemp -d /tmp/virmux_browser_test_XXXXXX)"
cleanup() {
  rm -rf "$TMP_MOCK_DIR"
}
trap cleanup EXIT INT TERM

# Create mock browser script that prints all received arguments line-by-line
MOCK_CHROMIUM="${TMP_MOCK_DIR}/chromium"
cat << 'EOF' > "$MOCK_CHROMIUM"
#!/bin/sh
for arg in "$@"; do
  echo "$arg"
done
EOF
chmod +x "$MOCK_CHROMIUM"

# Create mock non-browser tool to verify passthrough
MOCK_TOOL="${TMP_MOCK_DIR}/mock-tool"
cat << 'EOF' > "$MOCK_TOOL"
#!/bin/sh
for arg in "$@"; do
  echo "$arg"
done
EOF
chmod +x "$MOCK_TOOL"

# Run mock browser via shell under virmux run to test execve interception
BROWSER_OUT=$("$VIRMUX" run -- /bin/sh -c "\"${MOCK_CHROMIUM}\" --remote-debugging-port=9222 http://localhost:8080")

for flag in "--disable-gpu" "--single-process" "--disable-software-rasterizer" "--remote-debugging-port=9222" "http://localhost:8080"; do
  if ! echo "$BROWSER_OUT" | grep -q -- "$flag"; then
    log_error "Expected flag '${flag}' not found in mock browser argv output"
    echo "Actual output:" >&2
    echo "$BROWSER_OUT" >&2
    exit 1
  fi
done
log_success "Mock browser binary intercepted and slim flags injected"

# Verify non-browser binary does not receive slim flags
TOOL_OUT=$("$VIRMUX" run -- /bin/sh -c "\"${MOCK_TOOL}\" --verbose")
if echo "$TOOL_OUT" | grep -q -- "--disable-gpu"; then
  log_error "Non-browser tool unexpectedly received slim flags"
  exit 1
fi
log_success "Non-browser binary correctly passed through without flag injection"

# ---------------------------------------------------------------------------
# Test 4: Concurrency smoke test
# ---------------------------------------------------------------------------
echo -e "\n${BOLD}Test 4: Concurrency smoke test (parallel virmux run --heavy)${RESET}"

PIDS=()
for i in 1 2 3 4; do
  "$VIRMUX" run --heavy -- python3 -c "import time; time.sleep(0.05)" &
  PIDS+=($!)
done

CONCURRENCY_FAILED=0
for pid in "${PIDS[@]}"; do
  if ! wait "$pid"; then
    log_error "Concurrent heavy task (PID ${pid}) failed"
    CONCURRENCY_FAILED=1
  fi
done

if [[ "$CONCURRENCY_FAILED" -ne 0 ]]; then
  log_error "Concurrency smoke test failed"
  exit 1
fi
log_success "4 parallel heavy tasks completed cleanly without errors"

# ---------------------------------------------------------------------------
# Test 5: Exit code propagation
# ---------------------------------------------------------------------------
echo -e "\n${BOLD}Test 5: Exit code propagation${RESET}"

set +e
"$VIRMUX" run -- /bin/sh -c "exit 33"
EC33=$?

"$VIRMUX" run -- /bin/sh -c "exit 0"
EC0=$?

"$VIRMUX" run -- /bin/sh -c "exit 7"
EC7=$?
set -e

if [[ "$EC33" -eq 33 ]]; then
  log_success "Exit code 33 propagated correctly"
else
  log_error "Expected exit code 33, got ${EC33}"
  exit 1
fi

if [[ "$EC0" -eq 0 ]]; then
  log_success "Exit code 0 propagated correctly"
else
  log_error "Expected exit code 0, got ${EC0}"
  exit 1
fi

if [[ "$EC7" -eq 7 ]]; then
  log_success "Exit code 7 propagated correctly"
else
  log_error "Expected exit code 7, got ${EC7}"
  exit 1
fi

# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------
echo "================================================="
echo -e "${GREEN}${BOLD}All virmux run integration tests passed successfully! (5/5)${RESET}"
exit 0
