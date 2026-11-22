/**
 * @file cpu_select.hpp
 * @brief CPU list parsing and the choice of which CPUs a sandbox runs on.
 *
 * CPU lists use the kernel's cpulist format ("0-3,8,10-11"), as found in
 * /sys/devices/system/cpu/online, cpuset.cpus and Cpus_allowed_list.
 */

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace virmux {

/// Sorted, duplicate-free list of CPU numbers.
using CpuList = std::vector<int>;

/// Highest CPU number accepted by parse_cpu_list (the kernel's NR_CPUS cap).
inline constexpr int kMaxCpuId = 8191;

/**
 * @brief Parse a kernel cpulist such as "0-3,8,10-11".
 *
 * Surrounding whitespace (sysfs files end in '\n') is ignored.
 * @param text cpulist text.
 * @return Sorted, de-duplicated CPU numbers, or std::nullopt if @p text is
 * empty or malformed (reversed ranges, empty items, non-digits, ids above
 * kMaxCpuId).
 */
[[nodiscard]] std::optional<CpuList> parse_cpu_list(std::string_view text);

/**
 * @brief Format CPUs as a compact cpulist ("0-3,8,10-11").
 * @param cpus Sorted, duplicate-free CPU numbers.
 * @return cpulist text ("" for an empty list).
 */
[[nodiscard]] std::string format_cpu_list(const CpuList& cpus);

/**
 * @brief CPUs this process may run on: online CPUs intersected with the
 * calling thread's affinity mask.
 *
 * Respecting the inherited mask means a virmux started under `taskset` (or
 * inside a container's cpuset) never pins a child outside it.
 * @return Available CPUs; falls back to the affinity mask alone if the online
 * list cannot be read, and is empty only if both sources fail.
 */
[[nodiscard]] CpuList available_cpus();

/**
 * @brief Choose @p n CPUs for a sandbox from @p avail.
 *
 * With four or more available CPUs the two lowest are kept free for the
 * interactive shell, unless @p n needs them. The choice is a contiguous
 * window (over pool positions, wrapping around) whose start rotates with
 * @p rotation so concurrent sandboxes spread out. If @p load is given
 * (indexed by CPU number, missing entries count as 0), the least-loaded
 * window wins, ties going to the earliest window in rotation order.
 *
 * @param avail Available CPUs (from available_cpus()).
 * @param n Requested CPU count; clamped to [1, avail.size()].
 * @param rotation Window start offset, taken modulo the pool size.
 * @param load Optional per-CPU load (reserved for least-loaded placement).
 * @return Sorted selection; empty only when @p avail is empty.
 */
[[nodiscard]] CpuList select_cpus(const CpuList& avail, int n,
                                  uint64_t rotation,
                                  const std::vector<double>& load);

}  // namespace virmux
