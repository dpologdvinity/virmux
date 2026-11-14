# virmux

> **Virtual Resource Multiplexer in C++20/C for Linux & WSL.**  
> High-Density Memory Deduplication, CPU Slicing, Workspace Isolation, and Admission Scheduling for Heavy Concurrent Workloads.

[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)
[![Standard: C++20 / C17](https://img.shields.io/badge/Standard-C%2B%2B20%20%7C%20C17-blue.svg)](https://en.wikipedia.org/wiki/C%2B%2B20)
[![Platform: Linux / WSL2](https://img.shields.io/badge/Platform-Linux%20%7C%20WSL2-orange.svg)](https://learn.microsoft.com/en-us/windows/wsl/)

---

### The Problem

Running multiple terminal sessions or autonomous AI agent swarms simultaneously (e.g. running Playwright tests, compiling C++/Rust, training neural models in PyTorch, building Docker images) quickly brings even high-end laptops and workstations to a crawl:
- Test frameworks and compilers greedily spawn 12+ worker threads each, invalidating CPU caches and starving the host OS.
- Dozens of Chromium or Python runtimes duplicate gigabytes of identical memory pages in RAM, rapidly overflowing physical memory into swap thrashing.
- In WSL2, host memory saturation (`vmmem`) locks up the entire Windows desktop.

### The Solution: `virmux`

`virmux` acts as an in-kernel resource multiplexer and hypervisor. While tools like `tmux` multiplex terminal screens, `virmux` multiplexes the underlying hardware:

1. **Memory Multiplexing:** Deduplicates identical memory pages across processes via **KSM (Kernel Samepage Merging)**, compresses inactive heaps into RAM with **`zram`**, and actively reclaims memory using **`process_madvise(MADV_PAGEOUT)`**.
2. **CPU Shielding & Admission Gate:** Pins Cores 0–1 strictly for your interactive shell and editor (`cpuset.cpus`), while queuing heavy bursts (compilers, test runners) through an admission token-bucket to eliminate CPU cache thrashing.
3. **Copy-on-Write Workspaces:** Provisions ephemeral **OverlayFS** workspaces with in-memory `tmpfs` scratch directories—12 isolated agent workspaces share a single repository on disk with zero duplication.
4. **Transparent Hardware Spoofing:** Intercepts `nproc` and `/proc/meminfo` via `LD_PRELOAD`, tricking greedy runtimes into behaving politely without changing a single line of project code.
5. **Interactive TUI Dashboard:** Real-time ncurses dashboard (`virmux top`) showing active sessions, memory saved, queued tasks, and instant pause/resume controls.

---

### Quickstart

#### Prerequisites
- Linux kernel 5.10+ (standard in Ubuntu 22.04+, WSL2, Debian 12+)
- `meson`, `ninja`, `gcc` or `clang`
- `libncurses-dev`

```bash
# Clone the repository
git clone https://github.com/dpologdvinity/virmux.git
cd virmux

# Build with Meson
meson setup build
ninja -C build

# Run test suite
ninja -C build test
```

---

### Usage

#### 1. Run a Command in a Managed Sandbox
```bash
# Runs Playwright inside a 2-virtual-core, 2GB soft-ceiling sandbox
virmux run --cores 2 --mem 2G -- npx playwright test
```

#### 2. Spawn a High-Density Interactive Shell
```bash
# Opens an interactive shell where all spawned commands inherit virmux protections
virmux sh --workspace ~/git/daydaze
```

#### 3. Monitor All Sessions via TUI
```bash
virmux top
```

---

### Architecture & Detailed Specs

Comprehensive technical design specifications are available in the [`docs/specs/`](docs/specs/) directory:
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
