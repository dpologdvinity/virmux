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

}  // namespace virmux
