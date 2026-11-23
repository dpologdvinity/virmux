/**
 * @file hw_view.hpp
 * @brief The "smaller machine" a sandboxed command sees: memory limits and the
 * synthetic /proc and /sys files that describe them.
 *
 * Every value here is derived from the same inputs that configure the real
 * cgroup limits, so the spoofed view and the enforced limits always agree.
 */

#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "virmux/cpu_select.hpp"

namespace virmux {

/**
 * @brief Memory limits derived from a single `--mem` value.
 *
 * All fields are bytes; 0 means "no limit" (the field is not written).
 */
struct MemoryLimits {
  uint64_t max{0};       ///< memory.max: hard ceiling (OOM kill above it).
  uint64_t high{0};      ///< memory.high: reclaim/throttle point, 15/16 of max.
  uint64_t swap_max{0};  ///< memory.swap.max: max/4.
};

/**
 * @brief Derive the cgroup memory limits for a `--mem` value.
 * @param limit_bytes Requested memory ceiling in bytes (0 = unlimited).
 * @return memory.max = limit, memory.high = 15/16 of it, swap = limit/4.
 */
[[nodiscard]] MemoryLimits compute_memory_limits(uint64_t limit_bytes);

/**
 * @brief Format a synthetic /proc/meminfo matching @p limits.
 *
 * MemTotal is memory.max and SwapTotal is memory.swap.max, so tools that
 * size themselves from /proc/meminfo see exactly what the cgroup enforces.
 * @param limits Limits from compute_memory_limits().
 * @return meminfo text.
 */
[[nodiscard]] std::string generate_synthetic_meminfo(
    const MemoryLimits& limits);

/**
 * @brief Filter a /proc/cpuinfo text down to the CPUs in @p cpus.
 *
 * cpuinfo is a series of blank-line-separated stanzas, one per CPU, each
 * starting with "processor : N". Stanzas for other CPUs are dropped; stanzas
 * without a processor line (arm64's trailing "Hardware" section) are kept.
 * Processor numbers are not renumbered, so they match the affinity mask.
 * @param real Contents of the real /proc/cpuinfo.
 * @param cpus CPUs to keep (empty keeps everything).
 * @return Filtered cpuinfo text.
 */
[[nodiscard]] std::string filter_cpuinfo(const std::string& real,
                                         const CpuList& cpus);

/**
 * @brief Synthetic /sys/devices/system/cpu/online for @p cpus ("2-3\n").
 */
[[nodiscard]] std::string format_online(const CpuList& cpus);

/// getenv-compatible lookup, injectable for tests.
using GetenvFn = const char* (*)(const char*);

/**
 * @brief Thread-count environment variables for a run limited to @p n CPUs.
 *
 * Covers OpenMP, OpenBLAS, MKL, NumExpr, Go (GOMAXPROCS), Rayon, Cargo, CMake
 * and Python 3.13+ (PYTHON_CPU_COUNT). Policy "at most N": a user's value is
 * kept if it is a positive integer no larger than @p n (asking for fewer
 * threads is fine); anything else (unset, larger, junk) becomes @p n.
 * MAKEFLAGS and UV_THREADPOOL_SIZE are deliberately left alone: the first
 * mixes job counts with other flags, the second sizes an I/O pool.
 * @param n CPU count of the run (values below 1 are treated as 1).
 * @param getenv_fn Environment lookup (std::getenv in production).
 * @return (name, value) pairs to set in the child.
 */
[[nodiscard]] std::vector<std::pair<std::string, std::string>>
compute_thread_env(int n, GetenvFn getenv_fn);

}  // namespace virmux
