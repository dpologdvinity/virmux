# Architecture & System Design — `virmux`

### High-Level Architecture

`virmux` is decomposed into an unprivileged userspace CLI, an interceptor dynamic library, and a lightweight supervisor daemon communicating via Unix Domain Sockets:

```
+-------------------------------------------------------------------------+
|                                virmux CLI                               |
|        [cli/main.c] - Arg parsing, TUI interface, IPC Client           |
+------------------------------------+------------------------------------+
                                     |  Unix Domain Socket
                                     |  (/run/user/<UID>/virmux.sock)
+------------------------------------v------------------------------------+
|                         virmux Supervisor Daemon                        |
|                                                                         |
|  +--------------------+  +--------------------+  +-------------------+  |
|  |  Session Manager   |  |   Admission Gate   |  |   Memory Engine   |  |
|  | [src/core/session] |  | [src/sched/gate.c] |  | [src/memory/ksm]  |  |
|  | State machine, PTY |  | Token bucket FIFO  |  | process_madvise   |  |
|  +--------------------+  +--------------------+  +-------------------+  |
|  +--------------------+  +--------------------+  +-------------------+  |
|  |  Workspace Engine  |  |    IPC Dispatch    |  | Telemetry Aggreg  |  |
|  | [src/storage/ovl]  |  | [src/core/ipc.c]   |  | [src/core/stats]  |  |
|  | OverlayFS + tmpfs  |  | Non-blocking epoll |  | KSM & cgroup stat |  |
|  +--------------------+  +--------------------+  +-------------------+  |
+------------------------------------+------------------------------------+
                                     |
                                     | fork() / execve()
+------------------------------------v------------------------------------+
|                         Target Sandboxed Process                        |
|                     (Playwright, GCC, PyTorch, Node)                    |
|                                                                         |
|            +-----------------------------------------------+            |
|            |      libvirmux_intercept.so (LD_PRELOAD)      |            |
|            | Intercepts: sysconf(), open(), execve()       |            |
|            | Spoofs: nproc, meminfo, slim browser flags    |            |
|            +-----------------------------------------------+            |
+-------------------------------------------------------------------------+
```

---

### Module Boundaries

1. **`src/core/` (Supervisor & State Engine):**
   - Manages session lifecycle (`INITIALIZING`, `RUNNING_BURST`, `FROZEN`, etc.).
   - Holds the master non-blocking epoll loop handling IPC client connections and PTY streams.
   - Reaps child processes cleanly to prevent zombie leakage.

2. **`src/memory/` (Memory Packing Engine):**
   - Tunes host KSM via `/sys/kernel/mm/ksm/`.
   - Dispatches `process_madvise(MADV_PAGEOUT)` when sessions become idle.
   - Monitors cgroup memory metrics (`memory.current`, `memory.high`).

3. **`src/sched/` (CPU & Admission Controller):**
   - Enforces interactive core shielding via `/sys/fs/cgroup/virmux.slice/cpuset.cpus`.
   - Manages token-bucket concurrency gate for compute bursts.
   - Controls `cgroup.freeze` interfaces.

4. **`src/storage/` (Workspace Isolation):**
   - Sets up ephemeral OverlayFS mounts combining read-only lower directories with tmpfs upper scratch spaces.
   - Manages Content-Addressable Storage (CAS) build artifacts.

5. **`src/intercept/` (`libvirmux_intercept.so`):**
   - Dynamic library injected via `LD_PRELOAD`.
   - Traps resource discovery syscalls and transparently injects optimized flags into child processes.

6. **`src/tui/` (Terminal Dashboard):**
   - Ncurses interactive UI displaying real-time metrics with zero CPU overhead.

---

### Technology Stack Rationale

- **C17 (Language of Choice):** Low-level access to Linux kernel syscalls (`process_madvise`, `clone3`, `epoll`, `mount`, `posix_fadvise`) requires direct C ABI interaction. Zero garbage collection pauses ensures `virmux` never creates latency for the processes it monitors.
- **cgroups v2:** Modern Linux unified hierarchy provides atomic resource control across memory, CPU, and I/O.
- **OverlayFS:** Standard kernel filesystem driver providing instant, zero-copy copy-on-write workspaces without external daemon dependencies.
- **Meson + Ninja:** Fast, modern C build system with clean cross-platform dependency tracking.
