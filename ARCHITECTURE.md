# Architecture & System Design — `virmux`

## High-Level Architecture

`virmux` is decomposed into an unprivileged userspace CLI, an interceptor dynamic library (`libvirmux_intercept.so`), and a lightweight supervisor daemon communicating via Unix Domain Sockets:

```text
+-------------------------------------------------------------------------+
|                                virmux CLI                               |
|        [cli/main.cpp] - Arg parsing, TUI dashboard, IPC Client          |
+------------------------------------+------------------------------------+
                                     |  Unix Domain Socket
                                     |  (/run/user/<UID>/virmux.sock)
                                     |  Binary TLV Header: "VMUX"
+------------------------------------v------------------------------------+
|                         virmux Supervisor Daemon                        |
|                         [src/daemon/daemon.cpp]                         |
|                                                                         |
|  +--------------------+  +--------------------+  +-------------------+  |
|  |  Session Registry  |  |   Admission Gate   |  |   Memory Engine   |  |
|  | [src/daemon/sess]  |  | [src/sched/gate]   |  | [src/memory/ksm]  |  |
|  | Lifecycle, Procfs  |  | Token Bucket FIFO  |  | process_madvise   |  |
|  +--------------------+  +--------------------+  +-------------------+  |
|  +--------------------+  +--------------------+  +-------------------+  |
|  |  Workspace Engine  |  |    IPC Dispatch    |  | Telemetry Engine  |  |
|  | [src/storage/ovl]  |  | [src/ipc/ipc]      |  | [src/memory/telem]|  |
|  | OverlayFS + Scratch|  | TLV Stream Framer  |  | Host, KSM & zram  |  |
|  +--------------------+  +--------------------+  +-------------------+  |
+------------------------------------+------------------------------------+
                                     |
                                     | fork() / execvp()
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

## Subsystem Boundaries

1. **`src/daemon/` (Supervisor & Session Lifecycle):**
   - Implements `SupervisorDaemon` listening on Unix domain sockets (`/run/user/<UID>/virmux.sock` or fallback `/tmp/virmux-<UID>.sock`).
   - Stale socket detection: uses non-blocking probe to avoid clobbering active daemons; cleans stale sockets on `ECONNREFUSED` or `ENOENT`.
   - Socket permissions enforced to `0700` using `umask(0077)`.
   - Maintains thread-safe `SessionRegistry` with states: `INITIALIZING`, `RUNNING_BURST`, `RUNNING_NORMAL`, `WAITING_IO`, `FROZEN`, `CHECKPOINTED`, `DEAD`.
   - Inactivity auto-freeze engine: monitors CPU tick deltas and cgroups v2 `cpu.stat` aggregated usage; automatically freezes idle tasks (>500ms inactivity) and invokes `process_madvise(MADV_PAGEOUT)` compaction.
   - Gathers per-process telemetry from `/proc/<pid>/statm`, `/proc/<pid>/status`, and `/proc/<pid>/smaps_rollup`.
   - Manages process tree termination via `cgroup.kill` and recursive procfs task scanning with auto-thaw so frozen tasks process termination signals.
   - Automatically prunes dead sessions after a 5-second retention window.

2. **`src/core/checkpoint.cpp` (Checkpoint & Hibernation Subsystem):**
   - Implements `CheckpointManager` resolving storage paths (`$VIRMUX_CHECKPOINT_DIR`, `/run/user/<UID>/virmux/checkpoints`, or `/tmp/virmux-<UID>/checkpoints`) with `0700` directory hardening.
   - Probes CRIU binary and capabilities (`criu check`) via direct `fork()` + `execv()` without shell injection risks.
   - Executes privileged CRIU dump/restore (`criu dump --shell-job`, `criu restore -d`) or unprivileged deep-freeze fallback (cgroup freeze + `process_madvise(MADV_PAGEOUT)`).
   - Restores checkpoints on daemon restart by scanning checkpoint storage on startup.

3. **`src/core/sandbox.cpp` (Fault Isolation & Blast Radius):**
   - Deterministic OOM score adjuster (`set_process_oom_score`): writes `-900` to supervisor daemon (shielded from kernel OOM) and `+800` to worker children before `execvp` (sacrificial leaf tasks).
   - Fork-bomb containment: clamps cgroups v2 `pids.max` on slice with defensive `setrlimit(RLIMIT_NPROC)` fallback when cgroups are unavailable.

4. **`src/ipc/` (Binary Framing & Transport):**
   - Packed 12-byte binary TLV header (`IpcHeader`):
     - `magic`: 4-byte magic `0x564D5558` (`"VMUX"`).
     - `version`: Protocol version `1`.
     - `msg_type`: Request and response action identifiers.
     - `payload_len`: 32-bit payload length (bounded to 16 MiB sanity limit).
   - Non-blocking socket I/O using monotonic poll deadlines with fragmented stream reassembly.

5. **`src/tui/` (Terminal Dashboard `virmux top`):**
   - High-performance, zero-flicker terminal dashboard powered by Ncurses (`libncursesw`).
   - Maintains sub-0.5% CPU overhead during continuous 500ms refresh cycles.
   - Live system telemetry display: host load average, CPU shield vs worker partition, KSM deduplication, zram compression ratio.
   - Interactive keyboard navigation (`j`/`k`, `Up`/`Down`) with lifecycle controls (`Space` freeze/thaw, `K` kill, `q` quit).
   - Clean terminal restoration on `SIGINT` / `SIGTERM` via RAII and async-signal-safe flags.

6. **`src/memory/` (Memory Packing & Compaction):**
   - Direct kernel interface to tune KSM (`/sys/kernel/mm/ksm/pages_to_scan`, `sleep_millisecs`, `run`).
   - Compacts anonymous memory into swap/zram via `process_madvise(MADV_PAGEOUT)`.
   - Real-time telemetry reader for host memory, `/proc/meminfo`, KSM statistics, and `/sys/block/zram*` metrics.

7. **`src/sched/` (CPU Slicing & Admission Gate):**
   - Shields interactive cores (Cores 0–1) by pinning managed tasks to worker cores via cgroups v2 `cpuset.cpus`.
   - Enforces memory soft ceiling via `memory.high`.
   - Token-bucket concurrency gate (`AdmissionGate`) throttling heavy bursts to prevent cache thrashing.

8. **`src/storage/` (Workspace Isolation & CAS Subsystem):**
   - Unprivileged ephemeral OverlayFS controller using user and mount namespaces (`CLONE_NEWUSER | CLONE_NEWNS`).
   - Automatic secondary bind mount shadowing (`merged_dir` over `lower_dir`) preventing host lowerdir leakage.
   - Content-Addressable Storage (CAS) with 2-level directory sharding (`objects/ab/cd/<hash>`), atomic staging, RFC 6234 SHA-256 validation, and true LRU eviction on `prune()`.
   - Temporary scratch space redirection (`TMPDIR`, `TEMP`, `TMP`) with page cache eviction (`cleanse_path`).

9. **`src/intercept/` (`libvirmux_intercept.so`):**
   - Dynamic library injected via `LD_PRELOAD`.
   - Intercepts `sysconf(_SC_NPROCESSORS_ONLN)` and `/proc/meminfo` to spoof available hardware resources.
   - Intercepts Chromium/Node process launches to inject low-memory flags (`--single-process`, `--disable-gpu`, `--renderer-process-limit=1`).

10. **`src/bench/` (Stress Benchmark & Gauntlet Engine):**
    - Synthetic workload generators simulating real-world concurrency profiles: `BROWSER` (page allocation + dirtying + sleep), `COMPILER` (matrix multiplication CPU burn), `MODEL` (large buffer tensor allocation + dirtying), `BUILD` (ephemeral disk I/O file generation + deletion).
    - Monotonic PTY input latency probe (`InputLatencyProbe`) measuring interactive keystroke round-trip latency under synthetic load via non-canonical raw slave PTY.
    - System telemetry sampler (`TelemetrySampler`) tracking `/proc/meminfo` (MemTotal, MemAvailable, SwapUsed) and KSM deduplication deltas.
    - Comparative gauntlet runner (`BenchmarkRunner`) executing baseline unmanaged concurrent tasks vs virmux-managed tasks (with `AdmissionGate`, Core 0-1 shielding, and OOM score adjustment) across a 12-terminal matrix.
    - JSON export and human-readable comparative reporting (`format_comparison_table`, `format_comparison_json`).

---

## Technology Stack Rationale

- **C++20 (Core Engine & Daemon):** Strong RAII semantics (`ScopedFd`, `ScopedMount`, `ScopedCgroup`, `ScopedThread`) guarantee leak-free kernel resource cleanup. Concurrency primitives (`std::mutex`, `std::jthread`, `std::atomic`) ensure thread-safe session tracking.
- **C ABI (`extern "C"`) for `libvirmux_intercept.so`:** The `LD_PRELOAD` interceptor library compiles with C linkage so it can transparently intercept syscalls across any executable on the system (C, C++, Rust, Go, Python, Node).
- **Linux cgroups v2 & Procfs:** Native unified hierarchy provides atomic resource control across memory, CPU, and process lifecycles without external dependencies.
- **Unprivileged OverlayFS:** Standard kernel filesystem driver providing instant, zero-copy copy-on-write workspaces inside user namespaces without requiring root permissions.
- **Meson + Ninja:** Fast, modern build system with strict warning enforcement (`-Wall -Wextra -Wpedantic -Werror`).
