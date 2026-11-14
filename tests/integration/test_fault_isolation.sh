#!/usr/bin/env bash
#
# tests/integration/test_fault_isolation.sh - Fault Isolation & Blast-Radius Integration Test Suite
#
# Tests worker sacrificial OOM priority (+800), sibling isolation on leaf termination,
# fork-bomb / pids limit containment (EAGAIN ceiling), inactivity auto-freeze (>500ms),
# and session checkpoint & restore lifecycle.
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

export VIRMUX_SOCKET="/tmp/virmux_fi_$$.sock"
export VIRMUX_CHECKPOINT_DIR="/tmp/virmux_fi_chk_$$"
mkdir -p "$VIRMUX_CHECKPOINT_DIR"

DAEMON_PID=""
WORKER_PIDS=""

stop_daemon() {
  if [[ -n "${DAEMON_PID:-}" ]] && kill -0 "$DAEMON_PID" 2>/dev/null; then
    kill -TERM "$DAEMON_PID" 2>/dev/null || true
    for _ in {1..30}; do
      if ! kill -0 "$DAEMON_PID" 2>/dev/null; then
        break
      fi
      sleep 0.1
    done
    if kill -0 "$DAEMON_PID" 2>/dev/null; then
      kill -KILL "$DAEMON_PID" 2>/dev/null || true
    fi
  elif [[ -S "${VIRMUX_SOCKET:-}" ]] && command -v fuser >/dev/null 2>&1; then
    local dpid
    dpid=$(fuser "$VIRMUX_SOCKET" 2>/dev/null | tr -d ' ' || true)
    if [[ -n "$dpid" ]]; then
      kill -KILL $dpid 2>/dev/null || true
    fi
  fi
  DAEMON_PID=""
  rm -f "${VIRMUX_SOCKET:-}"
}

cleanup() {
  if [[ -n "${WORKER_PIDS:-}" ]]; then
    kill -KILL $WORKER_PIDS 2>/dev/null || true
    wait $WORKER_PIDS 2>/dev/null || true
  fi
  stop_daemon
  rm -rf "${VIRMUX_CHECKPOINT_DIR:-}"
  rm -f "${VIRMUX_SOCKET:-}"
}
trap cleanup EXIT INT TERM

wait_for_socket() {
  local ready=0
  for _ in {1..50}; do
    if [[ -S "$VIRMUX_SOCKET" ]]; then
      ready=1
      break
    fi
    sleep 0.1
  done
  if [[ "$ready" -ne 1 ]]; then
    log_error "Supervisor daemon failed to create socket within 5s at: $VIRMUX_SOCKET"
    exit 1
  fi
}

echo -e "${BOLD}Starting Virmux Fault Isolation & Blast-Radius Integration Test Suite${RESET}"
echo "Binary: ${VIRMUX}"
echo "Build Dir: ${BUILD_DIR}"
echo "Socket: ${VIRMUX_SOCKET}"
echo "Checkpoint Dir: ${VIRMUX_CHECKPOINT_DIR}"
echo "================================================="

# ---------------------------------------------------------------------------
# Test Case 1: Worker Sacrificial OOM Score & Sibling Isolation
# ---------------------------------------------------------------------------
echo -e "\n${BOLD}Test 1: Worker Sacrificial OOM Score & Sibling Isolation${RESET}"

rm -f "$VIRMUX_SOCKET"
DAEMON_OUT=$("$VIRMUX" daemon)
DAEMON_PID=$(echo "$DAEMON_OUT" | grep -oP 'PID \K[0-9]+' || true)
wait_for_socket

# 1. Start sibling worker session
"$VIRMUX" run -- /bin/sh -c "sleep 30" >/dev/null 2>&1 &
RUN_PID_1=$!
WORKER_PIDS="$RUN_PID_1"

# Wait for session 1 to register
for _ in {1..50}; do
  LIST_JSON=$("$VIRMUX" list --json 2>/dev/null || echo "[]")
  COUNT=$(echo "$LIST_JSON" | python3 -c "import sys, json; print(len(json.load(sys.stdin)))" 2>/dev/null || echo "0")
  if [[ "$COUNT" -ge 1 ]]; then
    break
  fi
  sleep 0.1
done

# 2. Start second worker session
"$VIRMUX" run -- /bin/sh -c "sleep 30" >/dev/null 2>&1 &
RUN_PID_2=$!
WORKER_PIDS="$WORKER_PIDS $RUN_PID_2"

# Wait for session 2 to register
for _ in {1..50}; do
  LIST_JSON=$("$VIRMUX" list --json 2>/dev/null || echo "[]")
  COUNT=$(echo "$LIST_JSON" | python3 -c "import sys, json; print(len(json.load(sys.stdin)))" 2>/dev/null || echo "0")
  if [[ "$COUNT" -ge 2 ]]; then
    break
  fi
  sleep 0.1
done

LIST_JSON=$("$VIRMUX" list --json)
ID_1=$(echo "$LIST_JSON" | python3 -c "import sys, json; print(json.load(sys.stdin)[0]['id'])")
PID_1=$(echo "$LIST_JSON" | python3 -c "import sys, json; print(json.load(sys.stdin)[0]['pid'])")
ID_2=$(echo "$LIST_JSON" | python3 -c "import sys, json; print(json.load(sys.stdin)[1]['id'])")
PID_2=$(echo "$LIST_JSON" | python3 -c "import sys, json; print(json.load(sys.stdin)[1]['pid'])")

if [[ ! -r "/proc/$PID_2/oom_score_adj" ]]; then
  log_error "Could not read /proc/$PID_2/oom_score_adj for second worker"
  exit 1
fi

OOM_SCORE=$(cat "/proc/$PID_2/oom_score_adj")
if [[ "$OOM_SCORE" -ne 800 ]]; then
  log_error "Expected oom_score_adj 800 for worker process, got: $OOM_SCORE"
  exit 1
fi
log_success "Worker sacrificial OOM score verified: /proc/$PID_2/oom_score_adj is 800"

# 3. Terminate second worker (simulating OOM kill by sending SIGKILL to leaf process)
kill -9 "$PID_2"
# Allow supervisor telemetry loop (1.0s interval) to detect process termination
sleep 1.2

AFTER_JSON=$("$VIRMUX" list --json)
python3 -c "
import sys, json
data = json.loads('''$AFTER_JSON''')
s1 = next((s for s in data if s['id'] == $ID_1), None)
assert s1 is not None, f'Sibling session $ID_1 missing from list: {data}'
assert 'RUNNING' in s1['state'], f'Sibling worker expected RUNNING, got {s1[\"state\"]}'

s2 = next((s for s in data if s['id'] == $ID_2), None)
assert s2 is not None, f'Terminated worker session $ID_2 missing from list: {data}'
assert s2['state'] == 'DEAD', f'Terminated worker expected DEAD, got {s2[\"state\"]}'
"

# Verify daemon is healthy and responding
if ! "$VIRMUX" stats --json >/dev/null 2>&1; then
  log_error "Supervisor daemon failed or unresponsive after worker termination"
  exit 1
fi

kill -KILL "$RUN_PID_1" "$RUN_PID_2" 2>/dev/null || true
wait "$RUN_PID_1" "$RUN_PID_2" 2>/dev/null || true
WORKER_PIDS=""

log_success "Fault isolation verified: terminated worker marked DEAD while sibling and daemon remain healthy"

# ---------------------------------------------------------------------------
# Test Case 2: Fork-Bomb / PIDs Limit Containment
# ---------------------------------------------------------------------------
echo -e "\n${BOLD}Test 2: Fork-Bomb / PIDs Limit Containment${RESET}"

FORK_BOMB_OUTPUT=$("$VIRMUX" run --max-pids 40 -- python3 -c '
import os, sys, errno, time

pids = []
hit_ceiling = False
for i in range(120):
    try:
        p = os.fork()
        if p == 0:
            time.sleep(5)
            os._exit(0)
        pids.append(p)
    except OSError as e:
        if e.errno == errno.EAGAIN:
            hit_ceiling = True
            break
        raise

for p in pids:
    try:
        os.kill(p, 9)
        os.waitpid(p, 0)
    except OSError:
        pass

if not hit_ceiling:
    print("FAILED: Fork ceiling was not hit")
    sys.exit(1)

print(f"PASS: Fork ceiling hit EAGAIN after {len(pids)} children")
sys.exit(0)
')
echo "$FORK_BOMB_OUTPUT"

# Verify daemon remains healthy and responsive after fork-bomb attempt
if ! "$VIRMUX" stats --json >/dev/null 2>&1; then
  log_error "Supervisor daemon became unresponsive after fork-bomb attempt"
  exit 1
fi

log_success "Fork-bomb containment verified: process hit EAGAIN ceiling without starving host or daemon"

# ---------------------------------------------------------------------------
# Test Case 3: Inactivity Auto-Freeze Integration
# ---------------------------------------------------------------------------
echo -e "\n${BOLD}Test 3: Inactivity Auto-Freeze Integration${RESET}"

stop_daemon
rm -f "$VIRMUX_SOCKET"

DAEMON_OUT=$("$VIRMUX" daemon --auto-freeze --idle-ms 500)
DAEMON_PID=$(echo "$DAEMON_OUT" | grep -oP 'PID \K[0-9]+' || true)
wait_for_socket

# Spawn idle worker process
"$VIRMUX" run -- /bin/sh -c "sleep 30" >/dev/null 2>&1 &
RUN_PID_3=$!
WORKER_PIDS="$RUN_PID_3"

for _ in {1..50}; do
  LIST_JSON=$("$VIRMUX" list --json 2>/dev/null || echo "[]")
  COUNT=$(echo "$LIST_JSON" | python3 -c "import sys, json; print(len(json.load(sys.stdin)))" 2>/dev/null || echo "0")
  if [[ "$COUNT" -ge 1 ]]; then
    break
  fi
  sleep 0.1
done

ID_3=$(echo "$LIST_JSON" | python3 -c "import sys, json; print(json.load(sys.stdin)[0]['id'])")

# Wait >1.0s (1.5s) for auto-freeze to trigger (>500ms inactivity + telemetry cycle)
sleep 1.5

FROZEN_JSON=$("$VIRMUX" list --json)
python3 -c "
import sys, json
data = json.loads('''$FROZEN_JSON''')
s = next((s for s in data if s['id'] == $ID_3), None)
assert s is not None, 'Session $ID_3 not found in session list'
assert s['state'] == 'FROZEN', f'Expected state FROZEN, got {s[\"state\"]}'
"
log_success "Auto-freeze verified: idle session transitioned to FROZEN after >1.0s"

# Run virmux thaw <ID>
"$VIRMUX" thaw "$ID_3"
THAWED_JSON=$("$VIRMUX" list --json)
python3 -c "
import sys, json
data = json.loads('''$THAWED_JSON''')
s = next((s for s in data if s['id'] == $ID_3), None)
assert s is not None, 'Session $ID_3 not found in session list'
assert 'RUNNING' in s['state'], f'Expected state RUNNING, got {s[\"state\"]}'
"
log_success "Session thaw verified: session transitioned back to RUNNING"

kill -KILL "$RUN_PID_3" 2>/dev/null || true
wait "$RUN_PID_3" 2>/dev/null || true
WORKER_PIDS=""

# ---------------------------------------------------------------------------
# Test Case 4: Checkpoint & Restore Lifecycle
# ---------------------------------------------------------------------------
echo -e "\n${BOLD}Test 4: Checkpoint & Restore Lifecycle${RESET}"

# Spawn active worker process
"$VIRMUX" run -- /bin/sh -c "sleep 30" >/dev/null 2>&1 &
RUN_PID_4=$!
WORKER_PIDS="$RUN_PID_4"

for _ in {1..50}; do
  LIST_JSON=$("$VIRMUX" list --json 2>/dev/null || echo "[]")
  COUNT=$(echo "$LIST_JSON" | python3 -c "import sys, json; print(len(json.load(sys.stdin)))" 2>/dev/null || echo "0")
  if [[ "$COUNT" -ge 1 ]]; then
    break
  fi
  sleep 0.1
done

ID_4=$(echo "$LIST_JSON" | python3 -c "import sys, json; print(json.load(sys.stdin)[0]['id'])")

# Run checkpoint
"$VIRMUX" checkpoint "$ID_4"

CHKPT_JSON=$("$VIRMUX" list --json)
python3 -c "
import sys, json
data = json.loads('''$CHKPT_JSON''')
s = next((s for s in data if s['id'] == $ID_4), None)
assert s is not None, 'Session $ID_4 not found in session list'
assert s['state'] == 'CHECKPOINTED', f'Expected state CHECKPOINTED, got {s[\"state\"]}'
"
log_success "Session checkpoint verified: state transitioned to CHECKPOINTED"

# Verify metadata file exists
META_FILE="${VIRMUX_CHECKPOINT_DIR}/${ID_4}/checkpoint.json"
if [[ ! -f "$META_FILE" ]]; then
  log_error "Checkpoint metadata file missing at: $META_FILE"
  exit 1
fi
log_success "Checkpoint metadata verified: $META_FILE exists"

# Run restore
"$VIRMUX" restore "$ID_4"

RESTORE_JSON=$("$VIRMUX" list --json)
python3 -c "
import sys, json
data = json.loads('''$RESTORE_JSON''')
s = next((s for s in data if s['id'] == $ID_4), None)
assert s is not None, 'Session $ID_4 not found in session list'
assert 'RUNNING' in s['state'], f'Expected state RUNNING, got {s[\"state\"]}'
"
log_success "Session restore verified: state transitioned back to RUNNING"

# Verify checkpoint directory is cleaned up
if [[ -d "${VIRMUX_CHECKPOINT_DIR}/${ID_4}" ]]; then
  log_error "Checkpoint directory was not cleaned up after restore: ${VIRMUX_CHECKPOINT_DIR}/${ID_4}"
  exit 1
fi
log_success "Checkpoint cleanup verified: image directory removed upon restore"

kill -KILL "$RUN_PID_4" 2>/dev/null || true
wait "$RUN_PID_4" 2>/dev/null || true
WORKER_PIDS=""

stop_daemon

echo -e "\n${BOLD}${GREEN}All fault isolation and blast-radius integration tests passed!${RESET}\n"
