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
                                     | clone() / execvpe()
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
   - Request payloads are decoded by the pure, I/O-free `parse_daemon_request()` (`src/daemon/protocol.cpp`, `virmux/daemon_protocol.hpp`) before dispatch, so the untrusted-input parser is unit-tested and fuzzed (`tests/fuzz/`) in isolation from the registry.

2. **`src/core/checkpoint.cpp` (Checkpoint & Hibernation Subsystem):**
   - Implements `CheckpointManager` resolving storage paths (`$VIRMUX_CHECKPOINT_DIR`, `/run/user/<UID>/virmux/checkpoints`, or `/tmp/virmux-<UID>/checkpoints`) with `0700` directory hardening.
   - Probes CRIU binary and capabilities (`criu check`) via direct `fork()` + `execv()` without shell injection risks.
   - Executes privileged CRIU dump/restore (`criu dump --shell-job`, `criu restore -d`) or unprivileged deep-freeze fallback (cgroup freeze + `process_madvise(MADV_PAGEOUT)`).
   - Restores checkpoints on daemon restart by scanning checkpoint storage on startup.

3. **`src/core/sandbox.cpp` (Fault Isolation & Blast Radius):**
   - Deterministic OOM score adjuster (`set_process_oom_score`): writes `-900` to supervisor daemon (shielded from kernel OOM) and `+800` to worker children before `execvpe` (sacrificial leaf tasks).
   - Child launch: the parent prepares envp, chdir targets, cgroup and rlimit values, then starts the child with `clone()` (with `CLONE_NEWUSER | CLONE_NEWNS` for ephemeral runs, so namespaces are created atomically instead of via `unshare()` in a possibly multi-threaded child). The child only makes async-signal-safe syscalls before `execvpe`.
   - Per-run cgroup without root: `find_cgroup_delegation()` (`src/core/cgroup.cpp`) reads the `0::` line of `/proc/self/cgroup` and walks up to the nearest writable `user@<uid>.service` (systemd delegates `cpu memory pids` there), else the highest writable ancestor (root, containers). `create_run()` creates `<D>/virmux/run-<pid>-<seq>`, enabling in virmux's own `virmux` directory every controller D hands down; D's own files are never written. Stale run cgroups are removed by `sweep_stale_runs()`. Shells in an ssh/console `session-N.scope` cannot leave their scope without D-Bus and fall back with a single warning suggesting `systemd-run --user --scope`.
   - Limits are applied independently (`CgroupSlice::apply_limits()`): `memory.max` = `--mem`, `memory.high` = 15/16 of it, `memory.swap.max` = 1/4, `cpu.max` = N CPUs' worth, `cpuset.cpus` when delegated, and `pids.max`. Control files are opened without `O_CREAT`, so a controller that is not delegated fails visibly instead of creating a stray file. `compute_memory_limits()` (`src/core/hw_view.cpp`) feeds both the cgroup and the synthetic `/proc/meminfo`, so they always agree.
   - Fork-bomb containment: clamps cgroups v2 `pids.max` with defensive `setrlimit(RLIMIT_NPROC)` fallback when cgroups are unavailable.

4. **`src/ipc/` (Binary Framing & Transport):**
   - Packed 12-byte binary TLV header (`IpcHeader`):
     - `magic`: 4-byte magic `0x564D5558` (`"VMUX"`).
     - `version`: Protocol version `1`.
     - `msg_type`: Request and response action identifiers.
     - `payload_len`: 32-bit payload length (bounded to 16 MiB sanity limit).
   - Non-blocking socket I/O using monotonic poll deadlines with fragmented stream reassembly.
   - Header validation lives in the pure `decode_ipc_header()` / `decode_ipc_frame()` functions shared by `recv_ipc_message()` and the `fuzz_ipc_frame` harness.

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
   - CPU placement (`src/core/cpu_select.cpp`): available CPUs are the online CPUs intersected with the parent's affinity mask. With four or more, the two lowest are kept for the interactive shell; `select_cpus()` takes a rotating contiguous window of N CPUs from the rest (a per-CPU load input is reserved for least-loaded placement). The parent builds the CPU mask and the child applies it with `sched_setaffinity` before `execvpe`, plus `cpuset.cpus` when cpuset is delegated. `--no-pin` keeps the inherited mask; `cpu.max` still caps CPU time.
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
    - Comparative gauntlet runner (`BenchmarkRunner`) executing baseline unmanaged concurrent tasks vs virmux-managed tasks (with `AdmissionGate`, Core 0-1 shielding, and OOM score adjustment) across a 12-terminal matrix. Workload processes are forked and parked before the suite starts any thread, then released by their worker thread after admission, so no fork happens in a multi-threaded process.
    - JSON export and human-readable comparative reporting (`format_comparison_table`, `format_comparison_json`).

---

## Technology Stack Rationale

- **C++20 (Core Engine & Daemon):** Strong RAII semantics (`ScopedFd`, `ScopedMount`, `ScopedCgroup`, `ScopedThread`) guarantee leak-free kernel resource cleanup. Concurrency primitives (`std::mutex`, `std::jthread`, `std::atomic`) ensure thread-safe session tracking.
- **C ABI (`extern "C"`) for `libvirmux_intercept.so`:** The `LD_PRELOAD` interceptor library compiles with C linkage so it can transparently intercept syscalls across any executable on the system (C, C++, Rust, Go, Python, Node).
- **Linux cgroups v2 & Procfs:** Native unified hierarchy provides atomic resource control across memory, CPU, and process lifecycles without external dependencies.
- **Unprivileged OverlayFS:** Standard kernel filesystem driver providing instant, zero-copy copy-on-write workspaces inside user namespaces without requiring root permissions.
- **Meson + Ninja:** Fast, modern build system with strict warning enforcement (`-Wall -Wextra -Wpedantic -Werror`).
