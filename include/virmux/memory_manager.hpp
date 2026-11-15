/**
 * @file memory_manager.hpp
 * @brief Unified memory manager coordinating VMA scanning, KSM, compaction, and
 * telemetry.
 */

#pragma once

#include <sys/types.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "virmux/compactor.hpp"
#include "virmux/ksm.hpp"
#include "virmux/telemetry.hpp"

namespace virmux {

/**
 * @brief Virtual Memory Area (VMA) range metadata from procfs maps.
 */
struct VmaRange {
  uintptr_t start{0};
  uintptr_t end{0};
  bool is_anonymous{false};
  bool is_writable{false};
  bool is_stack{false};
};

/**
 * @brief Unified memory manager coordinating VMA inspection, compaction, and
 * system telemetry.
 */
class MemoryManager {
 public:
  /**
   * @brief Construct a new MemoryManager instance.
   * @param procfs_root Root directory for procfs (defaults to "/proc").
   * @param ksm_root Root directory for KSM sysfs (defaults to
   * "/sys/kernel/mm/ksm").
   * @param sysfs_block_root Root directory for block sysfs (defaults to
   * "/sys/block").
   */
  explicit MemoryManager(std::string procfs_root = "/proc",
                         std::string ksm_root = "/sys/kernel/mm/ksm",
                         std::string sysfs_block_root = "/sys/block");

  /**
   * @brief Scan virtual memory areas for target process from /proc/<pid>/maps.
   * @param pid Target process ID.
   * @param anonymous_only If true, filter only anonymous memory areas.
   * @return Vector of valid, page-aligned VMA ranges.
   */
  [[nodiscard]] std::vector<VmaRange> scan_process_vmas(
      pid_t pid, bool anonymous_only = true) const;

  /**
   * @brief Advise and compact anonymous memory for target process.
   * @param pid Target process ID.
   * @param advice Reclaim advice mode (defaults to Pageout).
   * @return ReclaimResult containing status, bytes advised, range count, and
   * error code.
   */
  [[nodiscard]] ReclaimResult compact_process(
      pid_t pid, ReclaimAdvice advice = ReclaimAdvice::Pageout);

  /**
   * @brief Compact all processes listed in a cgroup.procs file.
   * @param cgroup_procs_path Path to cgroup.procs file.
   * @param advice Reclaim advice mode (defaults to Pageout).
   * @return Total number of successfully compacted processes.
   */
  [[nodiscard]] size_t compact_cgroup_procs(
      const std::string& cgroup_procs_path,
      ReclaimAdvice advice = ReclaimAdvice::Pageout);

  /**
   * @brief Read KSM telemetry statistics.
   * @return KsmStats snapshot.
   */
  [[nodiscard]] KsmStats get_ksm_stats() const;

  /**
   * @brief Read host memory and zram statistics.
   * @return HostMemoryStats snapshot.
   */
  [[nodiscard]] HostMemoryStats get_host_stats() const;

 private:
  std::string procfs_root_;
  KsmController ksm_;
  ProcessCompactor compactor_;
  MemoryTelemetry telemetry_;
};

}  // namespace virmux
