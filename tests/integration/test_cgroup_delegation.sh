#!/usr/bin/env bash
#
# tests/integration/test_cgroup_delegation.sh - Real cgroup limits for an
# unprivileged user through a delegated cgroup subtree.
#
# Needs a shell inside the systemd user manager (a desktop terminal, or
# `systemd-run --user --wait --pipe ...`). Without delegation the test exits
# 77 (meson: SKIP) unless VIRMUX_REQUIRE_DELEGATION=1, which turns the skip
# into a failure so CI cannot silently lose coverage.
#
# Usage: test_cgroup_delegation.sh [BUILD_DIR]
#

set -euo pipefail

if [[ -t 1 ]]; then
  GREEN="\033[0;32m"
  RED="\033[0;31m"
  YELLOW="\033[0;33m"
  BOLD="\033[1m"
  RESET="\033[0m"
else
  GREEN=""
  RED=""
  YELLOW=""
  BOLD=""
  RESET=""
fi

log_success() { echo -e "  [${GREEN}PASS${RESET}] $1"; }
log_error() { echo -e "  [${RED}FAIL${RESET}] $1" >&2; }
log_skip() { echo -e "  [${YELLOW}SKIP${RESET}] $1"; }

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
BUILD_DIR="${1:-${REPO_ROOT}/build}"
if [[ "$BUILD_DIR" != /* ]]; then
  BUILD_DIR="$(cd "$BUILD_DIR" && pwd)"
fi

VIRMUX="${BUILD_DIR}/virmux"
if [[ ! -x "$VIRMUX" ]]; then
  log_error "virmux executable not found at: ${VIRMUX}"
  exit 1
fi
if [[ -f "${BUILD_DIR}/libvirmux_intercept.so" ]]; then
  export VIRMUX_INTERCEPT_LIB="${BUILD_DIR}/libvirmux_intercept.so"
fi

FAILURES=0
fail() {
  log_error "$1"
  FAILURES=$((FAILURES + 1))
}

# The child's own cgroup directory, as seen from inside the sandbox. Single
# quotes are intended: the child's shell expands this, not ours.
# shellcheck disable=SC2016
OWN_CGROUP='/sys/fs/cgroup$(sed -n "s/^0:://p" /proc/self/cgroup)'

echo -e "${BOLD}Running delegated cgroup integration tests...${RESET}"

# ---------------------------------------------------------------------------
# 0. Is there a delegated subtree at all?
# ---------------------------------------------------------------------------
CG_LINE="$("$VIRMUX" run --cores 2 --mem 64M -- cat /proc/self/cgroup 2>/dev/null | grep '^0::' || true)"
if [[ "$CG_LINE" != *"/virmux/run-"* ]]; then
  if [[ "${VIRMUX_REQUIRE_DELEGATION:-0}" == "1" ]]; then
    log_error "no delegated cgroup (child cgroup: '${CG_LINE}') but VIRMUX_REQUIRE_DELEGATION=1"
    exit 1
  fi
  log_skip "no delegated cgroup subtree here (child cgroup: '${CG_LINE}')"
  exit 77
fi
log_success "child runs in a virmux run cgroup: ${CG_LINE#0::}"

# ---------------------------------------------------------------------------
# 1. memory.max matches --mem
# ---------------------------------------------------------------------------
MEM_MAX="$("$VIRMUX" run --cores 2 --mem 64M -- sh -c "cat ${OWN_CGROUP}/memory.max")"
if [[ "$MEM_MAX" == "67108864" ]]; then
  log_success "memory.max is 64M"
else
  fail "memory.max: expected 67108864, got '${MEM_MAX}'"
fi

# ---------------------------------------------------------------------------
# 2. Allocating past the limit is OOM-killed (128 + SIGKILL)
# ---------------------------------------------------------------------------
if command -v python3 >/dev/null 2>&1; then
  # bytearray(n) zero-fills, so every page is really allocated.
  ALLOC=(python3 -c 'b = bytearray(200 * 1024 * 1024)')
else
  # shellcheck disable=SC2016 # expanded by the sandboxed shell
  ALLOC=(sh -c 'x=$(head -c 209715200 /dev/zero | tr "\0" a); echo ${#x}')
fi
set +e
"$VIRMUX" run --cores 2 --mem 64M -- "${ALLOC[@]}" >/dev/null 2>&1
OOM_EC=$?
set -e
if [[ "$OOM_EC" -eq 137 ]]; then
  log_success "200M allocation under --mem 64M was OOM-killed (exit 137)"
else
  fail "200M allocation under --mem 64M: expected exit 137, got ${OOM_EC}"
fi

# ---------------------------------------------------------------------------
# 3. The child is pinned to --cores real CPUs
# ---------------------------------------------------------------------------
HOST_CPUS="$(nproc --all)"
WANT_CPUS=2
if [[ "$HOST_CPUS" -lt 2 ]]; then
  WANT_CPUS="$HOST_CPUS"
fi
if command -v python3 >/dev/null 2>&1; then
  AFFINITY="$("$VIRMUX" run --cores 2 -- python3 -c 'import os; print(len(os.sched_getaffinity(0)))')"
else
  AFFINITY="$("$VIRMUX" run --cores 2 -- nproc)"
fi
if [[ "$AFFINITY" == "$WANT_CPUS" ]]; then
  log_success "affinity mask has ${WANT_CPUS} CPUs"
else
  fail "affinity: expected ${WANT_CPUS} CPUs, got '${AFFINITY}'"
fi

# ---------------------------------------------------------------------------
# 4. cpu.max caps CPU time at --cores CPUs' worth
# ---------------------------------------------------------------------------
CPU_MAX="$("$VIRMUX" run --cores 2 -- sh -c "cat ${OWN_CGROUP}/cpu.max")"
if [[ "$CPU_MAX" == "200000 100000" ]]; then
  log_success "cpu.max is '200000 100000'"
else
  fail "cpu.max: expected '200000 100000', got '${CPU_MAX}'"
fi

# ---------------------------------------------------------------------------
# 5. Run cgroups are removed afterwards
# ---------------------------------------------------------------------------
RUN_DIR="$("$VIRMUX" run --cores 1 -- sh -c "echo ${OWN_CGROUP}")"
if [[ ! -d "$RUN_DIR" ]]; then
  log_success "run cgroup removed after exit"
else
  fail "run cgroup still exists after exit: ${RUN_DIR}"
fi

echo "================================================="
if [[ "$FAILURES" -ne 0 ]]; then
  echo -e "${RED}${BOLD}${FAILURES} delegated cgroup check(s) failed${RESET}" >&2
  exit 1
fi
echo -e "${GREEN}${BOLD}All delegated cgroup integration tests passed!${RESET}"
exit 0
