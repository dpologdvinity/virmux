# Virmux — System Design Specification

## 08. Tooling, Diagnostics & Developer Environment

### 1. Architectural Purpose

Developing a low-level Linux systems multiplexer in C that manipulates kernel primitives (`cgroups v2`, `process_madvise`, `KSM`, `epoll`, `OverlayFS`, `seccomp`, `LD_PRELOAD`) requires specialized toolchains for compilation, memory validation, dynamic tracing, static analysis, and stress benchmarking.

This document details the exact toolchain matrix, setup commands, diagnostic configurations, and commands for the developer and CI/CD environments.

---

### 2. Comprehensive Toolchain Matrix

| Category                  | Tool               | Binary / Package            | Purpose in `virmux`                                              | Status on Host |
| ------------------------- | ------------------ | --------------------------- | ---------------------------------------------------------------- | -------------- |
| **Compilers**             | GCC                | `/usr/bin/gcc`              | Primary C17 compiler with `-Wall -Wextra -Wpedantic`             | Installed      |
|                           | Clang              | `/usr/bin/clang`            | Secondary compiler & static analyzer                             | Installed      |
| **Build Systems**         | Meson              | `~/.local/bin/meson`        | Fast dependency resolution & build configuration                 | Installed      |
|                           | Ninja              | `/usr/bin/ninja`            | High-speed parallel build executor                               | Installed      |
| **Memory Validation**     | Valgrind           | `/usr/bin/valgrind`         | Heap leak detection (`memcheck`) & cache profiling               | Installed      |
|                           | AddressSanitizer   | Built into GCC/Clang        | In-process runtime memory error detection (`-fsanitize=address`) | Available      |
|                           | UndefinedSanitizer | Built into GCC/Clang        | Undefined behavior detection (`-fsanitize=undefined`)            | Available      |
| **Debuggers & Tracers**   | GDB                | `/usr/bin/gdb`              | Interactive C execution inspection & core dump analysis          | Installed      |
|                           | `strace`           | `strace`                    | Tracing syscalls (`process_madvise`, `mount`, `epoll`, `clone3`) | Installed      |
|                           | `ltrace`           | `ltrace`                    | Intercepted dynamic library call tracing                         | Installed      |
| **Static Analysis & LSP** | `clangd`           | `/usr/bin/clangd`           | C Language Server Protocol for editor auto-completion            | Installed      |
|                           | `clang-format`     | `~/.local/bin/clang-format` | Deterministic C17 code formatting                                | Installed      |
|                           | `clang-tidy`       | `~/.local/bin/clang-tidy`   | Modern static linting & code smell detection                     | Installed      |
|                           | `cppcheck`         | `/usr/bin/cppcheck`         | Static analysis for memory leaks & null dereferences             | Installed      |
| **Kernel & Sandboxing**   | `zramctl`          | `/usr/sbin/zramctl`         | Querying & configuring in-RAM compressed swap devices            | Installed      |
|                           | `libncurses-dev`   | `libncurses-dev`            | Header and libraries for `virmux top` TUI dashboard              | Installed      |
|                           | `libseccomp-dev`   | `libseccomp-dev`            | Kernel BPF syscall filtering headers                             | Installed      |
|                           | `criu`             | `criu`                      | Checkpoint/Restore In Userspace engine                           | Installed      |
| **Stress Benchmarks**     | `stress-ng`        | `stress-ng`                 | Generating CPU, memory, and I/O pressure for benchmarks          | Installed      |

---

### 3. Quick Installation Command

Run the following one-line command to install all recommended development and diagnostic packages:

```bash
sudo add-apt-repository -y ppa:criu/ppa && sudo apt update && sudo apt install -y \
  gcc \
  clang \
  meson \
  ninja-build \
  valgrind \
  gdb \
  strace \
  ltrace \
  clangd \
  clang-format \
  clang-tidy \
  cppcheck \
  util-linux \
  libncurses-dev \
  libseccomp-dev \
  criu \
  stress-ng
```

---

### 4. How Tools Are Used in `virmux` Workflows

#### A. Memory Leak & Pointer Bug Detection (ASan & Valgrind)

1. **AddressSanitizer (Preferred during development):**
   Compiled directly with Meson:

   ```bash
   meson setup build-asan -Db_sanitize=address,undefined
   ninja -C build-asan test
   ```

   _Detects:_ Use-after-free, buffer overflows, memory leaks, and stack misalignment with zero false positives at near-native speed.

2. **Valgrind Memcheck (Deep auditing):**
   ```bash
   valgrind --leak-check=full --show-leak-kinds=all --track-origins=yes \
       ./build/virmux run -- /bin/echo "test"
   ```

#### B. Syscall Tracing (`strace`)

Essential for verifying kernel interactions:

```bash
# Verify process_madvise(MADV_PAGEOUT) syscall execution
strace -e process_madvise,madvise,mount,pivot_root ./build/virmux run -- sleep 1

# Trace cgroup v2 file write events
strace -e openat,write -P /sys/fs/cgroup ./build/virmux run -- ls
```

#### C. Language Server & Compilation Database (`clangd`)

Meson automatically outputs `compile_commands.json` in the build directory. Symlink it to the repository root for instantaneous IDE completions:

```bash
ln -sf build/compile_commands.json compile_commands.json
```

#### D. Stress-Testing the Multiplexer (`stress-ng`)

Used in `bench/` to verify that `virmux` prevents system collapse under aggressive loads:

```bash
# Simulates 8 memory-thrashing workers and 8 CPU spinners inside virmux
virmux run -- stress-ng --vm 8 --vm-bytes 1G --cpu 8 --timeout 30s
```

---

### 5. Shell Automation & Utility Scripts (`scripts/`)

Low-level kernel configurations and automated testing harnesses are codified into reproducible Bash scripts:

1. **`scripts/setup-host.sh`:**
   - Detects cgroup v2 mount and enables delegation controllers (`cpu memory io cpuset`).
   - Enables and tunes KSM (`/sys/kernel/mm/ksm/run`).
   - Provisions `/dev/zram0` compressed RAM swap if unconfigured.
2. **`scripts/cleanup-sandboxes.sh`:**
   - Safety emergency script: unmounts any dangling OverlayFS mount points and wipes stale tmpfs directories in `/run/user/<UID>/virmux/`.
3. **`scripts/run-12-terminal-gauntlet.sh`:**
   - Spawns 12 concurrent background tasks (Playwright, CMake, model training, Docker) and measures host responsiveness and memory savings.
4. **`scripts/doctor.sh`:**
   - Health check script verifying kernel version (>=5.10), cgroups v2, KSM support, compiler versions, and user namespace permissions.

---

### 6. VS Code / Clangd Recommended Settings

Create `.clangd` in repository root:

```yaml
CompileFlags:
  CompilationDatabase: "build"
  Add:
    - "-Wall"
    - "-Wextra"
    - "-Wpedantic"
    - "-std=c17"
    - "-D_GNU_SOURCE"

Diagnostics:
  UnusedIncludes: Strict
  ClangTidy:
    Add:
      - "readability-*"
      - "bugprone-*"
      - "cert-*"
```
