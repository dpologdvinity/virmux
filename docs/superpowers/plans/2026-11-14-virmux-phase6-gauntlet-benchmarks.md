# Phase 6: "12-Terminal Gauntlet" Stress Benchmark Suite Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use `superpowers:subagent-driven-development` or `superpowers:executing-plans` to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Implement the "12-Terminal Gauntlet" benchmark engine, synthetic workload generators, real-time input-lag PTY latency sampler, and telemetry reporter (`virmux bench run`, `virmux bench report`) comparing unmanaged baseline against Virmux-managed execution.

**Architecture:**
- High-frequency telemetry sampler captures host memory, swap, KSM deduplication, and task status.
- PTY input-latency probe runs on Core 0–1 measuring interactive shell latency every 50ms.
- 12 synthetic workload generators emulate 4x Browser (DOM/JS memory allocation), 4x Compiler (multi-threaded CPU burst), 2x Model (tensor math), and 2x Build (I/O streaming).
- CLI subcommand `virmux bench run` runs baseline vs managed runs, generating ANSI terminal comparison matrices and JSON artifacts.

**Tech Stack:** C++20, Linux pseudo-terminals (`openpty`), `/proc/meminfo`, POSIX timers, Unity test framework, Meson build system.

**Spec:** `docs/specs/07-TESTING-AND-BENCHMARKS.md`

## Global Constraints
- Linux kernel 5.10+ standard operation without requiring root.
- C++20 standard; `-Wall -Wextra -Wpedantic -Werror` clean.
- Never write or commit absolute home paths (`/home/<username>/`), always use `~` or `$HOME`.
- Conventional commits (`feat:`, `fix:`, `test:`, `docs:`, `chore:`).

## Review Focus
1. Benchmark worker termination: Ensure all child processes in the 12-terminal gauntlet are cleanly terminated even on early abort (`SIGINT`/`SIGTERM`).
2. Telemetry sampler thread safety: Concurrent access between background sampling thread and report aggregator.
3. Input lag probe accuracy: PTY round-trip measurement using monotonic high-resolution clock (`std::chrono::steady_clock`).
4. Resource limit safety: Synthetic workers must respect configured iteration caps and memory limits so benchmarks cannot freeze low-memory test runners.
5. JSON report stability: Format must conform to machine-readable schema for Phase 7 CI artifact ingestion.

---

## Tasks

- [x] **Task 1: Benchmark Workload Generator & Input Latency Probe**
  - Implement `include/virmux/bench.hpp` and `src/bench/bench.cpp`.
  - Implement synthetic benchmark workers:
    - `Browser`: 1.5 GB anonymous page allocation with text/DOM rendering loops.
    - `Compiler`: Multi-threaded CPU burst parsing and matrix transforms.
    - `Model`: Repeated tensor multiplication and memory compaction cycles.
    - `Build`: File creation, streaming hashing, and CAS staging.
  - Implement `InputLatencyProbe`: spawns PTY master/slave pair, writes periodic test byte, measures round-trip time with monotonic nanosecond precision.
  - Implement `TelemetrySampler`: samples `/proc/meminfo`, swap, KSM savings, and active PIDs at configurable intervals.
  - Unit tests in `tests/unit/test_bench.cpp`.

- [x] **Task 2: Benchmark Engine & Comparison Reporter (`virmux bench run`, `report`)**
  - Implement `BenchmarkRunner` orchestrating 12 concurrent tasks under:
    - Mode A: Unmanaged baseline (direct background fork/exec without virmux controls).
    - Mode B: Virmux-managed (sandboxed with admission gate, core shielding, memory limits).
  - Implement `BenchmarkReport` formatter:
    - Formats side-by-side terminal comparison table (Peak RAM, Swap, Input Lag, Wall-Clock, OOM count).
    - Generates structured JSON report (`--output <file.json>`).
  - Add CLI subcommands `virmux bench run` and `virmux bench report` in `src/cli/cli_bench.cpp`, `include/virmux/cli_bench.hpp`, and `cli/main.cpp`.
  - Unit tests for CLI parsing and reporting in `tests/unit/test_cli_bench.cpp`.

- [x] **Task 3: Integration Benchmark Gauntlet Suite**
  - Implement `tests/integration/test_bench_gauntlet.sh`.
  - Validate quick 4-task and full 12-task gauntlet runs.
  - Verify JSON output schema, zero leaked background processes, and clean exit codes.
  - Register integration test in `meson.build`.
