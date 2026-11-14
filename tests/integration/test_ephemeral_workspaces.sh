#!/usr/bin/env bash
#
# tests/integration/test_ephemeral_workspaces.sh - Integration tests for ephemeral workspaces and CAS
#
# Tests OverlayFS workspace immutability, child-process ephemeral visibility,
# scratch space redirection via TMPDIR, and CAS cache telemetry and pruning.
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

BASE_DIR="/tmp/virmux_test_base_$$"
TEST_CAS_DIR="/tmp/virmux_test_cas_$$"

cleanup() {
  rm -rf "$BASE_DIR" "$TEST_CAS_DIR"
}
trap cleanup EXIT INT TERM

echo -e "${BOLD}Starting virmux ephemeral workspaces and storage integration test suite${RESET}"
echo "Binary: ${VIRMUX}"
echo "Build Dir: ${BUILD_DIR}"
echo "================================================="

# ---------------------------------------------------------------------------
# Test Case 1: Workspace Immutability
# ---------------------------------------------------------------------------
echo -e "\n${BOLD}Test 1: Workspace Immutability${RESET}"

rm -rf "$BASE_DIR"
mkdir -p "$BASE_DIR"
echo "immutable seed data" > "$BASE_DIR/seed.txt"

"$VIRMUX" run --ephemeral --workspace "$BASE_DIR" -- touch "$BASE_DIR/new_file.txt"

if [[ -f "$BASE_DIR/new_file.txt" ]]; then
  log_error "new_file.txt unexpectedly exists on host filesystem"
  exit 1
fi

if [[ ! -f "$BASE_DIR/seed.txt" ]]; then
  log_error "seed.txt is missing on host filesystem"
  exit 1
fi

SEED_CONTENT="$(cat "$BASE_DIR/seed.txt")"
if [[ "$SEED_CONTENT" != "immutable seed data" ]]; then
  log_error "seed.txt was modified on host filesystem: expected 'immutable seed data', got '${SEED_CONTENT}'"
  exit 1
fi

log_success "Workspace immutability verified: host base directory left unmodified"

# ---------------------------------------------------------------------------
# Test Case 2: Ephemeral Visibility During Child Execution
# ---------------------------------------------------------------------------
echo -e "\n${BOLD}Test 2: Ephemeral Visibility During Child Execution${RESET}"

"$VIRMUX" run --ephemeral --workspace "$BASE_DIR" -- /bin/sh -c 'echo "overlay_data" > "'"$BASE_DIR"'/visible.txt" && grep -q "overlay_data" "'"$BASE_DIR"'/visible.txt"'

if [[ -f "$BASE_DIR/visible.txt" ]]; then
  log_error "visible.txt leaked to host filesystem"
  exit 1
fi

log_success "Ephemeral visibility verified: child process observed overlay data, host unaffected"

# ---------------------------------------------------------------------------
# Test Case 3: Scratch Workspace Redirection
# ---------------------------------------------------------------------------
echo -e "\n${BOLD}Test 3: Scratch Workspace Redirection${RESET}"

"$VIRMUX" run --ephemeral -- /bin/sh -c 'test -n "$TMPDIR" && echo "scratch" > "$TMPDIR/scratch_probe.txt" && test -f "$TMPDIR/scratch_probe.txt"'

log_success "Scratch workspace redirection verified via TMPDIR probe"

# ---------------------------------------------------------------------------
# Test Case 4: virmux cas stats Table and JSON Schema
# ---------------------------------------------------------------------------
echo -e "\n${BOLD}Test 4: virmux cas stats Table and JSON Schema${RESET}"

rm -rf "$TEST_CAS_DIR"
mkdir -p "$TEST_CAS_DIR"
export VIRMUX_CAS_DIR="$TEST_CAS_DIR"

# Populate mock object into CAS cache
MOCK_KEY="0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"
mkdir -p "$TEST_CAS_DIR/objects/${MOCK_KEY:0:2}"
echo "sample mock cas payload" > "$TEST_CAS_DIR/objects/${MOCK_KEY:0:2}/$MOCK_KEY"

STATS_TABLE_OUT=$("$VIRMUX" cas stats)
if ! echo "$STATS_TABLE_OUT" | grep -q "Virmux Content-Addressable Storage (CAS)"; then
  log_error "Header 'Virmux Content-Addressable Storage (CAS)' not found in 'virmux cas stats' output"
  echo "$STATS_TABLE_OUT" >&2
  exit 1
fi

"$VIRMUX" cas stats --json | python3 -c "import sys, json; data = json.load(sys.stdin); assert 'total_bytes' in data; assert 'object_count' in data"

log_success "virmux cas stats table and JSON schema verified successfully"

# ---------------------------------------------------------------------------
# Test Case 5: virmux cas clean
# ---------------------------------------------------------------------------
echo -e "\n${BOLD}Test 5: virmux cas clean${RESET}"

CLEAN_OUT=$("$VIRMUX" cas clean --max-bytes 0)
if ! echo "$CLEAN_OUT" | grep -q "Pruned"; then
  log_error "'virmux cas clean --max-bytes 0' did not report pruned objects: ${CLEAN_OUT}"
  exit 1
fi

"$VIRMUX" cas stats --json | python3 -c "import sys, json; data = json.load(sys.stdin); assert data['object_count'] == 0, f'Expected 0 objects, got {data[\"object_count\"]}'"

log_success "virmux cas clean successfully pruned all objects (object_count == 0)"

# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------
echo "================================================="
echo -e "${GREEN}${BOLD}All ephemeral workspace and CAS storage integration tests passed! (5/5)${RESET}"
exit 0
