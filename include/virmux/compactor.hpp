#pragma once

#include <cstddef>
#include <cstdint>
#include <sys/types.h>
#include <utility>
#include <vector>

namespace virmux {

/**
 * @brief Memory reclaim advice types for process_madvise.
 */
enum class ReclaimAdvice {
  Pageout,  // MADV_PAGEOUT (forces inactive pages into swap/zram immediately)
  Cold,     // MADV_COLD (deactivates pages without immediate writeout)
  Collapse  // MADV_COLLAPSE (merges pages into transparent hugepages)
};

/**
 * @brief Result of a memory reclaim operation.
 */
struct ReclaimResult {
  bool success{false};
  size_t bytes_advised{0};
  size_t ranges_count{0};
  int error_code{0};
};

/**
 * @brief ProcessCompactor leverages Linux 5.10+ pidfd_open and process_madvise(2)
 * to advise and reclaim memory ranges of target processes into swap/zram.
 */
class ProcessCompactor {
 public:
  ProcessCompactor() = default;

  /**
   * @brief Check whether pidfd_open and process_madvise syscalls are available.
   * @return true if both syscalls are supported by the host kernel, false otherwise.
   */
  [[nodiscard]] static bool is_supported() noexcept;

  /**
   * @brief Advise specified virtual address ranges of target_pid.
   *
   * Addresses are page-aligned down and lengths expanded to page boundaries.
   * Calls are batched into chunks of at most IOV_MAX (1024).
   * If advice is Pageout and fails with EPERM or EACCES, falls back to Cold.
   *
   * @param target_pid Process ID to compact.
   * @param ranges Vector of (start_address, length) pairs.
   * @param advice Reclaim advice mode.
   * @return ReclaimResult containing status, bytes advised, range count, and error code.
   */
  [[nodiscard]] ReclaimResult reclaim_ranges(
      pid_t target_pid,
      const std::vector<std::pair<uintptr_t, size_t>>& ranges,
      ReclaimAdvice advice = ReclaimAdvice::Pageout);

  /**
   * @brief Scan /proc/<target_pid>/maps and advise all anonymous and writable ranges.
   *
   * @param target_pid Process ID to compact.
   * @param advice Reclaim advice mode.
   * @return ReclaimResult containing status, bytes advised, range count, and error code.
   */
  [[nodiscard]] ReclaimResult reclaim_memory(
      pid_t target_pid,
      ReclaimAdvice advice = ReclaimAdvice::Pageout);

  /**
   * @brief Compact anonymous heap pages for a process into swap/zram via process_madvise.
   * Alias for reclaim_memory(target_pid, advice).
   * @param target_pid Process ID to compact.
   * @param advice Reclaim advice mode (default: Pageout).
   * @return ReclaimResult containing status and reclaimed bytes.
   */
  [[nodiscard]] ReclaimResult compact_pid(
      pid_t target_pid,
      ReclaimAdvice advice = ReclaimAdvice::Pageout) {
    return reclaim_memory(target_pid, advice);
  }
};

}  // namespace virmux
