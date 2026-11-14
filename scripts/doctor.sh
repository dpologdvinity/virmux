#!/usr/bin/env bash
#
# scripts/doctor.sh - Diagnostic environment & kernel prerequisite checker for virmux
#

set -uo pipefail

# ANSI color codes (disabled if not in an interactive terminal)
if [[ -t 1 ]]; then
  GREEN="\033[0;32m"
  YELLOW="\033[0;33m"
  RED="\033[0;31m"
  BLUE="\033[0;34m"
  BOLD="\033[1m"
  RESET="\033[0m"
else
  GREEN=""
  YELLOW=""
  RED=""
  BLUE=""
  BOLD=""
  RESET=""
fi

TOTAL_CHECKS=0
PASSED_CHECKS=0
WARNINGS=0
ERRORS=0

log_pass() {
  TOTAL_CHECKS=$((TOTAL_CHECKS + 1))
  PASSED_CHECKS=$((PASSED_CHECKS + 1))
  echo -e "  [${GREEN}OK${RESET}] $1"
}

log_warn() {
  TOTAL_CHECKS=$((TOTAL_CHECKS + 1))
  WARNINGS=$((WARNINGS + 1))
  echo -e "  [${YELLOW}WARN${RESET}] $1"
}

log_fail() {
  TOTAL_CHECKS=$((TOTAL_CHECKS + 1))
  ERRORS=$((ERRORS + 1))
  echo -e "  [${RED}FAIL${RESET}] $1"
}

log_info() {
  echo -e "  [${BLUE}INFO${RESET}] $1"
}

echo -e "${BOLD}virmux system prerequisite diagnostics${RESET}"
echo "========================================"

# 1. Linux Kernel Version Check (>= 5.10 required for process_madvise & unified cgroup v2)
echo -e "\n${BOLD}1. Kernel Version${RESET}"
KERNEL_RELEASE=$(uname -r)
KERNEL_MAJOR=$(echo "$KERNEL_RELEASE" | cut -d. -f1 | sed 's/[^0-9]//g')
KERNEL_MINOR=$(echo "$KERNEL_RELEASE" | cut -d. -f2 | sed 's/[^0-9]//g')

if [[ -z "$KERNEL_MAJOR" || -z "$KERNEL_MINOR" ]]; then
  log_fail "Unable to parse kernel version: ${KERNEL_RELEASE}"
elif (( KERNEL_MAJOR > 5 || (KERNEL_MAJOR == 5 && KERNEL_MINOR >= 10) )); then
  log_pass "Linux kernel version: ${KERNEL_RELEASE} (>= 5.10 required)"
else
  log_fail "Linux kernel version: ${KERNEL_RELEASE} is older than required 5.10"
fi

# 2. cgroups v2 Availability
echo -e "\n${BOLD}2. cgroups v2 Status${RESET}"
if [[ -f "/sys/fs/cgroup/cgroup.controllers" ]]; then
  CONTROLLERS=$(cat /sys/fs/cgroup/cgroup.controllers 2>/dev/null || true)
  log_pass "Unified cgroup v2 hierarchy mounted at /sys/fs/cgroup"
  log_info "Available root controllers: ${CONTROLLERS:-none}"

  # Check delegation or unprivileged access
  if [[ -w "/sys/fs/cgroup" ]]; then
    log_pass "cgroups v2 hierarchy is writable (root or privileged delegation)"
  else
    log_info "cgroups root is read-only (unprivileged fallback mode enabled for virmux)"
  fi
elif grep -qw "cgroup2" /proc/mounts 2>/dev/null; then
  log_pass "cgroup2 filesystem mounted"
else
  log_fail "cgroups v2 not mounted. virmux requires cgroups v2 for resource slicing"
fi

# 3. KSM (Kernel Samepage Merging) Status
echo -e "\n${BOLD}3. KSM (Kernel Samepage Merging)${RESET}"
if [[ -d "/sys/kernel/mm/ksm" ]]; then
  if [[ -f "/sys/kernel/mm/ksm/run" ]]; then
    KSM_RUN=$(cat /sys/kernel/mm/ksm/run 2>/dev/null || echo "-1")
    case "$KSM_RUN" in
      1)
        log_pass "KSM is active (run=1)"
        if [[ -f "/sys/kernel/mm/ksm/pages_sharing" ]]; then
          PAGES_SHARING=$(cat /sys/kernel/mm/ksm/pages_sharing 2>/dev/null || echo "0")
          log_info "KSM pages sharing: ${PAGES_SHARING}"
        fi
        ;;
      0)
        log_pass "KSM support is present in kernel (currently disabled: run=0)"
        log_info "Enable KSM page deduplication with: echo 1 | sudo tee /sys/kernel/mm/ksm/run"
        ;;
      2)
        log_warn "KSM is currently in unmerge mode (run=2)"
        ;;
      *)
        log_warn "KSM run status unknown: ${KSM_RUN}"
        ;;
    esac
  else
    log_warn "KSM directory exists, but /sys/kernel/mm/ksm/run is unavailable"
  fi
else
  log_warn "KSM (Kernel Samepage Merging) is not supported or compiled in this kernel"
fi

# 4. Compilers & Build Tools Availability
echo -e "\n${BOLD}4. Compilers & Build Tools${RESET}"
HAVE_COMPILER=false

if command -v gcc &>/dev/null; then
  GCC_VER=$(gcc --version 2>&1 | head -n 1)
  log_pass "GCC found: ${GCC_VER}"
  HAVE_COMPILER=true
else
  log_info "GCC not found"
fi

if command -v clang &>/dev/null; then
  CLANG_VER=$(clang --version 2>&1 | head -n 1)
  log_pass "Clang found: ${CLANG_VER}"
  HAVE_COMPILER=true
else
  log_info "Clang not found"
fi

if [[ "$HAVE_COMPILER" = true ]]; then
  log_pass "At least one supported C/C++ compiler is installed"
else
  log_fail "No C/C++ compiler found (install gcc or clang)"
fi

if command -v meson &>/dev/null; then
  MESON_VER=$(meson --version 2>&1)
  log_pass "Meson found: version ${MESON_VER}"
else
  log_fail "Meson build system not found"
fi

if command -v ninja &>/dev/null; then
  NINJA_VER=$(ninja --version 2>&1)
  log_pass "Ninja found: version ${NINJA_VER}"
else
  log_fail "Ninja build executor not found"
fi

# 5. User Namespace Permissions
echo -e "\n${BOLD}5. User Namespace Permissions${RESET}"
USERNS_OK=false
if unshare --user true 2>/dev/null; then
  log_pass "Unprivileged user namespaces are functional via unshare(1)"
  USERNS_OK=true
fi

if [[ -f "/proc/sys/kernel/unprivileged_userns_clone" ]]; then
  CLONE_VAL=$(cat /proc/sys/kernel/unprivileged_userns_clone 2>/dev/null || echo "")
  if [[ "$CLONE_VAL" == "1" ]]; then
    log_pass "unprivileged_userns_clone sysctl is enabled (1)"
  elif [[ "$CLONE_VAL" == "0" ]]; then
    log_warn "unprivileged_userns_clone sysctl is disabled (0)"
  fi
fi

if [[ -f "/proc/sys/user/max_user_namespaces" ]]; then
  MAX_USERNS=$(cat /proc/sys/user/max_user_namespaces 2>/dev/null || echo "0")
  if (( MAX_USERNS > 0 )); then
    log_pass "max_user_namespaces limit is positive: ${MAX_USERNS}"
  else
    log_fail "max_user_namespaces is 0 (user namespaces disabled)"
  fi
fi

if [[ "$USERNS_OK" = false && ! -f "/proc/sys/kernel/unprivileged_userns_clone" ]]; then
  log_warn "Unprivileged user namespaces could not be tested directly"
fi

# Diagnostics Summary
echo -e "\n========================================"
echo -e "${BOLD}Diagnostics Summary:${RESET} ${PASSED_CHECKS}/${TOTAL_CHECKS} checks passed, ${WARNINGS} warnings, ${ERRORS} errors."

if (( ERRORS > 0 )); then
  echo -e "${RED}${BOLD}Doctor detected critical errors. Some virmux features may fail.${RESET}"
  exit 1
else
  echo -e "${GREEN}${BOLD}All critical system prerequisites satisfied.${RESET}"
  exit 0
fi
