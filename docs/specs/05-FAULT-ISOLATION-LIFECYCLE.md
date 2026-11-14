# Virmux — System Design Specification
## 05. Fault Isolation & Lifecycle Management Subsystem

### 1. Architectural Objective

The Fault Isolation Subsystem ensures deterministic blast-radius containment. If a runaway process leaks 50 GB of memory, executes a fork-bomb, or triggers a fatal segmentation fault in Session 1, **it must never crash Session 2, the `virmux` supervisor, or the host desktop environment**.

```
+-------------------------------------------------------------------------+
|                  Linux Kernel OOM Killer Priority Hierarchy             |
|                                                                         |
|  [oom_score_adj = -1000]  Host System & Desktop (IMMUNE FROM KILL)      |
|                           - Windows/WSL init, systemd, SSH, X11/Wayland |
|                           - User's interactive editor & shell           |
|                                                                         |
|  [oom_score_adj = -900]   virmux Supervisor Daemon (PROTECTED)          |
|                                                                         |
|  [oom_score_adj = 0]      Interactive virmux Terminal Sessions          |
|                                                                         |
|  [oom_score_adj = +800]   virmux Worker / Background Child Processes    |
|                           - Compilers, Playwright browsers, Test runners|
|                           (FIRST TO BE TERMINATED ON PRESSURE)          |
+-------------------------------------------------------------------------+
```

---

### 2. Deterministic OOM Killer Shielding (`oom_score_adj`)

#### The Problem
When the Linux kernel runs out of physical memory and swap, the Out-Of-Memory (OOM) killer selects a victim using a heuristic (`badness` score). In unmanaged systems, the OOM killer frequently chooses the largest process by RSS—which is often the user's IDE, browser, or X server—killing the entire desktop and destroying unsaved work.

#### The Implementation
`virmux` explicitly configures `/proc/<pid>/oom_score_adj` for every process in its hierarchy:

1. **Host & Supervisor Immunity:**
   The `virmux` supervisor process marks itself protected:
   ```c
   void virmux_protect_self(void) {
       int fd = open("/proc/self/oom_score_adj", O_WRONLY);
       if (fd >= 0) {
           write(fd, "-900\n", 5); /* Substantially reduces kill priority */
           close(fd);
       }
   }
   ```
2. **Worker Sacrificial Priority:**
   When spawning a command sandbox, `virmux` writes a positive score to the child's `oom_score_adj` before executing `execve`:
   ```c
   void virmux_set_worker_oom_score(pid_t child_pid) {
       char path[64];
       snprintf(path, sizeof(path), "/proc/%d/oom_score_adj", child_pid);
       int fd = open(path, O_WRONLY);
       if (fd >= 0) {
           write(fd, "800\n", 4); /* High sacrifice priority */
           close(fd);
       }
   }
   ```
3. **Outcome:**
   If a Playwright test or model script leaks memory, the kernel terminates **only that specific leaf process**. The parent session receives a clean `SIGCHLD`, logs an out-of-memory error code, and the other 11 sessions continue running undisturbed.

---

### 3. Session Lifecycle State Machine

Each session managed by `virmux` transitions through a deterministic state machine:

```
                  +-------------------------+
                  |       INITIALIZING      |
                  +------------+------------+
                               |
                               v
                  +-------------------------+
       +--------->|      RUNNING_BURST      | (Holding admission token,
       |          +------------+------------+  100% CPU on worker cores)
       |                       |
       |  Token Yield /        v
       |  Command Finished +-------------------------+
       |          +------->|     RUNNING_NORMAL      | (Normal execution,
       |          |        +------------+------------+  cpu.weight throttled)
       |          |                     |
       |          |                     v
       |  I/O     |        +-------------------------+
       |  Arrives |        |       WAITING_IO        | (Waiting on terminal
       |          |        +------------+------------+  or network packet)
       |          |                     |
       |          |                     v  (>500ms idle)
       |          |        +-------------------------+
       |          +--------+         FROZEN          | (cgroup.freeze = 1,
       |                   +------------+------------+  0.00% CPU, pageout)
       |                                |
       |                   Low Mem      v
       |                   Eviction +-------------------------+
       +----------------------------+       CHECKPOINTED      | (Memory dumped
       |                            +------------+------------+  to zram/disk)
       |                                         |
       |                   Exit / Kill           v
       +--------------------------------->+-------------------------+
                                          |        TERMINATED       |
                                          +-------------------------+
```

---

### 4. Session Checkpoint & Hibernation Engine (CRIU Integration)

#### The Problem
A user is running a multi-hour model training task or large simulation in Session 1, but urgently needs to run an intensive full-suite test in Session 2 that requires maximum physical RAM.

#### The Implementation
`virmux` integrates with **CRIU (Checkpoint/Restore In Userspace)**:

1. **`virmux checkpoint <session-id>`:**
   - Dumps process tree registers, memory maps, socket descriptors, and IPC state into `/run/user/1000/virmux/checkpoints/<session-id>/`.
   - Compresses the image using `lz4`.
   - Terminates running processes, releasing **100% of physical RAM and CPU**.
2. **`virmux restore <session-id>`:**
   - Reconstructs the exact process tree with identical PIDs and resumed file descriptors.
   - Resumes execution in **<50 milliseconds**.
