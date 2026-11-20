# Phase 5: Fault Isolation, OOM Shielding & Auto-Hibernation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use `superpowers:subagent-driven-development` or `superpowers:executing-plans` to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Implement deterministic Linux OOM priority hierarchy (`oom_score_adj`), fork-bomb protection (`pids.max` & `RLIMIT_NPROC`), inactivity auto-freeze with heap pageout, and experimental session checkpointing (`virmux checkpoint` / `restore`).

**Architecture:**

1. The supervisor daemon marks itself protected against kernel OOM kills (`oom_score_adj = -900`).
2. Sandboxed worker children are marked sacrificial (`oom_score_adj = +800`) before `execvp` so kernel OOM terminates greedy leaf tasks first.
3. Cgroup v2 `pids.max` and `RLIMIT_NPROC` ceilings contain fork-bombs from exhausting the host process table.
4. Background telemetry loop detects sustained idle periods (>500ms inactivity), triggers transparent cgroup freezing, and calls `process_madvise(MADV_PAGEOUT)` to reclaim inactive memory into zram/swap.
5. Checkpoint engine dumps session process state using CRIU when elevated capabilities exist, falling back to deep freeze + pageout in standard unprivileged environments.

**Tech Stack:** C++20, Linux cgroups v2 (`pids.max`, `cgroup.freeze`), `/proc/<pid>/oom_score_adj`, `process_madvise(MADV_PAGEOUT)`, CRIU, Unity test framework, Meson build system.

**Spec:** `docs/specs/05-FAULT-ISOLATION-LIFECYCLE.md`

---

## Global Constraints

- **Language Standard:** C++20 (`-std=c++20`), zero compiler warnings with `-Wall -Wextra -Wpedantic -Werror`.
- **Path Hygiene:** Never hardcode or emit `/home/<username>/` in any source code, documentation, config, commit, or test fixture. Always use `~` or `$HOME`.
- **Unprivileged Operation:** Graceful degradation on permission denial; core isolation operates in standard Linux/WSL2 userspace.
- **Fail-Closed Blast Containment:** Failures in worker sandboxes must never crash sibling sessions or supervisor daemon.

---

## Review Focus

1. **`oom_score_adj` Permission Boundaries:** Daemon must safely ignore `EPERM` when attempting negative adjustment unprivileged. Worker child adjustment (`+800`) must succeed for normal users.
2. **Fork-Bomb Blast Containment:** `pids.max` write failure must trigger `setrlimit(RLIMIT_NPROC)` fallback.
3. **Auto-Freeze Inactivity Guard:** Do not freeze tasks that are waiting for quick compute bursts; sustain 0% CPU delta check across ticks.
4. **Checkpoint Directory Isolation:** Clean up temporary checkpoint images on failure; enforce 0700 permissions.
5. **No Regressions:** Existing 34 unit and integration tests must remain passing.

---

## Task Breakdown

- [x] **Task 1: Deterministic OOM Score Adjuster & Fork-Bomb Protection**
  - Implement `/proc/self/oom_score_adj` writing with safe fallback in daemon (`-900`) and worker child (`+800`).
  - Implement `CgroupSlice::set_pids_max()` and `RLIMIT_NPROC` fallback in `sandbox.cpp`.
  - Add unit tests in `tests/unit/test_fault_isolation.cpp`.

- [x] **Task 2: Inactivity Detection & Transparent Auto-Freeze Engine**
  - Add CPU tick delta tracking to `SessionRegistry::refresh_telemetry()`.
  - Implement configurable auto-freeze threshold (`idle_timeout_ms = 500`) with automatic `freeze_session` and `MemoryCompactor::compact_pid()` pageout.
  - Add auto-thaw hook on activity/IPC.
  - Add unit tests in `tests/unit/test_idle_autofreeze.cpp`.

- [x] **Task 3: Session Checkpoint & Restore Subsystem (`virmux checkpoint` / `restore`)**
  - Implement `CheckpointManager` in `include/virmux/checkpoint.hpp` and `src/core/checkpoint.cpp`.
  - Add CLI subcommands `virmux checkpoint <ID>` and `virmux restore <ID>`.
  - Add unit tests in `tests/unit/test_checkpoint.cpp`.

- [x] **Task 4: Fault Isolation & Blast-Radius Integration Test Suite**
  - Implement `tests/integration/test_fault_isolation.sh`.
  - Register integration test in `meson.build`.
  - Verify worker sacrificial OOM kill, fork-bomb containment, and auto-freeze pageout.
