# Virmux — System Design Specification

## 02. CPU & Concurrency Scheduling Subsystem

### 1. Architectural Objective

The CPU Scheduling Subsystem prevents scheduler thrashing, CPU cache invalidation, and system unresponsiveness. It guarantees that interactive terminals and the host OS remain fluid (60fps UI, immediate terminal echo) while managing bursts from 10–30 simultaneous heavy worker sessions.

```text
+-------------------------------------------------------------------------+
|                    Hardware CPU Topology (12 Cores)                     |
|                                                                         |
|  +-----------------------------+  +----------------------------------+  |
|  |     Cores 0 - 1 (Shielded)  |  |      Cores 2 - 11 (Worker Pool)  |  |
|  |  Interactive Host & Shells  |  |   virmux Managed Sandboxes       |  |
|  |   - VS Code / Terminal GUI  |  |   - Compilation tasks            |  |
|  |   - Host WSL bash shell     |  |   - Browser tests (Playwright)   |  |
|  |   - Zero background tasks   |  |   - Neural network training      |  |
|  +-----------------------------+  +----------------------------------+  |
|                                                     ^                   |
+-----------------------------------------------------|-------------------+
                                                      |
                  +-----------------------------------+-------------------+
                  |           virmux Admission Controller                 |
                  |                                                       |
                  |  [Token Bucket: Max 2 Concurrent Heavy Bursts]        |
                  |                                                       |
                  |  Session A: [RUNNING]  -> 100% Burst on Cores 2-11    |
                  |  Session B: [RUNNING]  -> 100% Burst on Cores 2-11    |
                  |  Session C: [QUEUED]   -> Paused via SIGSTOP / FIFO   |
                  |  Session D: [FROZEN]   -> cgroup.freeze = 1 (0% CPU)  |
                  +-------------------------------------------------------+
```

---

### 2. Interactive Core Shielding (`cpuset.cpus`)

#### The Problem

When a test suite or compiler detects 12 cores, it launches 12 worker processes that saturate 100% of every CPU core. Every hardware core becomes unavailable for handling keyboard input, mouse events, or display redraws in the host OS, creating perceptible latency and freeze.

#### The Implementation

`virmux` partitions the CPU topology using the Linux cgroups v2 `cpuset` controller:

1. **Topology Detection:**
   At startup, `virmux` reads `/sys/devices/system/cpu/online` to discover total logical cores (e.g. `0-11`).
2. **Partition Assignment:**
   - **Host Reserved (Cores 0–1):** Never allocated to background worker sessions. Reserved strictly for user interactive shells, window managers, and host processes.
   - **Worker Pool (Cores 2–11):** Assigned to the root `virmux.slice` via:

     ```bash
     echo "2-11" > /sys/fs/cgroup/virmux.slice/cpuset.cpus
     ```

3. **Outcome:**
   Even if all worker sessions hit 100% compute load, Cores 0 and 1 remain completely unencumbered, guaranteeing instant shell responsiveness.

---

### 3. Admission Control Engine (Token Bucket Concurrency Limiter)

#### The Problem

If 10 sessions execute `cargo build`, `playwright test`, or `cmake --build` simultaneously:

- 10 sessions × 10 threads = **100 threads competing for 10 worker cores**.
- Instruction cache hit rates collapse from 95% down to <40%.
- Total time to complete all 10 builds balloons by 500% due to context-switching overhead.

#### The Implementation

`virmux` treats heavy compute execution as an admission-controlled resource queue:

```c
typedef enum {
    BURST_CLASS_LIGHT,   /* Shell commands, git, cat, echo -> Run immediately */
    BURST_CLASS_HEAVY    /* Compilers, test runners, Docker, Python training */
} virmux_burst_class_t;

typedef struct {
    int max_concurrent_heavy; /* e.g. 2 heavy tasks */
    int active_heavy_count;
    pthread_mutex_t lock;
    pthread_cond_t available_cond;
} virmux_admission_gate_t;
```

1. **Classification:**
   Commands launched via `virmux run <cmd>` are classified based on binary matching and resource heuristics:
   - Heavy binaries: `gcc`, `g++`, `clang`, `rustc`, `cargo`, `cmake`, `ninja`, `node` (when executing test runners), `python` (when training/evaluating), `playwright`, `docker`, `blender`.
2. **Queueing & Bursts:**
   - If `active_heavy_count < max_concurrent_heavy` (default: 2), the command is granted a ticket and allowed to burst across all available worker cores at full speed.
   - If the limit is reached, the subsequent heavy command is paused in the `virmux` FIFO queue before execution begins.
3. **Deadlock Prevention & Timeout Fallback:**
   When child tools execute nested commands (e.g., `ninja` executing `gcc` or `cargo` invoking rustc while holding lockfiles), the admission ticket acquisition includes a **10-second timeout fallback**. If the admission gate is full for >10 seconds, the task is admitted with a lower `cpu.weight = 20` rather than blocking indefinitely, preventing deadlocks.

---

### 4. Zero-Cost State Preservation: PTY Freezing vs. Network Yielding

#### The Implementation

`virmux` distinguishes between terminal idle and network wait to protect active network connections:

1. **Terminal / PTY Idle (Hard Freeze):**
   When a session's root process is blocked on `read(0, ...)` (waiting for terminal input from a user or agent controller) for >500ms:
   - `virmux` writes `"1\n"` to `cgroup.freeze`.
   - CPU usage drops to strictly **0.00%**.
   - Thaws instantly on incoming PTY keystroke or command data.
2. **Network I/O Wait (Soft Yield — No TCP Reset):**
   When a session is waiting on an external network connection (e.g. streaming LLM responses or downloading packages):
   - **Do NOT freeze cgroup:** Freezing stops TCP window updates and keep-alive packets, causing remote servers to abort with `ECONNRESET`.
   - **Soft Yield:** `virmux` sets process scheduling to `SCHED_IDLE` and dispatches `process_madvise(MADV_COLD)`.
   - Result: Minimal CPU consumption and cold heap compression without breaking TCP sockets.

---

### 5. Micro-Quantum Time-Slicing & Dynamic Fair-Share Weights

1. **`cpu.weight` Proportional Sharing:**
   Sessions running continuous tasks are assigned dynamic cgroup weights:
   - Interactive sessions: `cpu.weight = 200`
   - Background worker sessions: `cpu.weight = 50`
   - Yielding / background batch: `cpu.weight = 10`
2. **`SCHED_IDLE` Execution Mode:**
   For ultra-background workloads (e.g. background indexing, long-term training), `virmux` sets the scheduling policy to `SCHED_IDLE` via `sched_setscheduler(pid, SCHED_IDLE, &param)`:
   - A `SCHED_IDLE` task runs **only** when no other runnable thread in the entire operating system requests CPU time.
   - Ensures background training can run continuously without ever competing with active terminal tasks.
