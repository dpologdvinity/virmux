# Virmux (Phase 3: Ephemeral OverlayFS Workspaces & Storage Subsystem) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Implement the storage multiplexing subsystem for `virmux`: ephemeral Copy-on-Write workspaces using user-namespace OverlayFS mounts (`--ephemeral`), in-memory scratch space redirection (`TMPDIR`), content-addressable storage (CAS) build caching, and seamless workspace teardown.

**Architecture:** An unprivileged Linux mount namespace (`CLONE_NEWUSER | CLONE_NEWNS`) constructs an OverlayFS mount with the base git repository as a read-only lower directory (`lowerdir`) and a RAM tmpfs directory as upper (`upperdir`). All file writes, compiler outputs, and git state remain fully isolated in RAM and vanish on session exit without touching the host filesystem. A Content-Addressable Storage (CAS) cache deduplicates build artifacts across workspaces using SHA-256 object addressing.

**Tech Stack:** C++20, Linux Namespaces (`unshare`, `mount`, `CLONE_NEWUSER`, `CLONE_NEWNS`), OverlayFS, POSIX APIs (`posix_fadvise`), Meson, Ninja, Unity Test Harness, Bash.

**Spec:** `docs/specs/03-STORAGE-WORKSPACE-SUBSYSTEM.md`, `docs/specs/06-CLI-AND-TUI-SPEC.md`

## Global Constraints

- Language: C++20 for all storage and workspace controllers; 0 compiler warnings with `-Wall -Wextra -Wpedantic`.
- Target platform: Linux kernel 5.10+ (WSL2 Ubuntu 22.04+ and bare-metal Linux).
- Unprivileged execution: OverlayFS mounts must run via unprivileged user namespaces (`CLONE_NEWUSER | CLONE_NEWNS`) without requiring `sudo` or root permissions.
- Non-invasive execution: Base repository must remain strictly read-only and immutable during `--ephemeral` execution.
- No absolute home paths: All paths in configs, scripts, and code must use relative paths, `~`, or `$HOME`.
- Rapid teardown: Ephemeral workspace unmount and directory wipe must complete in `< 50ms`.

## Review Focus

1. **Unprivileged User Namespace Setup & UID/GID Mapping:** Inside a new user namespace (`CLONE_NEWUSER`), the process must map its outer UID/GID to root (0) inside `uid_map` and `gid_map` before invoking `mount("overlay", ...)` to avoid `EPERM`.
2. **Base Repository Immutability:** During `--ephemeral` execution, no file, directory, or `.git` index in `lowerdir` may be modified, created, or deleted.
3. **Namespace Mount Cleanup & Leaked Descriptors:** If a command crashes or terminates unexpectedly, mount points and temporary directories (`upper`, `work`, `merged`) must not leak in `/run/user/<UID>/virmux/` or `/tmp`.
4. **CAS Atomicity & Corruption Protection:** Objects written to the CAS cache must be staged in temporary files and committed via atomic `rename` to prevent corrupted partial objects from cache reads.
5. **Path Resolution Across Mount Boundaries:** When a child process changes root or current working directory to `merged`, relative paths and environment variables (`PATH`, `TMPDIR`, `LD_PRELOAD`) must remain valid.

---

### Task 1: SHA-256 Hashing & Content-Addressable Storage (CAS)

**Files:**

- Create: `include/virmux/sha256.hpp`
- Create: `include/virmux/cas.hpp`
- Create: `src/storage/sha256.cpp`
- Create: `src/storage/cas.cpp`
- Create: `tests/unit/test_cas.cpp`
- Modify: `meson.build`

**Interfaces:**

```cpp
namespace virmux {

/**
 * @brief Computes SHA-256 hash string (64 lowercase hex characters).
 */
[[nodiscard]] std::string sha256_hash(std::span<const uint8_t> data);
[[nodiscard]] std::string sha256_hash_file(const std::string& file_path);

struct CasStats {
  size_t object_count{0};
  uint64_t total_bytes{0};
  uint64_t hits{0};
  uint64_t misses{0};
};

class CasStore {
 public:
  explicit CasStore(std::string cache_root = "");

  [[nodiscard]] bool contains(const std::string& key) const;
  bool put(const std::string& key, const std::string& source_file);
  bool put_bytes(const std::string& key, std::span<const uint8_t> data);
  bool get(const std::string& key, const std::string& dest_path);
  [[nodiscard]] std::string get_path(const std::string& key) const;
  size_t prune(uint64_t max_bytes);
  [[nodiscard]] CasStats get_stats() const;
  void reset_stats() noexcept;

 private:
  std::string cache_root_;
  mutable uint64_t hits_{0};
  mutable uint64_t misses_{0};
};

} // namespace virmux
```

- [ ] **Step 1: Write unit tests in `tests/unit/test_cas.cpp`**
  - Verify SHA-256 against standard NIST test vectors (empty string, "abc", multi-block input).
  - Test `CasStore` put and get round-trip with temporary file fixture.
  - Verify atomic commit: ensure half-written files do not appear in CAS.
  - Verify `prune` removes oldest files when size exceeds `max_bytes`.
  - Verify statistics tracking (hits and misses).

- [ ] **Step 2: Add test target to `meson.build`**
  - Add `src/storage/sha256.cpp` and `src/storage/cas.cpp` to `virmux_core_lib`.
  - Register `test_cas` executable and test runner.

- [ ] **Step 3: Implement `sha256.cpp` and `cas.cpp`**
  - Clean C++20 RFC 6234 compliant SHA-256 implementation with zero external dependencies.
  - Implement atomic file writing (`.tmp` + `std::filesystem::rename`).
  - Implement directory scanning and LRU pruning based on file last write times.

- [ ] **Step 4: Run tests and verify**
  - Run `ninja -C build test` and ensure all tests pass.

- [ ] **Step 5: Commit**
  - `git commit -m "feat(storage): implement SHA-256 hasher and Content-Addressable Storage (CAS)"`

---

### Task 2: In-Memory Scratch Space & Environment Redirection

**Files:**

- Create: `include/virmux/scratch.hpp`
- Create: `src/storage/scratch.cpp`
- Create: `tests/unit/test_scratch.cpp`
- Modify: `meson.build`

**Interfaces:**

```cpp
namespace virmux {

class ScratchWorkspace {
 public:
  explicit ScratchWorkspace(std::string base_scratch_dir = "");
  ~ScratchWorkspace();

  ScratchWorkspace(const ScratchWorkspace&) = delete;
  ScratchWorkspace& operator=(const ScratchWorkspace&) = delete;
  ScratchWorkspace(ScratchWorkspace&& other) noexcept;
  ScratchWorkspace& operator=(ScratchWorkspace&& other) noexcept;

  bool init();
  [[nodiscard]] const std::string& path() const noexcept { return path_; }
  [[nodiscard]] bool is_valid() const noexcept { return !path_.empty(); }
  void populate_env(std::vector<std::string>& env_vars) const;
  void cleanup() noexcept;

 private:
  std::string base_dir_;
  std::string path_;
  bool created_{false};
};

} // namespace virmux
```

- [ ] **Step 1: Write unit tests in `tests/unit/test_scratch.cpp`**
  - Test `init()` creates directory with mode 0700 in `/run/user/<UID>/virmux/scratch/` or `/tmp/`.
  - Verify `populate_env()` sets `TMPDIR`, `TEMP`, and `TMP` pointing to `path()`.
  - Verify destructor and `cleanup()` deletes scratch folder and its contents.
  - Verify move semantics transfer directory ownership without accidental deletion.

- [ ] **Step 2: Add test target to `meson.build`**
  - Add `src/storage/scratch.cpp` to `virmux_core_lib` and register `test_scratch`.

- [ ] **Step 3: Implement `include/virmux/scratch.hpp` and `src/storage/scratch.cpp`**
  - Implement atomic sequence counter for unique scratch folder naming.
  - Use `cleanse_path` on scratch contents before deletion.

- [ ] **Step 4: Run tests and verify**
  - Run `ninja -C build test` and ensure all tests pass.

- [ ] **Step 5: Commit**
  - `git commit -m "feat(storage): implement in-memory scratch space manager"`

---

### Task 3: Ephemeral OverlayFS Mount Namespace Controller

**Files:**

- Create: `include/virmux/overlay.hpp`
- Create: `src/storage/overlay.cpp`
- Create: `tests/unit/test_overlay.cpp`
- Modify: `meson.build`

**Interfaces:**

```cpp
namespace virmux {

struct OverlayConfig {
  std::string lower_dir;
  std::string runtime_dir; // Default: /run/user/<UID>/virmux or /tmp/virmux
};

class OverlayWorkspace {
 public:
  explicit OverlayWorkspace(OverlayConfig config);
  ~OverlayWorkspace();

  OverlayWorkspace(const OverlayWorkspace&) = delete;
  OverlayWorkspace& operator=(const OverlayWorkspace&) = delete;
  OverlayWorkspace(OverlayWorkspace&& other) noexcept;
  OverlayWorkspace& operator=(OverlayWorkspace&& other) noexcept;

  bool setup();
  [[nodiscard]] const std::string& merged_dir() const noexcept { return merged_dir_; }
  [[nodiscard]] const std::string& upper_dir() const noexcept { return upper_dir_; }
  [[nodiscard]] const std::string& lower_dir() const noexcept { return config_.lower_dir; }
  [[nodiscard]] bool is_mounted() const noexcept { return mounted_; }
  bool mount_in_current_namespace();
  void teardown() noexcept;

 private:
  OverlayConfig config_;
  std::string session_dir_;
  std::string upper_dir_;
  std::string work_dir_;
  std::string merged_dir_;
  bool mounted_{false};
};

} // namespace virmux
```

- [ ] **Step 1: Write unit tests in `tests/unit/test_overlay.cpp`**
  - Test directory structure creation (`upper`, `work`, `merged`).
  - Test mounting overlay in user namespace using `unshare(CLONE_NEWUSER | CLONE_NEWNS)`.
  - Verify writes inside `merged` appear in `upper`, while `lower` remains strictly unmodified.
  - Test teardown: unmounts and removes `session_dir_` completely.

- [ ] **Step 2: Add test target to `meson.build`**
  - Add `src/storage/overlay.cpp` to `virmux_core_lib` and register `test_overlay`.

- [ ] **Step 3: Implement `include/virmux/overlay.hpp` and `src/storage/overlay.cpp`**
  - Configure `uid_map` and `gid_map` for unprivileged user namespaces.
  - Mount OverlayFS with options: `lowerdir=...,upperdir=...,workdir=...`.
  - Implement unmount with `umount2(merged_dir, MNT_DETACH)` and clean up temp directories.

- [ ] **Step 4: Run tests and verify**
  - Run `ninja -C build test` and ensure all tests pass.

- [ ] **Step 5: Commit**
  - `git commit -m "feat(storage): implement unprivileged OverlayFS workspace controller"`

---

### Task 4: Sandbox Integration & Execution in Overlay Workspace

**Files:**

- Modify: `include/virmux/sandbox.hpp`
- Modify: `src/core/sandbox.cpp`
- Create: `tests/unit/test_sandbox_overlay.cpp`
- Modify: `meson.build`

**Requirements:**

- Update `SandboxConfig` to support:
  - `bool ephemeral{false};`
  - `std::string workspace_dir;`
  - `std::string scratch_dir;`
- In `spawn_command`:
  - When `cfg.ephemeral` is true:
    - Pre-fork: initialize `OverlayWorkspace` over `cfg.workspace_dir`.
    - Setup `ScratchWorkspace` and inject `TMPDIR` into environment.
    - Fork child process:
      - Child calls `unshare(CLONE_NEWUSER | CLONE_NEWNS)`.
      - Mounts overlay.
      - Sets working directory to `overlay.merged_dir()`.
      - Executes command.
    - Parent waits for child exit code, preserves exit status.
    - Post-command: cleanses Page Cache on scratch workspace and tears down overlay.
- Ensure lower workspace remains 100% bitwise identical before and after command execution.

- [ ] **Step 1: Write unit tests in `tests/unit/test_sandbox_overlay.cpp`**
  - Create test fixture directory with base test files.
  - Run command under `spawn_command` with `ephemeral = true` that creates and modifies files.
  - Assert command succeeds and child observed new files.
  - Assert base fixture directory has ZERO new or modified files.

- [ ] **Step 2: Add test target to `meson.build`**
  - Register `test_sandbox_overlay` in `meson.build`.

- [ ] **Step 3: Update `include/virmux/sandbox.hpp` and `src/core/sandbox.cpp`**
  - Wire `OverlayWorkspace` and `ScratchWorkspace` into `spawn_command`.

- [ ] **Step 4: Run tests and verify**
  - Run `ninja -C build test` and ensure all tests pass.

- [ ] **Step 5: Commit**
  - `git commit -m "feat(core): integrate ephemeral overlay workspaces and scratch routing into sandbox"`

---

### Task 5: CLI Subcommands `virmux cas` and `virmux run --ephemeral`

**Files:**

- Modify: `cli/main.cpp`
- Create: `include/virmux/cli_cas.hpp`
- Create: `src/cli/cli_cas.cpp`
- Create: `tests/unit/test_cli_cas.cpp`
- Modify: `meson.build`

**Requirements:**

- Update `virmux run` argument parser:
  - `--ephemeral`: sets `cfg.ephemeral = true`.
  - `--workspace <path>`: sets `cfg.workspace_dir = path`.
  - `--scratch <path>`: sets `cfg.scratch_dir = path`.
- Add `virmux cas` subcommand:
  - `virmux cas stats [--json]`: prints CAS cache statistics (object count, disk space used, hits, misses).
  - `virmux cas clean [--max-bytes <size>]`: prunes cache objects exceeding size ceiling.
- Document options in `virmux --help` and `virmux run --help`.

- [ ] **Step 1: Write unit tests in `tests/unit/test_cli_cas.cpp`**
  - Test CLI argument parsing for `cas stats`, `cas stats --json`, and `cas clean`.
  - Test `virmux run --ephemeral` argument parsing.

- [ ] **Step 2: Add test target to `meson.build`**
  - Add `src/cli/cli_cas.cpp` to `virmux_core_lib` and register `test_cli_cas`.

- [ ] **Step 3: Implement `cli_cas.cpp` and update `cli/main.cpp`**
  - Wire `cas` subcommand into main CLI dispatcher.

- [ ] **Step 4: Run tests and verify**
  - Run `ninja -C build test` and ensure all tests pass.

- [ ] **Step 5: Commit**
  - `git commit -m "feat(cli): add virmux cas subcommand and --ephemeral flag to virmux run"`

---

### Task 6: End-to-End Integration Suite & Storage Verification

**Files:**

- Create: `tests/integration/test_ephemeral_workspaces.sh`
- Modify: `meson.build`

**Requirements:**

- Integration test script verifying:
  1. `virmux run --ephemeral --workspace /tmp/base -- touch new_file.txt`:
     - Verifies `new_file.txt` is NOT present in `/tmp/base` after command exit.
     - Verifies exit code 0.
  2. `virmux run --ephemeral -- python3 -c "import os; open('test.bin', 'wb').write(b'x'*1024*1024); assert os.path.exists('test.bin')"`:
     - Verifies command sees file in its ephemeral merged root.
     - Verifies host directory has no `test.bin`.
  3. `virmux cas stats` and `virmux cas stats --json`:
     - Validates JSON output via Python `json.load`.
  4. `virmux cas clean`:
     - Verifies cache prune runs with exit code 0.
- Register `integration_ephemeral_workspaces` in `meson.build` with `timeout: 120`.

- [ ] **Step 1: Write `tests/integration/test_ephemeral_workspaces.sh`**
- [ ] **Step 2: Register in `meson.build`**
- [ ] **Step 3: Run full test suite**
- [ ] **Step 4: Commit**
  - `git commit -m "test(integration): add ephemeral workspace isolation and CAS cache test suite"`
