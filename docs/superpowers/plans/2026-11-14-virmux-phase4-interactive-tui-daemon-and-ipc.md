# Phase 4: Interactive TUI (`virmux top`), Supervisor Daemon & IPC Subsystem Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build a low-overhead supervisor daemon, Unix domain socket IPC subsystem with binary TLV framing, session lifecycle controller (freeze/thaw/kill/cgroup management), CLI control subcommands (`list`, `freeze`, `thaw`, `kill`, `daemon`), and a full-screen interactive Ncurses dashboard (`virmux top`).

**Architecture:** A lightweight unprivileged background daemon listens on a local Unix domain socket (`/run/user/<UID>/virmux.sock`) using binary TLV framed messages with structured JSON payloads. When processes are executed via `virmux run`, they register session metadata with the daemon. The daemon tracks live process telemetry (virtual cores, RSS, KSM savings, cgroup freeze state, command lines) and mediates admission queueing and lifecycle control. The interactive TUI (`virmux top`) connects over IPC or falls back to direct procfs inspection, rendering real-time telemetry, host load, worker core allocation, and keyboard-driven session manipulation (`j`/`k`, `Space` freeze/thaw, `k` kill, `q` quit).

**Tech Stack:** C++20, Linux Unix Domain Sockets (`AF_UNIX`), Ncurses (`libncursesw`), Linux cgroups v2 (`cgroup.freeze`), procfs (`/proc/<pid>/stat`, `/proc/<pid>/status`, `/proc/<pid>/smaps_rollup`), Unity test framework, Meson build system, Bash integration testing.

**Spec:**
- `docs/specs/06-CLI-AND-TUI-SPEC.md`
- `docs/specs/05-FAULT-ISOLATION-LIFECYCLE.md`
- `docs/specs/02-CPU-SCHEDULING-SUBSYSTEM.md`
- `docs/specs/01-MEMORY-SUBSYSTEM.md`

---

## Global Constraints

- **Language Standard:** C++20 (`-std=c++20`), zero compiler warnings with `-Wall -Wextra -Wpedantic -Werror`.
- **Path Hygiene:** Never hardcode or emit `/home/<username>/` in any source code, documentation, config, commit, or test fixture. Always use `~` or `$HOME`.
- **Socket Path:** Default Unix domain socket at `/run/user/<UID>/virmux.sock`; fallback to `/tmp/virmux-<UID>.sock` if `/run/user/<UID>` is unavailable or unwritable.
- **Protocol Magic:** Binary TLV header must start with 4-byte magic `0x564D5558` (`"VMUX"`), version `1`.
- **Unprivileged Operation:** All socket communications, daemon execution, session telemetry, and freeze/thaw operations must execute without requiring `sudo` or root privileges.
- **Fail-Safe & Non-Blocking:** Socket I/O must use timeouts and non-blocking semantics; client calls must gracefully handle daemon unavailability without hanging or crashing `virmux run`.
- **TUI Resource Budget:** `virmux top` CPU consumption must remain below 0.5% during continuous 500ms refresh cycles.

---

## Review Focus

1. **Socket Cleanup & Stale Socket Recovery:** When daemon crashes or terminates via `SIGKILL`, the socket file remains. The daemon must detect stale sockets (failed connect with `ECONNREFUSED`) and unlink them cleanly on startup without stranding users.
2. **TLV Buffer Framing & Fragmentation:** Stream sockets can fragment or coalesce packets. Reading the TLV header and payload must handle partial reads and oversized payload rejection without buffer overflow or undefined behavior.
3. **Session Teardown Race Conditions:** If a session process terminates concurrently while `virmux top` or `virmux freeze` is sending a command, the daemon must handle `ESRCH` and invalid pids gracefully without throwing or crashing.
4. **Terminal State Restoration:** On `SIGINT`, `SIGTERM`, or abnormal exit in `virmux top`, the terminal screen mode and cursor must always be restored (`endwin()`), preventing garbled user shells.
5. **IPC Reconnection & Daemon Independence:** `virmux run` must execute successfully even if the daemon is completely absent, stopped, or unreachable.

---

## Task Decomposition

### Task 1: IPC Protocol, Binary TLV Framing & Socket Transport

**Files:**
- Create: `include/virmux/ipc.hpp`
- Create: `src/ipc/ipc.cpp`
- Create: `tests/unit/test_ipc.cpp`
- Modify: `meson.build`

**Requirements:**
- Define `IpcHeader` (packed 12 bytes):
  - `magic` (uint32_t): `0x564D5558` (`"VMUX"`).
  - `version` (uint16_t): `1`.
  - `msg_type` (uint16_t):
    - `0x0001` (`PING`)
    - `0x0002` (`PONG`)
    - `0x0010` (`REGISTER_SESSION`)
    - `0x0011` (`UNREGISTER_SESSION`)
    - `0x0020` (`LIST_SESSIONS_REQ`)
    - `0x0021` (`LIST_SESSIONS_RESP`)
    - `0x0030` (`FREEZE_SESSION_REQ`)
    - `0x0031` (`FREEZE_SESSION_RESP`)
    - `0x0032` (`THAW_SESSION_REQ`)
    - `0x0033` (`THAW_SESSION_RESP`)
    - `0x0040` (`KILL_SESSION_REQ`)
    - `0x0041` (`KILL_SESSION_RESP`)
    - `0x0050` (`GET_STATS_REQ`)
    - `0x0051` (`GET_STATS_RESP`)
    - `0x00E0` (`ERROR_RESP`)
  - `payload_len` (uint32_t): byte count of payload.
- Implement `resolve_socket_path()`:
  - If `VIRMUX_SOCKET` env var set, use it.
  - Else if `/run/user/<UID>` exists and is writable, use `/run/user/<UID>/virmux.sock`.
  - Else use `/tmp/virmux-<UID>.sock`.
- Implement `IpcChannel` (RAII socket client and server primitives):
  - `send_message(int fd, uint16_t msg_type, std::string_view payload, int timeout_ms = 1000)`.
  - `recv_message(int fd, uint16_t& out_type, std::string& out_payload, int timeout_ms = 1000)`.
  - Non-blocking I/O with `poll()` / `select()` and timeout handling.
  - Rejection of payloads larger than 16 MiB.
- Unit tests covering header serialization, partial buffer reading, timeout handling, socket path resolution, and error reporting.

- [ ] **Step 1: Write unit tests in `tests/unit/test_ipc.cpp`**
  - Test header packing and byte order.
  - Test `resolve_socket_path` resolution logic.
  - Test loopback socket pair communication with `socketpair(AF_UNIX, SOCK_STREAM, 0)`.
  - Test oversized payload rejection and timeout expiration.
- [ ] **Step 2: Add test target to `meson.build`**
  - Add `src/ipc/ipc.cpp` to `virmux_core_lib` and register `test_ipc`.
- [ ] **Step 3: Implement `include/virmux/ipc.hpp` and `src/ipc/ipc.cpp`**
  - Implement TLV encoding, decoding, and socket send/recv with `poll()`.
- [ ] **Step 4: Run tests and verify**
  - Run `ninja -C build test` and ensure all tests pass.
- [ ] **Step 5: Commit**
  - `git commit -m "feat(ipc): implement binary TLV framing and Unix socket transport"`

---

### Task 2: Session Registry & Lifecycle Controller

**Files:**
- Create: `include/virmux/session.hpp`
- Create: `src/daemon/session.cpp`
- Create: `tests/unit/test_session.cpp`
- Modify: `meson.build`

**Requirements:**
- Define `SessionState`:
  - `INITIALIZING`, `RUNNING_BURST`, `RUNNING_NORMAL`, `WAITING_IO`, `FROZEN`, `DEAD`.
- Define `SessionInfo`:
  - `uint64_t id`: unique incremental session counter.
  - `std::string name`: session label (defaults to binary command basename).
  - `pid_t pid`: root process ID of the session.
  - `SessionState state`: current state.
  - `int virtual_cores`: virtual cores allocated.
  - `uint64_t memory_rss_bytes`: resident set size from procfs.
  - `uint64_t memory_limit_bytes`: soft memory ceiling.
  - `uint64_t ksm_dedup_bytes`: memory deduplicated.
  - `std::string cgroup_path`: path to session's cgroup v2 slice.
  - `std::string command`: command string.
  - `std::chrono::system_clock::time_point start_time`: session creation timestamp.
- Implement `SessionRegistry`:
  - Thread-safe or mutex-protected collection of active sessions.
  - `register_session(const SessionRegistration& req) -> uint64_t`.
  - `unregister_session(uint64_t id) -> bool`.
  - `get_session(uint64_t id) -> std::optional<SessionInfo>`.
  - `list_sessions() -> std::vector<SessionInfo>`.
  - `refresh_telemetry()`: walks registered sessions, reads `/proc/<pid>/stat`, `/proc/<pid>/status`, and removes dead PIDs.
  - `freeze_session(uint64_t id) -> bool`: writes `1` to `cgroup.freeze` or falls back to `kill(pid, SIGSTOP)`.
  - `thaw_session(uint64_t id) -> bool`: writes `0` to `cgroup.freeze` or falls back to `kill(pid, SIGCONT)`.
  - `kill_session(uint64_t id, int signal = SIGTERM) -> bool`: sends signal to process tree.
  - JSON serialization of session list (`to_json()`).

- [ ] **Step 1: Write unit tests in `tests/unit/test_session.cpp`**
  - Test session registration, lookup, listing, and unregistration.
  - Test telemetry polling for child process created with `fork()`.
  - Test freeze and thaw state transitions using `SIGSTOP`/`SIGCONT` or cgroup fallback.
  - Test JSON serialization.
- [ ] **Step 2: Add test target to `meson.build`**
  - Add `src/daemon/session.cpp` to `virmux_core_lib` and register `test_session`.
- [ ] **Step 3: Implement `session.hpp` and `session.cpp`**
  - Implement `SessionRegistry` methods with proper locking and procfs scanning.
- [ ] **Step 4: Run tests and verify**
  - Run `ninja -C build test` and ensure all tests pass.
- [ ] **Step 5: Commit**
  - `git commit -m "feat(daemon): implement session registry and lifecycle management"`

---

### Task 3: Supervisor Daemon Server

**Files:**
- Create: `include/virmux/daemon.hpp`
- Create: `src/daemon/daemon.cpp`
- Create: `tests/unit/test_daemon.cpp`
- Modify: `meson.build`

**Requirements:**
- Implement `SupervisorDaemon`:
  - Manages Unix socket listener (`AF_UNIX`, `SOCK_STREAM`, `listen()`).
  - Stale socket detection: tests connecting to existing socket. If connection fails (`ECONNREFUSED` or `ENOENT`), unlinks stale socket before binding.
  - Socket permission enforcement (0700).
  - Event loop using `poll()` or `epoll`: handles client connections, reads TLV requests, dispatches to `SessionRegistry`, sends TLV responses.
  - Background telemetry loop: runs every 1000ms updating session memory, KSM metrics, and dead process pruning.
  - Signal handling: gracefully shuts down on `SIGINT` / `SIGTERM`, closes connections, unlinks socket file.
  - Client helper: `DaemonClient` class providing high-level RPCs:
    - `ping() -> bool`
    - `register_session(const SessionRegistration& req) -> uint64_t`
    - `unregister_session(uint64_t id) -> bool`
    - `list_sessions() -> std::vector<SessionInfo>`
    - `freeze_session(uint64_t id) -> bool`
    - `thaw_session(uint64_t id) -> bool`
    - `kill_session(uint64_t id) -> bool`
    - `get_stats() -> SystemStats`

- [ ] **Step 1: Write unit tests in `tests/unit/test_daemon.cpp`**
  - Test daemon startup, socket creation, and stale socket recovery.
  - Test `DaemonClient` connecting, sending `PING`, receiving `PONG`.
  - Test session registration, query, freeze/thaw, and unregistration via RPC.
  - Test clean shutdown and socket unlinking.
- [ ] **Step 2: Add test target to `meson.build`**
  - Add `src/daemon/daemon.cpp` to `virmux_core_lib` and register `test_daemon`.
- [ ] **Step 3: Implement `include/virmux/daemon.hpp` and `src/daemon/daemon.cpp`**
  - Implement `SupervisorDaemon` server and `DaemonClient` transport.
- [ ] **Step 4: Run tests and verify**
  - Run `ninja -C build test` and ensure all tests pass.
- [ ] **Step 5: Commit**
  - `git commit -m "feat(daemon): implement supervisor daemon server and client RPC"`

---

### Task 4: CLI Control Subcommands & Sandbox Registration

**Files:**
- Modify: `cli/main.cpp`
- Create: `include/virmux/cli_daemon.hpp`
- Create: `src/cli/cli_daemon.cpp`
- Modify: `src/core/sandbox.cpp`
- Create: `tests/unit/test_cli_daemon.cpp`
- Modify: `meson.build`

**Requirements:**
- Implement CLI subcommands:
  - `virmux daemon [--foreground]`: runs supervisor daemon process.
  - `virmux list [--json]`: connects to daemon, queries session list, outputs ASCII table or JSON.
  - `virmux freeze <SESSION_ID | PID>`: sends freeze request to daemon.
  - `virmux thaw <SESSION_ID | PID>`: sends thaw request to daemon.
  - `virmux kill <SESSION_ID | PID>`: sends kill request to daemon.
- Integrate with `virmux run`:
  - When `spawn_command()` forks child, attempts to register session with daemon if daemon socket is active.
  - When child exits, unregisters session from daemon.
  - If daemon is not running, continues execution transparently without error.
- Help screens and usage documentation updated in `cli/main.cpp`.

- [ ] **Step 1: Write unit tests in `tests/unit/test_cli_daemon.cpp`**
  - Test argument parsing for `list`, `freeze`, `thaw`, `kill`, `daemon`.
  - Test formatting of session table in ASCII and JSON modes.
  - Test error handling when daemon is unreachable (returns clean error message).
- [ ] **Step 2: Add test target to `meson.build`**
  - Add `src/cli/cli_daemon.cpp` to `virmux_core_lib` and register `test_cli_daemon`.
- [ ] **Step 3: Implement `cli_daemon.cpp`, update `cli/main.cpp`, and hook `src/core/sandbox.cpp`**
  - Add CLI dispatching and sandbox IPC registration hooks.
- [ ] **Step 4: Run tests and verify**
  - Run `ninja -C build test` and ensure all tests pass.
- [ ] **Step 5: Commit**
  - `git commit -m "feat(cli): add list, freeze, thaw, kill, daemon subcommands and sandbox registration"`

---

### Task 5: Interactive TUI Dashboard (`virmux top`) with Ncurses

**Files:**
- Create: `include/virmux/tui.hpp`
- Create: `src/tui/tui.cpp`
- Create: `tests/unit/test_tui.cpp`
- Modify: `cli/main.cpp`
- Modify: `meson.build`

**Requirements:**
- Dependency: Link `ncursesw` via Meson `dependency('ncursesw')`.
- Implement `VirmuxTui`:
  - Initializes curses mode (`initscr()`, `raw()`, `noecho()`, `keypad()`, `curs_set(0)`).
  - Terminal cleanup RAII wrapper ensuring `endwin()` is called on all exits or signals.
  - Layout:
    - **Header Panel:**
      - Title and version `VIRMUX v0.1.0`.
      - System info: WSL2/Linux, CPU core count, Host Load (`/proc/loadavg`), Total RAM.
      - Shield vs Worker CPU partition info.
      - Memory telemetry: KSM deduplication savings, zram compression ratio.
    - **Session Table Panel:**
      - Headers: `S#`, `NAME`, `STATE`, `V-CORES`, `RAM (RSS)`, `DEDUP`, `COMMAND`.
      - Interactive cursor highlighting currently selected row.
      - Support for scrolling when session count exceeds screen height.
    - **Footer Help Panel:**
      - Hotkeys: `[j/k] Navigate  [Space] Freeze/Thaw  [k] Kill  [q] Quit`.
  - Keyboard handling:
    - `KEY_UP` / `k`: move selection up.
    - `KEY_DOWN` / `j`: move selection down.
    - `Space`: toggle freeze / thaw on selected session via daemon.
    - `k`: kill selected session via daemon.
    - `q`: exit cleanly.
  - Non-blocking loop: uses `timeout(500)` for responsive hotkeys and 500ms data refresh.
  - Fallback mode: If daemon is offline, polls local procfs for running `virmux` processes or displays `Daemon Offline (running in standalone mode)`.
  - Headless test mode: `VirmuxTui::render_frame_to_buffer()` to test string output without physical TTY.

- [ ] **Step 1: Write unit tests in `tests/unit/test_tui.cpp`**
  - Test frame rendering buffer (header formatting, session row formatting, footer help).
  - Test navigation bounds and hotkey dispatch logic.
  - Test standalone mode fallback when daemon is unavailable.
- [ ] **Step 2: Add test target and ncurses dependency to `meson.build`**
  - Add `dependency('ncursesw')`, add `src/tui/tui.cpp` to library, register `test_tui`.
- [ ] **Step 3: Implement `include/virmux/tui.hpp` and `src/tui/tui.cpp`**
  - Implement full-screen dashboard and keyboard interaction.
- [ ] **Step 4: Wire `virmux top` into `cli/main.cpp`**
- [ ] **Step 5: Run tests and verify**
  - Run `ninja -C build test` and ensure all tests pass.
- [ ] **Step 6: Commit**
  - `git commit -m "feat(tui): implement virmux top interactive ncurses dashboard"`

---

### Task 6: End-to-End Integration Suite & System Verification

**Files:**
- Create: `tests/integration/test_daemon_tui.sh`
- Modify: `meson.build`

**Requirements:**
- Automated integration script (`test_daemon_tui.sh`):
  - Starts background `virmux daemon` with custom `VIRMUX_SOCKET`.
  - Tests `virmux list` and `virmux list --json` when no sessions are active.
  - Spawns background workload: `virmux run -- /bin/sh -c "sleep 30" &`.
  - Tests `virmux list` shows running session with correct command name.
  - Tests `virmux freeze <SESSION_ID>` transitions state to `FROZEN`.
  - Tests `virmux thaw <SESSION_ID>` transitions state back to `RUNNING`.
  - Tests `virmux kill <SESSION_ID>` terminates session cleanly.
  - Tests `virmux top --test-render` or headless TUI snapshot.
  - Verifies daemon shutdown on SIGTERM and socket unlinking.
- Register `integration_daemon_tui` test in `meson.build` with `timeout: 120`.

- [x] **Step 1: Write `tests/integration/test_daemon_tui.sh`**
- [x] **Step 2: Register in `meson.build`**
- [x] **Step 3: Run full test suite (`ninja -C build test`)**
  - All unit and integration tests must pass.
- [x] **Step 4: Commit**
  - `git commit -m "test(integration): add supervisor daemon and TUI lifecycle integration suite"`

---

## Execution Order
1. Task 1: IPC Protocol, Binary TLV Framing & Socket Transport
2. Task 2: Session Registry & Lifecycle Controller
3. Task 3: Supervisor Daemon Server
4. Task 4: CLI Control Subcommands & Sandbox Registration
5. Task 5: Interactive TUI Dashboard (`virmux top`) with Ncurses
6. Task 6: End-to-End Integration Suite & System Verification
7. Whole-Branch Senior Code Review & Consolidated Fix Cycle
8. Merge to `main`, push to GitHub, and branch finalization
