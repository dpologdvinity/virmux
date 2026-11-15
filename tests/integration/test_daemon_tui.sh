#!/usr/bin/env bash
#
# tests/integration/test_daemon_tui.sh - Integration tests for supervisor daemon and TUI
#
# Tests daemon background startup, session auto-registration, session freeze/thaw/kill,
# headless TUI snapshot rendering, and clean daemon shutdown on SIGTERM.
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

export VIRMUX_SOCKET="/tmp/virmux_integ_$$.sock"
DAEMON_PID=""
WORKER_PID=""
CHILD_WORKERS=""

cleanup() {
  if [[ -n "${CHILD_WORKERS:-}" ]]; then
    kill -KILL $CHILD_WORKERS 2>/dev/null || true
  fi
  if [[ -n "${WORKER_PID:-}" ]] && kill -0 "$WORKER_PID" 2>/dev/null; then
    kill -KILL "$WORKER_PID" 2>/dev/null || true
    wait "$WORKER_PID" 2>/dev/null || true
  fi
  if [[ -n "${DAEMON_PID:-}" ]] && kill -0 "$DAEMON_PID" 2>/dev/null; then
    kill -TERM "$DAEMON_PID" 2>/dev/null || true
    wait "$DAEMON_PID" 2>/dev/null || true
    if kill -0 "$DAEMON_PID" 2>/dev/null; then
      kill -KILL "$DAEMON_PID" 2>/dev/null || true
    fi
  fi
  rm -f "$VIRMUX_SOCKET"
}
trap cleanup EXIT INT TERM

echo -e "${BOLD}Starting virmux supervisor daemon and TUI integration test suite${RESET}"
echo "Binary: ${VIRMUX}"
echo "Build Dir: ${BUILD_DIR}"
echo "Socket: ${VIRMUX_SOCKET}"
echo "================================================="

# ---------------------------------------------------------------------------
# Test Case 1: Daemon Startup in Background
# ---------------------------------------------------------------------------
echo -e "\n${BOLD}Test 1: Daemon Startup in Background${RESET}"

rm -f "$VIRMUX_SOCKET"

DAEMON_OUT=$("$VIRMUX" daemon)
DAEMON_PID=$(echo "$DAEMON_OUT" | grep -oP 'PID \K[0-9]+' || true)

SOCKET_READY=0
for _ in {1..50}; do
  if [[ -S "$VIRMUX_SOCKET" ]]; then
    SOCKET_READY=1
    break
  fi
  sleep 0.1
done

if [[ "$SOCKET_READY" -ne 1 ]]; then
  log_error "Supervisor daemon failed to create socket within 5s at: $VIRMUX_SOCKET"
  exit 1
fi

if [[ -z "$DAEMON_PID" ]] && command -v fuser >/dev/null 2>&1; then
  DAEMON_PID=$(fuser "$VIRMUX_SOCKET" 2>/dev/null | tr -d ' ' || true)
fi

LIST_OUT=$("$VIRMUX" list --json)
if [[ "$LIST_OUT" != "[]" ]]; then
  log_error "Expected empty JSON array '[]' on clean daemon start, got: $LIST_OUT"
  exit 1
fi

log_success "Daemon startup verified: background daemon running and returned empty session list"

# ---------------------------------------------------------------------------
# Test Case 2: Process Auto-Registration
# ---------------------------------------------------------------------------
echo -e "\n${BOLD}Test 2: Process Auto-Registration${RESET}"

"$VIRMUX" run -- /bin/sh -c "sleep 30" >/dev/null 2>&1 &
WORKER_PID=$!

REG_READY=0
LIST_JSON="[]"
for _ in {1..50}; do
  LIST_JSON=$("$VIRMUX" list --json 2>/dev/null || echo "[]")
  COUNT=$(echo "$LIST_JSON" | python3 -c "import sys, json; data = json.load(sys.stdin); print(len(data))" 2>/dev/null || echo "0")
  if [[ "$COUNT" -ge 1 ]]; then
    REG_READY=1
    break
  fi
  sleep 0.1
done

if [[ "$REG_READY" -ne 1 ]]; then
  log_error "Worker process failed to auto-register with supervisor daemon within 2s"
  exit 1
fi

echo "$LIST_JSON" | python3 -c "
import sys, json
data = json.load(sys.stdin)
assert len(data) == 1, f'Expected 1 session, got {len(data)}'
assert 'sleep' in data[0]['command'], f'Expected sleep in command, got {data[0][\"command\"]}'
"

TABLE_OUT=$("$VIRMUX" list)
SESSION_NAME=$(echo "$LIST_JSON" | python3 -c "import sys, json; data = json.load(sys.stdin); print(data[0]['name'])")
SH_PID=$(echo "$LIST_JSON" | python3 -c "import sys, json; data = json.load(sys.stdin); print(data[0]['pid'])")
CHILD_WORKERS=$(pgrep -P "$SH_PID" 2>/dev/null || true)

if ! echo "$TABLE_OUT" | grep -q "RUNNING"; then
  log_error "'virmux list' table output does not contain 'RUNNING'"
  echo "$TABLE_OUT" >&2
  exit 1
fi

if ! echo "$TABLE_OUT" | grep -q "$SESSION_NAME"; then
  log_error "'virmux list' table output does not contain session name '$SESSION_NAME'"
  echo "$TABLE_OUT" >&2
  exit 1
fi

log_success "Process auto-registration verified: session registered with command and RUNNING state"

# ---------------------------------------------------------------------------
# Test Case 3: Session Freeze & Thaw
# ---------------------------------------------------------------------------
echo -e "\n${BOLD}Test 3: Session Freeze & Thaw${RESET}"

SESSION_ID=$(echo "$LIST_JSON" | python3 -c "import sys, json; data = json.load(sys.stdin); print(data[0]['id'])")

"$VIRMUX" freeze "$SESSION_ID"
FROZEN_JSON=$("$VIRMUX" list --json)
echo "$FROZEN_JSON" | python3 -c "
import sys, json
data = json.load(sys.stdin)
assert len(data) >= 1, 'No sessions found in JSON'
assert data[0]['state'] == 'FROZEN', f'Expected state FROZEN, got {data[0][\"state\"]}'
"
log_success "Session freeze verified: state transitioned to FROZEN"

"$VIRMUX" thaw "$SESSION_ID"
THAWED_JSON=$("$VIRMUX" list --json)
echo "$THAWED_JSON" | python3 -c "
import sys, json
data = json.load(sys.stdin)
assert len(data) >= 1, 'No sessions found in JSON'
assert 'RUNNING' in data[0]['state'], f'Expected state RUNNING, got {data[0][\"state\"]}'
"
log_success "Session thaw verified: state transitioned back to RUNNING"

# ---------------------------------------------------------------------------
# Test Case 4: Session Kill
# ---------------------------------------------------------------------------
echo -e "\n${BOLD}Test 4: Session Kill${RESET}"

"$VIRMUX" kill "$SESSION_ID"
wait "$WORKER_PID" 2>/dev/null || true

if kill -0 "$WORKER_PID" 2>/dev/null; then
  log_error "Worker process $WORKER_PID still running after kill"
  exit 1
fi

if [[ -n "$CHILD_WORKERS" ]]; then
  for _ in {1..20}; do
    any_alive=0
    for cp in $CHILD_WORKERS; do
      if kill -0 "$cp" 2>/dev/null; then
        any_alive=1
        break
      fi
    done
    if [[ "$any_alive" -eq 0 ]]; then
      break
    fi
    sleep 0.05
  done
  for cp in $CHILD_WORKERS; do
    if kill -0 "$cp" 2>/dev/null; then
      log_error "Child worker process $cp still running after session kill"
      exit 1
    fi
  done
fi
WORKER_PID=""
CHILD_WORKERS=""

log_success "Session kill verified: worker process terminated cleanly"

# ---------------------------------------------------------------------------
# Test Case 5: Headless TUI Snapshot
# ---------------------------------------------------------------------------
echo -e "\n${BOLD}Test 5: Headless TUI Snapshot${RESET}"

OUTPUT=$("$VIRMUX" top --headless)
for expected in "VIRMUX v" "Linux" "Cores" "Host Load:" "[j/k] Navigate" "[K] Kill"; do
  if ! echo "$OUTPUT" | grep -Fq "$expected"; then
    log_error "Headless TUI snapshot missing expected substring: '$expected'"
    echo "$OUTPUT" >&2
    exit 1
  fi
done

log_success "Headless TUI snapshot verified: header, load, and navigation keys rendered"

# ---------------------------------------------------------------------------
# Test Case 6: Daemon Clean Shutdown
# ---------------------------------------------------------------------------
echo -e "\n${BOLD}Test 6: Daemon Clean Shutdown${RESET}"

if [[ -z "${DAEMON_PID:-}" ]]; then
  DAEMON_PID=$(fuser "$VIRMUX_SOCKET" 2>/dev/null | tr -d ' ' || true)
fi

if [[ -z "${DAEMON_PID:-}" ]]; then
  log_error "Failed to locate daemon PID"
  exit 1
fi

kill -TERM "$DAEMON_PID"

DAEMON_STOPPED=0
for _ in {1..50}; do
  if ! kill -0 "$DAEMON_PID" 2>/dev/null; then
    DAEMON_STOPPED=1
    break
  fi
  sleep 0.1
done

if [[ "$DAEMON_STOPPED" -ne 1 ]]; then
  log_error "Daemon PID $DAEMON_PID failed to exit within 5s"
  exit 1
fi

DAEMON_PID=""

if [[ -e "$VIRMUX_SOCKET" ]]; then
  log_error "VIRMUX_SOCKET was not unlinked on daemon shutdown: $VIRMUX_SOCKET"
  exit 1
fi

log_success "Daemon clean shutdown verified: SIGTERM handled and socket cleanly unlinked"

echo -e "\n${GREEN}${BOLD}All supervisor daemon and TUI integration tests passed!${RESET}"
