# Virmux (Phase 2: Memory Packing Daemon & Heap Compaction) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build the proactive memory multiplexing subsystem for `virmux`: dynamic KSM auto-tuning, kernel heap compaction via `process_madvise(MADV_PAGEOUT/MADV_COLD)`, VMA range extraction from `/proc/[pid]/maps`, in-RAM zram compression telemetry, and user-facing `virmux stats` and `virmux pack` CLI subcommands.

**Architecture:** A modular C++20 memory management engine interfaces directly with Linux kernel memory APIs (`process_madvise`, `pidfd_open`, `/sys/kernel/mm/ksm/`, `/sys/block/zram*/`). The memory engine scans process VMAs, compacts idle heap pages into zram swap, dynamically calculates deduplication savings, and exposes live telemetry through CLI tables and JSON payloads.

**Tech Stack:** C++20, Linux Kernel Syscalls (`__NR_process_madvise`, `__NR_pidfd_open`), POSIX APIs (`madvise`, `sysinfo`), Meson, Ninja, Unity Test Harness, Bash.

**Spec:** `docs/specs/01-MEMORY-SUBSYSTEM.md`, `docs/specs/06-CLI-AND-TUI-SPEC.md`

## Global Constraints

- Language: C++20 for core engine and CLI; C ABI (`extern "C"`) for any interceptor additions.
- Compilers: GCC or Clang with `-Wall -Wextra -Wpedantic -std=c++20`.
- Target platform: Linux kernel 5.10+ (WSL2 Ubuntu 22.04+ and bare-metal Linux).
- Non-invasive execution: Target processes (Playwright, CMake, PyTorch) must execute completely unmodified.
- No absolute home paths: All paths in configs, scripts, and code must use relative paths, `~`, or `$HOME`.
- Unprivileged fallback: If KSM sysfs is read-only or `process_madvise` lacks root capabilities, degrade gracefully to `MADV_COLD` or telemetry-only without crashing or aborting.
- Page alignment: Every address passed to `process_madvise` must be strictly 4096-byte page aligned.

## Review Focus

1. **`process_madvise` Page Alignment & Address Validity:** Linux kernel rejects non-page-aligned `iovec` addresses with `EINVAL` (22) and unmapped addresses with `EFAULT` (14). VMA parser must clamp and page-align ranges from `/proc/[pid]/maps`.
2. **`IOV_MAX` Batching:** Linux kernel enforces a hard limit of `IOV_MAX` (1024) iovecs per `process_madvise` syscall. Processes with thousands of VMAs must be chunked into batches `<= 1024`.
3. **Dead / Exited PID Race Condition:** A process may exit between VMA inspection and `process_madvise`. `pidfd_open` or `process_madvise` returning `ESRCH`, `EINVAL`, or `EFAULT` must be handled cleanly without crashing.
4. **Unprivileged / Read-Only Sysfs:** On standard user accounts, `/sys/kernel/mm/ksm/run` and `/sys/block/zram0/` cannot be written without `sudo`. The KSM controller and zram manager must read metrics cleanly and report permission limits without aborting.
5. **JSON & Output Formatting Rigor:** `virmux stats --json` must output strictly valid JSON parsable by `jq` or `python3 -m json.tool` even when KSM or zram is disabled or unconfigured on the host.

---

### Task 1: KSM Controller & Sysfs Telemetry

**Files:**
- Create: `include/virmux/ksm.hpp`
- Create: `src/memory/ksm.cpp`
- Create: `tests/unit/test_ksm.cpp`
- Modify: `meson.build`

**Interfaces:**
```cpp
namespace virmux {

struct KsmStats {
  bool present{false};
  bool enabled{false};
  uint64_t pages_shared{0};
  uint64_t pages_sharing{0};
  uint64_t pages_unshared{0};
  uint64_t pages_volatile{0};
  uint32_t pages_to_scan{0};
  uint32_t sleep_millisecs{0};
  uint64_t bytes_saved{0};
};

class KsmController {
 public:
  explicit KsmController(std::string sysfs_root = "/sys/kernel/mm/ksm");
  [[nodiscard]] KsmStats read_stats() const;
  [[nodiscard]] bool is_available() const;
  [[nodiscard]] bool is_enabled() const;
  bool enable(bool activate = true);
  bool tune(uint32_t pages_to_scan, uint32_t sleep_millisecs);
  [[nodiscard]] uint32_t compute_adaptive_pages(size_t active_sessions) const;

 private:
  std::string sysfs_root_;
};

} // namespace virmux
```

- [ ] **Step 1: Write unit tests in `tests/unit/test_ksm.cpp`**
  - Test `read_stats` against mock sysfs directory structure (created in temporary folder via `mkdtemp`).
  - Verify `bytes_saved` equals `pages_sharing * sysconf(_SC_PAGESIZE)`.
  - Verify missing sysfs directory degrades to `present = false, enabled = false`.
  - Verify `compute_adaptive_pages` scales appropriately with session count.
  - Verify permission failures on `enable()` and `tune()` return `false` cleanly without exceptions.

- [ ] **Step 2: Add test target to `meson.build`**
  - Add `test_ksm_exe` and register `test('test_ksm', test_ksm_exe)`.
  - Run `ninja -C build test` to verify build failure (TDD).

- [ ] **Step 3: Implement `include/virmux/ksm.hpp` and `src/memory/ksm.cpp`**
  - Implement file reading helpers with defensive parsing (stripping whitespace, handling empty files).
  - Implement calculation of memory savings based on host page size.
  - Implement write operations with error logging on `EACCES`/`EROFS`.

- [ ] **Step 4: Run tests and verify**
  - Run `ninja -C build test` and ensure all tests pass.

- [ ] **Step 5: Commit**
  - `git commit -m "feat(memory): implement KSM controller and sysfs telemetry parser"`

---

### Task 2: Process Compactor via `process_madvise(2)`

**Files:**
- Create: `include/virmux/compactor.hpp`
- Create: `src/memory/compactor.cpp`
- Create: `tests/unit/test_compactor.cpp`
- Modify: `meson.build`

**Interfaces:**
```cpp
namespace virmux {

enum class ReclaimAdvice {
  Pageout,  // MADV_PAGEOUT (forces pages into swap/zram immediately)
  Cold,     // MADV_COLD (deactivates pages without immediate writeout)
  Collapse  // MADV_COLLAPSE (merges pages into transparent hugepages)
};

struct ReclaimResult {
  bool success{false};
  size_t bytes_advised{0};
  size_t ranges_count{0};
  int error_code{0};
};

class ProcessCompactor {
 public:
  ProcessCompactor() = default;

  [[nodiscard]] static bool is_supported() noexcept;

  [[nodiscard]] ReclaimResult reclaim_ranges(
      pid_t target_pid,
      const std::vector<std::pair<uintptr_t, size_t>>& ranges,
      ReclaimAdvice advice = ReclaimAdvice::Pageout);

  [[nodiscard]] ReclaimResult reclaim_memory(
      pid_t target_pid,
      ReclaimAdvice advice = ReclaimAdvice::Pageout);
};

} // namespace virmux
```

- [ ] **Step 1: Write unit tests in `tests/unit/test_compactor.cpp`**
  - Test self-reclaim: allocate 10 MB anonymous memory with `mmap`, dirty it, call `reclaim_ranges` with `ReclaimAdvice::Cold` and `ReclaimAdvice::Pageout`.
  - Verify `bytes_advised > 0` and `success == true`.
  - Test batching when passing more than `IOV_MAX` (1024) ranges.
  - Test invalid PID handling (e.g. `pid = -99999`) returning `success == false, error_code == ESRCH`.

- [ ] **Step 2: Add test target to `meson.build`**
  - Add `test_compactor_exe` and register `test('test_compactor', test_compactor_exe)`.
  - Run `ninja -C build test` to verify build failure.

- [ ] **Step 3: Implement `include/virmux/compactor.hpp` and `src/memory/compactor.cpp`**
  - Use `syscall(__NR_pidfd_open, target_pid, 0)` wrapped in `ScopedFd`.
  - Chunk `iovec` array into slices of at most `IOV_MAX` (1024).
  - Map `ReclaimAdvice` to kernel values (`MADV_PAGEOUT = 21`, `MADV_COLD = 20`, `MADV_COLLAPSE = 25`).
  - Implement graceful fallback: if `MADV_PAGEOUT` fails with `EPERM`, try `MADV_COLD`.

- [ ] **Step 4: Run tests and verify**
  - Run `ninja -C build test` and ensure all assertions pass.

- [ ] **Step 5: Commit**
  - `git commit -m "feat(memory): implement process compactor using process_madvise and pidfd_open"`

---

### Task 3: Memory Telemetry & `zram` Parser

**Files:**
- Create: `include/virmux/telemetry.hpp`
- Create: `src/memory/telemetry.cpp`
- Create: `tests/unit/test_telemetry.cpp`
- Modify: `meson.build`

**Interfaces:**
```cpp
namespace virmux {

struct ProcessMemoryStats {
  pid_t pid{0};
  uint64_t rss_bytes{0};
  uint64_t pss_bytes{0};
  uint64_t swap_bytes{0};
  uint64_t swap_pss_bytes{0};
  uint64_t referenced_bytes{0};
  uint64_t private_dirty_bytes{0};
};

struct ZramDeviceStats {
  bool present{false};
  std::string name;
  std::string algorithm;
  uint64_t disk_size{0};
  uint64_t orig_data_size{0};
  uint64_t compr_data_size{0};
  uint64_t mem_used_total{0};
  double compression_ratio{1.0};
};

struct HostMemoryStats {
  uint64_t mem_total_bytes{0};
  uint64_t mem_available_bytes{0};
  uint64_t mem_free_bytes{0};
  uint64_t cached_bytes{0};
  uint64_t swap_total_bytes{0};
  uint64_t swap_free_bytes{0};
  std::vector<ZramDeviceStats> zram_devices;
};

class MemoryTelemetry {
 public:
  explicit MemoryTelemetry(std::string procfs_root = "/proc",
                           std::string sysfs_block_root = "/sys/block");

  [[nodiscard]] ProcessMemoryStats read_process_stats(pid_t pid) const;
  [[nodiscard]] HostMemoryStats read_host_stats() const;
  [[nodiscard]] std::vector<ZramDeviceStats> read_zram_stats() const;
};

} // namespace virmux
```

- [ ] **Step 1: Write unit tests in `tests/unit/test_telemetry.cpp`**
  - Test `/proc/meminfo` parser with synthetic file fixture.
  - Test `/proc/[pid]/smaps_rollup` parser with mock file containing typical smaps fields.
  - Test `/sys/block/zram0/mm_stat` parser verifying computation of `compression_ratio` (`orig / compr`).
  - Test behavior on dead PID or non-existent zram device (returns zeros without crashing).

- [ ] **Step 2: Add test target to `meson.build`**
  - Add `test_telemetry_exe` and register `test('test_telemetry', test_telemetry_exe)`.
  - Run `ninja -C build test` to verify build failure.

- [ ] **Step 3: Implement `include/virmux/telemetry.hpp` and `src/memory/telemetry.cpp`**
  - Implement robust line-by-line parsing for `meminfo` and `smaps_rollup`.
  - Parse multi-column whitespace format of `mm_stat`.
  - Compute compression ratios safely guarding against divide-by-zero.

- [ ] **Step 4: Run tests and verify**
  - Run `ninja -C build test` and ensure all tests pass.

- [ ] **Step 5: Commit**
  - `git commit -m "feat(memory): implement host memory telemetry and zram statistics parser"`

---

### Task 4: VMA Scanner & Unified Memory Manager

**Files:**
- Create: `include/virmux/memory_manager.hpp`
- Create: `src/memory/memory_manager.cpp`
- Create: `tests/unit/test_memory_manager.cpp`
- Modify: `meson.build`

**Interfaces:**
```cpp
namespace virmux {

struct VmaRange {
  uintptr_t start{0};
  uintptr_t end{0};
  bool is_anonymous{false};
  bool is_writable{false};
};

class MemoryManager {
 public:
  explicit MemoryManager(std::string procfs_root = "/proc",
                         std::string ksm_root = "/sys/kernel/mm/ksm",
                         std::string sysfs_block_root = "/sys/block");

  [[nodiscard]] std::vector<VmaRange> scan_process_vmas(
      pid_t pid, bool anonymous_only = true) const;

  [[nodiscard]] ReclaimResult compact_process(
      pid_t pid, ReclaimAdvice advice = ReclaimAdvice::Pageout);

  [[nodiscard]] size_t compact_cgroup_procs(
      const std::string& cgroup_procs_path,
      ReclaimAdvice advice = ReclaimAdvice::Pageout);

  [[nodiscard]] KsmStats get_ksm_stats() const;
  [[nodiscard]] HostMemoryStats get_host_stats() const;

 private:
  std::string procfs_root_;
  KsmController ksm_;
  ProcessCompactor compactor_;
  MemoryTelemetry telemetry_;
};

} // namespace virmux
```

- [ ] **Step 1: Write unit tests in `tests/unit/test_memory_manager.cpp`**
  - Test `/proc/[pid]/maps` parsing: verify extracted address ranges are page-aligned and identify anonymous vs file-backed mappings.
  - Test `compact_process` against self and child test process.
  - Test `compact_cgroup_procs` using synthetic mock `cgroup.procs` file containing list of PIDs.
  - Verify error handling on unreadable or vanished processes.

- [ ] **Step 2: Add test target to `meson.build`**
  - Add `test_memory_manager_exe` and register `test('test_memory_manager', test_memory_manager_exe)`.
  - Run `ninja -C build test` to verify build failure.

- [ ] **Step 3: Implement `include/virmux/memory_manager.hpp` and `src/memory/memory_manager.cpp`**
  - Implement `/proc/[pid]/maps` parser extracting hex addresses `start-end` and permission flags.
  - Convert parsed VMAs to `(start, len)` pairs and pass to `ProcessCompactor`.
  - Read `cgroup.procs` line-by-line to compact all processes in a cgroup slice.

- [ ] **Step 4: Run tests and verify**
  - Run `ninja -C build test` and ensure all tests pass.

- [ ] **Step 5: Commit**
  - `git commit -m "feat(memory): implement VMA scanner and unified memory manager"`

---

### Task 5: Dynamic Preload Interceptor Memory Hook (`MADV_MERGEABLE`)

**Files:**
- Modify: `include/virmux/intercept.h`
- Modify: `src/intercept/interceptor.cpp`
- Create: `tests/unit/test_interceptor_mem.cpp`
- Modify: `meson.build`

**Requirements:**
- The dynamic interceptor library `libvirmux_intercept.so` must register heap allocations as KSM merge candidates when `VIRMUX_KSM_AUTO_MERGE=1` is enabled.
- Intercepts POSIX `mmap` / `mmap64` and flags `MAP_ANONYMOUS | MAP_PRIVATE` allocations with `madvise(addr, length, MADV_MERGEABLE)`.
- Includes reentrancy guard protection to avoid recursing during dynamic linker loading.

- [ ] **Step 1: Write unit test `tests/unit/test_interceptor_mem.cpp`**
  - Test calling `mmap` under `LD_PRELOAD=libvirmux_intercept.so` with `VIRMUX_KSM_AUTO_MERGE=1`.
  - Verify mmap returns valid memory, is writable, and does not deadlock.
  - Verify unsetting `VIRMUX_KSM_AUTO_MERGE` passes mmap through unmodified.

- [ ] **Step 2: Add test target to `meson.build`**
  - Add `test_interceptor_mem_exe` with `virmux_intercept_lib` dependency.
  - Run `ninja -C build test` to verify build failure.

- [ ] **Step 3: Update `src/intercept/interceptor.cpp`**
  - Add `mmap` and `mmap64` interception logic with `ReentrancyGuard`.
  - On anonymous mappings, invoke `madvise(res, length, MADV_MERGEABLE)`.

- [ ] **Step 4: Run tests and verify**
  - Run `ninja -C build test` and ensure 100% pass.

- [ ] **Step 5: Commit**
  - `git commit -m "feat(intercept): add proactive MADV_MERGEABLE registration in dynamic interceptor"`

---

### Task 6: CLI Subcommands `virmux stats` and `virmux pack`

**Files:**
- Create: `include/virmux/cli_stats.hpp`
- Create: `src/cli/cli_stats.cpp`
- Modify: `cli/main.cpp`
- Create: `tests/unit/test_cli_stats.cpp`
- Modify: `meson.build`

**Requirements:**
- `virmux stats`: Formatted terminal output detailing:
  - System memory (Total, Available, Free, Cached).
  - KSM deduplication (Status, Pages Shared, Pages Sharing, Memory Saved).
  - zram compression (Status, Original Size, Compressed Size, Ratio).
- `virmux stats --json`: Strict JSON representation matching the same telemetry.
- `virmux pack [PID...]`: Triggers immediate `MADV_PAGEOUT` on specified PIDs, or active `virmux` processes if omitted.

- [ ] **Step 1: Write unit tests in `tests/unit/test_cli_stats.cpp`**
  - Test JSON generator with dummy telemetry struct: verify valid JSON string.
  - Test table formatter: verify output contains human-readable byte units (KiB, MiB, GiB).
  - Test CLI argument parsing for `stats` and `pack`.

- [ ] **Step 2: Add test target to `meson.build`**
  - Add `test_cli_stats_exe` and register `test('test_cli_stats', test_cli_stats_exe)`.
  - Run `ninja -C build test` to verify build failure.

- [ ] **Step 3: Implement `include/virmux/cli_stats.hpp`, `src/cli/cli_stats.cpp`, and wire into `cli/main.cpp`**
  - Add `stats` and `pack` command dispatching in `cli/main.cpp`.
  - Implement JSON serialization and table printing helpers.

- [ ] **Step 4: Run tests and verify**
  - Run `ninja -C build test` and verify CLI help and execution tests.

- [ ] **Step 5: Commit**
  - `git commit -m "feat(cli): add virmux stats and virmux pack subcommands with JSON output"`

---

### Task 7: Integration Tests & Benchmark Verification

**Files:**
- Create: `tests/integration/test_memory_pack.sh`
- Modify: `meson.build`

**Requirements:**
- Automated integration test:
  1. `virmux stats` prints cleanly with exit code 0.
  2. `virmux stats --json | jq .` parses valid JSON with required keys (`memory`, `ksm`, `zram`).
  3. `virmux run --mem 512M -- python3 -c "import time; x = bytearray(50*1024*1024); time.sleep(0.5)"` allocates memory under `virmux`.
  4. Concurrently executes `virmux pack <pid>` and verifies zero crashes and successful pageout.

- [ ] **Step 1: Write `tests/integration/test_memory_pack.sh`**
  - Script exercises CLI commands against real running processes.

- [ ] **Step 2: Register in `meson.build`**
  - Add `integration_memory_pack` test with `timeout: 120`.

- [ ] **Step 3: Run full test suite**
  - Run `ninja -C build test` and verify 100% passing rate.

- [ ] **Step 4: Commit**
  - `git commit -m "test(integration): add memory compaction and telemetry integration suite"`
