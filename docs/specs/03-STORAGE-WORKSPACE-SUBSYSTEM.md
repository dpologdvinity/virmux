# Virmux — System Design Specification
## 03. Storage & Workspace Subsystem

### 1. Architectural Objective

The Storage Subsystem eliminates physical disk duplication, eliminates WSL VHDX I/O bottlenecks, and provides instant, throwaway workspace sandboxes for 10–30 simultaneous sessions.

```
+-------------------------------------------------------------------------+
|                      Shared Base Repository (Read-Only)                 |
|             e.g. ~/git/project/ (Code, History, Static Assets)          |
+------------------------------------+------------------------------------+
                                     |
               +---------------------+---------------------+
               |                     |                     |
         (lowerdir: RO)        (lowerdir: RO)        (lowerdir: RO)
               |                     |                     |
+--------------v-------------+ +-----v---------------+ +---v---------------+
|    Session 1 Sandbox       | |  Session 2 Sandbox  | | Session 3 Sandbox |
|                            | |                     | |                   |
|  OverlayFS Mount:          | | OverlayFS Mount:    | | OverlayFS Mount:  |
|  upperdir: /run/user/      | | upperdir: /run/...  | | upperdir: /run/...|
|            virmux/s1/upper | | (RAM tmpfs)         | | (RAM tmpfs)       |
|  workdir:  virmux/s1/work  | |                     | |                   |
|                            | |                     | |                   |
|  Only modified files (.o,  | | Only modified files | | Only modified     |
|  build/, logs) written here| | written here        | | files written here|
+----------------------------+ +---------------------+ +-------------------+
```

---

### 2. Workspace Modes: Persistent (Default) vs. Ephemeral

#### Mode A: Persistent Workspaces (Default for `virmux sh` & `virmux run`)
- **Code & Commit Preservation:** When an agent or developer edits files or creates git commits, modifications write directly to the working repository.
- **Cache & Scratch Redirection:** Heavy temporary folders and build directories (`build/`, `target/`, `node_modules/.cache/`, `.pytest_cache/`, `TMPDIR`) are automatically redirected to in-memory `tmpfs` mounts via bind-mounts or symlinks.
- **Outcome:** Code edits are preserved across sessions, while gigabytes of compiler scratch files remain in RAM and vanish on cleanups.

#### Mode B: Ephemeral Sandboxes (`virmux run --ephemeral`)
- **Throwaway OverlayFS Mount:**
  ```bash
  mount -t overlay overlay \
    -o lowerdir=/path/to/base/repo,\
       upperdir=/run/user/1000/virmux/sessions/s1/upper,\
       workdir=/run/user/1000/virmux/sessions/s1/work \
    /run/user/1000/virmux/sessions/s1/merged
  ```
- **Behavior:**
  - Base repository remains strictly read-only.
  - All writes (edits, builds, test artifacts) write to RAM `upperdir`.
  - On session exit, the sandbox is unmounted and wiped in **<10ms**. Ideal for running untrusted agent test passes or dirty build tests.

---

### 3. I/O Throttling (`io.max`) & tmpfs Scratch Routing

#### The Problem
When 5 sessions simultaneously execute `npm install` or extract Docker layers, disk I/O queue depth spikes to >128. In WSL2, host disk communication goes through the 9P filesystem bridge or hypervisor virtual SCSI controller, causing system-wide stalls.

#### The Implementation
`virmux` enforces I/O rate limits and redirects heavy temporary writes to RAM:

1. **cgroups v2 `io.max` Configuration:**
   Each session cgroup is assigned an explicit bandwidth cap for physical block devices (e.g. `/dev/sdb`):
   ```
   # Set 20 MB/s read, 10 MB/s write cap per session
   8:16 rbps=20971520 wbps=10485760
   ```
   This prevents any single background session from monopolizing the host SSD bus.
2. **tmpfs Scratch Redirection:**
   `virmux` sets `TMPDIR=/run/user/1000/virmux/tmp/session-<id>` inside the sandboxed environment. Temporary files created by compilers, bundlers, and test frameworks live exclusively in compressed memory and never hit physical storage.

---

### 4. Global Content-Addressable Storage (CAS) Build Cache

#### The Problem
In multi-agent or multi-terminal development, identical source files are compiled repeatedly across different sessions, burning billions of CPU cycles generating identical machine code.

#### The Implementation
`virmux` includes a shared, lockless content-addressable cache:

1. **Hash Generation:**
   Before invoking the compiler (`gcc`, `clang`, `rustc`), `virmux` computes a blake3/sha256 hash of:
   $$\text{Hash} = \mathcal{H}(\text{source\_content} \mathbin{\Vert} \text{compiler\_version} \mathbin{\Vert} \text{cli\_flags} \mathbin{\Vert} \text{included\_headers})$$
2. **Lookup & Cache Hit:**
   - If the hash exists in `/var/cache/virmux/cas/objects/<hash>`, `virmux` links or copies the compiled object directly into the target output path.
   - Command returns in **0.001 seconds**.
   - Zero CPU cycles spent in compilation.
3. **Cache Storage:**
   All sessions share this single CAS directory, automatically deduplicating build outputs across the entire workstation.
