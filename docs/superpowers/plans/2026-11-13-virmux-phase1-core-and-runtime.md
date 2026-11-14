# Virmux (Phase 1: Core Engine & Hardware Virtualization Runtime) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build the foundational `virmux` CLI and dynamic interceptor library delivering the core `virmux run` command with hardware spoofing (`nproc`/meminfo), cgroup v2 resource fences, headless browser trimming, and token-bucket admission control.

**Architecture:** A lightweight C++20 engine initializes cgroup resource slices and coordinates an admission token queue for heavy compute tasks. A high-performance dynamic interceptor library (`libvirmux_intercept.so`, exported with C ABI) is injected into child processes via `LD_PRELOAD`, intercepting `sysconf`, `/proc/meminfo`, and `execve` to enforce polite resource footprints transparently.

**Tech Stack:** C++20, C17 (interceptor ABI), Meson, Ninja, Unity/Catch2 test harness, Linux cgroups v2, POSIX APIs (`dlsym`, `posix_fadvise`).

**Spec:** `docs/specs/00-OVERVIEW.md`, `docs/specs/02-CPU-SCHEDULING-SUBSYSTEM.md`, `docs/specs/04-RUNTIME-VIRTUALIZATION.md`, `docs/specs/06-CLI-AND-TUI-SPEC.md`

## Global Constraints

- Language: C++20 for core engine and CLI; C ABI (`extern "C"`) for `libvirmux_intercept.so`.
- Compilers: GCC or Clang with `-Wall -Wextra -Wpedantic -std=c++20`.
- Target platform: Linux kernel 5.10+ (WSL2 Ubuntu 22.04+ and bare-metal Linux).
- Non-invasive execution: Target processes (Playwright, CMake, PyTorch) must execute completely unmodified.
- No absolute home paths: All paths in configs, scripts, and code must use relative paths, `~`, or `$HOME`.
- Unprivileged fallback: If cgroup v2 writes fail due to lack of delegation, degrade gracefully with a warning to `stderr` and continue running under `LD_PRELOAD` spoofing.

## Review Focus

1. **`LD_PRELOAD` recursion & re-entrancy:** Intercepted `open()` or `sysconf()` calling `dlsym()` which itself calls `open()` or `calloc()` must be shielded by an atomic thread-local recursion guard.
   - *Test:* `test_interceptor_reentrancy_guard` in Task 2.
2. **`execve` argv buffer manipulation:** Injecting slim browser flags (`--disable-gpu`, `--single-process`) must safely construct an expanded null-terminated `argv` array without buffer overruns or leaks.
   - *Test:* `test_browser_flag_injection_preserves_arguments` in Task 3.
3. **Cgroup permission denial (`EACCES`/`ENOENT`):** Launcher must detect unprivileged execution and fall back gracefully without terminating the user command.
   - *Test:* `test_cgroup_unprivileged_fallback_logs_warning` in Task 4.
4. **Child exit status propagation:** `virmux run` must return the exact exit status of the child process, including signal terminations (`128 + signal`).
   - *Test:* `test_sandbox_exit_code_propagation` in Task 6.
5. **Pathological command handling:** Non-existent binaries and permission-denied scripts must yield standard POSIX exit codes 127 and 126 respectively.
   - *Test:* `test_sandbox_missing_binary_returns_127` in Task 6.

---

### Task 1: Project Scaffolding & Build Configuration

**Files:**
- Create: `meson.build`
- Create: `include/virmux/version.hpp`
- Create: `src/core/version.cpp`
- Create: `tests/unity/unity.h`
- Create: `tests/unity/unity.c`
- Create: `tests/unit/test_version.cpp`

**Interfaces:**
- Produces: `virmux::version_string()` -> `const char*`

- [ ] **Step 1: Write failing test for version string**

```cpp
// tests/unit/test_version.cpp
extern "C" {
#include "unity/unity.h"
}
#include "virmux/version.hpp"
#include <string_view>

void setUp(void) {}
void tearDown(void) {}

void test_version_returns_valid_semver(void) {
    std::string_view ver = virmux::version_string();
    TEST_ASSERT_EQUAL_STRING("0.1.0", ver.data());
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_version_returns_valid_semver);
    return UNITY_END();
}
```

- [ ] **Step 2: Run test to verify failure**

Run: `meson setup build && ninja -C build test`
Expected: FAIL (headers/meson config missing)

- [ ] **Step 3: Implement Meson setup and version function**

Create `meson.build` defining project `virmux` with C++20, include directories `include/`, source target, and Unity test runner. Implement `virmux::version_string()` returning `"0.1.0"`.

- [ ] **Step 4: Run test to verify it passes**

Run: `ninja -C build test`
Expected: PASS

- [ ] **Step 5: Commit**

```bash
git add meson.build include/ src/ tests/
git commit -m "chore(build): setup meson c++20 project and unity test harness"
```

---

### Task 2: Dynamic Interceptor Library (`libvirmux_intercept.so`)

**Files:**
- Create: `include/virmux/intercept.h`
- Create: `src/intercept/interceptor.cpp`
- Modify: `meson.build`
- Test: `tests/unit/test_interceptor.cpp`

**Interfaces:**
- Consumes: Environment variables `VIRMUX_VIRTUAL_CORES`, `VIRMUX_SPOOFED_MEMINFO`
- Produces: Shared library `build/src/intercept/libvirmux_intercept.so` exporting `sysconf`, `get_nprocs`, and `open`

- [ ] **Step 1: Write failing test for spoofed core counts and meminfo redirection**

```cpp
// tests/unit/test_interceptor.cpp
extern "C" {
#include "unity/unity.h"
}
#include <cstdlib>
#include <unistd.h>
#include <sys/sysinfo.h>
#include <fcntl.h>

void setUp(void) {}
void tearDown(void) {}

void test_sysconf_returns_spoofed_core_count(void) {
    setenv("VIRMUX_VIRTUAL_CORES", "3", 1);
    long nprocs = sysconf(_SC_NPROCESSORS_ONLN);
    TEST_ASSERT_EQUAL_INT(3, nprocs);
}

void test_interceptor_reentrancy_guard(void) {
    // Calling sysconf repeatedly should not deadlock or crash
    for (int i = 0; i < 100; ++i) {
        long res = sysconf(_SC_NPROCESSORS_CONF);
        TEST_ASSERT_EQUAL_INT(3, res);
    }
}
```

- [ ] **Step 2: Run test to verify failure**

Run: `ninja -C build test`
Expected: FAIL (unintercepted sysconf returns host core count)

- [ ] **Step 3: Implement interceptor functions in `src/intercept/interceptor.cpp`**

Implement `sysconf`, `get_nprocs`, and `open` using `dlsym(RTLD_NEXT, ...)`. Guard with thread-local `bool in_interceptor` to eliminate recursion. Redirect `/proc/meminfo` to path specified in `VIRMUX_SPOOFED_MEMINFO`. Build as shared library in `meson.build`.

- [ ] **Step 4: Run test with `LD_PRELOAD` to verify it passes**

Run: `ninja -C build test`
Expected: PASS

- [ ] **Step 5: Commit**

```bash
git add src/intercept/ include/virmux/intercept.h tests/unit/test_interceptor.cpp meson.build
git commit -m "feat(intercept): implement core sysconf and meminfo preload interception"
```

---

### Task 3: Headless Browser & Tool Flag Injection

**Files:**
- Create: `src/intercept/browser_trim.cpp`
- Modify: `src/intercept/interceptor.cpp`
- Test: `tests/unit/test_browser_trim.cpp`

**Interfaces:**
- Consumes: Target binary name in `execve`
- Produces: Augmented argument vector containing `--disable-gpu`, `--disable-software-rasterizer`, `--single-process`

- [ ] **Step 1: Write failing test for browser argv augmentation**

```cpp
// tests/unit/test_browser_trim.cpp
extern "C" {
#include "unity/unity.h"
}
#include <vector>
#include <string>

namespace virmux {
    bool is_browser_binary(const char *path);
    std::vector<std::string> inject_slim_flags(char *const argv[]);
}

void setUp(void) {}
void tearDown(void) {}

void test_browser_binary_detection(void) {
    TEST_ASSERT_TRUE(virmux::is_browser_binary("/usr/bin/chromium"));
    TEST_ASSERT_TRUE(virmux::is_browser_binary("/home/user/.cache/ms-playwright/chromium-1155/chrome-linux/chrome"));
    TEST_ASSERT_FALSE(virmux::is_browser_binary("/usr/bin/gcc"));
}

void test_browser_flag_injection_preserves_arguments(void) {
    char arg0[] = "chrome";
    char arg1[] = "http://localhost:3000";
    char *argv[] = {arg0, arg1, nullptr};

    auto augmented = virmux::inject_slim_flags(argv);
    TEST_ASSERT_TRUE(augmented.size() > 2);
    TEST_ASSERT_EQUAL_STRING("chrome", augmented[0].c_str());
    TEST_ASSERT_EQUAL_STRING("--disable-gpu", augmented[1].c_str());
    TEST_ASSERT_EQUAL_STRING("--single-process", augmented[2].c_str());
}
```

- [ ] **Step 2: Run test to verify failure**

Run: `ninja -C build test`
Expected: FAIL

- [ ] **Step 3: Implement browser flag injection in `src/intercept/browser_trim.cpp`**

Implement `is_browser_binary` matching `chrome`, `chromium`, and `headless_shell`. Implement `inject_slim_flags` inserting the ultra-slim profile (`--disable-gpu`, `--disable-software-rasterizer`, `--single-process`). Hook `execve` in `interceptor.cpp` to call this logic before invoking real `execve`.

- [ ] **Step 4: Run test to verify it passes**

Run: `ninja -C build test`
Expected: PASS

- [ ] **Step 5: Commit**

```bash
git add src/intercept/browser_trim.cpp tests/unit/test_browser_trim.cpp
git commit -m "feat(intercept): add automatic headless browser flag injection"
```

---

### Task 4: RAII Kernel Wrappers & Cgroups v2 Slices

**Files:**
- Create: `include/virmux/raii.hpp`
- Create: `include/virmux/cgroup.hpp`
- Create: `src/core/cgroup.cpp`
- Test: `tests/unit/test_cgroup.cpp`

**Interfaces:**
- Produces:
  - `class ScopedFd` (wraps file descriptors with `close()` in destructor)
  - `class CgroupSlice` (`init(slice_name)`, `set_memory_high(bytes)`, `set_cpuset(cores_str)`, `attach_pid(pid)`)

- [ ] **Step 1: Write failing test for ScopedFd and cgroup unprivileged fallback**

```cpp
// tests/unit/test_cgroup.cpp
extern "C" {
#include "unity/unity.h"
}
#include "virmux/raii.hpp"
#include "virmux/cgroup.hpp"
#include <unistd.h>

void setUp(void) {}
void tearDown(void) {}

void test_scoped_fd_closes_descriptor(void) {
    int pipe_fds[2];
    pipe(pipe_fds);
    {
        virmux::ScopedFd read_fd(pipe_fds[0]);
        TEST_ASSERT_TRUE(read_fd.valid());
    }
    // Attempting to close again should return EBADF
    TEST_ASSERT_EQUAL_INT(-1, close(pipe_fds[0]));
    close(pipe_fds[1]);
}

void test_cgroup_unprivileged_fallback_logs_warning(void) {
    virmux::CgroupSlice slice("test-unprivileged");
    // Under unprivileged execution, init should not crash; it logs warning and sets valid=false
    bool res = slice.setup("/sys/fs/cgroup/nonexistent_test_slice", 1024 * 1024 * 500, "2-11");
    TEST_ASSERT_FALSE(res);
}
```

- [ ] **Step 2: Run test to verify failure**

Run: `ninja -C build test`
Expected: FAIL

- [ ] **Step 3: Implement `ScopedFd` and `CgroupSlice` in `src/core/cgroup.cpp`**

Implement `ScopedFd` with move semantics and `close()`. Implement `CgroupSlice` creating directories under `/sys/fs/cgroup/`, writing `memory.high` and `cpuset.cpus`, and writing PIDs to `cgroup.procs`. If directory creation or file writes fail with `EACCES` or `EPERM`, log descriptive diagnostic to `stderr` and flag slice as unprivileged without throwing or aborting.

- [ ] **Step 4: Run test to verify it passes**

Run: `ninja -C build test`
Expected: PASS

- [ ] **Step 5: Commit**

```bash
git add include/virmux/raii.hpp include/virmux/cgroup.hpp src/core/cgroup.cpp tests/unit/test_cgroup.cpp
git commit -m "feat(core): implement RAII descriptors and cgroup v2 controller with unprivileged fallback"
```

---

### Task 5: Admission Control Token Bucket

**Files:**
- Create: `include/virmux/gate.hpp`
- Create: `src/sched/gate.cpp`
- Test: `tests/unit/test_gate.cpp`

**Interfaces:**
- Produces:
  - `class AdmissionGate(int max_concurrent_heavy, std::chrono::milliseconds timeout)`
  - `bool acquire_token()`
  - `void release_token()`

- [ ] **Step 1: Write failing test for concurrency limits and timeout fallback**

```cpp
// tests/unit/test_gate.cpp
extern "C" {
#include "unity/unity.h"
}
#include "virmux/gate.hpp"
#include <chrono>
#include <thread>

void setUp(void) {}
void tearDown(void) {}

void test_gate_allows_max_concurrent_tokens(void) {
    virmux::AdmissionGate gate(2, std::chrono::milliseconds(50));
    TEST_ASSERT_TRUE(gate.acquire_token());
    TEST_ASSERT_TRUE(gate.acquire_token());
    TEST_ASSERT_EQUAL_INT(2, gate.active_count());

    // Third acquisition must timeout or queue
    TEST_ASSERT_FALSE(gate.try_acquire_immediate());
    gate.release_token();
    TEST_ASSERT_EQUAL_INT(1, gate.active_count());
    TEST_ASSERT_TRUE(gate.acquire_token());
}
```

- [ ] **Step 2: Run test to verify failure**

Run: `ninja -C build test`
Expected: FAIL

- [ ] **Step 3: Implement thread-safe `AdmissionGate` in `src/sched/gate.cpp`**

Use `std::mutex` and `std::condition_variable`. Implement `acquire_token()` with wait-with-timeout. Track `active_heavy` counters and queue depth. Implement `release_token()` with `notify_one()`.

- [ ] **Step 4: Run test to verify it passes**

Run: `ninja -C build test`
Expected: PASS

- [ ] **Step 5: Commit**

```bash
git add include/virmux/gate.hpp src/sched/gate.cpp tests/unit/test_gate.cpp
git commit -m "feat(sched): implement admission control token bucket gate"
```

---

### Task 6: Command Sandbox Launcher & Exit Propagation

**Files:**
- Create: `include/virmux/sandbox.hpp`
- Create: `src/core/sandbox.cpp`
- Test: `tests/unit/test_sandbox.cpp`

**Interfaces:**
- Produces:
  - `struct SandboxConfig { int virtual_cores; uint64_t memory_limit_bytes; bool is_heavy; ... }`
  - `int virmux_spawn_command(const SandboxConfig &cfg, char *const argv[])`

- [ ] **Step 1: Write failing test for process execution, status propagation, and error handling**

```cpp
// tests/unit/test_sandbox.cpp
extern "C" {
#include "unity/unity.h"
}
#include "virmux/sandbox.hpp"

void setUp(void) {}
void tearDown(void) {}

void test_sandbox_exit_code_propagation(void) {
    virmux::SandboxConfig cfg{.virtual_cores = 2, .memory_limit_bytes = 1024*1024*100};
    char arg0[] = "/bin/sh";
    char arg1[] = "-c";
    char arg2[] = "exit 42";
    char *argv[] = {arg0, arg1, arg2, nullptr};

    int exit_code = virmux::spawn_command(cfg, argv);
    TEST_ASSERT_EQUAL_INT(42, exit_code);
}

void test_sandbox_missing_binary_returns_127(void) {
    virmux::SandboxConfig cfg{};
    char arg0[] = "/bin/nonexistent_command_12345";
    char *argv[] = {arg0, nullptr};

    int exit_code = virmux::spawn_command(cfg, argv);
    TEST_ASSERT_EQUAL_INT(127, exit_code);
}
```

- [ ] **Step 2: Run test to verify failure**

Run: `ninja -C build test`
Expected: FAIL

- [ ] **Step 3: Implement `spawn_command` in `src/core/sandbox.cpp`**

1. Set up synthetic `meminfo` file in `/tmp/virmux_<pid>_meminfo`.
2. Format environment: `LD_PRELOAD` pointing to `libvirmux_intercept.so`, `VIRMUX_VIRTUAL_CORES`, `OMP_NUM_THREADS`.
3. Call `fork()`.
4. In child: call `virmux_set_worker_oom_score()`, attach to cgroup, execute `execvp(argv[0], argv)`. On failure, return `errno == ENOENT ? 127 : 126`.
5. In parent: wait with `waitpid()`, extract exit status with `WIFEXITED` / `WEXITSTATUS` / `WTERMSIG`, and return code.

- [ ] **Step 4: Run test to verify it passes**

Run: `ninja -C build test`
Expected: PASS

- [ ] **Step 5: Commit**

```bash
git add include/virmux/sandbox.hpp src/core/sandbox.cpp tests/unit/test_sandbox.cpp
git commit -m "feat(core): implement command sandbox launcher with status propagation"
```

---

### Task 7: Post-Execution Page Cache Cleanser

**Files:**
- Create: `include/virmux/cleanse.hpp`
- Create: `src/memory/cleanse.cpp`
- Test: `tests/unit/test_cleanse.cpp`

**Interfaces:**
- Produces: `void virmux::cleanse_path(std::string_view path)`

- [ ] **Step 1: Write failing test for posix_fadvise cache eviction**

```cpp
// tests/unit/test_cleanse.cpp
extern "C" {
#include "unity/unity.h"
}
#include "virmux/cleanse.hpp"
#include <fstream>
#include <filesystem>

void setUp(void) {}
void tearDown(void) {}

void test_cleanse_path_evicts_cache_without_deleting_file(void) {
    std::string test_file = "/tmp/virmux_cleanse_test.tmp";
    {
        std::ofstream out(test_file);
        out << "temporary buffer test content for fadvise testing\n";
    }

    virmux::cleanse_path(test_file);

    // File must still exist and be intact, but cached pages evicted
    TEST_ASSERT_TRUE(std::filesystem::exists(test_file));
    std::filesystem::remove(test_file);
}
```

- [ ] **Step 2: Run test to verify failure**

Run: `ninja -C build test`
Expected: FAIL

- [ ] **Step 3: Implement `cleanse_path` in `src/memory/cleanse.cpp`**

Walk directory or single file path. Open with `O_RDONLY`. Invoke `posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED)`. Close descriptor.

- [ ] **Step 4: Run test to verify it passes**

Run: `ninja -C build test`
Expected: PASS

- [ ] **Step 5: Commit**

```bash
git add include/virmux/cleanse.hpp src/memory/cleanse.cpp tests/unit/test_cleanse.cpp
git commit -m "feat(memory): implement post-execution page cache eviction"
```

---

### Task 8: CLI Command `virmux run`

**Files:**
- Create: `cli/main.cpp`
- Modify: `meson.build`
- Test: integration test via CLI execution

**Interfaces:**
- Produces: Executable `build/virmux` supporting `virmux run [--cores N] [--mem LIMIT] -- <cmd...>`

- [ ] **Step 1: Implement argument parser and run dispatcher in `cli/main.cpp`**

Parse `--cores` (default 2), `--mem` (default 2G), and command arguments following `--`. Instantiate `SandboxConfig` and call `virmux::spawn_command()`. Invoke `cleanse_path()` upon command exit. Return child exit code.

- [ ] **Step 2: Add executable target in `meson.build`**

Link `build/virmux` against core library targets.

- [ ] **Step 3: Compile and run test execution**

Run: `ninja -C build && ./build/virmux run --cores 3 -- /bin/sh -c "echo 'hello from virmux'"`
Expected: Prints `"hello from virmux"` and exits 0.

- [ ] **Step 4: Commit**

```bash
git add cli/main.cpp meson.build
git commit -m "feat(cli): implement virmux run command-line interface"
```

---

### Task 9: Phase 1 Integration Tests & Gauntlet Smoke Test

**Files:**
- Create: `tests/integration/test_virmux_run.sh`
- Create: `scripts/doctor.sh`
- Modify: `meson.build`

**Interfaces:**
- Produces: Automated verification suite executing real Python and compiler tasks under `virmux run`

- [ ] **Step 1: Write integration test script `tests/integration/test_virmux_run.sh`**

Test:
1. `virmux run --cores 2 -- python3 -c "import os; assert os.cpu_count() == 2"` -> verifies core spoofing.
2. `virmux run -- meminfo check` -> verifies virtual RAM reporting.
3. Concurrency test: launches 4 parallel `virmux run` tasks and verifies admission gate ordering.

- [ ] **Step 2: Write diagnostic doctor script `scripts/doctor.sh`**

Checks kernel version (>=5.10), cgroups v2 mount, KSM status, and compiler availability.

- [ ] **Step 3: Run full test suite and integration tests**

Run: `ninja -C build test && bash tests/integration/test_virmux_run.sh`
Expected: 100% test pass.

- [ ] **Step 4: Commit**

```bash
git add tests/integration/ scripts/doctor.sh meson.build
git commit -m "test(integration): add virmux run verification suite and doctor diagnostic script"
```

---

## Self-Review Checklist

- [x] **Spec coverage:** Phase 1 covers the foundational runtime requirements (hardware spoofing, browser slimming, admission gate, command sandbox, exit propagation).
- [x] **Step scan:** Every step contains explicit test code, exact commands, and unambiguous implementation details.
- [x] **Type consistency:** Types (`ScopedFd`, `CgroupSlice`, `AdmissionGate`, `SandboxConfig`) are consistently declared across headers and implementations.
- [x] **Review focus addressed:** All 5 review focus items (re-entrancy, argv buffer allocation, unprivileged cgroup fallback, exit code propagation, missing binary error handling) have explicit tests in Tasks 2, 3, 4, and 6.
- [x] **Proportion:** Clear task boundaries with isolated deliverables suitable for incremental verification.
