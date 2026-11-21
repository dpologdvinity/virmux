# Changelog

All notable changes to `virmux` will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added

- CI: lint workflow (pinned clang-format/clang-tidy, markdownlint, actionlint), arm64 and newest-toolchain (Fedora) builds, coverage summary, TSan, CodeQL, libFuzzer fuzzing on PRs and nightly, and attested release tarballs on version tags.
- libFuzzer harnesses for IPC frames, daemon requests, session-list JSON, bench report JSON and size arguments (`-Dfuzzing=true`).

### Fixed

- Synthetic `/proc/meminfo` is only exported to sandboxes when fully written.
- Backgrounded daemon exits if it cannot report readiness, and treats a failed `chdir("/")` as an init failure.
- Session-list JSON parsing no longer reads past the end of its input, and rejects overflowing timestamps.
- Bench comparison JSON: range-checked integers, string-aware brace matching and escaped `mode` values.
- Page-cache eviction tests are skipped instead of failing when `/tmp` is tmpfs.

## [0.1.0] - 2026-11-14

### Added

- **Core Architecture & Virtualization Engine (Phase 1):**
  - Meson + Ninja C++20/C17 build system with strict warning flags (`-Wall -Wextra -Wpedantic -Werror`).
  - Kernel prerequisite diagnostic utility (`scripts/doctor.sh`).
  - Scoped RAII resource wrappers (`ScopedFd`, `ScopedMount`, `ScopedCgroup`, `ScopedThread`).
  - Dynamic `LD_PRELOAD` interceptor (`libvirmux_intercept.so`) spoofing `_SC_NPROCESSORS_ONLN` and `/proc/meminfo`.
  - Browser memory trimming interceptor injecting single-process and renderer limits into Chromium/Node.
  - Ephemeral user/mount namespace OverlayFS workspaces and in-memory tmpfs scratch directories.
  - CLI subcommand scaffolding (`virmux run`, `virmux stats`, `virmux pack`, `virmux cas`, `virmux top`).

- **Memory Packing & Sched Subsystem (Phase 2):**
  - Kernel Samepage Merging (KSM) auto-tuning controller with sysfs deduplication telemetry.
  - Anonymous memory compaction into zram/swap using `process_madvise(MADV_PAGEOUT)`.
  - Content-Addressable Storage (CAS) with 2-level directory sharding, SHA-256 validation, and LRU cache eviction.
  - Token-bucket concurrency gate (`AdmissionGate`) throttling heavy bursts.
  - CPU shielding pinning interactive tasks to Cores 0–1 while restricting workloads via cgroups v2 `cpuset.cpus`.

- **Supervisor Daemon & Binary IPC Protocol (Phase 3):**
  - Unix domain socket server with non-blocking polling and stale socket detection (`virmux daemon`).
  - Packed 12-byte binary TLV protocol header (`VMUX`) with fragmented stream reassembly.
  - Thread-safe session registry (`SessionRegistry`) tracking process lifecycles and procfs telemetry.
  - Lifecycle management subcommands (`virmux list`, `virmux freeze`, `virmux thaw`, `virmux kill`).

- **Real-Time Interactive TUI Dashboard (Phase 4):**
  - High-performance, zero-flicker Ncurses dashboard (`virmux top`) with sub-0.5% CPU overhead.
  - Visual layout rendering host load, CPU shield vs worker partition, KSM deduplication, and zram compression.
  - Interactive keyboard navigation (`j`/`k`, `Up`/`Down`) with hotkey actions (`Space` freeze/thaw, `K` kill, `q` quit).
  - Headless automated snapshot mode (`virmux top --headless`) for CI/CD and scripted validation.

- **Fault Isolation, OOM Shielding & Auto-Hibernation (Phase 5):**
  - Deterministic OOM score priority hierarchy (`oom_score_adj = -900` daemon protection, `+800` sacrificial worker).
  - Fork-bomb containment clamping cgroups v2 `pids.max` with fallback `RLIMIT_NPROC` scaling.
  - Inactivity auto-freeze engine monitoring CPU tick deltas and cgroups v2 `cpu.stat` usage (>500ms inactivity).
  - Session checkpointing and hibernation (`virmux checkpoint`, `virmux restore`) using CRIU with deep-freeze fallback.

- **12-Terminal Gauntlet Benchmark Suite (Phase 6):**
  - Synthetic workload generators simulating Browser (DOM/JS), Compiler (CPU burn), Model (PyTorch tensor loop), and Build (Docker packaging).
  - Monotonic PTY input latency probe (`InputLatencyProbe`) measuring keystroke round-trip latency under synthetic load.
  - Comparative benchmark runner (`BenchmarkRunner`) evaluating unmanaged baseline vs virmux-managed concurrency.
  - CLI benchmark commands (`virmux bench run`, `virmux bench report`) generating formatted tables and JSON reports.

- **Production CI/CD Pipeline & GitHub Automation (Phase 7):**
  - Multi-platform GitHub Actions matrix for Ubuntu 24.04 and Ubuntu 22.04 with GCC and Clang (`.github/workflows/ci.yml`).
  - AddressSanitizer and UndefinedBehaviorSanitizer workflow (`.github/workflows/sanitizers.yml`).
  - Automated code formatting check (`clang-format --dry-run --Werror`).
  - Repository scaffolding files (`.editorconfig`, `SECURITY.md`, `CHANGELOG.md`, Dependabot, issue and PR templates).
