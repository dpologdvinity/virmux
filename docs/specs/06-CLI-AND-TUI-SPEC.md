# Virmux — System Design Specification
## 06. CLI & Interactive TUI Specification

### 1. Architectural Objective

The CLI and Terminal User Interface (TUI) provide ergonomic, instantaneous control over all multiplexed sessions. The interface must be completely keyboard-driven, fast, lightweight (low CPU overhead <0.5%), and work cleanly over SSH and in WSL terminal emulators.

---

### 2. CLI Command Line Interface

```bash
virmux [GLOBAL OPTIONS] <COMMAND> [COMMAND OPTIONS]
```

#### Core Commands

1. **`virmux run [OPTIONS] -- <COMMAND...>`**
   Executes a single command inside a managed, resource-isolated sandbox.
   - `--cores <N>`: Override virtual core count exposed to command (default: `2`).
   - `--mem <SIZE>`: Set memory soft ceiling (default: `2G`).
   - `--workspace <PATH>`: Mount path as an isolated Copy-on-Write OverlayFS workspace.
   - `--heavy`: Explicitly register task with the Admission Control queue.
   - `--priority <PRIO>`: Set priority class: `interactive`, `normal`, `idle` (`SCHED_IDLE`).
   - *Example:*
     ```bash
     virmux run --workspace ~/git/daydaze -- npx playwright test
     ```

2. **`virmux sh [OPTIONS]`**
   Spawns an interactive shell (`bash` / `zsh`) inside an isolated session.
   - Every command typed inside this shell inherits `virmux` resource protection automatically.

3. **`virmux top`**
   Launches the full-screen interactive Ncurses monitoring and multiplexing dashboard.

4. **`virmux list`**
   Prints an ASCII table or JSON listing of all active, idle, frozen, and checkpointed sessions.

5. **`virmux attach <SESSION_ID>`**
   Attaches current terminal to a running or frozen background session PTY.

6. **`virmux freeze <SESSION_ID>` / `virmux thaw <SESSION_ID>`**
   Manually freezes or thaws a session's cgroup subtree.

7. **`virmux checkpoint <SESSION_ID>` / `virmux restore <SESSION_ID>`**
   Dumps session state to disk / resumes from checkpoint.

8. **`virmux stats [--json]`**
   Dumps aggregate machine metrics: Total RAM saved via KSM, zram compression ratio, active admission tickets, and queued jobs.

---

### 3. Interactive TUI (`virmux top`) Design

```
+-----------------------------------------------------------------------------+
| VIRMUX v0.1.0  [WSL2 / 12 Cores / 15.0G RAM]            Host Load: 2.15    |
| Dedicated Shield: Cores 0-1 (Fluid)   | Worker Pool: Cores 2-11 (Managed)   |
| KSM Dedup: 4.2 GB Saved  | zram: 2.1 GB Compressed (3.2x) | Heavy Gate: 2/2 |
+-----------------------------------------------------------------------------+
| S# | NAME        | STATE   | V-CORES | RAM (RSS) | DEDUP | IO/s | COMMAND   |
+----+-------------+---------+---------+-----------+-------+------+-----------+
| 01 | daydaze-e2e | BURST   | 2 (10)  |  412 MB   | 280MB | 12M  | npx playw |
| 02 | nibble-trai | BURST   | 2 (10)  | 1.84 GB   | 620MB | 2.1M | ./bin/tra |
| 03 | face-docker | QUEUED  | 2 (0)   |   45 MB   |  30MB |  0K  | docker bu |
| 04 | agent-term4 | FROZEN  | 2 (0)   |  120 MB   |  95MB |  0K  | python3 a |
| 05 | agent-term5 | WAITING | 2 (0)   |  115 MB   |  90MB |  0K  | bash (pty |
+----+-------------+---------+---------+-----------+-------+------+-----------+
| [F1] Help  [F2] Attach  [F5] Pause/Thaw  [F7] Checkpoint  [F9] Kill  [q] Quit |
+-----------------------------------------------------------------------------+
```

#### TUI Hotkeys
- `Up` / `Down` or `j` / `k`: Select session in table.
- `Enter`: Attach terminal to selected session.
- `Space`: Toggle freeze/thaw on selected session.
- `c`: Checkpoint selected session to disk.
- `k`: Terminate selected session subtree cleanly (`SIGTERM` -> `SIGKILL`).
- `Tab`: Switch view (Sessions View <-> Memory Savings View <-> CPU Topology View).
- `q`: Exit TUI (background sessions continue running).

---

### 4. IPC Architecture & Socket Protocol

Communication between `virmux` CLI instances and the supervisor daemon uses a high-performance Unix Domain Socket at `/run/user/<UID>/virmux.sock`.

#### Message Format
Binary TLV (Type-Length-Value) packet protocol with JSON payload for complex queries:

```c
typedef struct __attribute__((packed)) {
    uint32_t magic;         /* 0x564D5558 "VMUX" */
    uint16_t version;       /* 1 */
    uint16_t msg_type;      /* e.g. MSG_SPAWN, MSG_FREEZE, MSG_STATS */
    uint32_t payload_len;
} virmux_ipc_header_t;
```

Socket supports file descriptor passing via `SCM_RIGHTS` to hand off pseudo-terminal PTY master descriptors seamlessly between the daemon and CLI frontends.
