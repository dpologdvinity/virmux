# virmux

> **Virtual Resource Multiplexer in C++20/C for Linux & WSL2.**  
> High-Density Memory Deduplication, CPU Slicing, Workspace Isolation, and Admission Scheduling for Heavy Concurrent Workloads.

[![CI](https://github.com/dpologdvinity/virmux/actions/workflows/ci.yml/badge.svg)](https://github.com/dpologdvinity/virmux/actions/workflows/ci.yml)
[![Sanitizers](https://github.com/dpologdvinity/virmux/actions/workflows/sanitizers.yml/badge.svg)](https://github.com/dpologdvinity/virmux/actions/workflows/sanitizers.yml)
[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)
[![Standard: C++20 / C17](https://img.shields.io/badge/Standard-C%2B%2B20%20%7C%20C17-blue.svg)](https://en.wikipedia.org/wiki/C%2B%2B20)
[![Platform: Linux / WSL2](https://img.shields.io/badge/Platform-Linux%20%7C%20WSL2-orange.svg)](https://learn.microsoft.com/en-us/windows/wsl/)
[![Build: Meson + Ninja](https://img.shields.io/badge/Build-Meson%20%7C%20Ninja-green.svg)](https://mesonbuild.com/)

---

### The Problem

Running multiple terminal sessions or autonomous AI agent swarms simultaneously (e.g. running Playwright tests, compiling C++/Rust, training neural models in PyTorch, building Docker images) quickly brings even high-end laptops and workstations to a crawl:
- Test frameworks and compilers greedily spawn 12+ worker threads each, invalidating CPU caches and starving the host OS.
- Dozens of Chromium or Python runtimes duplicate gigabytes of identical memory pages in RAM, rapidly overflowing physical memory into swap thrashing.
- In WSL2, host memory saturation (`vmmem`) locks up the entire Windows desktop and triggers harsh Linux OOM kills.

### The Solution: `virmux`

`virmux` acts as an in-kernel resource multiplexer and hypervisor. While tools like `tmux` multiplex terminal screens, `virmux` multiplexes the underlying hardware:

1. **Memory Multiplexing:** Deduplicates identical memory pages across processes via **KSM (Kernel Samepage Merging)**, compresses inactive heaps into RAM with **`zram`**, and actively reclaims memory using **`process_madvise(MADV_PAGEOUT)`**.
2. **CPU Shielding & Admission Gate:** Pins Cores 0–1 strictly for your interactive shell and editor (`cpuset.cpus`), while queuing heavy bursts (compilers, test runners) through an admission token-bucket to eliminate CPU cache thrashing.
3. **Fault Isolation & Blast Radius Containment:** Configures deterministic OOM priority hierarchy (`oom_score_adj = -900` daemon protection, `+800` sacrificial worker) and fork-bomb limits (`pids.max` with `RLIMIT_NPROC` fallback) so unruly child processes cannot bring down siblings or the host OS.
4. **Transparent Inactivity Auto-Freeze:** Background telemetry continuously monitors CPU tick deltas and cgroups v2 `cpu.stat`; idle sessions (>500ms inactivity) are automatically frozen via cgroups or `SIGSTOP` with heap memory paged out via `process_madvise(MADV_PAGEOUT)`.
5. **Session Checkpointing & Hibernation:** Save and restore running sessions (`virmux checkpoint` / `virmux restore`) using CRIU dump/restore when elevated capabilities exist, falling back to deep freeze + pageout in standard unprivileged environments.
6. **Copy-on-Write Ephemeral Workspaces:** Provisions unprivileged **OverlayFS** workspaces with in-memory `tmpfs` scratch directories—dozens of agent workspaces share a single repository on disk with zero write leakage.
7. **Content-Addressable Storage (CAS):** 2-level directory-sharded artifact cache (`objects/ab/cd/<hash>`) with atomic staging, RFC 6234 SHA-256 validation, and true LRU eviction.
8. **Transparent Hardware Spoofing:** Intercepts `nproc` and `/proc/meminfo` via `LD_PRELOAD`, tricking greedy runtimes into behaving politely without changing a single line of project code.
9. **Supervisor Daemon & IPC:** Lightweight Unix domain socket server with binary TLV framing (`VMUX`), background telemetry, process tree signaling, and lifecycle state management.
10. **Interactive TUI Dashboard:** Real-time ncurses dashboard (`virmux top`) showing active sessions, memory saved, CPU topology, and hotkey lifecycle controls (`Space` freeze/thaw, `K` kill, `q` quit).

---

### Quickstart

#### Prerequisites
- Linux kernel 5.10+ (standard in Ubuntu 22.04+, WSL2, Debian 12+)
- `meson`, `ninja`, `gcc` (>= 13) or `clang`
- `libncursesw5-dev`

```bash
# Clone the repository
git clone https://github.com/dpologdvinity/virmux.git
cd virmux

# Build with Meson
meson setup build
ninja -C build

# Run unit and integration test suite (46 tests)
ninja -C build test
```

---

### Usage & Subcommands

#### 1. Execute Command in Managed Sandbox (`virmux run`)
```bash
# Runs command in 2-core, 2GB memory ceiling sandbox with hardware virtualization
virmux run --cores 2 --mem 2G -- npm test

# Runs command inside a throwaway Copy-on-Write ephemeral workspace (OverlayFS)
virmux run --ephemeral --workspace ~/git/my-repo -- npx playwright test

# Enqueue as heavy compute burst through admission gate
virmux run --heavy --cores 4 --mem 4G -- cargo build --release
```

#### 2. Start Supervisor Daemon (`virmux daemon`)
```bash
# Start background daemon (uses readiness pipe for instant synchronization)
virmux daemon

# Run daemon in foreground for debugging
virmux daemon --foreground
```

#### 3. Real-Time Interactive TUI Dashboard (`virmux top`)
```bash
# Launch interactive ncurses dashboard (sub-0.5% CPU budget)
virmux top

# Render one-shot headless snapshot to stdout
virmux top --headless
```

**TUI Hotkeys:**
- `j` / `k` or `Up` / `Down`: Navigate active session table
- `Space`: Freeze or thaw selected session
- `K`: Kill selected session process tree
- `q`: Exit dashboard

#### 4. Manage Session Lifecycles (`virmux list`, `freeze`, `thaw`, `kill`)
```bash
# List all active managed sessions in formatted table
virmux list

# Output session state as machine-readable JSON
virmux list --json

# Freeze and thaw sessions via cgroups v2 / SIGSTOP fallback
virmux freeze 1
virmux thaw 1

# Terminate session process tree and child tasks
virmux kill 1
```

#### 5. Session Checkpointing & Hibernation (`virmux checkpoint`, `virmux restore`)
```bash
# Checkpoint running session to disk (CRIU dump or deep-freeze pageout fallback)
virmux checkpoint 1

# Restore checkpointed session back to running state
virmux restore 1
```

#### 6. Fault Isolation & Auto-Freeze Options
```bash
# Start daemon with automatic inactivity freeze (defaults to 500ms idle threshold)
virmux daemon --auto-freeze --idle-ms 500

# Execute sandboxed workload with fork-bomb ceiling (pids.max and RLIMIT_NPROC fallback)
virmux run --max-pids 64 -- python3 worker.py
```

#### 7. Telemetry & Compaction (`virmux stats`, `virmux pack`)
```bash
# Inspect host memory, KSM deduplication, and zram compression telemetry
virmux stats

# Output telemetry as JSON
virmux stats --json

# Proactively compact anonymous memory into swap/zram via process_madvise
virmux pack
virmux pack 1234 5678
```

#### 8. Content-Addressable Storage (`virmux cas`)
```bash
# Inspect CAS cache size, hit/miss ratios, and entry counts
virmux cas stats

# Prune CAS cache down to a maximum byte threshold via LRU
virmux cas clean --max-bytes 500M
```

#### 9. Stress Benchmarking (`virmux bench run`, `virmux bench report`)
```bash
# Run 12-task baseline vs managed gauntlet stress benchmark
virmux bench run --quick --tasks 12 --output report.json

# Display formatted comparison report from JSON file or stdin
virmux bench report report.json
cat report.json | virmux bench report
```

---

### Architecture & Detailed Specs

Comprehensive technical design specifications are available in [`docs/specs/`](docs/specs/):
- [00. Overview & Core Architecture](docs/specs/00-OVERVIEW.md)
- [01. Memory Multiplexing Subsystem](docs/specs/01-MEMORY-SUBSYSTEM.md)
- [02. CPU & Concurrency Scheduling Subsystem](docs/specs/02-CPU-SCHEDULING-SUBSYSTEM.md)
- [03. Storage & Workspace Subsystem](docs/specs/03-STORAGE-WORKSPACE-SUBSYSTEM.md)
- [04. Runtime Virtualization & Spoofing](docs/specs/04-RUNTIME-VIRTUALIZATION.md)
- [05. Fault Isolation & Lifecycle Management](docs/specs/05-FAULT-ISOLATION-LIFECYCLE.md)
- [06. CLI & Interactive TUI Specification](docs/specs/06-CLI-AND-TUI-SPEC.md)
- [07. Testing, Verification & Benchmark Strategy](docs/specs/07-TESTING-AND-BENCHMARKS.md)
- [08. Tooling, Diagnostics & Dev Environment](docs/specs/08-TOOLING-AND-DEV-ENVIRONMENT.md)

---

### License

MIT License. See [LICENSE](LICENSE) for details.
