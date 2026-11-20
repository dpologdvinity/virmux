# Virmux — System Design Specification

## 01. Memory Multiplexing Subsystem

### 1. Architectural Objective

The Memory Subsystem enables 10–30 heavy sessions to run concurrently within a constrained memory footprint (e.g., 15 GB total WSL RAM) without triggering Linux Out-Of-Memory (OOM) kills or physical disk swapping. It transforms physical RAM into an elastic, deduplicated, and compressed pool.

```text
+-------------------------------------------------------------------------+
|                        Active Physical DRAM (15 GB)                     |
|                                                                         |
|  +--------------------+  +--------------------+  +-------------------+  |
|  | Uncompressed Hot   |  |   zram Compressed  |  |    KSM Merged     |  |
|  | Working Set        |  |   Swap Pool (LZ4)  |  |    Read-Only      |  |
|  | (Active Processes) |  |   ~3:1 Ratio       |  |    Shared Pages   |  |
|  +--------------------+  +--------------------+  +-------------------+  |
|            ^                        ^                       ^           |
+------------|------------------------|-----------------------|-----------+
             |                        |                       |
   process_madvise(PAGEOUT)   Kernel Memory Pressure    madvise(MERGEABLE)
             |                        |                       |
   +---------+------------------------+-----------------------+-------+
   |                        virmux Memory Engine                      |
   |   - Scans session lifecycle (active -> waiting)                  |
   |   - Dispatches pageout requests for idle sessions                |
   |   - Enforces cgroups v2 memory.high throttling                   |
   |   - Evicts dirty disk cache via posix_fadvise                    |
   +------------------------------------------------------------------+
```

---

### 2. Kernel Samepage Merging (KSM) Integration

#### The Problem

When 12 terminal sessions launch Python runtimes, Node.js, GCC, or headless Chromium, each process independently loads the identical executable binaries, dynamic link libraries (`libc.so`, `libpython.so`, `libnode.so`), and runtime heap initialization structures. This duplicates hundreds of megabytes of identical memory pages across sessions.

#### The Implementation

`virmux` interfaces with the Linux kernel KSM subsystem via `/sys/kernel/mm/ksm/`:

1. **Host Configuration & Tuning:**
   `virmux` inspects and tunes KSM control files:
   - `/sys/kernel/mm/ksm/run`: set to `1` (activate scanning).
   - `/sys/kernel/mm/ksm/pages_to_scan`: tuned dynamically based on session count (default: `500` pages per scan cycle).
   - `/sys/kernel/mm/ksm/sleep_millisecs`: set to `20` ms for responsive deduplication.
   - `/sys/kernel/mm/ksm/merge_across_nodes`: set to `1`.

2. **Proactive Page Registration:**
   For processes spawned under `virmux`, the `virmux` preload library (`libvirmux_intercept.so`) flags anonymous memory allocations as merge candidates:

   ```c
   /* Intercepted or wrapper hook in child runtime */
   madvise(addr, length, MADV_MERGEABLE);
   ```

   This directs the kernel scanner specifically to identical runtime heaps without wasting CPU cycles scanning random binary buffers.

3. **Metrics Telemetry:**
   `virmux` reads `/sys/kernel/mm/ksm/pages_sharing` and `/sys/kernel/mm/ksm/pages_shared` to compute real-time deduplication metrics:
   $$\text{Memory Saved} = \text{pages\_sharing} \times \text{PAGE\_SIZE}$$
   These metrics are piped directly to the `virmux top` TUI.

---

### 3. Proactive Heap Compaction via `process_madvise(2)`

#### The Problem

When a session finishes a heavy tool command (e.g. running a test suite or model training step) and transitions to waiting for user input or an API network response, its allocated heap remains resident in physical RAM. Traditional Linux memory reclamation (`kswapd`) is reactive: it only reclaims memory when total system memory pressure is critically high.

#### The Implementation

`virmux` uses the Linux 5.10+ `process_madvise(2)` syscall to actively instruct the kernel to evict or compress memory for specific target processes:

```c
#define _GNU_SOURCE
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

int virmux_memory_reclaim_pid(int pidfd) {
    /* Instruct kernel to page out anonymous memory to zram immediately */
    struct iovec vec = {
        .iov_base = NULL, /* Entire address space */
        .iov_len = 0
    };

    /* System call: process_madvise(pidfd, iovec, vlen, advice, flags) */
    ssize_t ret = syscall(SYS_process_madvise, pidfd, &vec, 1, MADV_PAGEOUT, 0);
    if (ret < 0) {
        /* Fallback for unprivileged: advise MADV_COLD */
        syscall(SYS_process_madvise, pidfd, &vec, 1, MADV_COLD, 0);
        return -1;
    }
    return 0;
}
```

- When session state transitions to `SESSION_STATE_IDLE` or `SESSION_STATE_WAITING_IO`, `virmux` triggers `MADV_PAGEOUT`.
- The kernel compresses the process's inactive pages into `zram` instantly, freeing physical RAM frames for other active sessions.
- When the process receives input or awakens, page faults restore compressed pages with microsecond latency.

---

### 4. In-RAM Compressed Swap (`zram` / `zswap`) Management

1. **WSL Integration:**
   `virmux` verifies whether a `zram` device exists (`/dev/zram0`). If absent, it instructs the user or provisions a compressed swap device configured with the `lz4` or `zstd` compression algorithm.
2. **Elimination of Disk Thrashing:**
   Traditional swap in WSL writes to Windows VHDX virtual disks on SSD/HDD, causing I/O stalls that freeze the host. `zram` swap remains 100% in physical RAM, compressing data at 3:1 to 4:1 ratios at memory bus speeds (gigabytes/sec).

---

### 5. Cgroups v2 Soft Throttling vs. Hard OOM Kills

Each `virmux` session runs inside a dedicated cgroup v2 slice under `/sys/fs/cgroup/virmux.slice/session-<id>/`.

```text
/sys/fs/cgroup/virmux.slice/
├── cgroup.subtree_control ("cpu memory io")
├── session-1/
│   ├── memory.high (Soft ceiling: triggers aggressive reclaim & throttling)
│   ├── memory.max  (Hard ceiling: absolute limit)
│   └── memory.current
└── session-2/
    ├── memory.high
    ├── memory.max
    └── memory.current
```

- **`memory.high` Protection:** Set to 80% of session allocation (e.g. `2.0G`). When a session exceeds `memory.high`, the Linux kernel does **not** terminate the process. Instead, it throttles the process's execution and forces aggressive background page reclamation.
- **`memory.max` Boundary:** Set to a safety ceiling (e.g. `3.0G`). Prevents a runaway memory leak from threatening system stability.

---

### 6. Post-Execution Page Cache Cleanse (`posix_fadvise`)

#### The Problem

Build tools and compilers (`gcc`, `rustc`, `cmake`, `npm`) read and write thousands of intermediate files (`.o`, `.a`, `.tmp`). The Linux kernel caches every single page read and written in the Page Cache (`buff/cache`). In a 15GB RAM system, page cache rapidly consumes 7–8 GB of RAM, triggering artificial memory pressure.

#### The Implementation

Upon completion of a build or command within a session sandbox:

```c
void virmux_cache_purge_path(const char *dir_path) {
    /* Walk directory tree of workspace scratch area */
    /* Open each temporary file descriptor */
    int fd = open(temp_file, O_RDONLY);
    if (fd >= 0) {
        /* Advise kernel to release cached pages from memory immediately */
        posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED);
        close(fd);
    }
}
```

This guarantees physical RAM is returned to the available pool the moment a compilation or test run completes.
