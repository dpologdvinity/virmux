# Virmux — System Design Specification
## 04. Runtime Virtualization & Spoofing Subsystem

### 1. Architectural Objective

Target applications and runtimes (Python, Node.js, Chromium, PyTorch, CMake) are designed to inspect the host machine and greedily claim all available resources. The Runtime Virtualization Subsystem transparently intercepts these resource discovery queries and provides spoofed, polite answers without modifying a single line of project code.

```
+-------------------------------------------------------------------------+
|                  Target Process (e.g. Playwright / PyTorch)             |
|                                                                         |
|      sysconf(_SC_NPROCESSORS_ONLN)      open("/proc/meminfo")           |
|                  |                                   |                  |
+------------------|-----------------------------------|------------------+
                   |                                   |
+------------------v-----------------------------------v------------------+
|                   libvirmux_intercept.so (LD_PRELOAD)                   |
|                                                                         |
|   intercept sysconf():                intercept open():                 |
|   "Host has 12 cores?                 Redirects to synthetic            |
|    Return 2 cores."                   /tmp/virmux_meminfo (2 GB)        |
+------------------|-----------------------------------|------------------+
                   |                                   |
+------------------v-----------------------------------v------------------+
|   Result: Process spawns 2 threads    Result: Runtime sets internal GC  |
|   instead of 12 worker processes      heaps to 512MB instead of 10GB    |
+-------------------------------------------------------------------------+
```

---

### 2. Hardware Resource Spoofing (`libvirmux_intercept.so`)

#### Intercepted Interfaces

1. **Processor Count Queries:**
   ```c
   /* Intercepted sysconf */
   long sysconf(int name) {
       if (name == _SC_NPROCESSORS_ONLN || name == _SC_NPROCESSORS_CONF) {
           const char *env_cores = getenv("VIRMUX_VIRTUAL_CORES");
           if (env_cores) {
               long vcores = atol(env_cores);
               if (vcores > 0) return vcores; /* Return e.g. 2 */
           }
       }
       /* Call real libc sysconf */
       static long (*real_sysconf)(int) = NULL;
       if (!real_sysconf) real_sysconf = dlsym(RTLD_NEXT, "sysconf");
       return real_sysconf(name);
   }

   /* Intercepted get_nprocs */
   int get_nprocs(void) {
       const char *env_cores = getenv("VIRMUX_VIRTUAL_CORES");
       if (env_cores) return atoi(env_cores);
       static int (*real_get_nprocs)(void) = NULL;
       if (!real_get_nprocs) real_get_nprocs = dlsym(RTLD_NEXT, "get_nprocs");
       return real_get_nprocs();
   }
   ```

2. **Memory Information Queries (`/proc/meminfo`):**
   When target processes attempt to read `/proc/meminfo` to calculate heap limits:
   ```c
   int open(const char *pathname, int flags, ...) {
       if (pathname && strcmp(pathname, "/proc/meminfo") == 0) {
           const char *spoofed_path = getenv("VIRMUX_SPOOFED_MEMINFO");
           if (spoofed_path) {
               pathname = spoofed_path; /* Point to virtual 2GB meminfo */
           }
       }
       /* Call real open */
       ...
   }
   ```
   The synthetic `meminfo` provides balanced, proportional fields:
   ```
   MemTotal:        2097152 kB  (2 GB)
   MemFree:         1572864 kB  (1.5 GB)
   MemAvailable:    1835008 kB  (1.75 GB)
   SwapTotal:        524288 kB  (512 MB)
   SwapFree:         524288 kB  (512 MB)
   ```

3. **OpenMP & BLAS Automatic Injection:**
   `virmux` automatically injects environment variables into child sessions:
   ```bash
   export OMP_NUM_THREADS=2
   export OPENBLAS_NUM_THREADS=2
   export MKL_NUM_THREADS=2
   export NUMEXPR_NUM_THREADS=2
   ```
   This prevents PyTorch, NumPy, and scientific C/C++ libraries from overwhelming CPU execution units.

---

### 3. Headless Browser Trimming Engine (Playwright / Chromium)

#### The Problem
Modern end-to-end testing frameworks like Playwright spawn full Chromium instances. Even in headless mode, Chromium initializes:
- GPU emulation shaders and software rasterizers (`swiftshader`).
- Crash reporting daemons and metrics reporters.
- Background network discovery services.
- Multi-process architecture (separate browser, GPU, network, and renderer processes), eating **800 MB–1.2 GB per test runner**.

#### The Implementation
`virmux` monitors `execve` calls in session subtrees. When an executable matching `chrome`, `chromium`, or `headless_shell` is detected, `virmux` injects the **Ultra-Slim Headless Profile**:

```c
static const char *SLIM_BROWSER_FLAGS[] = {
    "--disable-gpu",
    "--disable-software-rasterizer",
    "--disable-dev-shm-usage",
    "--no-sandbox",
    "--single-process",            /* Combines GPU/network/renderer into 1 process */
    "--disable-extensions",
    "--disable-background-networking",
    "--disable-component-update",
    "--mute-audio",
    "--disable-gl-drawing-for-tests",
    "--js-flags=--max-old-space-size=256", /* Cages V8 engine to 256MB */
    NULL
};
```

#### Outcome
- Process count per browser instance drops from **7 processes down to 1 process**.
- Memory footprint drops from **~1 GB down to ~180 MB** (an **82% reduction**).
- 10 parallel Playwright tests run in **1.8 GB of RAM** instead of 10 GB.

---

### 4. GPU & VRAM Multi-Tenancy (NVIDIA MPS)

#### The Problem
By default, the CUDA runtime creates an exclusive context per process. If Session 1 runs a PyTorch training loop (`nibble`) and Session 2 attempts to run Blender or GPU test shaders:
- Session 1 preallocates almost all VRAM.
- Session 2 fails with: `CUDA out of memory` or `CUDA initialization error`.

#### The Implementation
`virmux` integrates with **NVIDIA Multi-Process Service (MPS)**:

1. **MPS Server Initialization:**
   `virmux` ensures `nvidia-cuda-mps-control -d` is running. MPS allows multiple CUDA processes to share a single GPU context concurrently.
2. **Per-Session VRAM Fraction Enforcement:**
   `virmux` injects environment boundaries:
   ```bash
   # Enforce max 20% of total GPU memory per session
   export CUDA_MPS_PINNED_DEVICE_MEM_LIMIT=0:2000M
   export PYTORCH_CUDA_ALLOC_CONF=max_split_size_mb:128
   ```
3. **Outcome:**
   Multiple sessions can execute neural network training and GPU-accelerated workloads concurrently on a single physical GPU without memory collisions.
