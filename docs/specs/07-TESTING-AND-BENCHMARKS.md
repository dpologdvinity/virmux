# Virmux — System Design Specification

## 07. Testing, Verification & Benchmark Strategy

### 1. Architectural Objective

Because `virmux` interfaces directly with Linux kernel subsystems (cgroups v2, KSM, namespaces, `process_madvise`), rigorous automated verification is mandatory to prevent kernel crashes, lockups, memory leaks, and race conditions.

---

### 2. Testing Pyramid

```text
                / \
               /   \     End-to-End Stress Matrix (12-terminal concurrent loads)
              /-----\
             /       \   Integration Tests (cgroups, OverlayFS mounts, IPC)
            /---------\
           /           \ Unit Tests (Unity: parsers, token bucket, hash tables)
          +-------------+
```

---

### 3. Unit Test Suite (Unity Framework in C)

Located in `tests/unit/`:

1. **`test_admission_gate.c`:**
   - Tests thread-safe ticket acquisition and release.
   - Tests FIFO ordering when `active_heavy == max_concurrent`.
   - Tests timeouts and clean shutdown.
2. **`test_intercept_parser.c`:**
   - Verifies spoofed `sysconf` values for various `VIRMUX_VIRTUAL_CORES` env inputs.
   - Verifies synthetic `/proc/meminfo` formatting and accuracy.
3. **`test_ipc_codec.c`:**
   - Verifies TLV header serialization, deserialization, and boundary checks against malformed network packets.
4. **`test_cas_hash.c`:**
   - Verifies Blake3/SHA256 content-addressable hash stability across identical file contents.

---

### 4. Integration Verification Suite

Located in `tests/integration/`:

1. **`test_cgroup_lifecycle.sh`:**
   - Spawns a dummy process in a test cgroup.
   - Verifies `cgroup.freeze` halts CPU consumption to 0%.
   - Verifies thawing restores execution.
   - Verifies `memory.high` triggers throttling without an OOM kill.
2. **`test_overlay_isolation.sh`:**
   - Mounts an OverlayFS workspace over a test repository.
   - Writes 100 MB of random data in the upper layer.
   - Verifies the lower directory is untouched.
   - Unmounts and verifies scratch space clean deletion.
3. **`test_browser_trim_injection.sh`:**
   - Spawns a mock browser process.
   - Verifies `libvirmux_intercept.so` injects `--disable-gpu` and `--single-process` into `argv`.

---

### 5. Automated Benchmark & Stress Test Harness

Located in `bench/`:
`virmux-bench` runs a standardized 12-terminal stress test to measure real-world performance gains:

#### Benchmark Protocol: The "12-Terminal Gauntlet"

- **Workload:**
  - 4 sessions running `npx playwright test` (headless Chromium).
  - 4 sessions running CMake C++ compilations (`make -j`).
  - 2 sessions running Python model training loops (`nibble`).
  - 2 sessions running Docker container builds.
- **Comparison Matrix:**
  1. **Unmanaged Baseline:** Running all 12 tasks directly on WSL without `virmux`.
  2. **Virmux Managed:** Running all 12 tasks under `virmux`.

#### Key Metrics Tracked

| Metric | Expected Baseline | Expected With Virmux |
| --- | --- | --- |
| **Peak Host RAM (RSS)** | 28+ GB (100% swap saturation, thrashing) | **<10 GB** (KSM + zram active) |
| **Host Terminal Input Lag** | >800 ms (unusable UI freeze) | **<5 ms** (Core 0-1 shielding) |
| **Total Wall-Clock Completion** | 185 seconds (cache thrashing penalty) | **52 seconds** (Admission gate) |
| **OOM Termination Count** | 3–5 processes killed randomly | **0** processes killed |
