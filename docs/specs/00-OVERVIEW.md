# Virmux — System Design Specification
## 00. Overview & Core Architecture

### 1. Executive Summary

`virmux` (Virtual Resource Multiplexer) is a high-density systems runtime and resource supervisor written in C17 for Linux and WSL (Windows Subsystem for Linux).

While traditional terminal multiplexers like `tmux` multiplex screen real estate and input streams, `virmux` multiplexes physical hardware resources: CPU cores, cache hierarchies, physical DRAM, swap, and disk I/O. It enables developers and automated agent swarms to run 10–30 concurrent, heavy workloads (e.g., headless browser test suites, C++/Rust compilations, model training, Docker builds, Blender rendering) on a single workstation or laptop without system lag, scheduler thrashing, or Out-Of-Memory (OOM) crashes.

```
+-----------------------------------------------------------------------+
|                       virmux CLI & TUI Interface                      |
|       virmux run <cmd>  |  virmux sh  |  virmux top  |  virmux attach  |
+-----------------------------------v-----------------------------------+
|                           virmux Supervisor                           |
|  +---------------------+ +--------------------+ +------------------+  |
|  |  Workspace Engine   | |   Scheduler Gate   | |  Memory Manager  |  |
|  | OverlayFS + tmpfs   | |  Admission Control | |  KSM + zram      |  |
|  | CAS Build Cache     | |  cpuset Shielding  | |  process_madvise |  |
|  +---------------------+ +--------------------+ +------------------+  |
|  +---------------------+ +--------------------+ +------------------+  |
|  | Interceptor Engine  | | Lifecycle Manager  | | Telemetry & TUI  |  |
|  | nproc / mem spoof   | | OOM Shielding      | | Socket IPC       |  |
|  | Browser Trimming    | | CRIU Checkpoint    | | Live Metrics     |  |
|  +---------------------+ +--------------------+ +------------------+  |
+-----------------------------------v-----------------------------------+
|                        Linux Kernel Primitives                        |
|  cgroups v2  |  namespaces  |  process_madvise  |  KSM  |  OverlayFS   |
+-----------------------------------------------------------------------+
```

---

### 2. The Core Problem: Why Systems Collapse Under Multi-Tenant Local Workloads

Modern development tools are engineered to be greedy. When a tool initializes, it inspects host hardware capacity:
1. **Thread Spawning:** `nproc`, `os.cpu_count()`, and `sysconf(_SC_NPROCESSORS_ONLN)` report the total logical core count (e.g., 12 cores). Test runners (`playwright`, `pytest -n auto`, `vitest`), build systems (`cmake`, `ninja`, `make`, `cargo`), and runtime thread pools (`libgomp`, `OpenBLAS`, `MKL`, PyTorch) spawn 12 worker processes or threads by default.
2. **Buffer Allocation:** Runtimes inspect `MemTotal` from `/proc/meminfo` and tune garbage collection thresholds and internal allocation arenas for the entire machine.
3. **GPU Domination:** Frameworks like PyTorch preallocate up to 90% of total VRAM through the CUDA caching allocator on startup.

When 10–20 independent terminal sessions or autonomous agents run simultaneously on a 12-core, 16GB machine:
- **120+ active worker threads** fight for 12 physical execution units.
- **Cache Invalidation:** Constant context-switching evicts L1/L2/L3 caches, dropping CPU instruction throughput by up to 80%.
- **Memory Thrashing:** Total memory demand surges to 30–50 GB, overflowing RAM and saturating swap. In WSL, this manifests as total host lockup under the `vmmem` process.
- **Priority Inversion:** Background compilations starve interactive shells and the desktop environment of CPU slices.

---

### 3. The Solution: Hardware Multiplexing via 4 Core Pillars

`virmux` solves this by introducing four coordinated subsystems:

| Subsystem | Key Mechanisms | Outcome |
|---|---|---|
| **Memory Packing** | KSM deduplication, `zram` compression, `process_madvise(MADV_PAGEOUT)`, `posix_fadvise` cache flush | Multiplies effective RAM by 3×–4×; eliminates swap thrashing |
| **CPU Multiplexing** | `cpuset.cpus` core shielding, Admission Control token bucket, `cgroup.freeze` on I/O wait, `SCHED_IDLE` | Eliminates scheduler thrashing; keeps interactive shell 100% fluid |
| **Storage Virtualization** | Ephemeral OverlayFS workspaces, shared tmpfs, content-addressable build cache (CAS) | 12 full workspaces share 1 disk footprint; zero duplicate compilations |
| **Runtime Virtualization** | `nproc` & `/proc/meminfo` spoofing, headless browser optimization flags, NVIDIA MPS sharing | Forces greedy runtimes to behave politely without changing source code |

---

### 4. Technical Invariants & Constraints

1. **Non-Invasive to Source Code:** Target applications (e.g., Playwright scripts, CMakeLists, PyTorch models) must run completely unmodified.
2. **Pure C17 Core:** The core runtime, interceptors, and supervisor are written in standard C17 with zero heavy runtime dependencies.
3. **Unprivileged Operation with Graceful Elevation:** Monitoring, spoofing, and workspace virtualization work in unprivileged user namespaces. Advanced cgroup v2 controllers and system-wide KSM utilize delegated privileges or degrade gracefully.
4. **Deterministic Fault Isolation:** A failure, segfault, or OOM event in Session A must never terminate or corrupt Session B or the host shell.
5. **Zero Idle Overhead:** When a session is idle or awaiting network response, its CPU footprint must be strictly 0.00% via kernel freezing.

---

### 5. Architectural Directory Layout

```
virmux/
├── docs/
│   └── specs/
│       ├── 00-OVERVIEW.md
│       ├── 01-MEMORY-SUBSYSTEM.md
│       ├── 02-CPU-SCHEDULING-SUBSYSTEM.md
│       ├── 03-STORAGE-WORKSPACE-SUBSYSTEM.md
│       ├── 04-RUNTIME-VIRTUALIZATION.md
│       ├── 05-FAULT-ISOLATION-LIFECYCLE.md
│       ├── 06-CLI-AND-TUI-SPEC.md
│       └── 07-TESTING-AND-BENCHMARKS.md
├── include/
│   └── virmux/
│       ├── virmux.h
│       ├── memory.h
│       ├── sched.h
│       ├── storage.h
│       ├── intercept.h
│       ├── session.h
│       └── ipc.h
├── src/
│   ├── core/
│   ├── memory/
│   ├── sched/
│   ├── storage/
│   ├── intercept/
│   └── tui/
├── cli/
│   └── main.c
├── tests/
└── meson.build
```
